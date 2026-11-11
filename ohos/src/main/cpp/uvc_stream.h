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
#include "uvc_h26x.h"
#include "uvc_raw.h"
#include "uvc_recorder.h"
#include "uvc_uac.h"
#include "uvc_urb.h"
#include "uvc_surface.h"

namespace flutter_ffi_uvc {

// usbfs requests ---------------------------------------------------------------
// Each returns the ioctl's result, or -errno on failure.

inline int UsbClaimInterface(int fd, unsigned int interface_number) {
  if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &interface_number) == 0) {
    return 0;
  }
  if (errno != EBUSY) {
    return -errno;
  }
  // A kernel driver holds the interface (the system binds snd-usb-audio to
  // a camera's microphone, for instance). Detach it and claim in one step,
  // which is what libusb's detach_kernel_driver does for the Android side.
  struct usbdevfs_disconnect_claim claim = {};
  claim.interface = interface_number;
  claim.flags = 0;
  return ioctl(fd, USBDEVFS_DISCONNECT_CLAIM, &claim) < 0 ? -errno : 0;
}

inline int UsbReleaseInterface(int fd, unsigned int interface_number) {
  return ioctl(fd, USBDEVFS_RELEASEINTERFACE, &interface_number) < 0 ? -errno : 0;
}

inline int UsbSetInterface(int fd, unsigned int interface_number, unsigned int alt_setting) {
  struct usbdevfs_setinterface request = {interface_number, alt_setting};
  return ioctl(fd, USBDEVFS_SETINTERFACE, &request) < 0 ? -errno : 0;
}

// CLEAR_FEATURE(ENDPOINT_HALT) on `endpoint`, which also resets the data
// toggle on both sides. How a bulk UVC camera is told the stream stopped:
// the spec says nothing, Windows and Linux's uvcvideo do this.
inline int UsbClearHalt(int fd, unsigned int endpoint) {
  return ioctl(fd, USBDEVFS_CLEAR_HALT, &endpoint) < 0 ? -errno : 0;
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
  // H.264/H.265 only: frames the hardware decoder could not take in time,
  // and frames skipped while waiting for a keyframe.
  uint64_t starved = 0;
  uint64_t waiting_keyframe = 0;
};

// Where the assembled camera frames go. Called from the stream thread.
class FrameSink {
 public:
  virtual ~FrameSink() = default;
  virtual void Submit(const uint8_t *data, size_t length) = 0;
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
class FramePresenter : public FrameSink {
 public:
  ~FramePresenter() override { Stop(); }

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

  // Every decoded frame is also fed to `recorder` while it is set.
  void SetRecorder(std::shared_ptr<VideoRecorder> recorder) {
    std::lock_guard<std::mutex> lock(mutex_);
    recorder_ = std::move(recorder);
  }

  // The most recent complete frame, as the camera sent it; empty before the first.
  std::vector<uint8_t> LatestJpeg() {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_jpeg_;
  }

  // Size of the decoded frames, as reported by the decoder; 0 before the first.
  void DecodedSize(int32_t *width, int32_t *height) {
    std::lock_guard<std::mutex> lock(mutex_);
    *width = decoded_width_;
    *height = decoded_height_;
  }

