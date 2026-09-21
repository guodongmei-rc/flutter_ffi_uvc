#ifndef IMU_CAPTURE_H
#define IMU_CAPTURE_H

#include <stdint.h>

#include <libusb.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct imu_capture imu_capture_t;

/* Vendor-specific (class 0xFF) Bulk IN IMU stream: some UVC cameras expose
 * gyroscope/accelerometer data as a separate vendor-specific interface next
 * to the video interfaces. libuvc only claims the video interfaces, so the
 * IMU interface can be claimed and read with bulk IN transfers on the same
 * libusb context (the libuvc event thread services them).
 *
 * The byte stream carries length-prefixed packets (32-byte header + payload
 * + 8-byte CRC32/tail); the payload is a Gyroflow protobuf blob holding the
 * per-frame IMU samples. Samples are currently only parsed and printed to
 * logcat; no data leaves the native layer. */

typedef struct {
  int interface_number;      /* bInterfaceNumber to claim */
  uint8_t endpoint_address;  /* bulk IN endpoint */
  uint16_t max_packet_size;  /* endpoint wMaxPacketSize */
} imu_capture_info_t;

/* Scans the active configuration for a class-0xFF interface with a bulk IN
 * endpoint. When several candidates exist, one whose interface string
 * contains "IMU" is preferred. Returns 0 and fills *out on success, -1 when
 * the device has no usable IMU interface. */
int imu_capture_probe(libusb_device_handle *usb_devh, imu_capture_info_t *out);

/* Claims the probed interface and starts bulk IN transfers, parsing and
 * logging packets from the transfer callbacks. Returns NULL on failure
 * (fully cleaned up). */
imu_capture_t *imu_capture_start(
    libusb_device_handle *usb_devh,
    const imu_capture_info_t *info);

/* Stops transfers, releases the interface and frees the session. Blocks
 * until in-flight callbacks have left the session, so the caller must not
 * hold a lock those threads need. Safe on NULL. */
void imu_capture_stop(imu_capture_t *imu);

/* Nonzero while the session is capturing (not stopping/stopped). */
int imu_capture_is_running(imu_capture_t *imu);

#ifdef __cplusplus
}
#endif

#endif  // IMU_CAPTURE_H
