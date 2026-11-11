// Cooperative reaping of usbfs URBs shared between the video stream and the
// microphone capture, which run on the same descriptor.

#ifndef FLUTTER_FFI_UVC_URB_H_
#define FLUTTER_FFI_UVC_URB_H_

#include <cerrno>
#include <sys/ioctl.h>

#include <linux/usbdevice_fs.h>

namespace flutter_ffi_uvc {

// URB reaping ---------------------------------------------------------------------

// usbfs hands completed URBs to whichever thread reaps the descriptor, with
// no regard for who submitted them. The video stream and the microphone
// capture share one descriptor, so every URB carries a tag naming its owner
// and ReapAll() delivers each one there; the owner processes it on its own
// thread.
class UrbOwner {
 public:
  virtual ~UrbOwner() = default;
  // Called from whichever thread reaped the URB; must only queue it.
  virtual void OnReaped(void *urb) = 0;
};

struct UrbTag {
  // Cleared when the URB is leaked past its owner's lifetime.
  UrbOwner *owner = nullptr;
  void *urb = nullptr;
};

// Reaps every completed URB on `fd` and dispatches it. Returns false when
// the ioctl fails for good (`*error` is the errno, ENODEV when unplugged).
inline bool ReapAll(int fd, int *error) {
  for (;;) {
    void *reaped = nullptr;
    if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &reaped) < 0) {
      if (errno == EAGAIN) {
        return true;
      }
      if (errno == EINTR) {
        continue;
      }
      *error = errno;
      return false;
    }
    auto *request = static_cast<struct usbdevfs_urb *>(reaped);
    auto *tag = request != nullptr ? static_cast<UrbTag *>(request->usercontext) : nullptr;
    if (tag != nullptr && tag->owner != nullptr) {
      tag->owner->OnReaped(tag->urb);
    }
  }
}

}  // namespace flutter_ffi_uvc

#endif  // FLUTTER_FFI_UVC_URB_H_