  // Takes one assembled frame. Called from the stream thread.
  void Submit(const uint8_t *data, size_t length) override {
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
      latest_jpeg_.assign(data, data + length);
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
      decoded_width_ = frame.pixels.width;
      decoded_height_ = frame.pixels.height;
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
      std::shared_ptr<VideoRecorder> recorder;
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
        recorder = recorder_;
      }
      const int64_t started_us = NowMicros();
      const int32_t result = window != nullptr ? WriteFrame(window.get(), sequence, frame.pixels) : 0;
      if (recorder != nullptr) {
        recorder->WriteFrame(frame.pixels);
      }
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
  std::shared_ptr<VideoRecorder> recorder_;
  // Compressed frame waiting for the decoder.
  std::vector<uint8_t> pending_;
  bool has_pending_ = false;
  std::vector<uint8_t> latest_jpeg_;
  int32_t decoded_width_ = 0;
  int32_t decoded_height_ = 0;
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

// H.264/H.265 presenter ---------------------------------------------------------

// Feeds the camera's access units to a hardware decoder that renders into the
// attached window, and to a second one rendering into the recorder's encoder
// surface while recording. Decoders exist only while they have a window and
// start at a keyframe; see uvc_h26x.h.
class H26xPresenter : public FrameSink {
 public:
  H26xPresenter(StreamCodec codec, int32_t width, int32_t height)
      : codec_(codec), width_(width), height_(height), params_(codec) {}
  ~H26xPresenter() override { Stop(); }

  void Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    started_at_us_ = NowMicros();
    stopped_at_us_ = 0;
  }

  void Stop() {
    std::unique_ptr<H26xDecoder> preview;
    std::unique_ptr<H26xDecoder> recording;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_at_us_ = NowMicros();
      preview = std::move(preview_);
      recording = std::move(recording_);
    }
    // Destroying waits for the codec callbacks, which take mutex_.
    preview.reset();
    recording.reset();
  }

