#include "uac_audio.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libuvc/uvc_log.h"

#if defined(__ANDROID__)
#include <android/log.h>
/* uvc_log is compiled out in this build; diagnostics here must still reach
 * logcat, so all logs go through __android_log_print directly. Info-level
 * logs (including the per-second stats line) are gated behind the DEBUG
 * native log level; warnings always print. */
#define UAC_STATS_LOGI(...) \
  do { \
    if (uvc_log_enabled(UVC_LOG_LEVEL_DEBUG)) { \
      __android_log_print(ANDROID_LOG_INFO, "flutter_ffi_uvc", __VA_ARGS__); \
    } \
  } while (0)
#define UAC_STATS_LOGW(...) \
  __android_log_print(ANDROID_LOG_WARN, "flutter_ffi_uvc", __VA_ARGS__)
#else
#define UAC_STATS_LOGI(...) UVC_LOGI(UAC_LOG_TAG, __VA_ARGS__)
#define UAC_STATS_LOGW(...) UVC_LOGW(UAC_LOG_TAG, __VA_ARGS__)
#endif

#define UAC_LOG_TAG "UVC_AUDIO"

#define UAC_NUM_TRANSFERS 8
#define UAC_ISO_PACKETS 8
#define UAC_RING_CAPACITY (512 * 1024)

/* USB Audio Class descriptor constants. */
#define UAC_CLASS_AUDIO 0x01
#define UAC_SUBCLASS_STREAMING 0x02
#define UAC_DT_CS_INTERFACE 0x24
#define UAC_ST_FORMAT_TYPE 0x02
#define UAC_FORMAT_TYPE_I 0x01

struct uac_audio {
  libusb_device_handle *usb_devh; /* not owned */
  uac_audio_info_t info;
  struct libusb_transfer *transfers[UAC_NUM_TRANSFERS];
  uint8_t *ring;
  size_t ring_head;  /* read position */
  size_t ring_count;
  uint64_t dropped_bytes;
  uint32_t transfer_errors;
  int pending;   /* transfers in flight (callback guaranteed) */
  int readers;   /* threads currently inside uac_audio_read */
  int stopping;
  /* Total delivered PCM bytes since start; uac_audio_start() samples it
   * twice to measure the real capture rate. Never reset while streaming. */
  uint64_t stat_bytes_total;
  /* Capture diagnostics: per-window (1000 packets ≈ 1s at 1ms/packet). */
  uint64_t stat_packets;
  uint64_t stat_packets_with_data;
  uint64_t stat_bytes;
  int stat_peak;
  int stat_first_dumped;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
};

// ---------------------------------------------------------------------------
// Probe
// ---------------------------------------------------------------------------

/* Walks the class-specific descriptors of an AudioStreaming interface and
 * parses the Format Type I descriptor. UAC1 carries channels/bit resolution
 * and discrete 3-byte sample rates; UAC2 carries only subslot size and bit
 * resolution (channels/rate live in the cluster/clock descriptors and are
 * left 0 here for the packet-size heuristic below). */
static void uac_parse_format(
    const unsigned char *extra,
    int extra_length,
    int *channels,
    int *bits,
    int *sample_rate) {
  int offset = 0;
  while (offset + 2 <= extra_length) {
    const int length = extra[offset];
    const int type = extra[offset + 1];
    if (length < 2 || offset + length > extra_length) {
      break;
    }
    if (type == UAC_DT_CS_INTERFACE && length >= 6 &&
        extra[offset + 2] == UAC_ST_FORMAT_TYPE &&
        extra[offset + 3] == UAC_FORMAT_TYPE_I) {
      const int freq_count = length >= 8 ? extra[offset + 7] : 0;
      if (length >= 11 && freq_count >= 1 && length >= 8 + 3 * freq_count) {
        /* UAC1 Type I: bNrChannels(4) bSubframeSize(5) bBitResolution(6)
         * bSamFreqType(7) tSamFreq[3 * n](8..). */
        *channels = extra[offset + 4];
        *bits = extra[offset + 6];
        *sample_rate = extra[offset + 8] | (extra[offset + 9] << 8) |
                       (extra[offset + 10] << 16);
      } else {
        /* UAC2 Type I: bSubslotSize(4) bBitResolution(5). */
        *bits = extra[offset + 5];
      }
      return;
    }
    offset += length;
  }
}

static int uac_snap_sample_rate(int estimate) {
  static const int std_rates[] = {
      8000, 11025, 12000, 16000, 22050, 24000, 32000,
      44100, 48000, 64000, 88200, 96000, 176400, 192000,
  };
  for (size_t i = 0; i < sizeof(std_rates) / sizeof(std_rates[0]); i++) {
    const int rate = std_rates[i];
    const int diff = estimate > rate ? estimate - rate : rate - estimate;
    if (diff <= rate / 32) {
      return rate;
    }
  }
  return 0;
}

