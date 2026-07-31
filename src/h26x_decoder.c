#include "h26x_decoder.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__ANDROID__)

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include "libuvc/uvc_log.h"

#define H26X_LOG_TAG "UVC_H26X"

/* Cap on a single NAL/frame accepted for decoding; larger frames are
 * dropped (and logged) rather than crashing the codec path. */
#define H26X_MAX_FRAME_BYTES (4u * 1024u * 1024u)
/* Cap on the collected codec-specific data (VPS/SPS/PPS with start codes). */
#define H26X_MAX_CSD_BYTES 256u
/* Consecutive codec-level failures tolerated before reporting fatal. */
#define H26X_MAX_CODEC_FAILURES 3

typedef enum {
  H26X_STATE_WAIT_CSD = 0,
  H26X_STATE_RUNNING,
} h26x_decoder_state_t;

struct h26x_decoder {
  int codec; /* H26X_CODEC_* */
  int width;
  int height;
  h26x_decoder_state_t state;
  AMediaCodec *media_codec;
  ANativeWindow *window;
  uint8_t csd[H26X_MAX_CSD_BYTES];
  size_t csd_bytes;
  int have_vps;
  int have_sps;
  int have_pps;
  int codec_failures;
  uint64_t pts_us;
};

static uint64_t h26x_now_us(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* Returns the start-code length at data[offset] (3 or 4), or 0. */
static size_t h26x_start_code_at(const uint8_t *data, size_t bytes, size_t offset) {
  if (offset + 3 <= bytes && data[offset] == 0 && data[offset + 1] == 0 &&
      data[offset + 2] == 1) {
    return 3;
  }
  if (offset + 4 <= bytes && data[offset] == 0 && data[offset + 1] == 0 &&
      data[offset + 2] == 0 && data[offset + 3] == 1) {
    return 4;
  }
  return 0;
}

static int h26x_nal_type(const h26x_decoder_t *decoder, uint8_t nal_header) {
  if (decoder->codec == H26X_CODEC_H265) {
    return (nal_header >> 1) & 0x3f;
  }
  return nal_header & 0x1f;
}

/* Returns non-zero when the NAL type is a parameter set we collect. */
static int h26x_is_parameter_nal(const h26x_decoder_t *decoder, int nal_type) {
  if (decoder->codec == H26X_CODEC_H265) {
    return nal_type == 32 || nal_type == 33 || nal_type == 34; /* VPS/SPS/PPS */
  }
  return nal_type == 7 || nal_type == 8; /* SPS/PPS */
}

static void h26x_mark_parameter(h26x_decoder_t *decoder, int nal_type) {
  if (decoder->codec == H26X_CODEC_H265) {
    if (nal_type == 32) decoder->have_vps = 1;
    if (nal_type == 33) decoder->have_sps = 1;
    if (nal_type == 34) decoder->have_pps = 1;
  } else {
    if (nal_type == 7) decoder->have_sps = 1;
    if (nal_type == 8) decoder->have_pps = 1;
  }
}

static int h26x_csd_complete(const h26x_decoder_t *decoder) {
  if (decoder->codec == H26X_CODEC_H265) {
    return decoder->have_vps && decoder->have_sps && decoder->have_pps;
  }
  return decoder->have_sps && decoder->have_pps;
}

/* Scans one frame for parameter-set NALs and appends them to the CSD
 * buffer. Parameter sets may arrive spread across several frames. */
static void h26x_collect_csd(h26x_decoder_t *decoder, const uint8_t *data, size_t bytes) {
  size_t offset = 0;
  while (offset + 3 < bytes) {
    const size_t sc = h26x_start_code_at(data, bytes, offset);
    if (sc == 0) {
      offset++;
      continue;
    }
    /* Find the end of this NAL (next start code or end of frame). */
    size_t next = offset + sc;
    while (next + 3 < bytes && h26x_start_code_at(data, bytes, next) == 0) {
      next++;
    }
    const size_t nal_bytes = next - offset;
    if (nal_bytes > sc) {
      const int nal_type = h26x_nal_type(decoder, data[offset + sc]);
      if (h26x_is_parameter_nal(decoder, nal_type)) {
        if (decoder->csd_bytes + nal_bytes <= H26X_MAX_CSD_BYTES) {
          memcpy(decoder->csd + decoder->csd_bytes, data + offset, nal_bytes);
          decoder->csd_bytes += nal_bytes;
          h26x_mark_parameter(decoder, nal_type);
          UVC_LOGI(
              H26X_LOG_TAG,
              "collected parameter nal type=%d bytes=%zu csd_total=%zu",
              nal_type,
              nal_bytes,
              decoder->csd_bytes);
        } else {
          UVC_LOGW(H26X_LOG_TAG, "csd buffer full, dropping parameter nal");
        }
      }
    }
    offset = next;
  }
}

static int h26x_configure(h26x_decoder_t *decoder, ANativeWindow *window) {
  if (decoder->media_codec != NULL) {
    AMediaCodec_stop(decoder->media_codec);
    AMediaCodec_delete(decoder->media_codec);
    decoder->media_codec = NULL;
  }

  const char *mime =
      decoder->codec == H26X_CODEC_H265 ? "video/hevc" : "video/avc";
  AMediaFormat *format = AMediaFormat_new();
  AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
  if (decoder->width > 0 && decoder->height > 0) {
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, decoder->width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, decoder->height);
  }
  if (decoder->csd_bytes > 0) {
    AMediaFormat_setBuffer(format, "csd-0", decoder->csd, decoder->csd_bytes);
  }

  decoder->media_codec = AMediaCodec_createDecoderByType(mime);
  if (decoder->media_codec == NULL) {
    UVC_LOGW(H26X_LOG_TAG, "AMediaCodec_createDecoderByType failed mime=%s", mime);
    AMediaFormat_delete(format);
    return -1;
  }

  media_status_t status =
      AMediaCodec_configure(decoder->media_codec, format, window, NULL, 0);
  AMediaFormat_delete(format);
  if (status != AMEDIA_OK) {
    UVC_LOGW(H26X_LOG_TAG, "AMediaCodec_configure failed mime=%s status=%d", mime, status);
    AMediaCodec_delete(decoder->media_codec);
    decoder->media_codec = NULL;
    return -1;
  }
  status = AMediaCodec_start(decoder->media_codec);
  if (status != AMEDIA_OK) {
    UVC_LOGW(H26X_LOG_TAG, "AMediaCodec_start failed mime=%s status=%d", mime, status);
    AMediaCodec_delete(decoder->media_codec);
    decoder->media_codec = NULL;
    return -1;
  }
  if (decoder->window != NULL) {
    ANativeWindow_release(decoder->window);
  }
  decoder->window = window;
  ANativeWindow_acquire(window);
  UVC_LOGI(H26X_LOG_TAG, "codec configured mime=%s csd=%zu", mime, decoder->csd_bytes);
  return 0;
}

