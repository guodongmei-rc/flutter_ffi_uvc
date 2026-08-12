#ifndef H26X_RAWREC_H
#define H26X_RAWREC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h26x_rawrec h26x_rawrec_t;

/* Passthrough recorder: assembles access units from the camera's Annex B
 * NAL stream and stores them in an interleaved temp file, which the
 * platform layer later remuxes into MP4. No re-encoding is involved.
 *
 * File layout:
 *   header: [8B magic "UVCRAW01"][1B codec][2B csd_len][2B width]
 *           [2B height][1B reserved][csd_len B csd]
 *   per AU: [8B pts_us][4B payload_len][1B flags][payload_len B Annex B AU]
 * codec: 0 = H.264, 1 = H.265. flags bit 0 = keyframe.
 */

h26x_rawrec_t *h26x_rawrec_start(
    const char *path,
    int codec,
    const uint8_t *csd,
    size_t csd_bytes,
    int width,
    int height);

/* Feeds one UVC frame (Annex B, one or more NALs with start codes).
 * Parameter-set NALs are skipped (already in the header CSD). Frames not
 * starting with a start code are continuation tails of frames the camera
 * split across UVC transfers: their leading bytes are appended to the
 * access unit currently being assembled, and any complete NALs after the
 * first start code are processed normally. */
void h26x_rawrec_write_nal(
    h26x_rawrec_t *rec,
    const uint8_t *data,
    size_t bytes,
    uint64_t pts_us);

/* Flushes the pending access unit and closes the file. Returns the number
 * of access units written (0 on NULL). Safe to call once; the recorder is
 * freed. */
uint32_t h26x_rawrec_stop(h26x_rawrec_t *rec);

#ifdef __cplusplus
}
#endif

#endif  // H26X_RAWREC_H