/* Isoc packets per second for an endpoint bInterval: high-speed devices
 * schedule 2^(bInterval-1) microframes (125us units) between packets. */
static int uac_packets_per_sec(uint8_t ep_interval) {
  int exponent = ep_interval > 0 ? ep_interval - 1 : 0;
  if (exponent > 4) {
    exponent = 4;
  }
  return 8000 >> exponent;
}

/* UAC2 descriptors do not state the capture sample rate in the streaming
 * interface, so estimate it from the isoc packet size: a synchronous IN
 * endpoint delivers one packet per service interval holding
 * rate / packets_per_sec PCM frames. */
static void uac_estimate_format(uac_audio_info_t *info, uint8_t ep_interval) {
  if (info->bits <= 0) {
    info->bits = 16;
  }
  if (info->channels > 0 && info->sample_rate > 0) {
    return;
  }
  const int bytes_per_sample = (info->bits + 7) / 8;
  const int packets_per_sec = uac_packets_per_sec(ep_interval);

  int candidates[2];
  int candidate_count = 0;
  if (info->channels > 0) {
    candidates[candidate_count++] = info->channels;
  } else {
    candidates[candidate_count++] = 2;
    candidates[candidate_count++] = 1;
  }
  for (int i = 0; i < candidate_count; i++) {
    const int frame_bytes = candidates[i] * bytes_per_sample;
    if (frame_bytes <= 0 || info->packet_size % (uint32_t)frame_bytes != 0) {
      continue;
    }
    const int estimate =
        (int)(info->packet_size / (uint32_t)frame_bytes) * packets_per_sec;
    const int snapped = uac_snap_sample_rate(estimate);
    if (snapped > 0) {
      info->channels = candidates[i];
      info->sample_rate = snapped;
      return;
    }
  }
  if (info->channels <= 0) {
    info->channels = 2;
  }
  if (info->sample_rate <= 0) {
    info->sample_rate = 48000;
  }
}

/* Reads bTerminalLink from the AS General descriptor of an AudioStreaming
 * interface; 0 when absent. The link tells which AudioControl terminal this
 * streaming interface carries — a capture interface should link to the
 * output terminal fed by the microphone. */
static int uac_as_terminal_link(const unsigned char *extra, int extra_length) {
  int offset = 0;
  while (offset + 4 <= extra_length) {
    const int length = extra[offset];
    const int type = extra[offset + 1];
    if (length < 2 || offset + length > extra_length) {
      break;
    }
    if (type == UAC_DT_CS_INTERFACE && extra[offset + 2] == 0x01 &&
        length >= 4) {
      return extra[offset + 3];
    }
    offset += length;
  }
  return 0;
}

int uac_audio_probe_all(
    libusb_device_handle *usb_devh,
    uac_audio_info_t *out,
    int max_candidates) {
  if (usb_devh == NULL || out == NULL || max_candidates <= 0) {
    return -1;
  }
  libusb_device *dev = libusb_get_device(usb_devh);
  struct libusb_config_descriptor *config = NULL;
  if (libusb_get_active_config_descriptor(dev, &config) != 0 &&
      libusb_get_config_descriptor(dev, 0, &config) != 0) {
    UAC_STATS_LOGW( "probe: cannot read config descriptor");
    return -1;
  }

  int found = 0;
  for (int i = 0; i < config->bNumInterfaces; i++) {
    const struct libusb_interface *iface = &config->interface[i];
    for (int a = 0; a < iface->num_altsetting; a++) {
      const struct libusb_interface_descriptor *alt = &iface->altsetting[a];
      if (alt->bInterfaceClass != UAC_CLASS_AUDIO ||
          alt->bInterfaceSubClass != UAC_SUBCLASS_STREAMING) {
        continue;
      }
      for (int e = 0; e < alt->bNumEndpoints; e++) {
        const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
        if ((ep->bmAttributes & 0x03) != 0x01) {
          continue;  /* not isochronous */
        }
        if ((ep->bEndpointAddress & 0x80) == 0) {
          continue;  /* capture needs IN */
        }
        const uint32_t packet_size = ep->wMaxPacketSize & 0x7ff;
        if (packet_size == 0) {
          continue;  /* zero-bandwidth altsetting */
        }
        /* Every candidate is logged: devices with several streaming
         * interfaces need the full list to diagnose a wrong pick. */
        uac_audio_info_t candidate;
        memset(&candidate, 0, sizeof(candidate));
        candidate.packet_size = packet_size;
        candidate.interface_no = alt->bInterfaceNumber;
        candidate.altsetting = alt->bAlternateSetting;
        candidate.ep_address = ep->bEndpointAddress;
        candidate.ep_interval = ep->bInterval;
        uac_parse_format(
            alt->extra,
            alt->extra_length,
            &candidate.channels,
            &candidate.bits,
            &candidate.sample_rate);
        uac_estimate_format(&candidate, ep->bInterval);
        UAC_STATS_LOGI(
            "probe candidate: interface=%d alt=%d ep=0x%02x packet=%u "
            "interval=%u rate=%d ch=%d bits=%d terminal_link=%d%s",
            candidate.interface_no,
            candidate.altsetting,
            candidate.ep_address,
            candidate.packet_size,
            candidate.ep_interval,
            candidate.sample_rate,
            candidate.channels,
            candidate.bits,
            uac_as_terminal_link(alt->extra, alt->extra_length),
            found == 0 ? " (probe default)" : "");
        if (found < max_candidates) {
          out[found] = candidate;
        }
        found++;
      }
    }
  }
  libusb_free_config_descriptor(config);

  if (found == 0) {
    UAC_STATS_LOGI( "probe: no UAC AudioStreaming capture interface");
    return -1;
  }
  if (found > max_candidates) {
    UAC_STATS_LOGI(
        "probe: %d candidates, keeping first %d", found, max_candidates);
    found = max_candidates;
  }
  return found;
}

