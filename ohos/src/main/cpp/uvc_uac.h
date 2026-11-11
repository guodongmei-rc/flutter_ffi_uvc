// USB Audio Class capture: PCM from the camera's microphone over its own
// isochronous IN endpoint, on the same usbfs descriptor as the video stream
// (so the URBs are reaped cooperatively, see UrbOwner in uvc_stream.h).
//
// Mirrors the Android side (src/uac_audio.c): the ArkTS layer has already
// claimed the AudioStreaming interface, selected the alt setting, set the
// sampling frequency and unmuted the feature units; this only moves bytes.
// Every packet's PCM is handed to the sink with its arrival time, which is
// the same clock the video frames are stamped with.

#ifndef FLUTTER_FFI_UVC_UAC_H_
#define FLUTTER_FFI_UVC_UAC_H_

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <poll.h>
#include <sys/ioctl.h>
#include <thread>
#include <vector>

#include <linux/usbdevice_fs.h>

#include "uvc_recorder.h"
#include "uvc_surface.h"
#include "uvc_urb.h"

namespace flutter_ffi_uvc {

struct UacCaptureConfig {
  int fd = -1;
  uint8_t endpoint = 0;
  uint32_t packet_size = 0;
  int32_t sample_rate = 0;
  int32_t channels = 0;
  int32_t bits = 16;
};

struct UacCaptureStats {
  bool running = false;
  int stop_errno = 0;
  uint64_t packets = 0;
  uint64_t bad_packets = 0;
  uint64_t empty_packets = 0;
  uint64_t bytes = 0;
};

class UacCapture : public UrbOwner {
 public:
  ~UacCapture() override { Stop(); }

  // Starts the transfers. Returns 0 or -errno.
  int Start(const UacCaptureConfig &config) {
    if (thread_.joinable() || config.fd < 0 || config.packet_size == 0) {
      return -EINVAL;
    }
    config_ = config;
    urbs_.clear();
    urbs_.reserve(kUrbCount);
    for (uint32_t i = 0; i < kUrbCount; i++) {
      Urb urb;
      urb.buffer.resize(static_cast<size_t>(config.packet_size) * kPacketsPerUrb);
      urb.storage.resize(sizeof(struct usbdevfs_urb) + kPacketsPerUrb * sizeof(struct usbdevfs_iso_packet_desc));
      urbs_.push_back(std::move(urb));
    }
    for (Urb &urb : urbs_) {
      urb.tag.owner = this;
      urb.tag.urb = &urb;
    }
    int first_error = 0;
    size_t submitted = 0;
    for (Urb &urb : urbs_) {
      const int ret = Submit(&urb);
      if (ret == 0) {
        submitted++;
      } else if (first_error == 0) {
        first_error = ret;
      }
    }
    if (submitted == 0) {
      urbs_.clear();
      return first_error != 0 ? first_error : -EIO;
    }
    stop_requested_ = false;
    {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      stats_ = UacCaptureStats();
      stats_.running = true;
    }
    thread_ = std::thread([this] { Loop(); });
    return 0;
  }

  void SetSink(PcmSink *sink) {
    std::lock_guard<std::mutex> lock(sink_mutex_);
    sink_ = sink;
  }

  void Stop() {
    stop_requested_ = true;
    if (thread_.joinable()) {
      thread_.join();
    }
    if (QueuedCount() > 0) {
      for (Urb &urb : urbs_) {
        urb.tag.owner = nullptr;
      }
      new std::vector<Urb>(std::move(urbs_));
    }
    urbs_.clear();
  }

  UacCaptureStats Stats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
  }

  void OnReaped(void *urb) override {
    std::lock_guard<std::mutex> lock(completed_mutex_);
    completed_.push_back(static_cast<Urb *>(urb));
  }

 private:
  struct Urb {
    std::vector<uint8_t> storage;
    std::vector<uint8_t> buffer;
    bool queued = false;
    UrbTag tag;

    struct usbdevfs_urb *get() { return reinterpret_cast<struct usbdevfs_urb *>(storage.data()); }
  };

  int Submit(Urb *urb) {
    struct usbdevfs_urb *request = urb->get();
    std::memset(urb->storage.data(), 0, urb->storage.size());
    request->type = USBDEVFS_URB_TYPE_ISO;
    request->endpoint = config_.endpoint;
    request->flags = USBDEVFS_URB_ISO_ASAP;
    request->buffer = urb->buffer.data();
    request->buffer_length = static_cast<int>(urb->buffer.size());
    request->number_of_packets = static_cast<int>(kPacketsPerUrb);
    request->usercontext = &urb->tag;
    for (uint32_t i = 0; i < kPacketsPerUrb; i++) {
      request->iso_frame_desc[i].length = config_.packet_size;
    }
    if (ioctl(config_.fd, USBDEVFS_SUBMITURB, request) < 0) {
      return -errno;
    }
    urb->queued = true;
    return 0;
  }