  void SetWindow(std::shared_ptr<PreviewWindow> window) {
    std::unique_ptr<H26xDecoder> old;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      window_ = std::move(window);
      OHNativeWindow *native = NativeWindow(window_);
      if (native == nullptr) {
        old = std::move(preview_);
      } else if (preview_ != nullptr && !preview_->SetSurface(native)) {
        old = std::move(preview_);
      }
    }
    old.reset();
  }

  // Starts rendering into `recorder`'s input surface from the next keyframe.
  void SetRecorder(std::shared_ptr<VideoRecorder> recorder) {
    std::unique_ptr<H26xDecoder> old;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      recorder_ = std::move(recorder);
      old = std::move(recording_);
    }
    old.reset();
  }

  void Size(int32_t *width, int32_t *height) {
    *width = width_;
    *height = height_;
  }

  void Submit(const uint8_t *data, size_t length) override {
    const int64_t now_us = NowMicros();
    const std::vector<NalUnit> nals = SplitNals(data, length);
    std::unique_ptr<H26xDecoder> retired_preview;
    std::unique_ptr<H26xDecoder> retired_recording;
    std::lock_guard<std::mutex> lock(mutex_);
    if (last_input_us_ != 0) {
      CountGap(&input_gaps_, (now_us - last_input_us_) / 1000);
    }
    last_input_us_ = now_us;
    if (nals.empty()) {
      invalid_++;
      return;
    }
    bool keyframe = false;
    bool has_slice = false;
    for (const NalUnit &nal : nals) {
      if (nal.header >= length) {
        continue;
      }
      const int type = NalType(codec_, data[nal.header]);
      if (params_.Offer(type, data + nal.offset, nal.length)) {
        continue;
      }
      if (IsSliceNal(codec_, type)) {
        has_slice = true;
        if (IsKeyframeNal(codec_, type)) {
          keyframe = true;
        }
      }
    }
    if (!has_slice) {
      return;
    }
    if (!params_.Complete()) {
      waiting_keyframe_++;
      return;
    }
    // Preview decoder: rebuilt at a keyframe when the parameter sets changed.
    OHNativeWindow *native = NativeWindow(window_);
    if (preview_ != nullptr && (preview_generation_ != params_.generation() || !preview_->Running()) && keyframe) {
      retired_preview = std::move(preview_);
    }
    if (preview_ == nullptr && native != nullptr && keyframe) {
      preview_ = MakeDecoder(native, /*recording=*/false);
      preview_generation_ = params_.generation();
      preview_waiting_key_ = false;
    }
    FeedOne(preview_.get(), &preview_waiting_key_, data, length, now_us, keyframe);

    // Recording decoder: created at the first keyframe after SetRecorder().
    std::shared_ptr<VideoRecorder> recorder = recorder_;
    OHNativeWindow *encoder_input = recorder != nullptr ? recorder->InputWindow() : nullptr;
    if (recording_ != nullptr && (encoder_input == nullptr || !recording_->Running())) {
      retired_recording = std::move(recording_);
    }
    if (recording_ == nullptr && encoder_input != nullptr && keyframe) {
      recording_ = MakeDecoder(encoder_input, /*recording=*/true);
      recording_waiting_key_ = false;
    }
    FeedOne(recording_.get(), &recording_waiting_key_, data, length, now_us, keyframe);
    frames_++;
  }

  void FillStats(StreamStats *stats) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats->invalid_jpeg = invalid_;
    stats->shown = shown_;
    stats->dropped = dropped_;
    stats->first_frame_ms = first_frame_ms_;
    stats->yuv = false;
    stats->waiting_keyframe = waiting_keyframe_;
    stats->input_gaps = input_gaps_;
    stats->shown_gaps = shown_gaps_;
    if (preview_ != nullptr) {
      const H26xDecoderStats decoder = preview_->Stats();
      stats->decode_failures = decoder.errors + decode_failures_;
      stats->starved = decoder.starved + starved_;
    } else {
      stats->decode_failures = decode_failures_;
      stats->starved = starved_;
    }
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
  static OHNativeWindow *NativeWindow(const std::shared_ptr<PreviewWindow> &window) {
    if (window == nullptr) {
      return nullptr;
    }
    std::lock_guard<std::mutex> lock(window->mutex);
    return window->window;
  }

  // mutex_ held.
  std::unique_ptr<H26xDecoder> MakeDecoder(OHNativeWindow *native, bool recording) {
    auto decoder = std::make_unique<H26xDecoder>();
    const int32_t ret = decoder->Start(codec_, width_, height_, native, params_.Bytes());
    if (ret != 0) {
      decode_failures_++;
      return nullptr;
    }
    if (recording) {
      decoder->SetOnRendered([this](int64_t) {
        std::shared_ptr<VideoRecorder> recorder;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          recorder = recorder_;
        }
        if (recorder != nullptr) {
          recorder->NoteProducedFrame();
        }
      });
    } else {
      decoder->SetOnRendered([this](int64_t) { OnShown(); });
    }
    OH_LOG_INFO(LOG_APP, "%{public}s decoder started for %{public}s", recording ? "recording" : "preview",
                codec_ == StreamCodec::kH265 ? "H.265" : "H.264");
    return decoder;
  }

  // mutex_ held. A frame the decoder could not take leaves it waiting for the
  // next keyframe, so that no picture is decoded against a missing reference.
  void FeedOne(H26xDecoder *decoder, bool *waiting_key, const uint8_t *data, size_t length, int64_t pts_us,
               bool keyframe) {
    if (decoder == nullptr) {
      return;
    }
    if (*waiting_key && !keyframe) {
      waiting_keyframe_++;
      return;
    }
    if (decoder->Feed(data, length, pts_us, keyframe)) {
      *waiting_key = false;
    } else {
      *waiting_key = true;
      dropped_++;
    }
  }

  // Called from the decoder's thread for every picture it rendered.
  void OnShown() {
    const int64_t now_us = NowMicros();
    std::lock_guard<std::mutex> lock(mutex_);
    NextFrameSequence();
    shown_++;
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

  const StreamCodec codec_;
  const int32_t width_;
  const int32_t height_;
  std::mutex mutex_;
  ParameterSets params_;
  std::shared_ptr<PreviewWindow> window_;
  std::shared_ptr<VideoRecorder> recorder_;
  std::unique_ptr<H26xDecoder> preview_;
  std::unique_ptr<H26xDecoder> recording_;
  uint64_t preview_generation_ = 0;
  bool preview_waiting_key_ = false;
  bool recording_waiting_key_ = false;

  int64_t started_at_us_ = 0;
  int64_t stopped_at_us_ = 0;
  int64_t last_input_us_ = 0;
  int64_t last_shown_us_ = 0;
  uint64_t frames_ = 0;
  uint64_t invalid_ = 0;
  uint64_t shown_ = 0;
  uint64_t dropped_ = 0;
  uint64_t starved_ = 0;
  uint64_t decode_failures_ = 0;
  uint64_t waiting_keyframe_ = 0;
  int64_t first_frame_ms_ = -1;
  int64_t gap_sum_ms_ = 0;
  int64_t gap_count_ = 0;
  int64_t gap_max_ms_ = 0;
  std::vector<int64_t> recent_gaps_;
  size_t gap_cursor_ = 0;
  std::array<uint64_t, kGapBuckets> input_gaps_ = {};
  std::array<uint64_t, kGapBuckets> shown_gaps_ = {};
};