int uac_audio_probe(libusb_device_handle *usb_devh, uac_audio_info_t *out) {
  return uac_audio_probe_all(usb_devh, out, 1) > 0 ? 0 : -1;
}

// ---------------------------------------------------------------------------
// Feature Unit volume
// ---------------------------------------------------------------------------

/* UAC1 AudioControl constants. */
#define UAC_SUBCLASS_CONTROL 0x01
#define UAC_ST_INPUT_TERMINAL 0x02
#define UAC_ST_OUTPUT_TERMINAL 0x03
#define UAC_ST_SELECTOR_UNIT 0x05
#define UAC_ST_FEATURE_UNIT 0x06
#define UAC_REQ_SET_CUR 0x01
#define UAC_REQ_GET_CUR 0x81
#define UAC_REQ_GET_MAX 0x83
#define UAC_CS_MUTE 0x01
#define UAC_CS_VOLUME 0x02

/* Best-effort mic enable. Walks every AudioControl interface and:
 *  - logs every unit/terminal descriptor (diagnosing a silent mic needs the
 *    topology in logcat);
 *  - unmutes and maxes the volume of EVERY channel of every UAC1 Feature
 *    Unit — some firmwares expose only per-channel controls, and handling
 *    just the master channel leaves the mic silent;
 *  - logs the current input of every Selector Unit (a selector defaulting
 *    to an unconnected input captures only noise floor). That GET_CUR is
 *    log-only, so it is skipped when video_streaming!=0: while the video
 *    stream runs, every synchronous EP0 transfer holds the libusb events
 *    lock and stalls video URB resubmission, so only the transfers the mic
 *    needs (unmute / volume) are issued.
 * Failures are logged and ignored. */
