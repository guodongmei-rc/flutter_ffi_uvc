#ifndef H26X_DECODER_H
#define H26X_DECODER_H

#include <stddef.h>
#include <stdint.h>

#if defined(__ANDROID__)
#include <android/native_window.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h26x_decoder h26x_decoder_t;

enum {
  H26X_CODEC_H264 = 0,
  H26X_CODEC_H265 = 1,
};

/* Feed results. */
enum {
  H26X_FEED_NEED_CSD = 0,   /* waiting for parameter sets or an output window */
  H26X_FEED_OK = 1,         /* NAL accepted, nothing rendered this call */
  H26X_FEED_RENDERED = 2,   /* at least one frame was rendered this call */
  H26X_FEED_ERROR = -1,     /* fatal decoder error, destroy and report */
};

/* codec is H26X_CODEC_*. width/height are the negotiated stream
 * dimensions, written into the MediaFormat at configure time — Qualcomm
 * codecs reject configure without them. Returns NULL on failure. */
h26x_decoder_t *h26x_decoder_create(int codec, int width, int height);

/* Feeds one UVC frame: Annex B NAL unit(s) including start codes. The
 * decoder is configured lazily once the required parameter sets (SPS/PPS,
 * plus VPS for H.265) have been collected and a non-NULL window is
 * available; frames are dropped until then. window may change between
 * calls (NULL = detached) — the codec is reconfigured as needed. */
int h26x_decoder_feed(
    h26x_decoder_t *decoder,
#if defined(__ANDROID__)
    ANativeWindow *window,
#else
    void *window,
#endif
    const uint8_t *data,
    size_t bytes);

void h26x_decoder_destroy(h26x_decoder_t *decoder);

#ifdef __cplusplus
}
#endif

#endif  // H26X_DECODER_H
