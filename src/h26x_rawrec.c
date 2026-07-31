#include "h26x_rawrec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libuvc/uvc_log.h"

#define RAWREC_LOG_TAG "UVC_RAWREC"

#define RAWREC_MAGIC "UVCRAW01"
#define RAWREC_MAGIC_BYTES 8
#define RAWREC_MAX_AU_BYTES (8u * 1024u * 1024u)
#define RAWREC_FLAG_KEYFRAME 0x01

struct h26x_rawrec {
  FILE *file;
  int codec; /* 0 = H.264, 1 = H.265 */
  uint8_t *au;
  size_t au_bytes;
  size_t au_capacity;
  uint64_t au_pts_us;
  int au_keyframe;
  uint32_t au_count;
};

static size_t rawrec_start_code_at(const uint8_t *data, size_t bytes, size_t offset) {
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

static void rawrec_write_u16le(FILE *file, uint16_t value) {
  fputc(value & 0xff, file);
  fputc((value >> 8) & 0xff, file);
}

static void rawrec_write_u32le(FILE *file, uint32_t value) {
  fputc(value & 0xff, file);
  fputc((value >> 8) & 0xff, file);
  fputc((value >> 16) & 0xff, file);
  fputc((value >> 24) & 0xff, file);
}

static void rawrec_write_u64le(FILE *file, uint64_t value) {
  for (int i = 0; i < 8; i++) {
    fputc((int)((value >> (8 * i)) & 0xff), file);
  }
}

static void rawrec_flush_au(h26x_rawrec_t *rec) {
  if (rec->au_bytes == 0) {
    return;
  }
  rawrec_write_u64le(rec->file, rec->au_pts_us);
  rawrec_write_u32le(rec->file, (uint32_t)rec->au_bytes);
  fputc(rec->au_keyframe ? RAWREC_FLAG_KEYFRAME : 0, rec->file);
  fwrite(rec->au, 1, rec->au_bytes, rec->file);
  rec->au_count += 1;
  rec->au_bytes = 0;
  rec->au_keyframe = 0;
}

static void rawrec_au_append(h26x_rawrec_t *rec, const uint8_t *data, size_t bytes) {
  if (rec->au_bytes + bytes > RAWREC_MAX_AU_BYTES) {
    UVC_LOGW(RAWREC_LOG_TAG, "AU exceeds %u bytes, dropping", RAWREC_MAX_AU_BYTES);
    rec->au_bytes = 0;
    rec->au_keyframe = 0;
    return;
  }
  if (rec->au_bytes + bytes > rec->au_capacity) {
    size_t new_capacity = rec->au_capacity == 0 ? 256 * 1024 : rec->au_capacity * 2;
    while (new_capacity < rec->au_bytes + bytes) {
      new_capacity *= 2;
    }
    uint8_t *new_au = realloc(rec->au, new_capacity);
    if (new_au == NULL) {
      UVC_LOGW(RAWREC_LOG_TAG, "AU buffer allocation failed, dropping AU");
      rec->au_bytes = 0;
      rec->au_keyframe = 0;
      return;
    }
    rec->au = new_au;
    rec->au_capacity = new_capacity;
  }
  memcpy(rec->au + rec->au_bytes, data, bytes);
  rec->au_bytes += bytes;
}

h26x_rawrec_t *h26x_rawrec_start(
    const char *path,
    int codec,
    const uint8_t *csd,
    size_t csd_bytes,
    int width,
    int height) {
  if (path == NULL || csd == NULL || csd_bytes == 0 || csd_bytes > 0xffff) {
    UVC_LOGW(RAWREC_LOG_TAG, "rawrec_start invalid args csd=%zu", csd_bytes);
    return NULL;
  }
  FILE *file = fopen(path, "wb");
  if (file == NULL) {
    UVC_LOGW(RAWREC_LOG_TAG, "rawrec_start cannot open %s", path);
    return NULL;
  }
  h26x_rawrec_t *rec = calloc(1, sizeof(h26x_rawrec_t));
  if (rec == NULL) {
    fclose(file);
    return NULL;
  }
  rec->file = file;
  rec->codec = codec;

  fwrite(RAWREC_MAGIC, 1, RAWREC_MAGIC_BYTES, file);
  fputc(codec & 0xff, file);
  rawrec_write_u16le(file, (uint16_t)csd_bytes);
  rawrec_write_u16le(file, (uint16_t)width);
  rawrec_write_u16le(file, (uint16_t)height);
  fputc(0, file); /* reserved */
  fwrite(csd, 1, csd_bytes, file);

  UVC_LOGI(
      RAWREC_LOG_TAG,
      "recording started codec=%d %dx%d csd=%zu path=%s",
      codec,
      width,
      height,
      csd_bytes,
      path);
  return rec;
}

void h26x_rawrec_write_nal(
    h26x_rawrec_t *rec,
    const uint8_t *data,
    size_t bytes,
    uint64_t pts_us) {
  if (rec == NULL || data == NULL || bytes == 0) {
    return;
  }
  /* Skip mid-stream-join tails without a leading start code. */
  if (rawrec_start_code_at(data, bytes, 0) == 0) {
    return;
  }

  size_t offset = 0;
  while (offset + 3 < bytes) {
    const size_t sc = rawrec_start_code_at(data, bytes, offset);
    if (sc == 0) {
      offset++;
      continue;
    }
    /* NAL spans [offset, next): next start code or end of frame. */
    size_t next = offset + sc;
    while (next + 3 < bytes && rawrec_start_code_at(data, bytes, next) == 0) {
      next++;
    }
    const size_t nal_bytes = next - offset;
    if (nal_bytes <= sc) {
      offset = next;
      continue;
    }

    const uint8_t nal_header = data[offset + sc];
    if (rec->codec == 1) {
      /* H.265: 2-byte NAL header; type in bits 1..6 of the first byte. */
      const int nal_type = (nal_header >> 1) & 0x3f;
      const int is_parameter =
          nal_type == 32 || nal_type == 33 || nal_type == 34 || nal_type == 36;
      if (!is_parameter && nal_type < 32) {
        /* VCL NAL: first_slice_segment_in_pic_flag is the MSB after the
         * 2-byte header. */
        const int au_start =
            nal_bytes > sc + 2 && (data[offset + sc + 2] & 0x80) != 0;
        if (au_start) {
          rawrec_flush_au(rec);
          rec->au_pts_us = pts_us;
        }
        if (nal_type == 19 || nal_type == 20) {
          rec->au_keyframe = 1;
        }
        if (rec->au_bytes == 0 && !au_start) {
          /* Continuation slice of an AU whose start was dropped; skip it to
           * keep every stored AU self-contained. */
        } else {
          if (rec->au_bytes == 0) {
            rec->au_pts_us = pts_us;
          }
          rawrec_au_append(rec, data + offset, nal_bytes);
        }
      }
    } else {
      /* H.264: 1-byte NAL header. */
      const int nal_type = nal_header & 0x1f;
      if (nal_type == 9) {
        /* Access unit delimiter: explicit boundary. */
        rawrec_flush_au(rec);
      } else if (nal_type >= 1 && nal_type <= 5) {
        /* VCL: first_mb_in_slice is ue(v); ue value 0 is coded as a single
         * '1' bit, so first_mb == 0 iff the MSB after the header is set. */
        const int au_start = (data[offset + sc + 1] & 0x80) != 0;
        if (au_start) {
          rawrec_flush_au(rec);
          rec->au_pts_us = pts_us;
        }
        if (nal_type == 5) {
          rec->au_keyframe = 1;
        }
        if (rec->au_bytes == 0 && !au_start) {
          /* Continuation slice without its AU start; skip. */
        } else {
          if (rec->au_bytes == 0) {
            rec->au_pts_us = pts_us;
          }
          rawrec_au_append(rec, data + offset, nal_bytes);
        }
      }
      /* Parameter sets (7/8) and SEI (6) are skipped: SPS/PPS live in the
       * header CSD, SEI is not needed for preview recording. */
    }
    offset = next;
  }
}

uint32_t h26x_rawrec_stop(h26x_rawrec_t *rec) {
  if (rec == NULL) {
    return 0;
  }
  rawrec_flush_au(rec);
  fclose(rec->file);
  const uint32_t count = rec->au_count;
  UVC_LOGI(RAWREC_LOG_TAG, "recording stopped aus=%u", count);
  free(rec->au);
  free(rec);
  return count;
}