static void uac_configure_audio_controls(
    libusb_device_handle *usb_devh, int video_streaming) {
  libusb_device *dev = libusb_get_device(usb_devh);
  struct libusb_config_descriptor *config = NULL;
  if (libusb_get_active_config_descriptor(dev, &config) != 0 &&
      libusb_get_config_descriptor(dev, 0, &config) != 0) {
    return;
  }

  for (int i = 0; i < config->bNumInterfaces; i++) {
    const struct libusb_interface *iface = &config->interface[i];
    if (iface->num_altsetting <= 0) {
      continue;
    }
    const struct libusb_interface_descriptor *alt = &iface->altsetting[0];
    if (alt->bInterfaceClass != UAC_CLASS_AUDIO ||
        alt->bInterfaceSubClass != UAC_SUBCLASS_CONTROL) {
      continue;
    }
    const unsigned char *extra = alt->extra;
    const int remaining = alt->extra_length;
    int offset = 0;
    while (offset + 3 <= remaining) {
      const int length = extra[offset];
      const int type = extra[offset + 1];
      if (length < 2 || offset + length > remaining) {
        break;
      }
      if (type != UAC_DT_CS_INTERFACE) {
        offset += length;
        continue;
      }
      const int subtype = extra[offset + 2];
      const unsigned char *d = extra + offset;
      const uint16_t ifc = alt->bInterfaceNumber;
      switch (subtype) {
        case UAC_ST_INPUT_TERMINAL:
          if (length >= 6) {
            UAC_STATS_LOGI(
                "ac: input terminal id=%d type=0x%02x%02x ifc=%d",
                d[3], d[5], d[4], ifc);
          }
          break;
        case UAC_ST_OUTPUT_TERMINAL:
          if (length >= 8) {
            /* d[4..5]=wTerminalType, d[6]=bAssocTerminal, d[7]=bSourceID. */
            UAC_STATS_LOGI(
                "ac: output terminal id=%d type=0x%02x%02x source=%d ifc=%d",
                d[3], d[5], d[4], d[7], ifc);
          }
          break;
        case UAC_ST_SELECTOR_UNIT:
          if (length >= 5) {
            const int unit_id = d[3];
            if (video_streaming) {
              /* Log-only probe; skipped while the video stream runs (see
               * the function comment). */
              UAC_STATS_LOGI(
                  "ac: selector unit=%d inputs=%d current=skipped ifc=%d",
                  unit_id, d[4], ifc);
              break;
            }
            uint8_t cur = 0;
            const int rc = libusb_control_transfer(
                usb_devh, 0xa1, UAC_REQ_GET_CUR, 0,
                (uint16_t)((unit_id << 8) | ifc), &cur, 1, 300);
            UAC_STATS_LOGI(
                "ac: selector unit=%d inputs=%d current=%d rc=%d ifc=%d",
                unit_id, d[4], rc == 1 ? (int)cur : -1, rc, ifc);
          }
          break;
        case UAC_ST_FEATURE_UNIT:
          if (length >= 7) {
            const int unit_id = d[3];
            const int control_size = d[5];
            if (control_size < 1 || control_size > 4) {
              break;
            }
            /* Entry 0 is the master channel, then one per logical channel. */
            const int entries = (length - 6) / control_size;
            for (int ch = 0; ch < entries && ch < 9; ch++) {
              const int controls = d[6 + ch * control_size];
              const uint16_t windex = (uint16_t)((unit_id << 8) | ifc);
              if (controls & 0x01) {
                uint8_t mute = 0;
                const int rc = libusb_control_transfer(
                    usb_devh, 0x21, UAC_REQ_SET_CUR,
                    (uint16_t)((UAC_CS_MUTE << 8) | ch),
                    windex, &mute, 1, 300);
                UAC_STATS_LOGI(
                    "ac: unmute unit=%d ch=%d rc=%d", unit_id, ch, rc);
              }
              if (controls & 0x02) {
                uint8_t buf[2] = {0, 0};
                int rc = libusb_control_transfer(
                    usb_devh, 0xa1, UAC_REQ_GET_MAX,
                    (uint16_t)((UAC_CS_VOLUME << 8) | ch),
                    windex, buf, 2, 300);
                if (rc == 2) {
                  const int16_t max_vol = (int16_t)(buf[0] | (buf[1] << 8));
                  rc = libusb_control_transfer(
                      usb_devh, 0x21, UAC_REQ_SET_CUR,
                      (uint16_t)((UAC_CS_VOLUME << 8) | ch),
                      windex, buf, 2, 300);
                  UAC_STATS_LOGI(
                      "ac: volume max unit=%d ch=%d max=%d/256dB rc=%d",
                      unit_id, ch, max_vol, rc);
                } else {
                  UAC_STATS_LOGW(
                      "ac: volume GET_MAX failed unit=%d ch=%d rc=%d",
                      unit_id, ch, rc);
                }
              }
            }
            if (entries == 0) {
              UAC_STATS_LOGI(
                  "ac: feature unit=%d ifc=%d has no controls", unit_id, ifc);
            }
          }
          break;
        default:
          UAC_STATS_LOGI(
              "ac: unit subtype=0x%02x id=%d len=%d ifc=%d",
              subtype, length >= 4 ? d[3] : -1, length, ifc);
          break;
      }
      offset += length;
    }
  }
  libusb_free_config_descriptor(config);
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

/* Appends to the ring buffer, dropping the oldest bytes on overflow. Caller
 * holds audio->mutex. */
static void uac_ring_append(uac_audio_t *audio, const uint8_t *data, size_t bytes) {
  if (bytes > UAC_RING_CAPACITY) {
    data += bytes - UAC_RING_CAPACITY;
    bytes = UAC_RING_CAPACITY;
  }
  if (audio->ring_count + bytes > UAC_RING_CAPACITY) {
    const size_t drop = audio->ring_count + bytes - UAC_RING_CAPACITY;
    audio->ring_head = (audio->ring_head + drop) % UAC_RING_CAPACITY;
    audio->ring_count -= drop;
    audio->dropped_bytes += drop;
    if (audio->dropped_bytes == drop || audio->dropped_bytes % (4 * 1024 * 1024) < drop) {
      UAC_STATS_LOGW(
          "ring overflow, dropping %zu bytes (total=%" PRIu64 ")",
          drop,
          audio->dropped_bytes);
    }
  }
  size_t tail = (audio->ring_head + audio->ring_count) % UAC_RING_CAPACITY;
  size_t first = UAC_RING_CAPACITY - tail;
  if (first > bytes) {
    first = bytes;
  }
  memcpy(audio->ring + tail, data, first);
  memcpy(audio->ring, data + first, bytes - first);
  audio->ring_count += bytes;
  pthread_cond_broadcast(&audio->cond);
}

/* Updates capture diagnostics for one isoc packet. Caller holds the mutex. */
static void uac_stats_packet(
    uac_audio_t *audio, const uint8_t *data, unsigned int length) {
  audio->stat_packets++;
  if (length == 0) {
    return;
  }
  audio->stat_packets_with_data++;
  audio->stat_bytes += length;
  /* Peak 16-bit amplitude: distinguishes live mic data from a zero stream. */
  for (unsigned int i = 0; i + 1 < length; i += 2) {
    const int16_t s = (int16_t)(data[i] | (data[i + 1] << 8));
    const int a = s < 0 ? -s : s;
    if (a > audio->stat_peak) {
      audio->stat_peak = a;
    }
  }
  if (!audio->stat_first_dumped && audio->stat_peak > 0) {
    audio->stat_first_dumped = 1;
    char hex[3 * 32 + 1];
    hex[0] = '\0';
    const unsigned int n = length < 32 ? length : 32;
    for (unsigned int i = 0; i < n; i++) {
      snprintf(hex + i * 3, 4, "%02x ", data[i]);
    }
    UAC_STATS_LOGI("uac first pcm data: %s", hex);
  }
  if (audio->stat_packets >= 1000) {
    UAC_STATS_LOGI(
        "uac stats: packets=%llu with_data=%llu bytes=%llu peak=%d",
        (unsigned long long)audio->stat_packets,
        (unsigned long long)audio->stat_packets_with_data,
        (unsigned long long)audio->stat_bytes,
        audio->stat_peak);
    audio->stat_packets = 0;
    audio->stat_packets_with_data = 0;
    audio->stat_bytes = 0;
    audio->stat_peak = 0;
  }
}

static void LIBUSB_CALL uac_audio_transfer_cb(struct libusb_transfer *transfer) {
  uac_audio_t *audio = transfer->user_data;
  int resubmit = 0;

  pthread_mutex_lock(&audio->mutex);
  audio->pending--;
  if (audio->stopping) {
    /* Drain: leave the transfer cancelled. */
  } else if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
    for (int p = 0; p < transfer->num_iso_packets; p++) {
      const struct libusb_iso_packet_descriptor *packet =
          &transfer->iso_packet_desc[p];
      audio->stat_bytes_total += packet->actual_length;
      uac_stats_packet(
          audio,
          transfer->buffer + p * audio->info.packet_size,
          packet->actual_length);
      if (packet->status == 0 && packet->actual_length > 0) {
        uac_ring_append(
            audio,
            transfer->buffer + p * audio->info.packet_size,
            packet->actual_length);
      }
    }
    resubmit = 1;
  } else if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
    /* Device unplugged: end the capture so readers finish cleanly. */
    audio->stopping = 1;
    pthread_cond_broadcast(&audio->cond);
  } else {
    audio->transfer_errors++;
    if (audio->transfer_errors == 1 || audio->transfer_errors % 256 == 0) {
      UAC_STATS_LOGW(
          "isoc transfer status=%d errors=%u",
          transfer->status,
          audio->transfer_errors);
    }
    resubmit = 1;
  }

  if (resubmit) {
    audio->pending++;
    pthread_mutex_unlock(&audio->mutex);
    if (libusb_submit_transfer(transfer) != 0) {
      pthread_mutex_lock(&audio->mutex);
      audio->pending--;
      audio->stopping = 1;
      pthread_cond_broadcast(&audio->cond);
      pthread_mutex_unlock(&audio->mutex);
    }
  } else {
    pthread_cond_broadcast(&audio->cond);  /* uac_audio_stop may be waiting */
    pthread_mutex_unlock(&audio->mutex);
  }
}

