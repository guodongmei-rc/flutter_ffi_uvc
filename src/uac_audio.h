#ifndef UAC_AUDIO_H
#define UAC_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#include <libusb.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uac_audio uac_audio_t;

/* USB Audio Class capture: many UVC cameras expose their microphone as a
 * separate UAC AudioStreaming interface (class 1 / subclass 2) next to the
 * video interfaces. libuvc only claims the video interfaces, so the audio
 * interface can be claimed and read with isochronous IN transfers on the
 * same libusb context (the libuvc event thread services them).
 *
 * The captured PCM is appended to a ring buffer; the platform layer polls
 * uac_audio_read() from a dedicated thread and feeds an AAC encoder.
 */

typedef struct {
  int interface_no;   /* bInterfaceNumber to claim */
  int altsetting;     /* alternate setting carrying the isoc endpoint */
  uint8_t ep_address; /* isochronous IN endpoint */
  uint8_t ep_interval;/* endpoint bInterval (isoc service interval) */
  uint32_t packet_size;
  int channels;
  int bits;
  int sample_rate;
} uac_audio_info_t;

/* Bounds the altsetting list uac_audio_probe_all() collects. */
#define UAC_MAX_CANDIDATES 16

/* Scans the active configuration for a UAC AudioStreaming interface with an
 * isochronous IN endpoint and parses its format descriptor (UAC1 Format
 * Type I; UAC2 falls back to a packet-size heuristic for the sample rate).
 * Returns 0 and fills *out on success, -1 when the device has no usable
 * audio capture interface. */
int uac_audio_probe(libusb_device_handle *usb_devh, uac_audio_info_t *out);

/* Same scan as uac_audio_probe() but collects EVERY usable altsetting into
 * out[] (up to max_candidates) and returns the count, -1 on descriptor
 * errors. The sample rates are packet-size heuristics until a stream is
 * actually measured — see uac_audio_start(). */
int uac_audio_probe_all(
    libusb_device_handle *usb_devh,
    uac_audio_info_t *out,
    int max_candidates);

/* Claims the interface of candidates[0], selects its altsetting and starts
 * isoc transfers into the ring buffer. Only ONE altsetting is ever set:
 * cycling altsettings while the video stream runs wedges some camera
 * firmwares. After the stream starts, the real delivery rate is measured
 * over a short window (~300ms); when it snaps cleanly to a standard sample
 * rate, that measured rate replaces the descriptor estimate — the winning
 * format is copied back to candidates[0] so the caller can configure its
 * encoder with the true rate.
 * Returns NULL on failure (fully cleaned up; the caller is expected to fall
 * back to video-only recording).
 * video_streaming!=0 skips the log-only AudioControl probes (selector-unit
 * GET_CUR) so the EP0 traffic added while the video stream is running stays
 * limited to the transfers the mic actually needs (unmute, volume). */
uac_audio_t *uac_audio_start(
    libusb_device_handle *usb_devh,
    uac_audio_info_t *candidates,
    int candidate_count,
    int video_streaming);

/* Pins the session while a reader thread is inside uac_audio_read(). The
 * owner stops/frees sessions only while holding its state lock, so pairing
 * retain with the pointer fetch under that lock keeps stop() from freeing
 * the session mid-read. */
void uac_audio_retain(uac_audio_t *audio);
void uac_audio_release(uac_audio_t *audio);

/* Copies up to max_bytes of PCM into dst. Returns the byte count, 0 on
 * timeout, or -1 once the capture is stopping and the ring is drained. */
int uac_audio_read(uac_audio_t *audio, uint8_t *dst, int max_bytes, int timeout_ms);

/* Stops transfers, releases the interface and frees the session. Blocks
 * until in-flight callbacks and active readers have left the session, so
 * the caller must not hold a lock those threads need. Safe on NULL. */
void uac_audio_stop(uac_audio_t *audio);

#ifdef __cplusplus
}
#endif

#endif  // UAC_AUDIO_H
