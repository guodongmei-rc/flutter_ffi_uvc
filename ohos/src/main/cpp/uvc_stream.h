// Native UVC streaming for HarmonyOS: isochronous transfers issued straight to
// the kernel's usbfs on the file descriptor from usbManager.getFileDescriptor(),
// frames assembled, decoded and shown without touching the ArkTS thread.
//
// Why not usbManager.usbSubmitTransfer(): measured on a real device, it sizes
// every isochronous packet from the first alt setting (800 bytes here), which
// capped a 1080p60 MJPEG stream at about 39 fps with uneven spacing, and each
// submit is a synchronous call of about 8 ms on the calling thread. The
// descriptor is a separate open file from the system USB service's own (an
// interface claimed through the service reads as busy here), so this code
// claims the interfaces itself and nothing else reaps its transfers.
//
// The request sequence around this (PROBE/COMMIT, alt setting selection)
// follows libuvc and stays in ArkTS; only the hot path lives here.

#ifndef FLUTTER_FFI_UVC_STREAM_H_
#define FLUTTER_FFI_UVC_STREAM_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <vector>

#include <linux/usbdevice_fs.h>

#include "uvc_frame_assembler.h"
#include "uvc_surface.h"

namespace flutter_ffi_uvc {

// usbfs requests ---------------------------------------------------------------
// Each returns the ioctl's result, or -errno on failure.

inline int UsbClaimInterface(int fd, unsigned int interface_number) {
  return ioctl(fd, USBDEVFS_CLAIMINTERFACE, &interface_number) < 0 ? -errno : 0;
}

inline int UsbReleaseInterface(int fd, unsigned int interface_number) {
  return ioctl(fd, USBDEVFS_RELEASEINTERFACE, &interface_number) < 0 ? -errno : 0;
}

inline int UsbSetInterface(int fd, unsigned int interface_number, unsigned int alt_setting) {
  struct usbdevfs_setinterface request = {interface_number, alt_setting};
  return ioctl(fd, USBDEVFS_SETINTERFACE, &request) < 0 ? -errno : 0;
}

// Returns the number of bytes transferred, or -errno.
inline int UsbControlTransfer(int fd, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                              uint8_t *data, uint16_t length, unsigned int timeout_ms) {
  struct usbdevfs_ctrltransfer transfer = {request_type, request, value, index, length, timeout_ms, data};
  const int ret = ioctl(fd, USBDEVFS_CONTROL, &transfer);
  return ret < 0 ? -errno : ret;
}

// Statistics --------------------------------------------------------------------

inline constexpr size_t kGapBuckets = 5;
// Upper bounds in milliseconds of the first four buckets; the fifth is open.
inline constexpr int64_t kGapBucketLimitsMs[kGapBuckets - 1] = {22, 30, 45, 70};
inline constexpr size_t kGapSamples = 256;

struct StreamStats {
  bool running = false;
  // errno that ended the stream on its own (ENODEV when unplugged), else 0.
  int stop_errno = 0;
  uint64_t urbs = 0;
  uint64_t packets = 0;
  uint64_t bad_packets = 0;
  uint64_t empty_packets = 0;
  uint64_t submit_failures = 0;
  UvcFrameAssemblerStats assembler;
  // Assembled frames that did not start with a JPEG start-of-image marker.
  uint64_t invalid_jpeg = 0;
  // Frames decoded and, when a window is attached, shown.
  uint64_t shown = 0;
  uint64_t decode_failures = 0;
  // Frames replaced by a newer one before a decoder was free.
  uint64_t dropped = 0;
  uint64_t decode_ms_total = 0;
  uint64_t show_ms_total = 0;
  // Milliseconds from start to the first shown frame; -1 before that.
  int64_t first_frame_ms = -1;
  double gap_avg_ms = 0;
  double gap_p95_ms = 0;
  double gap_max_ms = 0;
  int64_t elapsed_ms = 0;
  // Spacing of frames arriving from the camera and reaching the screen.
  std::array<uint64_t, kGapBuckets> input_gaps = {};
  std::array<uint64_t, kGapBuckets> shown_gaps = {};
  // Whether frames are decoded to NV12 (true) or RGBA.
  bool yuv = false;
};

inline void CountGap(std::array<uint64_t, kGapBuckets> *buckets, int64_t gap_ms) {
  size_t bucket = kGapBuckets - 1;
  for (size_t i = 0; i < kGapBuckets - 1; i++) {
    if (gap_ms <= kGapBucketLimitsMs[i]) {
      bucket = i;
      break;
    }
  }
  (*buckets)[bucket]++;
}

// Frame presenter ---------------------------------------------------------------

// Decodes frames on one thread and draws them into the attached window on
// another, so a frame can be decoded while the previous one is being drawn.
// Each stage holds one frame; a frame that arrives while its stage is busy
// waits, and is replaced if a newer one arrives first, so the preview stays
// at the latest image instead of falling behind when the camera is faster
// than this pipeline.
class FramePresenter {
 public:
  ~FramePresenter() { Stop(); }