/* How long the freshly started stream is measured to learn the real PCM
 * delivery rate before reporting the capture format. */
#define UAC_MEASURE_WINDOW_MS 300

/* UAC1 sampling-frequency control (spec 5.2.1.1): a class request on the
 * isochronous endpoint itself. Some firmwares power up on a very low
 * default clock (observed ~1000 Hz on a RunCam, delivering only 2000 B/s)
 * and stream at full rate only after the host sets the frequency — camera
 * apps whose UAC init sets it get full audio on the same hardware. */
#define UAC_EP_REQ_GET_CUR 0x81
#define UAC_EP_REQ_SET_CUR 0x01
#define UAC_EP_CS_SAMPLING_FREQ 0x01

/* Returns the endpoint's current sampling frequency, or -1 on failure. */
static int uac_get_sampling_freq(libusb_device_handle *usb_devh, uint8_t ep_address) {
  uint8_t buf[3] = {0, 0, 0};
  const int rc = libusb_control_transfer(
      usb_devh, 0xa2 /* IN | class | endpoint */, UAC_EP_REQ_GET_CUR,
      (uint16_t)(UAC_EP_CS_SAMPLING_FREQ << 8), ep_address, buf, 3, 300);
  if (rc != 3) {
    return -1;
  }
  return buf[0] | (buf[1] << 8) | (buf[2] << 16);
}