/* Queues one frame and drains any rendered output. Returns H26X_FEED_OK or
 * H26X_FEED_RENDERED; H26X_FEED_ERROR on fatal codec failure. */
static int h26x_feed_and_drain(
    h26x_decoder_t *decoder,
    const uint8_t *data,
    size_t bytes) {
  int rendered = 0;

  ssize_t input_index = AMediaCodec_dequeueInputBuffer(decoder->media_codec, 10000);
  if (input_index >= 0) {
    size_t capacity = 0;
    uint8_t *buffer =
        AMediaCodec_getInputBuffer(decoder->media_codec, (size_t)input_index, &capacity);
    if (buffer == NULL || bytes > capacity) {
      UVC_LOGW(
          H26X_LOG_TAG,
          "dropping nal too large for codec input bytes=%zu capacity=%zu",
          bytes,
          capacity);
      AMediaCodec_queueInputBuffer(
          decoder->media_codec, (size_t)input_index, 0, 0, 0, 0);
    } else {
      memcpy(buffer, data, bytes);
      decoder->pts_us = h26x_now_us();
      media_status_t status = AMediaCodec_queueInputBuffer(
          decoder->media_codec, (size_t)input_index, 0, bytes, decoder->pts_us, 0);
      if (status != AMEDIA_OK) {
        UVC_LOGW(H26X_LOG_TAG, "queueInputBuffer failed status=%d", status);
        return H26X_FEED_ERROR;
      }
    }
  }
  /* input_index < 0: no free input slot right now — drop the NAL; for
   * preview the next frame is a few ms away and the decoder will catch up. */

  for (;;) {
    AMediaCodecBufferInfo info;
    ssize_t output_index =
        AMediaCodec_dequeueOutputBuffer(decoder->media_codec, &info, 0);
    if (output_index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
      break;
    }
    if (output_index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      AMediaFormat *format = AMediaCodec_getOutputFormat(decoder->media_codec);
      int32_t width = 0, height = 0;
      AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width);
      AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height);
      UVC_LOGI(H26X_LOG_TAG, "output format changed %dx%d", width, height);
      AMediaFormat_delete(format);
      continue;
    }
    if (output_index < 0) {
      break;
    }
    media_status_t status = AMediaCodec_releaseOutputBuffer(
        decoder->media_codec, (size_t)output_index, true);
    if (status != AMEDIA_OK) {
      UVC_LOGW(H26X_LOG_TAG, "releaseOutputBuffer failed status=%d", status);
      return H26X_FEED_ERROR;
    }
    rendered = 1;
  }
  return rendered ? H26X_FEED_RENDERED : H26X_FEED_OK;
}