// Uncompressed presenter ----------------------------------------------------------

// Repacks each raw frame to NV12 on its own thread (latest wins, like the
// MJPEG presenter's draw stage) and writes it to the window and the recorder.
class RawPresenter : public FrameSink {
 public:
  RawPresenter(StreamCodec codec, int32_t width, int32_t height) : codec_(codec), width_(width), height_(height) {}
  ~RawPresenter() override { Stop(); }

  void Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
      return;
    }
    started_ = true;
    stopping_ = false;
    started_at_us_ = NowMicros();
    stopped_at_us_ = 0;
    worker_ = std::thread([this] { Loop(); });
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
    wake_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
  }

  void SetWindow(std::shared_ptr<PreviewWindow> window) {
    std::lock_guard<std::mutex> lock(mutex_);
    window_ = std::move(window);
  }

  void SetRecorder(std::shared_ptr<VideoRecorder> recorder) {
    std::lock_guard<std::mutex> lock(mutex_);
    recorder_ = std::move(recorder);
  }

  void Size(int32_t *width, int32_t *height) {
    *width = width_;
    *height = height_;
  }

  // The latest picture as JPEG, encoded on the caller's thread; empty before
  // the first frame or when encoding fails.
  std::vector<uint8_t> LatestJpeg() {
    Nv12Picture picture;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      picture = latest_;
    }
    return EncodeNv12ToJpeg(picture, 90);
  }

  void Submit(const uint8_t *data, size_t length) override {
    const int64_t now_us = NowMicros();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (last_input_us_ != 0) {
        CountGap(&input_gaps_, (now_us - last_input_us_) / 1000);
      }
      last_input_us_ = now_us;
      if (length != RawFrameBytes(codec_, width_, height_)) {
        invalid_++;
        return;
      }
      if (has_pending_) {
        dropped_++;
      }
      pending_.assign(data, data + length);
      has_pending_ = true;
    }
    wake_.notify_one();
  }

  void FillStats(StreamStats *stats) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats->invalid_jpeg = invalid_;
    stats->shown = shown_;
    stats->decode_failures = failures_;
    stats->dropped = dropped_;
    stats->show_ms_total = show_ms_total_;
    stats->first_frame_ms = first_frame_ms_;
    stats->yuv = true;
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
  void Loop() {
    std::vector<uint8_t> raw;
    Nv12Picture picture;
    for (;;) {
      std::shared_ptr<PreviewWindow> window;
      std::shared_ptr<VideoRecorder> recorder;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [this] { return stopping_ || has_pending_; });
        if (stopping_) {
          return;
        }
        raw.swap(pending_);
        has_pending_ = false;
        window = window_;
        recorder = recorder_;
      }
      const int64_t started_us = NowMicros();
      if (!RepackToNv12(codec_, raw.data(), raw.size(), width_, height_, &picture)) {
        std::lock_guard<std::mutex> lock(mutex_);
        failures_++;
        continue;
      }
      const FramePixels pixels = picture.Pixels();
      uint64_t sequence = 0;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        sequence = NextFrameSequence();
        latest_ = picture;
      }
      const int32_t result = window != nullptr ? WriteFrame(window.get(), sequence, pixels) : 0;
      if (recorder != nullptr) {
        recorder->WriteFrame(pixels);
      }
      const int64_t now_us = NowMicros();
      std::lock_guard<std::mutex> lock(mutex_);
      if (result == kResultSuperseded || result == kErrorWindowClosed || result == kResultYuvUnavailable) {
        dropped_++;
      } else if (result != 0) {
        failures_++;
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

  const StreamCodec codec_;
  const int32_t width_;
  const int32_t height_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::thread worker_;
  bool started_ = false;
  bool stopping_ = false;
  std::shared_ptr<PreviewWindow> window_;
  std::shared_ptr<VideoRecorder> recorder_;
  std::vector<uint8_t> pending_;
  bool has_pending_ = false;
  Nv12Picture latest_;

  int64_t started_at_us_ = 0;
  int64_t stopped_at_us_ = 0;
  int64_t last_input_us_ = 0;
  int64_t last_shown_us_ = 0;
  uint64_t invalid_ = 0;
  uint64_t shown_ = 0;
  uint64_t dropped_ = 0;
  uint64_t failures_ = 0;
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
  // Isochronous: bytes the selected alt setting can send per (micro)frame.
  // Bulk: the negotiated dwMaxPayloadTransferSize, so that every transfer
  // completes with exactly one payload (it ends with a short packet or fills
  // the buffer), as libuvc reads bulk cameras.
  uint32_t bytes_per_packet = 0;
  // Bulk endpoint instead of isochronous: one payload per transfer, no
  // packet descriptors, no alt setting.
  bool bulk = false;
  // Short transfers keep latency low: frames are handed on once per transfer.
  uint32_t packets_per_urb = 32;
  uint32_t urb_count = 16;
  size_t max_frame_bytes = 4 * 1024 * 1024;
  // What the frames are, and the picture size the mode promises (the
  // hardware video decoder is configured with it).
  StreamCodec codec = StreamCodec::kMjpeg;
  int32_t width = 0;
  int32_t height = 0;
};