/* Sets the endpoint's sampling frequency; returns the libusb status. */
static int uac_set_sampling_freq(
    libusb_device_handle *usb_devh, uint8_t ep_address, int freq) {
  uint8_t buf[3] = {
      (uint8_t)(freq & 0xff),
      (uint8_t)((freq >> 8) & 0xff),
      (uint8_t)((freq >> 16) & 0xff),
  };
  return libusb_control_transfer(
      usb_devh, 0x22 /* OUT | class | endpoint */, UAC_EP_REQ_SET_CUR,
      (uint16_t)(UAC_EP_CS_SAMPLING_FREQ << 8), ep_address, buf, 3, 300);
}

/* Claims an interface, detaching a kernel audio driver and retrying briefly:
 * a capture stopped moments ago can leave the interface briefly unclaimable
 * (the altsetting-0 reset is still in flight). Returns 0 on success. */
static int uac_claim_interface(libusb_device_handle *usb_devh, int interface_no) {
  int rc = -1;
  for (int attempt = 0; attempt < 3; attempt++) {
    rc = libusb_claim_interface(usb_devh, interface_no);
    if (rc == LIBUSB_ERROR_BUSY) {
      /* The kernel audio driver (snd-usb-audio) may hold the interface. */
      libusb_detach_kernel_driver(usb_devh, interface_no);
      rc = libusb_claim_interface(usb_devh, interface_no);
    }
    if (rc == 0) {
      return 0;
    }
    if (attempt + 1 < 3) {
      const struct timespec pause = {0, 50 * 1000 * 1000};
      nanosleep(&pause, NULL);
    }
  }
  return rc;
}

/* Points every session transfer at the candidate's endpoint and submits
 * them. Returns how many transfers went out (0 = candidate unusable, with
 * the last libusb error in out_rc when non-NULL). */
static int uac_submit_transfers(
    uac_audio_t *audio, const uac_audio_info_t *info, int *out_rc) {
  const int transfer_bytes = (int)(info->packet_size * UAC_ISO_PACKETS);
  audio->info = *info;
  for (int i = 0; i < UAC_NUM_TRANSFERS; i++) {
    struct libusb_transfer *transfer = audio->transfers[i];
    if (transfer == NULL) {
      continue;
    }
    libusb_fill_iso_transfer(
        transfer,
        audio->usb_devh,
        info->ep_address,
        transfer->buffer,
        transfer_bytes,
        UAC_ISO_PACKETS,
        uac_audio_transfer_cb,
        audio,
        0 /* no timeout; stop() cancels explicitly */);
    libusb_set_iso_packet_lengths(transfer, info->packet_size);
  }
  int submitted = 0;
  int last_rc = 0;
  for (int i = 0; i < UAC_NUM_TRANSFERS; i++) {
    if (audio->transfers[i] == NULL) {
      continue;
    }
    const int rc = libusb_submit_transfer(audio->transfers[i]);
    if (rc == 0) {
      submitted++;
    } else {
      last_rc = rc;
    }
  }
  if (out_rc != NULL) {
    *out_rc = last_rc;
  }
  pthread_mutex_lock(&audio->mutex);
  audio->pending += submitted;
  pthread_mutex_unlock(&audio->mutex);
  return submitted;
}

static int64_t uac_monotonic_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

