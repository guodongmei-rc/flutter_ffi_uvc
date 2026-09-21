#include "imu_capture.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libuvc/uvc_log.h"

#if defined(__ANDROID__)
#include <android/log.h>
/* uvc_log is compiled out in this build; diagnostics here must still reach
 * logcat, so all logs go through __android_log_print directly. */
#define IMU_LOGI(...) \
  __android_log_print(ANDROID_LOG_INFO, "flutter_ffi_uvc", __VA_ARGS__)
#define IMU_LOGW(...) \
  __android_log_print(ANDROID_LOG_WARN, "flutter_ffi_uvc", __VA_ARGS__)
#else
#define IMU_LOGI(...) UVC_LOGI(IMU_LOG_TAG, __VA_ARGS__)
#define IMU_LOGW(...) UVC_LOGW(IMU_LOG_TAG, __VA_ARGS__)
#endif

#define IMU_LOG_TAG "UVC_IMU"

#define IMU_NUM_TRANSFERS 4
#define IMU_TRANSFER_BYTES 16384
#define IMU_BUFFER_CAPACITY (256 * 1024)

#define IMU_INTERFACE_CLASS_VENDOR 0xFF
#define IMU_MAX_CANDIDATES 8

/* Packet framing (little-endian):
 *   header "<IHHIIQII" (32 bytes): magic, version, type, total_size,
 *     sequence, frame_pts_us, payload_size, flags
 *   payload (<= IMU_MAX_PAYLOAD_SIZE)
 *   tail "<II" (8 bytes): crc32(header+payload), end_magic */
#define IMU_HEADER_SIZE 32
#define IMU_TAIL_SIZE 8
#define IMU_USB_MAGIC 0x46554D49u        /* "IMUF" */
#define IMU_USB_END_MAGIC 0x21444E45u    /* "END!" */
#define IMU_USB_VERSION 1
#define IMU_USB_TYPE_GYROFLOW_FRAME 1
#define IMU_MAX_PAYLOAD_SIZE 6144
#define IMU_MAX_PACKET_SIZE (IMU_HEADER_SIZE + IMU_MAX_PAYLOAD_SIZE + IMU_TAIL_SIZE)

struct imu_capture {
  libusb_device_handle *usb_devh; /* not owned */
  imu_capture_info_t info;
  struct libusb_transfer *transfers[IMU_NUM_TRANSFERS];
  /* Reassembly buffer: USB reads may split a packet anywhere or carry
   * several packets, so bytes accumulate here until a full packet framed by
   * IMUF..END! is available. */
  uint8_t *buffer;
  size_t buffer_size;
  int has_last_sequence;
  uint32_t last_sequence;
  uint64_t good_packets;
  uint64_t bad_crc_packets;
  uint64_t bad_format_packets;
  uint64_t lost_packets;
  uint32_t transfer_errors;
  int pending;   /* transfers in flight (callback guaranteed) */
  int stopping;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
};

// ---------------------------------------------------------------------------
// CRC-32 (zlib variant: poly 0xEDB88320, init/xorout 0xFFFFFFFF)
// ---------------------------------------------------------------------------

static uint32_t imu_crc32_table[256];
static int imu_crc32_table_ready = 0;

static void imu_crc32_init(void) {
  if (imu_crc32_table_ready) {
    return;
  }
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) {
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    imu_crc32_table[i] = c;
  }
  imu_crc32_table_ready = 1;
}