// Keeps `urb_count` URBs queued on the endpoint from its own thread
// (isochronous with `packets_per_urb` packets each, or bulk with one payload
// each), resubmitting each one as soon as it is reaped, and feeds every
// packet to the frame assembler and completed frames to the presenter.
class IsoStream : public UrbOwner {
 public:
  explicit IsoStream(FrameSink *presenter) : presenter_(presenter) {}
  ~IsoStream() override { Stop(); }

  // Returns 0, or -errno when no transfer could be submitted.
  int Start(const IsoStreamConfig &config) {
    if (thread_.joinable() || config.fd < 0 || config.bytes_per_packet == 0 || config.packets_per_urb == 0 ||
        config.urb_count == 0) {
      return -EINVAL;
    }
    config_ = config;
    assembler_ = std::make_unique<UvcFrameAssembler>(config.max_frame_bytes);
    if (config.bulk) {
      config_.packets_per_urb = 1;
    }
    const size_t buffer_bytes = static_cast<size_t>(config.bytes_per_packet) * config_.packets_per_urb;
    urbs_.clear();
    urbs_.reserve(config.urb_count);
    for (uint32_t i = 0; i < config.urb_count; i++) {
      Urb urb;
      urb.buffer.resize(buffer_bytes);
      const size_t urb_bytes =
          sizeof(struct usbdevfs_urb) + config_.packets_per_urb * sizeof(struct usbdevfs_iso_packet_desc);
      urb.storage.resize(urb_bytes);
      urbs_.push_back(std::move(urb));
    }
    // The tags point at the vector's elements, which never move again.
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
      // closing the descriptor ends them. Leak them rather than free them,
      // and detach them from this object, which is about to go.
      for (Urb &urb : urbs_) {
        urb.tag.owner = nullptr;
      }
      new std::vector<Urb>(std::move(urbs_));
    }
    urbs_.clear();
  }

  void OnReaped(void *urb) override {
    std::lock_guard<std::mutex> lock(completed_mutex_);
    completed_.push_back(static_cast<Urb *>(urb));
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
    UrbTag tag;

    struct usbdevfs_urb *get() { return reinterpret_cast<struct usbdevfs_urb *>(storage.data()); }
  };

  int Submit(Urb *urb) {
    struct usbdevfs_urb *request = urb->get();
    memset(urb->storage.data(), 0, urb->storage.size());
    request->endpoint = config_.endpoint;
    request->buffer = urb->buffer.data();
    request->buffer_length = static_cast<int>(urb->buffer.size());
    request->usercontext = &urb->tag;
    if (config_.bulk) {
      request->type = USBDEVFS_URB_TYPE_BULK;
      request->number_of_packets = 0;
    } else {
      request->type = USBDEVFS_URB_TYPE_ISO;
      request->flags = USBDEVFS_URB_ISO_ASAP;
      request->number_of_packets = static_cast<int>(config_.packets_per_urb);
      for (uint32_t i = 0; i < config_.packets_per_urb; i++) {
        request->iso_frame_desc[i].length = config_.bytes_per_packet;
      }
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
    if (config_.bulk) {
      // The whole transfer is one payload.
      if (request->actual_length == 0) {
        empty++;
      } else if (static_cast<uint32_t>(request->actual_length) > config_.bytes_per_packet) {
        bad++;
        assembler_->MarkPacketLost();
      } else {
        assembler_->Push(buffer, static_cast<size_t>(request->actual_length), on_frame);
      }
      std::lock_guard<std::mutex> lock(stats_mutex_);
      urbs_done_++;
      packets_++;
      bad_packets_ += bad;
      empty_packets_ += empty;
      assembler_stats_ = assembler_->stats();
      return;
    }
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

  // Reaps what has completed on the descriptor (ours and the microphone's)
  // and handles the URBs that came back to us; returns false once the device
  // is gone.
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
      const struct usbdevfs_urb *request = urb->get();
      // A cancelled or failed transfer carries nothing worth assembling.
      if (request->status == 0) {
        Process(urb);
      } else if (config_.bulk && request->status != -ENOENT && request->status != -ECONNRESET) {
        // A failed bulk transfer lost part of a frame; -EPIPE (stall) also
        // ends the stream below when resubmission fails.
        assembler_->MarkPacketLost();
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
    return true;
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

  FrameSink *presenter_;
  IsoStreamConfig config_;
  std::mutex completed_mutex_;
  std::vector<Urb *> completed_;
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
// MJPEG frames go through FramePresenter (decode + draw threads); H.264 and
// H.265 through H26xPresenter (hardware decoder rendering into the window);
// uncompressed pictures through RawPresenter (repacked to NV12).
class StreamSession {
 public:
  int Start(const IsoStreamConfig &config, std::shared_ptr<PreviewWindow> window) {
    if (stream_ != nullptr) {
      return -EBUSY;
    }
    codec_ = config.codec;
    if (codec_ == StreamCodec::kMjpeg) {
      jpeg_ = std::make_unique<FramePresenter>();
      jpeg_->SetWindow(std::move(window));
      jpeg_->Start();
      stream_ = std::make_unique<IsoStream>(jpeg_.get());
    } else if (IsRawVideo(codec_)) {
      raw_ = std::make_unique<RawPresenter>(codec_, config.width, config.height);
      raw_->SetWindow(std::move(window));
      raw_->Start();
      stream_ = std::make_unique<IsoStream>(raw_.get());
    } else {
      h26x_ = std::make_unique<H26xPresenter>(codec_, config.width, config.height);
      h26x_->SetWindow(std::move(window));
      h26x_->Start();
      stream_ = std::make_unique<IsoStream>(h26x_.get());
    }
    const int ret = stream_->Start(config);
    if (ret != 0) {
      stream_.reset();
      StopPresenter();
    }
    return ret;
  }

  void SetWindow(std::shared_ptr<PreviewWindow> window) {
    if (jpeg_ != nullptr) {
      jpeg_->SetWindow(std::move(window));
    } else if (raw_ != nullptr) {
      raw_->SetWindow(std::move(window));
    } else if (h26x_ != nullptr) {
      h26x_->SetWindow(std::move(window));
    }
  }

  void Stop() {
    StopRecording();
    if (stream_ != nullptr) {
      stream_->Stop();
    }
    StopPresenter();
  }

  // The latest camera frame as JPEG (as sent for MJPEG, encoded here for
  // uncompressed streams); empty for H.264/H.265 streams, whose pictures only
  // exist inside the decoder.
  std::vector<uint8_t> LatestJpeg() {
    if (jpeg_ != nullptr) {
      return jpeg_->LatestJpeg();
    }
    if (raw_ != nullptr) {
      return raw_->LatestJpeg();
    }
    return std::vector<uint8_t>();
  }

  // Starts recording to `path`. MJPEG streams feed the encoder with decoded
  // NV12 frames; H.264/H.265 streams decode into the encoder's surface with a
  // second decoder instance (no passthrough, same as Android). With `audio`
  // the camera's microphone is captured on its (already configured) endpoint
  // into an AAC track; like Android, the real sample rate is measured over a
  // short window first, descriptor rates being estimates. Returns 0 or a
  // negative error code; a microphone that fails to start fails the call
  // (the caller may retry without audio).
  int32_t StartRecording(const std::string &path, int32_t fps, int64_t bitrate, const UacCaptureConfig *audio) {
    if (recorder_ != nullptr) {
      return kErrorRecorderBusy;
    }
    std::unique_ptr<UacCapture> capture;
    AudioTrackConfig track;
    if (audio != nullptr) {
      capture = std::make_unique<UacCapture>();
      const int ret = capture->Start(*audio);
      if (ret != 0) {
        OH_LOG_WARN(LOG_APP, "microphone capture failed to start: %{public}d", ret);
        return kErrorRecorderMicrophone;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kAudioMeasureMs));
      const UacCaptureStats stats = capture->Stats();
      if (!stats.running || stats.bytes == 0) {
        OH_LOG_WARN(LOG_APP, "microphone delivered nothing in %{public}d ms (errno %{public}d)", kAudioMeasureMs,
                    stats.stop_errno);
        return kErrorRecorderMicrophone;
      }
      track.channels = audio->channels;
      track.sample_rate = audio->sample_rate;
      const int64_t frame_bytes = static_cast<int64_t>(audio->channels) * ((audio->bits + 7) / 8);
      const int32_t measured = frame_bytes > 0 ? static_cast<int32_t>(static_cast<int64_t>(stats.bytes) * 1000 /
                                                                      kAudioMeasureMs / frame_bytes)
                                               : 0;
      const int32_t snapped = SnapSampleRate(measured);
      if (snapped > 0) {
        track.sample_rate = snapped;
      }
      OH_LOG_INFO(LOG_APP,
                  "microphone: %{public}llu bytes in %{public}d ms -> %{public}d Hz measured, %{public}d Hz used "
                  "(descriptor %{public}d), %{public}d ch, bad packets %{public}llu",
                  static_cast<unsigned long long>(stats.bytes), kAudioMeasureMs, measured, track.sample_rate,
                  audio->sample_rate, audio->channels, static_cast<unsigned long long>(stats.bad_packets));
    }
    int32_t width = 0;
    int32_t height = 0;
    if (jpeg_ != nullptr) {
      jpeg_->DecodedSize(&width, &height);
      if (width <= 0 || height <= 0) {
        return kErrorRecorderNoFrames;
      }
      StreamStats stats = Stats();
      if (!stats.yuv) {
        // The encoder takes NV12; the RGBA fallback has nothing to feed it.
        return kErrorRecorderFormat;
      }
    } else if (raw_ != nullptr) {
      raw_->Size(&width, &height);
    } else if (h26x_ != nullptr) {
      h26x_->Size(&width, &height);
    } else {
      return kErrorRecorderNoFrames;
    }
    auto recorder = std::make_shared<VideoRecorder>();
    const int32_t ret = recorder->Start(path, width, height, fps, bitrate, /*external_producer=*/h26x_ != nullptr,
                                        capture != nullptr ? &track : nullptr);
    if (ret != 0) {
      return ret;
    }
    recorder_ = recorder;
    if (capture != nullptr) {
      capture->SetSink(recorder.get());
      capture_ = std::move(capture);
    }
    if (jpeg_ != nullptr) {
      jpeg_->SetRecorder(recorder);
    } else if (raw_ != nullptr) {
      raw_->SetRecorder(recorder);
    } else {
      h26x_->SetRecorder(recorder);
    }
    return 0;
  }

  // Returns the number of frames in the file, or a negative error code.
  int64_t StopRecording() {
    std::shared_ptr<VideoRecorder> recorder = recorder_;
    recorder_ = nullptr;
    if (recorder == nullptr) {
      return kErrorRecorderNoFrames;
    }
    // The microphone first: its thread writes into the recorder.
    if (capture_ != nullptr) {
      capture_->SetSink(nullptr);
      capture_->Stop();
      capture_.reset();
    }
    // Detaching first stops the producer before the encoder's surface goes.
    if (jpeg_ != nullptr) {
      jpeg_->SetRecorder(nullptr);
    } else if (raw_ != nullptr) {
      raw_->SetRecorder(nullptr);
    } else if (h26x_ != nullptr) {
      h26x_->SetRecorder(nullptr);
    }
    return recorder->Stop();
  }

  bool IsRecording() const { return recorder_ != nullptr; }

  // Counters of the microphone capture while recording with audio.
  UacCaptureStats AudioStats() { return capture_ != nullptr ? capture_->Stats() : UacCaptureStats(); }

  StreamStats Stats() {
    StreamStats stats;
    if (stream_ != nullptr) {
      stream_->FillStats(&stats);
    }
    if (jpeg_ != nullptr) {
      jpeg_->FillStats(&stats);
    } else if (raw_ != nullptr) {
      raw_->FillStats(&stats);
    } else if (h26x_ != nullptr) {
      h26x_->FillStats(&stats);
    }
    return stats;
  }

 private:
  void StopPresenter() {
    if (jpeg_ != nullptr) {
      jpeg_->Stop();
    }
    if (raw_ != nullptr) {
      raw_->Stop();
    }
    if (h26x_ != nullptr) {
      h26x_->Stop();
    }
  }

  static constexpr int kAudioMeasureMs = 300;

  // The nearest standard rate within ~3 %, or 0.
  static int32_t SnapSampleRate(int32_t estimate) {
    static constexpr int32_t kRates[] = {8000,  11025, 12000, 16000, 22050, 24000,  32000,
                                         44100, 48000, 64000, 88200, 96000, 176400, 192000};
    for (const int32_t rate : kRates) {
      if (std::abs(estimate - rate) <= rate / 32) {
        return rate;
      }
    }
    return 0;
  }

  StreamCodec codec_ = StreamCodec::kMjpeg;
  std::unique_ptr<FramePresenter> jpeg_;
  std::unique_ptr<RawPresenter> raw_;
  std::unique_ptr<UacCapture> capture_;
  std::unique_ptr<H26xPresenter> h26x_;
  std::unique_ptr<IsoStream> stream_;
  std::shared_ptr<VideoRecorder> recorder_;
};

}  // namespace flutter_ffi_uvc

#endif  // FLUTTER_FFI_UVC_STREAM_H_