uac_audio_t *uac_audio_start(
    libusb_device_handle *usb_devh,
    uac_audio_info_t *candidates,
    int candidate_count,
    int video_streaming) {
  if (usb_devh == NULL || candidates == NULL || candidate_count <= 0) {
    return NULL;
  }
  const uac_audio_info_t *info = &candidates[0];
  if (info->packet_size == 0) {
    return NULL;
  }
  const int descriptor_rate = info->sample_rate;

  uac_audio_t *audio = calloc(1, sizeof(uac_audio_t));
  if (audio == NULL) {
    return NULL;
  }
  audio->usb_devh = usb_devh;
  audio->info = *info;
  pthread_mutex_init(&audio->mutex, NULL);
  pthread_condattr_t cond_attr;
  pthread_condattr_init(&cond_attr);
#if defined(__ANDROID__)
  /* Monotonic clock: uac_audio_read deadlines must not jump on NTP syncs.
   * (pthread_condattr_setclock is POSIX; guard so this file still compiles
   * on hosts like macOS that lack it.) */
  pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
#endif
  pthread_cond_init(&audio->cond, &cond_attr);
  pthread_condattr_destroy(&cond_attr);

  const size_t transfer_bytes = (size_t)info->packet_size * UAC_ISO_PACKETS;
  audio->ring = malloc(UAC_RING_CAPACITY);
  int transfers_ready = 0;
  if (audio->ring != NULL) {
    for (int i = 0; i < UAC_NUM_TRANSFERS; i++) {
      struct libusb_transfer *transfer = libusb_alloc_transfer(UAC_ISO_PACKETS);
      uint8_t *buffer = transfer != NULL ? malloc(transfer_bytes) : NULL;
      if (transfer == NULL || buffer == NULL) {
        if (transfer != NULL) {
          libusb_free_transfer(transfer);
        }
        free(buffer);
        break;
      }
      /* uac_submit_transfers() fills the transfer later and reads
       * transfer->buffer, so the buffer must be attached here. */
      transfer->buffer = buffer;
      audio->transfers[i] = transfer;
      transfers_ready++;
    }
  }
  if (transfers_ready == 0) {
    UAC_STATS_LOGW( "start: transfer allocation failed");
    uac_audio_stop(audio);
    return NULL;
  }

  /* Commit to the first candidate directly — one claim, one SET_INTERFACE.
   * Cycling altsettings while the video stream runs wedges some camera
   * firmwares (on a RunCam the video stream dies mid-switch and does not
   * recover until the next power cycle), and on such devices every
   * altsetting delivers the same trickle anyway, so hopping buys nothing. */
  const int claim_rc = uac_claim_interface(usb_devh, info->interface_no);
  if (claim_rc != 0) {
    UAC_STATS_LOGW(
        "start: claim interface=%d failed rc=%d", info->interface_no, claim_rc);
    uac_audio_stop(audio);
    return NULL;
  }
  if (libusb_set_interface_alt_setting(
          usb_devh, info->interface_no, info->altsetting) != 0) {
    UAC_STATS_LOGW( "start: altsetting=%d rejected", info->altsetting);
    uac_audio_stop(audio);
    return NULL;
  }

  /* EP0 works without claiming the audio interface; walk the AudioControl
   * topology and raise every mic gain found before streaming starts. */
  uac_configure_audio_controls(usb_devh, video_streaming);

  /* Read the endpoint's current sampling frequency and push the target:
   * firmwares idling on a low default clock only stream at full rate after
   * an explicit SET_CUR. Failures are logged and ignored — devices that do
   * not implement the control simply stream their (already correct) rate. */
  const int freq_before = uac_get_sampling_freq(usb_devh, info->ep_address);
  const int freq_set_rc =
      uac_set_sampling_freq(usb_devh, info->ep_address, info->sample_rate);
  const int freq_after = uac_get_sampling_freq(usb_devh, info->ep_address);
  UAC_STATS_LOGI(
      "ep sampling freq: before=%d set=%d rc=%d after=%d",
      freq_before, info->sample_rate, freq_set_rc, freq_after);

  int submit_rc = 0;
  const int submitted = uac_submit_transfers(audio, info, &submit_rc);
  if (submitted == 0) {
    UAC_STATS_LOGW( "start: no isoc transfer could be submitted rc=%d", submit_rc);
    uac_audio_stop(audio);
    return NULL;
  }

  /* Measure the real delivery rate for a short window: descriptor rates
   * are estimates (UAC2 does not even state one), so the encoder rate is
   * derived from measured bytes — but only when the measurement snaps
   * cleanly to a standard rate; otherwise the descriptor estimate stands.
   * PCM arriving during the window stays in the ring; the reader drains it
   * once the encoder starts. */
  pthread_mutex_lock(&audio->mutex);
  const uint64_t measure_b0 = audio->stat_bytes_total;
  pthread_mutex_unlock(&audio->mutex);
  const int64_t measure_t0 = uac_monotonic_ns();
  struct timespec window = {0, UAC_MEASURE_WINDOW_MS * 1000 * 1000};
  while (nanosleep(&window, &window) != 0 && errno == EINTR) {
  }
  pthread_mutex_lock(&audio->mutex);
  const uint64_t delivered = audio->stat_bytes_total - measure_b0;
  pthread_mutex_unlock(&audio->mutex);
  const int64_t elapsed_ns = uac_monotonic_ns() - measure_t0;
  const uint64_t bps = elapsed_ns > 0
      ? delivered * 1000000000ull / (uint64_t)elapsed_ns
      : 0;

  uac_audio_info_t final = *info;
  const int frame_bytes = final.channels * ((final.bits + 7) / 8);
  if (frame_bytes > 0 && bps > 0) {
    const int measured = (int)(bps / (uint64_t)frame_bytes);
    const int snapped = uac_snap_sample_rate(measured);
    if (snapped > 0) {
      final.sample_rate = snapped;
    }
  }
  audio->info = final;
  candidates[0] = final;
  UAC_STATS_LOGI(
      "capture started: alt=%d rate=%d descriptor_rate=%d ch=%d bits=%d "
      "packet=%u transfers=%d bytes/s=%llu",
      final.altsetting,
      final.sample_rate,
      descriptor_rate,
      final.channels,
      final.bits,
      final.packet_size,
      submitted,
      (unsigned long long)bps);
  return audio;
}