  void Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
      return;
    }
    started_ = true;
    stopping_ = false;
    started_at_us_ = NowMicros();
    decoder_ = std::thread([this] { DecodeLoop(); });
    drawer_ = std::thread([this] { DrawLoop(); });
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!started_) {
        return;
      }
      stopping_ = true;
      has_pending_ = false;
      stopped_at_us_ = NowMicros();
    }
    wake_decoder_.notify_all();
    wake_drawer_.notify_all();
    if (decoder_.joinable()) {
      decoder_.join();
    }
    if (drawer_.joinable()) {
      drawer_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (has_decoded_) {
      ReleaseFrame(&decoded_);
      has_decoded_ = false;
    }
    started_ = false;
  }

  // The window frames are drawn into; nullptr decodes without drawing, which
  // is how a mode is verified before a texture is attached.
  void SetWindow(std::shared_ptr<PreviewWindow> window) {
    std::lock_guard<std::mutex> lock(mutex_);
    window_ = std::move(window);
  }

  // Takes one assembled frame. Called from the stream thread.
  void Submit(const uint8_t *data, size_t length) {
    const int64_t now_us = NowMicros();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (last_input_us_ != 0) {
        CountGap(&input_gaps_, (now_us - last_input_us_) / 1000);
      }
      last_input_us_ = now_us;
      if (length < 4 || data[0] != 0xff || data[1] != 0xd8 || data[2] != 0xff) {
        invalid_jpeg_++;
        return;
      }
      if (has_pending_) {
        dropped_++;
      }
      pending_.assign(data, data + length);
      has_pending_ = true;
    }
    wake_decoder_.notify_one();
  }

  void FillStats(StreamStats *stats) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats->invalid_jpeg = invalid_jpeg_;
    stats->shown = shown_;
    stats->decode_failures = decode_failures_;
    stats->dropped = dropped_;
    stats->decode_ms_total = decode_ms_total_;
    stats->show_ms_total = show_ms_total_;
    stats->first_frame_ms = first_frame_ms_;
    stats->yuv = use_yuv_;
    stats->input_gaps = input_gaps_;
    stats->shown_gaps = shown_gaps_;
    stats->gap_avg_ms = gap_count_ > 0 ? static_cast<double>(gap_sum_ms_) / static_cast<double>(gap_count_) : 0;
    stats->gap_max_ms = static_cast<double>(gap_max_ms_);
    if (!recent_gaps_.empty()) {
      std::vector<int64_t> sorted = recent_gaps_;
      std::sort(sorted.begin(), sorted.end());
      stats->gap_p95_ms = static_cast<double>(sorted[std::min(sorted.size() - 1, sorted.size() * 95 / 100)]);
    }
    const int64_t end_us = stopped_at_us_ != 0 ? stopped_at_us_ : NowMicros();
    stats->elapsed_ms = started_at_us_ != 0 ? (end_us - started_at_us_) / 1000 : 0;
  }

 private:
  void DecodeLoop() {
    std::vector<uint8_t> jpeg;
    for (;;) {
      bool yuv = false;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_decoder_.wait(lock, [this] { return stopping_ || has_pending_; });
        if (stopping_) {
          return;
        }
        jpeg.swap(pending_);
        has_pending_ = false;
        yuv = use_yuv_;
      }
      const int64_t started_us = NowMicros();
      DecodedFrame frame;
      int32_t result = kResultYuvUnavailable;
      if (yuv) {
        result = DecodeFrame(jpeg.data(), jpeg.size(), true, &frame);
      }
      if (result == kResultYuvUnavailable) {
        yuv = false;
        result = DecodeFrame(jpeg.data(), jpeg.size(), false, &frame);
      }
      const uint64_t decode_ms = static_cast<uint64_t>((NowMicros() - started_us) / 1000);

      std::lock_guard<std::mutex> lock(mutex_);
      if (!yuv) {
        use_yuv_ = false;
      }
      if (result != 0) {
        decode_failures_++;
        continue;
      }
      decode_ms_total_ += decode_ms;
      if (has_decoded_) {
        ReleaseFrame(&decoded_);
        dropped_++;
      }
      decoded_ = frame;
      has_decoded_ = true;
      wake_drawer_.notify_one();
    }
  }

  void DrawLoop() {
    for (;;) {
      DecodedFrame frame;
      std::shared_ptr<PreviewWindow> window;
      uint64_t sequence = 0;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_drawer_.wait(lock, [this] { return stopping_ || has_decoded_; });
        if (stopping_) {
          return;
        }
        frame = decoded_;
        decoded_ = DecodedFrame();
        has_decoded_ = false;
        sequence = NextFrameSequence();
        window = window_;
      }
      const int64_t started_us = NowMicros();
      const int32_t result = window != nullptr ? WriteFrame(window.get(), sequence, frame.pixels) : 0;
      const bool was_yuv = frame.pixels.yuv;
      ReleaseFrame(&frame);
      const int64_t now_us = NowMicros();

      std::lock_guard<std::mutex> lock(mutex_);
      if (result == kResultYuvUnavailable) {
        // The surface's buffers do not take NV12 the way this code writes it.
        if (was_yuv) {
          use_yuv_ = false;
        }
        dropped_++;
      } else if (result == kResultSuperseded || result == kErrorWindowClosed) {
        // Skipped for a newer frame, or the texture went away mid-frame.
        dropped_++;
      } else if (result != 0) {
        decode_failures_++;
      } else {
        shown_++;
        show_ms_total_ += static_cast<uint64_t>((now_us - started_us) / 1000);
        if (first_frame_ms_ < 0) {
          first_frame_ms_ = (now_us - started_at_us_) / 1000;
        } else {
          const int64_t gap_ms = (now_us - last_shown_us_) / 1000;
          CountGap(&shown_gaps_, gap_ms);
          gap_sum_ms_ += gap_ms;
          gap_count_++;
          gap_max_ms_ = std::max(gap_max_ms_, gap_ms);
          if (recent_gaps_.size() < kGapSamples) {
            recent_gaps_.push_back(gap_ms);
          } else {
            recent_gaps_[gap_cursor_] = gap_ms;
            gap_cursor_ = (gap_cursor_ + 1) % kGapSamples;
          }
        }
        last_shown_us_ = now_us;
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable wake_decoder_;
  std::condition_variable wake_drawer_;
  std::thread decoder_;
  std::thread drawer_;
  bool started_ = false;
  bool stopping_ = false;
  std::shared_ptr<PreviewWindow> window_;
  // Compressed frame waiting for the decoder.
  std::vector<uint8_t> pending_;
  bool has_pending_ = false;
  // Decoded frame waiting for the drawer.
  DecodedFrame decoded_;
  bool has_decoded_ = false;
  // Cleared for good once NV12 output turns out not to work on this device.
  bool use_yuv_ = true;

  int64_t started_at_us_ = 0;
  int64_t stopped_at_us_ = 0;
  int64_t last_input_us_ = 0;
  int64_t last_shown_us_ = 0;
  uint64_t invalid_jpeg_ = 0;
  uint64_t shown_ = 0;
  uint64_t decode_failures_ = 0;
  uint64_t dropped_ = 0;
  uint64_t decode_ms_total_ = 0;
  uint64_t show_ms_total_ = 0;
  int64_t first_frame_ms_ = -1;
  int64_t gap_sum_ms_ = 0;
  int64_t gap_count_ = 0;
  int64_t gap_max_ms_ = 0;
  std::vector<int64_t> recent_gaps_;
  size_t gap_cursor_ = 0;
  std::array<uint64_t, kGapBuckets> input_gaps_ = {};
  std::array<uint64_t, kGapBuckets> shown_gaps_ = {};
};

// Isochronous stream -------------------------------------------------------------

struct IsoStreamConfig {
  int fd = -1;
  uint8_t endpoint = 0;
  // Bytes the selected alt setting can send per (micro)frame.
  uint32_t bytes_per_packet = 0;
  // Short transfers keep latency low: frames are handed on once per transfer.
  uint32_t packets_per_urb = 32;
  uint32_t urb_count = 16;
  size_t max_frame_bytes = 4 * 1024 * 1024;
};

// Keeps `urb_count` isochronous URBs queued on the endpoint from its own
// thread, resubmitting each one as soon as it is reaped, and feeds every
// packet to the frame assembler and completed frames to the presenter.
class IsoStream {
 public:
  explicit IsoStream(FramePresenter *presenter) : presenter_(presenter) {}
  ~IsoStream() { Stop(); }

  // Returns 0, or -errno when no transfer could be submitted.
  int Start(const IsoStreamConfig &config) {
    if (thread_.joinable() || config.fd < 0 || config.bytes_per_packet == 0 || config.packets_per_urb == 0 ||
        config.urb_count == 0) {
      return -EINVAL;
    }
    config_ = config;
    assembler_ = std::make_unique<UvcFrameAssembler>(config.max_frame_bytes);
    const size_t buffer_bytes = static_cast<size_t>(config.bytes_per_packet) * config.packets_per_urb;
    urbs_.clear();
    for (uint32_t i = 0; i < config.urb_count; i++) {
      Urb urb;
      urb.buffer.resize(buffer_bytes);
      const size_t urb_bytes =
          sizeof(struct usbdevfs_urb) + config.packets_per_urb * sizeof(struct usbdevfs_iso_packet_desc);
      urb.storage.resize(urb_bytes);
      urbs_.push_back(std::move(urb));
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
    running_ = true;
    stop_errno_ = 0;
    thread_ = std::thread([this] { Loop(); });
    return 0;
  }

  // Cancels the transfers and waits for the thread. Safe to call twice.
  void Stop() {
    stop_requested_ = true;
    if (thread_.joinable()) {
      thread_.join();
    }
    if (QueuedCount() > 0) {
      // The kernel still owns these transfers and may write to their buffers;
      // closing the descriptor ends them. Leak them rather than free them.
      new std::vector<Urb>(std::move(urbs_));
    }
    urbs_.clear();
  }

  void FillStats(StreamStats *stats) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats->running = running_;
    stats->stop_errno = stop_errno_;
    stats->urbs = urbs_done_;
    stats->packets = packets_;
    stats->bad_packets = bad_packets_;
    stats->empty_packets = empty_packets_;
    stats->submit_failures = submit_failures_;
    stats->assembler = assembler_stats_;
  }

 private:
  struct Urb {
    // Backing store for a usbdevfs_urb followed by its packet descriptors.
    std::vector<uint8_t> storage;
    std::vector<uint8_t> buffer;
    bool queued = false;

    struct usbdevfs_urb *get() { return reinterpret_cast<struct usbdevfs_urb *>(storage.data()); }
  };

  int Submit(Urb *urb) {
    struct usbdevfs_urb *request = urb->get();
    memset(urb->storage.data(), 0, urb->storage.size());
    request->type = USBDEVFS_URB_TYPE_ISO;
    request->endpoint = config_.endpoint;
    request->flags = USBDEVFS_URB_ISO_ASAP;
    request->buffer = urb->buffer.data();
    request->buffer_length = static_cast<int>(urb->buffer.size());
    request->number_of_packets = static_cast<int>(config_.packets_per_urb);
    request->usercontext = urb;
    for (uint32_t i = 0; i < config_.packets_per_urb; i++) {
      request->iso_frame_desc[i].length = config_.bytes_per_packet;
    }
    if (ioctl(config_.fd, USBDEVFS_SUBMITURB, request) < 0) {
      return -errno;
    }
    urb->queued = true;
    return 0;
  }

  void Process(Urb *urb) {
    const struct usbdevfs_urb *request = urb->get();
    uint64_t bad = 0;
    uint64_t empty = 0;
    const uint8_t *buffer = urb->buffer.data();
    const auto on_frame = [this](const uint8_t *data, size_t length) { presenter_->Submit(data, length); };
    // usbfs lays the packets out back to back at their requested lengths.
    for (int i = 0; i < request->number_of_packets; i++) {
      const struct usbdevfs_iso_packet_desc &packet = request->iso_frame_desc[i];
      if (packet.status != 0 || packet.actual_length > config_.bytes_per_packet) {
        bad++;
        assembler_->MarkPacketLost();
        continue;
      }
      if (packet.actual_length == 0) {
        empty++;
        continue;
      }
      assembler_->Push(buffer + static_cast<size_t>(i) * config_.bytes_per_packet, packet.actual_length, on_frame);
    }
    std::lock_guard<std::mutex> lock(stats_mutex_);
    urbs_done_++;
    packets_ += static_cast<uint64_t>(request->number_of_packets);
    bad_packets_ += bad;
    empty_packets_ += empty;
    assembler_stats_ = assembler_->stats();
  }

  // Reaps every completed URB; returns false once the device is gone.
  bool Reap(bool resubmit) {
    for (;;) {
      void *reaped = nullptr;
      if (ioctl(config_.fd, USBDEVFS_REAPURBNDELAY, &reaped) < 0) {
        if (errno == EAGAIN) {
          return true;
        }
        if (errno == EINTR) {
          continue;
        }
        SetStopErrno(errno);
        return false;
      }
      auto *request = static_cast<struct usbdevfs_urb *>(reaped);
      Urb *urb = request != nullptr ? static_cast<Urb *>(request->usercontext) : nullptr;
      if (urb == nullptr) {
        continue;
      }
      urb->queued = false;
      if (!resubmit) {
        continue;
      }
      // A cancelled or failed transfer carries nothing worth assembling.
      if (request->status == 0) {
        Process(urb);
      }
      const int ret = Submit(urb);
      if (ret != 0) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        submit_failures_++;
        if (ret == -ENODEV) {
          stop_errno_ = ENODEV;
          return false;
        }
      }
    }
  }

  void SetStopErrno(int error) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stop_errno_ = error;
  }

  size_t QueuedCount() const {
    size_t queued = 0;
    for (const Urb &urb : urbs_) {
      if (urb.queued) {
        queued++;
      }
    }
    return queued;
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
      // A rejected resubmit leaves its URB idle; with fewer queued the bus is
      // left unserved between transfers and frames arrive truncated.
      for (Urb &urb : urbs_) {
        if (!urb.queued && !stop_requested_) {
          const int ret = Submit(&urb);
          if (ret != 0) {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            submit_failures_++;
            break;
          }
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
      // Discarded URBs still have to be reaped before their memory is freed.
      for (int waited_ms = 0; QueuedCount() > 0 && waited_ms < kDrainTimeoutMs; waited_ms += kDrainStepMs) {
        struct pollfd wait = {config_.fd, POLLOUT, 0};
        poll(&wait, 1, kDrainStepMs);
        if (!Reap(false)) {
          break;
        }
      }
    }
    std::lock_guard<std::mutex> lock(stats_mutex_);
    running_ = false;
  }

  static constexpr int kPollTimeoutMs = 100;
  static constexpr int kDrainTimeoutMs = 500;
  static constexpr int kDrainStepMs = 10;

  FramePresenter *presenter_;
  IsoStreamConfig config_;
  std::unique_ptr<UvcFrameAssembler> assembler_;
  std::vector<Urb> urbs_;
  std::thread thread_;
  std::atomic<bool> stop_requested_{false};

  std::mutex stats_mutex_;
  bool running_ = false;
  int stop_errno_ = 0;
  uint64_t urbs_done_ = 0;
  uint64_t packets_ = 0;
  uint64_t bad_packets_ = 0;
  uint64_t empty_packets_ = 0;
  uint64_t submit_failures_ = 0;
  UvcFrameAssemblerStats assembler_stats_;
};

// One running preview: the transfers plus the frames' way to the screen.
class StreamSession {
 public:
  StreamSession() : stream_(&presenter_) {}

  int Start(const IsoStreamConfig &config, std::shared_ptr<PreviewWindow> window) {
    presenter_.SetWindow(std::move(window));
    presenter_.Start();
    const int ret = stream_.Start(config);
    if (ret != 0) {
      presenter_.Stop();
    }
    return ret;
  }

  void SetWindow(std::shared_ptr<PreviewWindow> window) { presenter_.SetWindow(std::move(window)); }

  void Stop() {
    stream_.Stop();
    presenter_.Stop();
  }

  StreamStats Stats() {
    StreamStats stats;
    stream_.FillStats(&stats);
    presenter_.FillStats(&stats);
    return stats;
  }

 private:
  FramePresenter presenter_;
  IsoStream stream_;
};

}  // namespace flutter_ffi_uvc

#endif  // FLUTTER_FFI_UVC_STREAM_H_