h26x_decoder_t *h26x_decoder_create(int codec, int width, int height) {
  h26x_decoder_t *decoder = calloc(1, sizeof(h26x_decoder_t));
  if (decoder == NULL) {
    return NULL;
  }
  decoder->codec = codec;
  decoder->width = width;
  decoder->height = height;
  decoder->state = H26X_STATE_WAIT_CSD;
  UVC_LOGI(H26X_LOG_TAG, "decoder created codec=%d %dx%d", codec, width, height);
  return decoder;
}

int h26x_decoder_feed(
    h26x_decoder_t *decoder,
    ANativeWindow *window,
    const uint8_t *data,
    size_t bytes) {
  if (decoder == NULL || data == NULL || bytes == 0) {
    return H26X_FEED_ERROR;
  }
  if (bytes > H26X_MAX_FRAME_BYTES) {
    UVC_LOGW(H26X_LOG_TAG, "dropping oversized h26x frame bytes=%zu", bytes);
    return H26X_FEED_OK;
  }
  /* Mid-stream joins deliver headless tails; feeding them would corrupt the
   * Annex B stream the codec parses. Only frames that start with a start
   * code are ever fed. */
  if (h26x_start_code_at(data, bytes, 0) == 0) {
    return H26X_FEED_OK;
  }

  if (decoder->state == H26X_STATE_WAIT_CSD) {
    if (!h26x_csd_complete(decoder)) {
      h26x_collect_csd(decoder, data, bytes);
    }
    if (!h26x_csd_complete(decoder) || window == NULL) {
      return H26X_FEED_NEED_CSD;
    }
    if (h26x_configure(decoder, window) != 0) {
      return H26X_FEED_ERROR;
    }
    decoder->state = H26X_STATE_RUNNING;
    /* Fall through: this frame carries the parameter sets (and usually the
     * first IDR) — feed it so decoding starts immediately. */
  } else if (window != NULL && window != decoder->window) {
    UVC_LOGI(H26X_LOG_TAG, "output window changed, reconfiguring codec");
    if (h26x_configure(decoder, window) != 0) {
      return H26X_FEED_ERROR;
    }
  }

  if (decoder->media_codec == NULL) {
    return H26X_FEED_NEED_CSD;
  }
  const int result = h26x_feed_and_drain(decoder, data, bytes);
  if (result == H26X_FEED_ERROR) {
    decoder->codec_failures += 1;
    if (decoder->codec_failures < H26X_MAX_CODEC_FAILURES) {
      UVC_LOGW(
          H26X_LOG_TAG,
          "codec failure %d/%d, reconfiguring",
          decoder->codec_failures,
          H26X_MAX_CODEC_FAILURES);
      /* Reconfigure with the stored CSD and carry on. */
      if (decoder->window != NULL &&
          h26x_configure(decoder, decoder->window) == 0) {
        return H26X_FEED_OK;
      }
    }
    return H26X_FEED_ERROR;
  }
  decoder->codec_failures = 0;
  return result;
}

int h26x_decoder_get_config(
    const h26x_decoder_t *decoder,
    int *codec,
    const uint8_t **csd,
    size_t *csd_bytes,
    int *width,
    int *height) {
  if (decoder == NULL || !h26x_csd_complete(decoder)) {
    return -1;
  }
  if (codec != NULL) *codec = decoder->codec;
  if (csd != NULL) *csd = decoder->csd;
  if (csd_bytes != NULL) *csd_bytes = decoder->csd_bytes;
  if (width != NULL) *width = decoder->width;
  if (height != NULL) *height = decoder->height;
  return 0;
}

void h26x_decoder_destroy(h26x_decoder_t *decoder) {
  if (decoder == NULL) {
    return;
  }
  if (decoder->media_codec != NULL) {
    AMediaCodec_stop(decoder->media_codec);
    AMediaCodec_delete(decoder->media_codec);
  }
  if (decoder->window != NULL) {
    ANativeWindow_release(decoder->window);
  }
  UVC_LOGI(H26X_LOG_TAG, "decoder destroyed codec=%d", decoder->codec);
  free(decoder);
}

#else  // !defined(__ANDROID__)

h26x_decoder_t *h26x_decoder_create(int codec, int width, int height) {
  (void)codec;
  (void)width;
  (void)height;
  return NULL;
}

int h26x_decoder_feed(
    h26x_decoder_t *decoder,
    void *window,
    const uint8_t *data,
    size_t bytes) {
  (void)decoder;
  (void)window;
  (void)data;
  (void)bytes;
  return H26X_FEED_ERROR;
}

int h26x_decoder_get_config(
    const h26x_decoder_t *decoder,
    int *codec,
    const uint8_t **csd,
    size_t *csd_bytes,
    int *width,
    int *height) {
  (void)decoder;
  (void)codec;
  (void)csd;
  (void)csd_bytes;
  (void)width;
  (void)height;
  return -1;
}

void h26x_decoder_destroy(h26x_decoder_t *decoder) {
  (void)decoder;
}

#endif  // defined(__ANDROID__)