void uac_audio_retain(uac_audio_t *audio) {
  pthread_mutex_lock(&audio->mutex);
  audio->readers++;
  pthread_mutex_unlock(&audio->mutex);
}

void uac_audio_release(uac_audio_t *audio) {
  pthread_mutex_lock(&audio->mutex);
  audio->readers--;
  if (audio->readers == 0) {
    pthread_cond_broadcast(&audio->cond);
  }
  pthread_mutex_unlock(&audio->mutex);
}

int uac_audio_read(uac_audio_t *audio, uint8_t *dst, int max_bytes, int timeout_ms) {
  if (audio == NULL || dst == NULL || max_bytes <= 0) {
    return -1;
  }
  pthread_mutex_lock(&audio->mutex);
  if (audio->ring_count == 0 && !audio->stopping && timeout_ms > 0) {
    struct timespec deadline;
#if defined(__ANDROID__)
    clock_gettime(CLOCK_MONOTONIC, &deadline);
#else
    clock_gettime(CLOCK_REALTIME, &deadline);
#endif
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
      deadline.tv_sec += 1;
      deadline.tv_nsec -= 1000000000L;
    }
    while (audio->ring_count == 0 && !audio->stopping) {
      if (pthread_cond_timedwait(&audio->cond, &audio->mutex, &deadline) == ETIMEDOUT) {
        pthread_mutex_unlock(&audio->mutex);
        return 0;
      }
    }
  }
  if (audio->ring_count == 0) {
    /* -1 only once stopping and fully drained; a non-blocking read on an
     * empty live capture is just "no data yet". */
    const int stopped = audio->stopping;
    pthread_mutex_unlock(&audio->mutex);
    return stopped ? -1 : 0;
  }
  size_t bytes = audio->ring_count < (size_t)max_bytes
      ? audio->ring_count
      : (size_t)max_bytes;
  size_t first = UAC_RING_CAPACITY - audio->ring_head;
  if (first > bytes) {
    first = bytes;
  }
  memcpy(dst, audio->ring + audio->ring_head, first);
  memcpy(dst + first, audio->ring, bytes - first);
  audio->ring_head = (audio->ring_head + bytes) % UAC_RING_CAPACITY;
  audio->ring_count -= bytes;
  pthread_mutex_unlock(&audio->mutex);
  return (int)bytes;
}

void uac_audio_stop(uac_audio_t *audio) {
  if (audio == NULL) {
    return;
  }

  pthread_mutex_lock(&audio->mutex);
  audio->stopping = 1;
  pthread_cond_broadcast(&audio->cond);
  pthread_mutex_unlock(&audio->mutex);

  for (int i = 0; i < UAC_NUM_TRANSFERS; i++) {
    if (audio->transfers[i] != NULL) {
      libusb_cancel_transfer(audio->transfers[i]);
    }
  }

  /* Wait until every in-flight callback has run and every reader has left;
   * the libusb event thread (libuvc) services the cancellations. Callbacks
   * and readers only touch audio->mutex, never the caller's locks. */
  pthread_mutex_lock(&audio->mutex);
  while (audio->pending > 0 || audio->readers > 0) {
    pthread_cond_wait(&audio->cond, &audio->mutex);
  }
  pthread_mutex_unlock(&audio->mutex);

  if (audio->usb_devh != NULL) {
    libusb_set_interface_alt_setting(audio->usb_devh, audio->info.interface_no, 0);
    libusb_release_interface(audio->usb_devh, audio->info.interface_no);
  }
  for (int i = 0; i < UAC_NUM_TRANSFERS; i++) {
    if (audio->transfers[i] != NULL) {
      free(audio->transfers[i]->buffer);
      libusb_free_transfer(audio->transfers[i]);
    }
  }
  UAC_STATS_LOGI(
      "capture stopped: dropped=%" PRIu64 " errors=%u",
      audio->dropped_bytes,
      audio->transfer_errors);
  free(audio->ring);
  pthread_cond_destroy(&audio->cond);
  pthread_mutex_destroy(&audio->mutex);
  free(audio);
}