static uint32_t imu_crc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; i++) {
    crc = imu_crc32_table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// Little-endian readers (headers are packed; avoid alignment assumptions)
// ---------------------------------------------------------------------------

static uint16_t imu_rd_u16le(const uint8_t *p) {
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t imu_rd_u32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t imu_rd_u64le(const uint8_t *p) {
  return (uint64_t)imu_rd_u32le(p) | ((uint64_t)imu_rd_u32le(p + 4) << 32);
}

// ---------------------------------------------------------------------------
// Gyroflow protobuf walker (varint + wire types 0/1/2/5)
// ---------------------------------------------------------------------------

typedef struct {
  uint32_t field_number;
  uint32_t wire_type;
  uint64_t varint;      /* wire type 0 */
  const uint8_t *bytes; /* wire types 1/2/5 */
  size_t length;
} imu_pb_field_t;

static int imu_pb_read_varint(
    const uint8_t *data,
    size_t size,
    size_t *offset,
    uint64_t *out) {
  uint64_t value = 0;
  int shift = 0;
  while (*offset < size) {
    const uint8_t byte = data[(*offset)++];
    value |= (uint64_t)(byte & 0x7Fu) << shift;
    if ((byte & 0x80u) == 0) {
      *out = value;
      return 0;
    }
    shift += 7;
    if (shift >= 70) {
      return -1; /* varint too long */
    }
  }
  return -1; /* truncated varint */
}

/* Advances *offset past the next field. Returns 1 when a field was read,
 * 0 at end of input, -1 on a malformed field. */
static int imu_pb_next_field(
    const uint8_t *data,
    size_t size,
    size_t *offset,
    imu_pb_field_t *field) {
  if (*offset >= size) {
    return 0;
  }
  uint64_t tag;
  if (imu_pb_read_varint(data, size, offset, &tag) != 0) {
    return -1;
  }
  field->field_number = (uint32_t)(tag >> 3);
  field->wire_type = (uint32_t)(tag & 0x07u);
  if (field->field_number == 0) {
    return -1;
  }
  switch (field->wire_type) {
    case 0:
      if (imu_pb_read_varint(data, size, offset, &field->varint) != 0) {
        return -1;
      }
      field->bytes = NULL;
      field->length = 0;
      return 1;
    case 1:
      if (size - *offset < 8) {
        return -1;
      }
      field->bytes = data + *offset;
      field->length = 8;
      *offset += 8;
      return 1;
    case 2: {
      uint64_t length;
      if (imu_pb_read_varint(data, size, offset, &length) != 0 ||
          length > size - *offset) {
        return -1;
      }
      field->bytes = data + *offset;
      field->length = (size_t)length;
      *offset += (size_t)length;
      return 1;
    }
    case 5:
      if (size - *offset < 4) {
        return -1;
      }
      field->bytes = data + *offset;
      field->length = 4;
      *offset += 4;
      return 1;
    default:
      return -1; /* unsupported wire type */
  }
}

static double imu_pb_double(const uint8_t *bytes) {
  double value;
  memcpy(&value, bytes, sizeof(value));
  return value;
}

static float imu_pb_float(const uint8_t *bytes) {
  float value;
  memcpy(&value, bytes, sizeof(value));
  return value;
}

/* IMUData:
 *   sample_timestamp_us = 1 (fixed64 double)
 *   gyroscope_x/y/z     = 2/3/4 (fixed32 float)
 *   accelerometer_x/y/z = 5/6/7 (fixed32 float) */
static int imu_parse_imu_sample(
    const uint8_t *data,
    size_t size,
    double *timestamp_us,
    float *gx,
    float *gy,
    float *gz,
    float *ax,
    float *ay,
    float *az) {
  *timestamp_us = 0.0;
  *gx = *gy = *gz = *ax = *ay = *az = 0.0f;
  size_t offset = 0;
  imu_pb_field_t field;
  int rc;
  while ((rc = imu_pb_next_field(data, size, &offset, &field)) == 1) {
    if (field.field_number == 1 && field.wire_type == 1) {
      *timestamp_us = imu_pb_double(field.bytes);
    } else if (field.wire_type == 5) {
      switch (field.field_number) {
        case 2: *gx = imu_pb_float(field.bytes); break;
        case 3: *gy = imu_pb_float(field.bytes); break;
        case 4: *gz = imu_pb_float(field.bytes); break;
        case 5: *ax = imu_pb_float(field.bytes); break;
        case 6: *ay = imu_pb_float(field.bytes); break;
        case 7: *az = imu_pb_float(field.bytes); break;
        default: break;
      }
    }
  }
  return rc == 0 ? 0 : -1;
}

/* FrameMetadata:
 *   start_timestamp_us = 1 (fixed64 double)
 *   end_timestamp_us   = 2 (fixed64 double)
 *   frame_number       = 3 (varint)
 *   imu                = 17 (repeated IMUData)
 *
 * Two passes: the first validates the blob and collects the frame summary,
 * the second prints each IMU sample, so the per-frame summary line precedes
 * its samples like the reference script. */
static int imu_parse_frame_metadata(
    uint32_t sequence,
    uint64_t frame_pts_us,
    uint32_t flags,
    const uint8_t *data,
    size_t size) {
  double start_us = 0.0;
  double end_us = 0.0;
  uint64_t frame_number = 0;
  int sample_count = 0;
  size_t offset = 0;
  imu_pb_field_t field;
  int rc;
  while ((rc = imu_pb_next_field(data, size, &offset, &field)) == 1) {
    if (field.field_number == 1 && field.wire_type == 1) {
      start_us = imu_pb_double(field.bytes);
    } else if (field.field_number == 2 && field.wire_type == 1) {
      end_us = imu_pb_double(field.bytes);
    } else if (field.field_number == 3 && field.wire_type == 0) {
      frame_number = field.varint;
    } else if (field.field_number == 17 && field.wire_type == 2) {
      double ts;
      float gx, gy, gz, ax, ay, az;
      if (imu_parse_imu_sample(
              field.bytes, field.length,
              &ts, &gx, &gy, &gz, &ax, &ay, &az) != 0) {
        return -1;
      }
      sample_count++;
    }
  }
  if (rc != 0) {
    return -1;
  }

  IMU_LOGI(
      "[IMU] packet=%8u frame=%8llu outer_pts=%14llu "
      "window=[%.0f,%.0f) samples=%2d flags=0x%08x",
      sequence,
      (unsigned long long)frame_number,
      (unsigned long long)frame_pts_us,
      start_us,
      end_us,
      sample_count,
      flags);

  offset = 0;
  while ((rc = imu_pb_next_field(data, size, &offset, &field)) == 1) {
    if (field.field_number == 17 && field.wire_type == 2) {
      double ts;
      float gx, gy, gz, ax, ay, az;
      if (imu_parse_imu_sample(
              field.bytes, field.length,
              &ts, &gx, &gy, &gz, &ax, &ay, &az) != 0) {
        return -1;
      }
      IMU_LOGI(
          "[IMU] ts=%.0f us gyro=(%.3f, %.3f, %.3f) dps "
          "accel=(%.3f, %.3f, %.3f)",
          ts, gx, gy, gz, ax, ay, az);
    }
  }
  return rc == 0 ? 0 : -1;
}

/* Main message:
 *   magic_string     = 1 (string, expected "GyroflowProtobuf")
 *   protocol_version = 2 (varint)
 *   header           = 3 (message)
 *   frame            = 4 (repeated FrameMetadata)
 * Returns the number of FrameMetadata entries, or -1 on a parse error. */
static int imu_parse_gyroflow_main(
    uint32_t sequence,
    uint64_t frame_pts_us,
    uint32_t flags,
    const uint8_t *payload,
    size_t payload_size,
    int *magic_ok) {
  *magic_ok = 0;
  int frame_count = 0;
  size_t offset = 0;
  imu_pb_field_t field;
  int rc;
  while ((rc = imu_pb_next_field(payload, payload_size, &offset, &field)) == 1) {
    if (field.field_number == 1 && field.wire_type == 2) {
      static const char expected[] = "GyroflowProtobuf";
      *magic_ok =
          field.length == sizeof(expected) - 1 &&
          memcmp(field.bytes, expected, sizeof(expected) - 1) == 0;
    } else if (field.field_number == 4 && field.wire_type == 2) {
      if (imu_parse_frame_metadata(
              sequence, frame_pts_us, flags, field.bytes, field.length) != 0) {
        return -1;
      }
      frame_count++;
    }
  }
  if (rc != 0) {
    return -1;
  }
  return frame_count;
}

// ---------------------------------------------------------------------------
// Packet framing / reassembly
// ---------------------------------------------------------------------------

typedef struct {
  uint16_t version;
  uint16_t type;
  uint32_t total_size;
  uint32_t sequence;
  uint64_t frame_pts_us;
  uint32_t payload_size;
  uint32_t flags;
} imu_packet_header_t;

static void imu_process_packet(
    imu_capture_t *imu,
    const uint8_t *packet,
    const imu_packet_header_t *header) {
  const uint8_t *payload = packet + IMU_HEADER_SIZE;
  const size_t payload_end = IMU_HEADER_SIZE + header->payload_size;
  const uint32_t received_crc = imu_rd_u32le(packet + payload_end);
  const uint32_t end_magic = imu_rd_u32le(packet + payload_end + 4);

  if (end_magic != IMU_USB_END_MAGIC) {
    IMU_LOGW("[IMU] bad packet tail: 0x%08x", end_magic);
    imu->bad_format_packets++;
    return;
  }

  const uint32_t calculated_crc = imu_crc32(packet, payload_end);
  if (calculated_crc != received_crc) {
    IMU_LOGW(
        "[IMU] CRC error: seq=%u recv=0x%08x calc=0x%08x",
        header->sequence,
        received_crc,
        calculated_crc);
    imu->bad_crc_packets++;
    return;
  }

  if (header->version != IMU_USB_VERSION) {
    IMU_LOGW("[IMU] unknown protocol version: %u", header->version);
  }

  if (header->type != IMU_USB_TYPE_GYROFLOW_FRAME) {
    IMU_LOGW("[IMU] unknown packet type: %u", header->type);
    return;
  }

  const uint32_t sequence = header->sequence;
  if (imu->has_last_sequence) {
    const uint32_t expected = imu->last_sequence + 1;
    if (sequence != expected) {
      const uint32_t lost = sequence - expected;
      imu->lost_packets += lost;
      IMU_LOGW(
          "[IMU] sequence jump: expected=%u actual=%u lost=%u",
          expected,
          sequence,
          lost);
    }
  }
  imu->last_sequence = sequence;
  imu->has_last_sequence = 1;

  int magic_ok = 0;
  const int frame_count = imu_parse_gyroflow_main(
      sequence,
      header->frame_pts_us,
      header->flags,
      payload,
      header->payload_size,
      &magic_ok);
  if (frame_count < 0) {
    IMU_LOGW("[IMU] protobuf parse failed: seq=%u", sequence);
    imu->bad_format_packets++;
    return;
  }
  imu->good_packets++;
  if (!magic_ok) {
    IMU_LOGW("[IMU] protobuf magic mismatch: seq=%u", sequence);
  }
  if (frame_count == 0) {
    IMU_LOGW("[IMU] seq=%u has no FrameMetadata", sequence);
  }
}

/* Extracts and processes every complete packet in the reassembly buffer.
 * Compatible with one USB read carrying half a packet, one packet, or
 * several. Caller holds imu->mutex. */
static void imu_extract_packets(imu_capture_t *imu) {
  static const uint8_t magic_bytes[4] = {0x49, 0x4D, 0x55, 0x46}; /* "IMUF" */

  for (;;) {
    /* Find the next magic, tolerating garbage before it. */
    size_t magic_position = imu->buffer_size;
    for (size_t i = 0; i + 4 <= imu->buffer_size; i++) {
      if (memcmp(imu->buffer + i, magic_bytes, 4) == 0) {
        magic_position = i;
        break;
      }
    }
    if (magic_position == imu->buffer_size) {
      /* No magic: keep the last 3 bytes so a split "IMUF" is not lost. */
      if (imu->buffer_size > 3) {
        memmove(
            imu->buffer,
            imu->buffer + imu->buffer_size - 3,
            3);
        imu->buffer_size = 3;
      }
      return;
    }
    if (magic_position > 0) {
      IMU_LOGW(
          "[IMU] discarding %zu invalid bytes before packet header",
          magic_position);
      memmove(
          imu->buffer,
          imu->buffer + magic_position,
          imu->buffer_size - magic_position);
      imu->buffer_size -= magic_position;
    }
    if (imu->buffer_size < IMU_HEADER_SIZE) {
      return;
    }

    imu_packet_header_t header;
    header.version = imu_rd_u16le(imu->buffer + 4);
    header.type = imu_rd_u16le(imu->buffer + 6);
    header.total_size = imu_rd_u32le(imu->buffer + 8);
    header.sequence = imu_rd_u32le(imu->buffer + 12);
    header.frame_pts_us = imu_rd_u64le(imu->buffer + 16);
    header.payload_size = imu_rd_u32le(imu->buffer + 24);
    header.flags = imu_rd_u32le(imu->buffer + 28);

    const uint32_t expected_total_size =
        IMU_HEADER_SIZE + header.payload_size + IMU_TAIL_SIZE;
    if (header.payload_size > IMU_MAX_PAYLOAD_SIZE ||
        header.total_size != expected_total_size ||
        header.total_size < IMU_HEADER_SIZE + IMU_TAIL_SIZE ||
        header.total_size > IMU_MAX_PACKET_SIZE) {
      IMU_LOGW(
          "[IMU] invalid packet length: total=%u payload=%u",
          header.total_size,
          header.payload_size);
      imu->bad_format_packets++;
      /* Skip one byte and resynchronize on the next magic. */
      memmove(imu->buffer, imu->buffer + 1, imu->buffer_size - 1);
      imu->buffer_size -= 1;
      continue;
    }

    if (imu->buffer_size < header.total_size) {
      return; /* wait for the rest of the packet */
    }

    imu_process_packet(imu, imu->buffer, &header);
    memmove(
        imu->buffer,
        imu->buffer + header.total_size,
        imu->buffer_size - header.total_size);
    imu->buffer_size -= header.total_size;
  }
}

/* Caller holds imu->mutex. */
static void imu_buffer_append(imu_capture_t *imu, const uint8_t *data, size_t bytes) {
  if (imu->buffer_size + bytes > IMU_BUFFER_CAPACITY) {
    /* Drop the oldest bytes; a sequence jump will be counted on the next
     * good packet. This should never happen while parsing keeps up. */
    const size_t drop = imu->buffer_size + bytes - IMU_BUFFER_CAPACITY;
    IMU_LOGW("[IMU] reassembly buffer overflow, dropping %zu bytes", drop);
    memmove(imu->buffer, imu->buffer + drop, imu->buffer_size - drop);
    imu->buffer_size -= drop;
  }
  memcpy(imu->buffer + imu->buffer_size, data, bytes);
  imu->buffer_size += bytes;
}

// ---------------------------------------------------------------------------
// Probe
// ---------------------------------------------------------------------------

static int imu_name_contains_imu(const char *name) {
  for (const char *p = name; *p != '\0'; p++) {
    if ((p[0] == 'I' || p[0] == 'i') &&
        (p[1] == 'M' || p[1] == 'm') &&
        (p[2] == 'U' || p[2] == 'u')) {
      return 1;
    }
  }
  return 0;
}

int imu_capture_probe(libusb_device_handle *usb_devh, imu_capture_info_t *out) {
  if (usb_devh == NULL || out == NULL) {
    return -1;
  }
  libusb_device *dev = libusb_get_device(usb_devh);
  struct libusb_config_descriptor *config = NULL;
  if (libusb_get_active_config_descriptor(dev, &config) != 0 &&
      libusb_get_config_descriptor(dev, 0, &config) != 0) {
    IMU_LOGW("imu probe: cannot read config descriptor");
    return -1;
  }

  imu_capture_info_t candidates[IMU_MAX_CANDIDATES];
  int imu_named = -1;
  int count = 0;
  for (int i = 0; i < config->bNumInterfaces && count < IMU_MAX_CANDIDATES; i++) {
    const struct libusb_interface *iface = &config->interface[i];
    for (int a = 0; a < iface->num_altsetting && count < IMU_MAX_CANDIDATES; a++) {
      const struct libusb_interface_descriptor *alt = &iface->altsetting[a];
      if (alt->bInterfaceClass != IMU_INTERFACE_CLASS_VENDOR) {
        continue;
      }
      for (int e = 0; e < alt->bNumEndpoints; e++) {
        const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
        if ((ep->bmAttributes & 0x03) != 0x02) {
          continue; /* not bulk */
        }
        if ((ep->bEndpointAddress & 0x80) == 0) {
          continue; /* capture needs IN */
        }
        char name[64];
        name[0] = '\0';
        if (alt->iInterface != 0) {
          /* Best effort: a string read failure must not lose a candidate. */
          if (libusb_get_string_descriptor_ascii(
                  usb_devh,
                  alt->iInterface,
                  (unsigned char *)name,
                  (int)sizeof(name) - 1) < 0) {
            name[0] = '\0';
          }
        }
        imu_capture_info_t candidate;
        candidate.interface_number = alt->bInterfaceNumber;
        candidate.endpoint_address = ep->bEndpointAddress;
        candidate.max_packet_size = ep->wMaxPacketSize;
        IMU_LOGI(
            "imu probe candidate: interface=%d alt=%d ep=0x%02x "
            "max_packet=%u name='%s'",
            candidate.interface_number,
            alt->bAlternateSetting,
            candidate.endpoint_address,
            candidate.max_packet_size,
            name);
        candidates[count] = candidate;
        if (imu_named < 0 && imu_name_contains_imu(name)) {
          imu_named = count;
        }
        count++;
        break; /* first bulk IN endpoint per interface, like the script */
      }
    }
  }
  libusb_free_config_descriptor(config);

  if (count == 0) {
    IMU_LOGI("imu probe: no vendor-specific Bulk IN interface");
    return -1;
  }
  *out = candidates[imu_named >= 0 ? imu_named : 0];
  IMU_LOGI(
      "imu probe: selected interface=%d ep=0x%02x max_packet=%u",
      out->interface_number,
      out->endpoint_address,
      out->max_packet_size);
  return 0;
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

static void LIBUSB_CALL imu_capture_transfer_cb(struct libusb_transfer *transfer) {
  imu_capture_t *imu = transfer->user_data;
  int resubmit = 0;

  pthread_mutex_lock(&imu->mutex);
  imu->pending--;
  if (imu->stopping) {
    /* Drain: leave the transfer cancelled. */
  } else if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
    if (transfer->actual_length > 0) {
      imu_buffer_append(
          imu, transfer->buffer, (size_t)transfer->actual_length);
      imu_extract_packets(imu);
    }
    resubmit = 1;
  } else if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
    /* Device unplugged: end the capture. */
    imu->stopping = 1;
    pthread_cond_broadcast(&imu->cond);
  } else {
    imu->transfer_errors++;
    if (imu->transfer_errors == 1 || imu->transfer_errors % 256 == 0) {
      IMU_LOGW(
          "imu bulk transfer status=%d errors=%u",
          transfer->status,
          imu->transfer_errors);
    }
    resubmit = 1;
  }

  if (resubmit) {
    imu->pending++;
    pthread_mutex_unlock(&imu->mutex);
    if (libusb_submit_transfer(transfer) != 0) {
      pthread_mutex_lock(&imu->mutex);
      imu->pending--;
      imu->stopping = 1;
      pthread_cond_broadcast(&imu->cond);
      pthread_mutex_unlock(&imu->mutex);
    }
  } else {
    pthread_cond_broadcast(&imu->cond); /* imu_capture_stop may be waiting */
    pthread_mutex_unlock(&imu->mutex);
  }
}

imu_capture_t *imu_capture_start(
    libusb_device_handle *usb_devh,
    const imu_capture_info_t *info) {
  if (usb_devh == NULL || info == NULL) {
    return NULL;
  }
  imu_crc32_init();

  imu_capture_t *imu = calloc(1, sizeof(imu_capture_t));
  if (imu == NULL) {
    return NULL;
  }
  imu->usb_devh = usb_devh;
  imu->info = *info;
  pthread_mutex_init(&imu->mutex, NULL);
  pthread_condattr_t cond_attr;
  pthread_condattr_init(&cond_attr);
#if defined(__ANDROID__)
  pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
#endif
  pthread_cond_init(&imu->cond, &cond_attr);
  pthread_condattr_destroy(&cond_attr);

  imu->buffer = malloc(IMU_BUFFER_CAPACITY);
  int transfers_ready = 0;
  if (imu->buffer != NULL) {
    for (int i = 0; i < IMU_NUM_TRANSFERS; i++) {
      struct libusb_transfer *transfer = libusb_alloc_transfer(0);
      uint8_t *buffer = transfer != NULL ? malloc(IMU_TRANSFER_BYTES) : NULL;
      if (transfer == NULL || buffer == NULL) {
        if (transfer != NULL) {
          libusb_free_transfer(transfer);
        }
        free(buffer);
        break;
      }
      libusb_fill_bulk_transfer(
          transfer,
          usb_devh,
          info->endpoint_address,
          buffer,
          IMU_TRANSFER_BYTES,
          imu_capture_transfer_cb,
          imu,
          0 /* no timeout; stop() cancels explicitly */);
      imu->transfers[i] = transfer;
      transfers_ready++;
    }
  }
  if (transfers_ready == 0) {
    IMU_LOGW("imu start: transfer allocation failed");
    imu_capture_stop(imu);
    return NULL;
  }

  /* A capture stopped moments ago can leave the interface briefly
   * unclaimable; retry a few times before giving up. */
  int rc = -1;
  for (int attempt = 0; attempt < 3; attempt++) {
    rc = libusb_claim_interface(usb_devh, info->interface_number);
    if (rc == LIBUSB_ERROR_BUSY) {
      /* A kernel driver may hold the interface. */
      libusb_detach_kernel_driver(usb_devh, info->interface_number);
      rc = libusb_claim_interface(usb_devh, info->interface_number);
    }
    if (rc == 0) {
      break;
    }
    if (attempt + 1 < 3) {
      const struct timespec pause = {0, 50 * 1000 * 1000};
      nanosleep(&pause, NULL);
    }
  }
  if (rc != 0) {
    IMU_LOGW("imu start: claim interface %d failed rc=%d",
        info->interface_number,
        rc);
    imu_capture_stop(imu);
    return NULL;
  }

  int submitted = 0;
  for (int i = 0; i < transfers_ready; i++) {
    if (libusb_submit_transfer(imu->transfers[i]) == 0) {
      submitted++;
    }
  }
  if (submitted == 0) {
    IMU_LOGW("imu start: no bulk transfer could be submitted");
    imu_capture_stop(imu);
    return NULL;
  }

  pthread_mutex_lock(&imu->mutex);
  imu->pending = submitted;
  pthread_mutex_unlock(&imu->mutex);
  IMU_LOGI(
      "imu capture started: interface=%d ep=0x%02x transfers=%d",
      info->interface_number,
      info->endpoint_address,
      submitted);
  return imu;
}

void imu_capture_stop(imu_capture_t *imu) {
  if (imu == NULL) {
    return;
  }

  pthread_mutex_lock(&imu->mutex);
  imu->stopping = 1;
  pthread_cond_broadcast(&imu->cond);
  pthread_mutex_unlock(&imu->mutex);

  for (int i = 0; i < IMU_NUM_TRANSFERS; i++) {
    if (imu->transfers[i] != NULL) {
      libusb_cancel_transfer(imu->transfers[i]);
    }
  }

  /* Wait until every in-flight callback has run; the libusb event thread
   * (libuvc) services the cancellations. Callbacks only touch imu->mutex,
   * never the caller's locks. */
  pthread_mutex_lock(&imu->mutex);
  while (imu->pending > 0) {
    pthread_cond_wait(&imu->cond, &imu->mutex);
  }
  pthread_mutex_unlock(&imu->mutex);

  if (imu->usb_devh != NULL) {
    libusb_release_interface(imu->usb_devh, imu->info.interface_number);
  }
  for (int i = 0; i < IMU_NUM_TRANSFERS; i++) {
    if (imu->transfers[i] != NULL) {
      free(imu->transfers[i]->buffer);
      libusb_free_transfer(imu->transfers[i]);
    }
  }
  IMU_LOGI(
      "imu capture stopped: good=%llu bad_crc=%llu bad_format=%llu "
      "lost=%llu leftover_bytes=%zu errors=%u",
      (unsigned long long)imu->good_packets,
      (unsigned long long)imu->bad_crc_packets,
      (unsigned long long)imu->bad_format_packets,
      (unsigned long long)imu->lost_packets,
      imu->buffer_size,
      imu->transfer_errors);
  free(imu->buffer);
  pthread_cond_destroy(&imu->cond);
  pthread_mutex_destroy(&imu->mutex);
  free(imu);
}

int imu_capture_is_running(imu_capture_t *imu) {
  if (imu == NULL) {
    return 0;
  }
  pthread_mutex_lock(&imu->mutex);
  const int running = !imu->stopping;
  pthread_mutex_unlock(&imu->mutex);
  return running;
}