  void Process(Urb *urb) {
    const struct usbdevfs_urb *request = urb->get();
    const int64_t now_us = NowMicros();
    // The packets of one URB are consecutive samples: hand them on as one
    // chunk, timestamped at the URB's completion.
    pcm_.clear();
    uint64_t bad = 0;
    uint64_t empty = 0;
    for (int i = 0; i < request->number_of_packets; i++) {
      const struct usbdevfs_iso_packet_desc &packet = request->iso_frame_desc[i];
      if (packet.status != 0 || packet.actual_length > config_.packet_size) {
        bad++;
        continue;
      }
      if (packet.actual_length == 0) {
        empty++;
        continue;
      }
      const uint8_t *start = urb->buffer.data() + static_cast<size_t>(i) * config_.packet_size;
      pcm_.insert(pcm_.end(), start, start + packet.actual_length);
    }
    {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      stats_.packets += static_cast<uint64_t>(request->number_of_packets);
      stats_.bad_packets += bad;
      stats_.empty_packets += empty;
      stats_.bytes += pcm_.size();
    }
    if (!pcm_.empty()) {
      std::lock_guard<std::mutex> lock(sink_mutex_);
      if (sink_ != nullptr) {
        sink_->WritePcm(pcm_.data(), pcm_.size(), now_us);
      }
    }
  }

  bool Reap(bool resubmit) {
    int error = 0;
    if (!ReapAll(config_.fd, &error)) {
      SetStopErrno(error);
      return false;
    }
    std::vector<Urb *> done;
    {
      std::lock_guard<std::mutex> lock(completed_mutex_);
      done.swap(completed_);
    }
    for (Urb *urb : done) {
      urb->queued = false;
      if (!resubmit) {
        continue;
      }
      if (urb->get()->status == 0) {
        Process(urb);
      }
      const int ret = Submit(urb);
      if (ret == -ENODEV) {
        SetStopErrno(ENODEV);
        return false;
      }
    }
    return true;
  }

  void Loop() {
    bool device_present = true;
    while (!stop_requested_) {
      struct pollfd wait = {config_.fd, POLLOUT, 0};
      const int ready = poll(&wait, 1, kPollTimeoutMs);
      if (ready < 0 && errno != EINTR) {
        SetStopErrno(errno);
        break;
      }
      if (!Reap(true)) {
        device_present = false;
        break;
      }
      for (Urb &urb : urbs_) {
        if (!urb.queued && !stop_requested_) {
          Submit(&urb);
        }
      }
      if (QueuedCount() == 0) {
        SetStopErrno(EIO);
        break;
      }
    }
    if (device_present) {
      for (Urb &urb : urbs_) {
        if (urb.queued) {
          ioctl(config_.fd, USBDEVFS_DISCARDURB, urb.get());
        }
      }
      for (int waited_ms = 0; QueuedCount() > 0 && waited_ms < kDrainTimeoutMs; waited_ms += kDrainStepMs) {
        struct pollfd wait = {config_.fd, POLLOUT, 0};
        poll(&wait, 1, kDrainStepMs);
        if (!Reap(false)) {
          break;
        }
      }
    }
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.running = false;
  }

  void SetStopErrno(int error) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.stop_errno = error;
  }

  size_t QueuedCount() const {
    size_t count = 0;
    for (const Urb &urb : urbs_) {
      if (urb.queued) {
        count++;
      }
    }
    return count;
  }

  static constexpr uint32_t kUrbCount = 8;
  static constexpr uint32_t kPacketsPerUrb = 8;
  static constexpr int kPollTimeoutMs = 100;
  static constexpr int kDrainTimeoutMs = 500;
  static constexpr int kDrainStepMs = 10;

  UacCaptureConfig config_;
  std::vector<Urb> urbs_;
  std::vector<uint8_t> pcm_;
  std::thread thread_;
  std::atomic<bool> stop_requested_{false};
  std::mutex completed_mutex_;
  std::vector<Urb *> completed_;
  std::mutex sink_mutex_;
  PcmSink *sink_ = nullptr;
  std::mutex stats_mutex_;
  UacCaptureStats stats_;
};

}  // namespace flutter_ffi_uvc

#endif  // FLUTTER_FFI_UVC_UAC_H_
