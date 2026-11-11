// Records the preview as an MP4 file: NV12 frames go into the input surface of
// the system's H.264 encoder, and its output is written by the system muxer.
// With an audio track, PCM from the camera's microphone (uvc_uac.h) goes
// through the system's AAC encoder into the same file; both tracks are
// stamped with the same monotonic clock and rebased to the first video
// frame, as the Android recorder aligns them.

#ifndef FLUTTER_FFI_UVC_RECORDER_H_
#define FLUTTER_FFI_UVC_RECORDER_H_

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <string>
#include <unistd.h>
#include <vector>

#include <hilog/log.h>
#include <deque>

#include <multimedia/native_audio_channel_layout.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_audiocodec.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_videoencoder.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_avmuxer.h>
#include <native_window/external_window.h>

#include "uvc_surface.h"

namespace flutter_ffi_uvc {

inline constexpr int32_t kErrorRecorderBusy = -20;
inline constexpr int32_t kErrorRecorderEncoder = -21;
inline constexpr int32_t kErrorRecorderMuxer = -22;
inline constexpr int32_t kErrorRecorderFile = -23;
inline constexpr int32_t kErrorRecorderNoFrames = -24;
inline constexpr int32_t kErrorRecorderFormat = -25;
inline constexpr int32_t kErrorRecorderAudio = -26;
inline constexpr int32_t kErrorRecorderMicrophone = -27;

// PCM format of the audio track; 16-bit little-endian samples.
struct AudioTrackConfig {
  int32_t sample_rate = 0;
  int32_t channels = 0;
};

// Where captured PCM goes. Called from the capture thread.
class PcmSink {
 public:
  virtual ~PcmSink() = default;
  virtual void WritePcm(const uint8_t *data, size_t length, int64_t pts_us) = 0;
};

class VideoRecorder : public PcmSink {
 public:
  ~VideoRecorder() override { Stop(); }

  // Opens `path` for writing and starts a video encoder for `width` x
  // `height` frames at `fps`. Returns 0 or a negative error code.
  //
  // The output codec follows the camera stream the same way the Android
  // plugin does: with `prefer_hevc` (camera streams H.265) the recording is
  // H.265 when the device has an HEVC encoder, otherwise H.264.
  //
  // With `external_producer` the frames are not copied in through
  // WriteFrame(): another component (a video decoder rendering into
  // InputWindow()) produces the encoder's buffers and reports each one via
  // NoteProducedFrame(). The surface is then left for that producer to
  // configure.
  //
  // With `audio` the file also gets an AAC track fed through WritePcm().
  int32_t Start(const std::string &path, int32_t width, int32_t height, int32_t fps, int64_t bitrate,
                bool external_producer = false, const AudioTrackConfig *audio = nullptr,
                bool prefer_hevc = false) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (encoder_ != nullptr) {
      return kErrorRecorderBusy;
    }
    fd_ = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd_ < 0) {
      return kErrorRecorderFile;
    }
    path_ = path;
    width_ = width;
    height_ = height;
    video_mime_ = OH_AVCODEC_MIMETYPE_VIDEO_AVC;
    if (prefer_hevc) {
      encoder_ = OH_VideoEncoder_CreateByMime(OH_AVCODEC_MIMETYPE_VIDEO_HEVC);
      if (encoder_ != nullptr) {
        video_mime_ = OH_AVCODEC_MIMETYPE_VIDEO_HEVC;
      } else {
        OH_LOG_WARN(LOG_APP, "no HEVC encoder on this device; recording H.265 camera as H.264");
      }
    }
    if (encoder_ == nullptr) {
      encoder_ = OH_VideoEncoder_CreateByMime(OH_AVCODEC_MIMETYPE_VIDEO_AVC);
    }
    if (encoder_ == nullptr) {
      Release();
      return kErrorRecorderEncoder;
    }
    OH_LOG_INFO(LOG_APP, "recorder video codec %{public}s %{public}dx%{public}d @%{public}d fps %{public}lld bps",
                video_mime_, width, height, fps, static_cast<long long>(bitrate));
    OH_AVCodecCallback callback = {OnError, OnStreamChanged, OnNeedInputBuffer, OnNewOutputBuffer};
    if (OH_VideoEncoder_RegisterCallback(encoder_, callback, this) != AV_ERR_OK) {
      Release();
      return kErrorRecorderEncoder;
    }
    OH_AVFormat *format = OH_AVFormat_Create();
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, width);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, height);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
    OH_AVFormat_SetDoubleValue(format, OH_MD_KEY_FRAME_RATE, static_cast<double>(fps));
    OH_AVFormat_SetLongValue(format, OH_MD_KEY_BITRATE, bitrate);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_I_FRAME_INTERVAL, 1000);
    const bool configured = OH_VideoEncoder_Configure(encoder_, format) == AV_ERR_OK;
    OH_AVFormat_Destroy(format);
    if (!configured || OH_VideoEncoder_GetSurface(encoder_, &input_) != AV_ERR_OK || input_ == nullptr ||
        OH_VideoEncoder_Prepare(encoder_) != AV_ERR_OK) {
      Release();
      return kErrorRecorderEncoder;
    }
    external_producer_ = external_producer;
    if (!external_producer) {
      // The producer side of the encoder's surface starts without a buffer
      // configuration; requesting a buffer before setting one fails.
      OH_NativeWindow_NativeWindowHandleOpt(input_, SET_FORMAT, NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP);
      OH_NativeWindow_NativeWindowHandleOpt(
          input_, SET_USAGE,
          static_cast<uint64_t>(NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE | NATIVEBUFFER_USAGE_MEM_DMA));
      OH_NativeWindow_NativeWindowHandleOpt(input_, SET_BUFFER_GEOMETRY, width, height);
    }

    muxer_ = OH_AVMuxer_Create(fd_, AV_OUTPUT_FORMAT_MPEG_4);
    if (muxer_ == nullptr) {
      Release();
      return kErrorRecorderMuxer;
    }
    OH_AVFormat *track = OH_AVFormat_CreateVideoFormat(video_mime_, width, height);
    OH_AVFormat_SetDoubleValue(track, OH_MD_KEY_FRAME_RATE, static_cast<double>(fps));
    const bool track_added = OH_AVMuxer_AddTrack(muxer_, &track_, track) == AV_ERR_OK;
    OH_AVFormat_Destroy(track);
    if (!track_added) {
      Release();
      return kErrorRecorderMuxer;
    }
    if (audio != nullptr && audio->sample_rate > 0 && audio->channels > 0) {
      const int32_t audio_ret = StartAudioLocked(*audio);
      if (audio_ret != 0) {
        Release();
        return audio_ret;
      }
    }
    if (OH_AVMuxer_Start(muxer_) != AV_ERR_OK) {
      Release();
      return kErrorRecorderMuxer;
    }
    if (OH_VideoEncoder_Start(encoder_) != AV_ERR_OK) {
      Release();
      return kErrorRecorderEncoder;
    }
    if (audio_encoder_ != nullptr && OH_AudioCodec_Start(audio_encoder_) != AV_ERR_OK) {
      Release();
      return kErrorRecorderAudio;
    }
    started_at_us_ = NowMicros();
    frames_in_ = 0;
    frames_out_ = 0;
    first_pts_us_ = 0;
    last_pts_us_ = 0;
    rejected_ = 0;
    eos_seen_ = false;
    failed_ = false;
    error_code_ = 0;
    return 0;
  }

  bool IsRecording() {
    std::lock_guard<std::mutex> lock(mutex_);
    return encoder_ != nullptr;
  }

  // The encoder's input surface, for an external producer. Valid between
  // Start(..., true) and Stop(); nullptr otherwise.
  OHNativeWindow *InputWindow() {
    std::lock_guard<std::mutex> lock(mutex_);
    return external_producer_ ? input_ : nullptr;
  }

  // Counts a frame an external producer rendered into InputWindow().
  void NoteProducedFrame() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (encoder_ != nullptr && external_producer_) {
      frames_in_++;
    }
  }

  bool HasAudio() {
    std::lock_guard<std::mutex> lock(mutex_);
    return audio_encoder_ != nullptr;
  }

  // Queues PCM for the audio track. `pts_us` is the arrival time of the
  // first sample on the same clock the video frames carry. Called from the
  // capture thread; copies the bytes and returns.
  void WritePcm(const uint8_t *data, size_t length, int64_t pts_us) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (audio_encoder_ == nullptr || failed_ || length == 0) {
      return;
    }
    if (pcm_.empty()) {
      pcm_pts_us_ = pts_us;
    }
    pcm_.insert(pcm_.end(), data, data + length);
    pcm_in_bytes_ += length;
    // Loudest sample so far, to tell a muted microphone from a working one.
    for (size_t i = 0; i + 1 < length; i += 2) {
      const int16_t sample = static_cast<int16_t>(data[i] | (data[i + 1] << 8));
      const int32_t magnitude = sample < 0 ? -static_cast<int32_t>(sample) : sample;
      if (magnitude > pcm_peak_) {
        pcm_peak_ = magnitude;
      }
    }
    // Bound the backlog to about a second; the encoder is far faster than
    // the microphone, so growth means it stalled.
    const size_t limit = static_cast<size_t>(audio_.sample_rate) * static_cast<size_t>(audio_.channels) * 2;
    if (pcm_.size() > limit) {
      const size_t drop = pcm_.size() - limit;
      pcm_.erase(pcm_.begin(), pcm_.begin() + static_cast<std::ptrdiff_t>(drop));
      pcm_pts_us_ += SamplesToMicros(drop);
      pcm_dropped_bytes_ += drop;
    }
    FeedAudioLocked();
  }

  // Feeds one frame. Only NV12 of the configured size is accepted; anything
  // else is counted and skipped. Called from the presenter's draw thread.
  void WriteFrame(const FramePixels &pixels) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (encoder_ == nullptr || input_ == nullptr || failed_ || external_producer_) {
      return;
    }
    if (!pixels.yuv || pixels.width != width_ || pixels.height != height_) {
      if (rejected_ == 0) {
        OH_LOG_WARN(LOG_APP, "recorder skips frames: yuv=%{public}d %{public}dx%{public}d, expected %{public}dx%{public}d",
                    pixels.yuv ? 1 : 0, pixels.width, pixels.height, width_, height_);
      }
      rejected_++;
      return;
    }
    OHNativeWindow *input = input_;
    const int64_t started_at_us = started_at_us_;
    writers_++;
    // The copy takes milliseconds; the encoder's callbacks must not wait for it.
    lock.unlock();
    const bool written = CopyInto(input, pixels, started_at_us);
    lock.lock();
    writers_--;
    if (written) {
      frames_in_++;
    } else {
      rejected_++;
    }
    writers_idle_.notify_all();
  }

  // Finishes the file. Returns the number of encoded frames, or a negative
  // error code when nothing usable was written.
  int64_t Stop() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (encoder_ == nullptr) {
      return kErrorRecorderNoFrames;
    }
    writers_idle_.wait(lock, [this] { return writers_ == 0; });
    if (frames_in_ > 0 && !failed_) {
      OH_VideoEncoder_NotifyEndOfStream(encoder_);
      // Lets the encoder deliver what it still holds before the muxer closes.
      eos_.wait_for(lock, std::chrono::milliseconds(kDrainTimeoutMs), [this] { return eos_seen_ || failed_; });
    }
    if (audio_encoder_ != nullptr && !failed_) {
      // The audio encoder takes its end of stream as an empty input buffer.
      audio_eos_requested_ = true;
      FeedAudioLocked();
      eos_.wait_for(lock, std::chrono::milliseconds(kDrainTimeoutMs),
                    [this] { return audio_eos_seen_ || failed_ || !audio_eos_pending_; });
    }
    const int64_t written = frames_out_;
    const bool failed = failed_;
    OH_LOG_WARN(LOG_APP,
                "recording stopped: frames in=%{public}lld out=%{public}lld rejected=%{public}lld failed=%{public}d "
                "eos=%{public}d error=%{public}d span=%{public}lld ms (pts %{public}lld..%{public}lld us) "
                "wall=%{public}lld ms",
                static_cast<long long>(frames_in_), static_cast<long long>(frames_out_),
                static_cast<long long>(rejected_), failed_ ? 1 : 0, eos_seen_ ? 1 : 0, error_code_,
                static_cast<long long>((last_pts_us_ - first_pts_us_) / 1000), static_cast<long long>(first_pts_us_),
                static_cast<long long>(last_pts_us_), static_cast<long long>((NowMicros() - started_at_us_) / 1000));
    if (audio_encoder_ != nullptr) {
      OH_LOG_WARN(LOG_APP,
                  "audio track: pcm in=%{public}llu bytes dropped=%{public}llu samples out=%{public}lld "
                  "before video=%{public}lld eos=%{public}d peak=%{public}d/32767",
                  static_cast<unsigned long long>(pcm_in_bytes_), static_cast<unsigned long long>(pcm_dropped_bytes_),
                  static_cast<long long>(audio_out_), static_cast<long long>(audio_before_video_),
                  audio_eos_seen_ ? 1 : 0, pcm_peak_);
    }
    OH_AVCodec *encoder = encoder_;
    OH_AVCodec *audio_encoder = audio_encoder_;
    OH_AVMuxer *muxer = muxer_;
    const int fd = fd_;
    encoder_ = nullptr;
    audio_encoder_ = nullptr;
    audio_inputs_.clear();
    pcm_.clear();
    input_ = nullptr;
    muxer_ = nullptr;
    track_ = -1;
    audio_track_ = -1;
    fd_ = -1;
    external_producer_ = false;
    // Stopping the encoders waits for their callbacks, which take the mutex.
    lock.unlock();
    Teardown(encoder, audio_encoder, muxer, fd);
    if (failed || written == 0) {
      // Nothing playable was written; do not leave an empty file behind.
      unlink(path_.c_str());
    }
    if (failed) {
      return kErrorRecorderEncoder;
    }
    return written > 0 ? written : kErrorRecorderNoFrames;
  }

  int64_t frames_in() {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_in_;
  }

 private:
  static constexpr int kDrainTimeoutMs = 1500;

  // Copies one NV12 frame into a buffer of the encoder's input surface and
  // queues it, stamped with the time since the recording started.
  bool CopyInto(OHNativeWindow *input, const FramePixels &pixels, int64_t started_at_us) {
    OHNativeWindowBuffer *buffer = nullptr;
    int fence_fd = -1;
    const int32_t requested = OH_NativeWindow_NativeWindowRequestBuffer(input, &buffer, &fence_fd);
    if (requested != 0 || buffer == nullptr) {
      if (!logged_failure_) {
        logged_failure_ = true;
        OH_LOG_WARN(LOG_APP, "encoder surface gave no buffer: %{public}d", requested);
      }
      return false;
    }
    if (fence_fd >= 0) {
      struct pollfd wait = {fence_fd, POLLIN, 0};
      poll(&wait, 1, kFenceTimeoutMs);
      close(fence_fd);
    }
    BufferHandle *handle = OH_NativeWindow_GetBufferHandleFromNative(buffer);
    OH_NativeBuffer *target_buffer = nullptr;
    YuvLayout target;
    const int32_t converted = OH_NativeBuffer_FromNativeWindowBuffer(buffer, &target_buffer);
    if (handle == nullptr || handle->fd < 0 || converted != 0 || !MapYuvLayout(target_buffer, &target) ||
        target.luma_stride < static_cast<size_t>(width_) || target.chroma_stride < static_cast<size_t>(width_)) {
      if (!logged_failure_) {
        logged_failure_ = true;
        OH_LOG_WARN(LOG_APP,
                    "encoder buffer unusable: handle=%{public}d fd=%{public}d format=%{public}d %{public}dx%{public}d "
                    "stride=%{public}d converted=%{public}d strides=%{public}zu/%{public}zu",
                    handle != nullptr ? 1 : 0, handle != nullptr ? handle->fd : -1, handle != nullptr ? handle->format : -1,
                    handle != nullptr ? handle->width : -1, handle != nullptr ? handle->height : -1,
                    handle != nullptr ? handle->stride : -1, converted, target.luma_stride, target.chroma_stride);
      }
      OH_NativeWindow_NativeWindowAbortBuffer(input, buffer);
      return false;
    }
    SyncBuffer(handle->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    const size_t row_bytes = static_cast<size_t>(width_);
    for (int32_t y = 0; y < height_; y++) {
      memcpy(target.luma + static_cast<size_t>(y) * target.luma_stride,
             pixels.plane0 + static_cast<size_t>(y) * pixels.stride0, row_bytes);
    }
    for (int32_t y = 0; y < height_ / 2; y++) {
      memcpy(target.chroma + static_cast<size_t>(y) * target.chroma_stride,
             pixels.plane1 + static_cast<size_t>(y) * pixels.stride1, row_bytes);
    }
    SyncBuffer(handle->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    // The encoder takes the presentation time from the buffer. Despite the
    // option's documentation this value is read as microseconds: nanoseconds
    // produced files a thousand times too long. Absolute monotonic time, the
    // clock the audio samples carry too; the muxer stage rebases both.
    (void)started_at_us;
    const uint64_t timestamp_us = static_cast<uint64_t>(NowMicros());
    OH_NativeWindow_NativeWindowHandleOpt(input, SET_UI_TIMESTAMP, timestamp_us);
    Region region = {nullptr, 0};
    return OH_NativeWindow_NativeWindowFlushBuffer(input, buffer, -1, region) == 0;
  }

  static constexpr int32_t kAacFrameSamples = 1024;

  int64_t SamplesToMicros(size_t bytes) const {
    const int64_t frame_bytes = static_cast<int64_t>(audio_.channels) * 2;
    if (frame_bytes <= 0 || audio_.sample_rate <= 0) {
      return 0;
    }
    return static_cast<int64_t>(bytes) / frame_bytes * 1000000 / audio_.sample_rate;
  }

  // Must hold mutex_. Creates the AAC encoder and the muxer's audio track.
  int32_t StartAudioLocked(const AudioTrackConfig &audio) {
    audio_ = audio;
    audio_encoder_ = OH_AudioCodec_CreateByMime(OH_AVCODEC_MIMETYPE_AUDIO_AAC, true);
    if (audio_encoder_ == nullptr) {
      OH_LOG_WARN(LOG_APP, "no AAC encoder");
      return kErrorRecorderAudio;
    }
    OH_AVCodecCallback callback = {OnAudioError, OnAudioStreamChanged, OnAudioNeedInputBuffer, OnAudioNewOutputBuffer};
    OH_AVFormat *format = OH_AVFormat_Create();
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_SAMPLE_RATE, audio.sample_rate);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_CHANNEL_COUNT, audio.channels);
    OH_AVFormat_SetLongValue(format, OH_MD_KEY_CHANNEL_LAYOUT,
                             audio.channels == 1 ? static_cast<int64_t>(CH_LAYOUT_MONO)
                                                 : static_cast<int64_t>(CH_LAYOUT_STEREO));
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUDIO_SAMPLE_FORMAT, SAMPLE_S16LE);
    OH_AVFormat_SetLongValue(format, OH_MD_KEY_BITRATE, static_cast<int64_t>(96000) * audio.channels);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_MAX_INPUT_SIZE, kAacFrameSamples * audio.channels * 2);
    const bool ok = OH_AudioCodec_RegisterCallback(audio_encoder_, callback, this) == AV_ERR_OK &&
                    OH_AudioCodec_Configure(audio_encoder_, format) == AV_ERR_OK &&
                    OH_AudioCodec_Prepare(audio_encoder_) == AV_ERR_OK;
    OH_AVFormat_Destroy(format);
    if (!ok) {
      OH_LOG_WARN(LOG_APP, "AAC encoder setup failed for %{public}d Hz x%{public}d", audio.sample_rate, audio.channels);
      return kErrorRecorderAudio;
    }
    OH_AVFormat *track = OH_AVFormat_CreateAudioFormat(OH_AVCODEC_MIMETYPE_AUDIO_AAC, audio.sample_rate, audio.channels);
    OH_AVFormat_SetIntValue(track, OH_MD_KEY_PROFILE, AAC_PROFILE_LC);
    const bool added = OH_AVMuxer_AddTrack(muxer_, &audio_track_, track) == AV_ERR_OK;
    OH_AVFormat_Destroy(track);
    if (!added) {
      OH_LOG_WARN(LOG_APP, "muxer refused the AAC track");
      return kErrorRecorderMuxer;
    }
    pcm_.clear();
    pcm_pts_us_ = 0;
    pcm_in_bytes_ = 0;
    pcm_dropped_bytes_ = 0;
    pcm_peak_ = 0;
    audio_out_ = 0;
    audio_fed_ = 0;
    audio_emitted_ = 0;
    audio_first_pts_us_ = 0;
    audio_before_video_ = 0;
    audio_eos_requested_ = false;
    audio_eos_pending_ = false;
    audio_eos_seen_ = false;
    return 0;
  }

  // Must hold mutex_. Moves whole AAC frames of PCM into free input buffers.
  void FeedAudioLocked() {
    if (audio_encoder_ == nullptr || failed_) {
      return;
    }
    const size_t frame_bytes = static_cast<size_t>(kAacFrameSamples) * static_cast<size_t>(audio_.channels) * 2;
    while (!audio_inputs_.empty() && pcm_.size() >= frame_bytes) {
      const AudioInput input = audio_inputs_.front();
      uint8_t *destination = OH_AVBuffer_GetAddr(input.buffer);
      const int32_t capacity = OH_AVBuffer_GetCapacity(input.buffer);
      if (destination == nullptr || capacity < static_cast<int32_t>(frame_bytes)) {
        OH_LOG_WARN(LOG_APP, "AAC input buffer too small: %{public}d < %{public}zu", capacity, frame_bytes);
        failed_ = true;
        return;
      }
      audio_inputs_.pop_front();
      std::memcpy(destination, pcm_.data(), frame_bytes);
      pcm_.erase(pcm_.begin(), pcm_.begin() + static_cast<std::ptrdiff_t>(frame_bytes));
      if (audio_fed_ == 0) {
        audio_first_pts_us_ = pcm_pts_us_;
      }
      audio_fed_++;
      OH_AVCodecBufferAttr attr = {pcm_pts_us_, static_cast<int32_t>(frame_bytes), 0, 0};
      pcm_pts_us_ += SamplesToMicros(frame_bytes);
      OH_AVBuffer_SetBufferAttr(input.buffer, &attr);
      if (OH_AudioCodec_PushInputBuffer(audio_encoder_, input.index) != AV_ERR_OK) {
        failed_ = true;
        return;
      }
    }
    if (audio_eos_requested_ && !audio_eos_pending_ && !audio_inputs_.empty()) {
      const AudioInput input = audio_inputs_.front();
      audio_inputs_.pop_front();
      OH_AVCodecBufferAttr attr = {pcm_pts_us_, 0, 0, AVCODEC_BUFFER_FLAGS_EOS};
      OH_AVBuffer_SetBufferAttr(input.buffer, &attr);
      audio_eos_pending_ = OH_AudioCodec_PushInputBuffer(audio_encoder_, input.index) == AV_ERR_OK;
      if (!audio_eos_pending_) {
        eos_.notify_all();
      }
    }
  }

  static void Teardown(OH_AVCodec *encoder, OH_AVCodec *audio_encoder, OH_AVMuxer *muxer, int fd) {
    if (encoder != nullptr) {
      OH_VideoEncoder_Stop(encoder);
      OH_VideoEncoder_Destroy(encoder);
    }
    if (audio_encoder != nullptr) {
      OH_AudioCodec_Stop(audio_encoder);
      OH_AudioCodec_Destroy(audio_encoder);
    }
    if (muxer != nullptr) {
      OH_AVMuxer_Stop(muxer);
      OH_AVMuxer_Destroy(muxer);
    }
    if (fd >= 0) {
      close(fd);
    }
  }

  // Must hold mutex_. Tears down a recorder that failed while starting.
  void Release() {
    OH_AVCodec *encoder = encoder_;
    OH_AVCodec *audio_encoder = audio_encoder_;
    OH_AVMuxer *muxer = muxer_;
    const int fd = fd_;
    encoder_ = nullptr;
    audio_encoder_ = nullptr;
    audio_inputs_.clear();
    input_ = nullptr;
    muxer_ = nullptr;
    track_ = -1;
    audio_track_ = -1;
    fd_ = -1;
    external_producer_ = false;
    Teardown(encoder, audio_encoder, muxer, fd);
    if (!path_.empty()) {
      unlink(path_.c_str());
    }
  }

  static void OnError(OH_AVCodec *codec, int32_t error_code, void *user_data) {
    auto *self = static_cast<VideoRecorder *>(user_data);
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->failed_ = true;
    self->error_code_ = error_code;
    self->eos_.notify_all();
  }

  static void OnStreamChanged(OH_AVCodec *codec, OH_AVFormat *format, void *user_data) {}

  static void OnNeedInputBuffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *user_data) {
    // Surface input: never called.
  }

  static void OnNewOutputBuffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *user_data) {
    auto *self = static_cast<VideoRecorder *>(user_data);
    std::lock_guard<std::mutex> lock(self->mutex_);
    OH_AVCodecBufferAttr attr = {};
    OH_AVBuffer_GetBufferAttr(buffer, &attr);
    if (self->muxer_ != nullptr && self->track_ >= 0 && attr.size > 0) {
      const bool is_frame = (attr.flags & AVCODEC_BUFFER_FLAGS_CODEC_DATA) == 0;
      if (is_frame) {
        // The file starts at zero whatever clock the producer stamped the
        // frames with: a decoder rendering into the surface passes the
        // camera's monotonic capture times through, and the muxer would make
        // those the file's duration.
        if (self->frames_out_ == 0) {
          self->first_pts_us_ = attr.pts;
        }
        self->last_pts_us_ = attr.pts;
        OH_AVCodecBufferAttr rebased = attr;
        rebased.pts = attr.pts - self->first_pts_us_;
        OH_AVBuffer_SetBufferAttr(buffer, &rebased);
      }
      const OH_AVErrCode wrote = OH_AVMuxer_WriteSampleBuffer(self->muxer_, static_cast<uint32_t>(self->track_), buffer);
      if (wrote == AV_ERR_OK && is_frame) {
        self->frames_out_++;
      } else if (wrote != AV_ERR_OK && self->error_code_ == 0) {
        self->error_code_ = 1000 + static_cast<int32_t>(wrote);
        OH_LOG_WARN(LOG_APP, "muxer rejected a sample: %{public}d (size %{public}d flags %{public}u pts %{public}lld)",
                    static_cast<int>(wrote), attr.size, attr.flags, static_cast<long long>(attr.pts));
      }
    }
    if (self->encoder_ != nullptr) {
      OH_VideoEncoder_FreeOutputBuffer(self->encoder_, index);
    }
    if ((attr.flags & AVCODEC_BUFFER_FLAGS_EOS) != 0) {
      self->eos_seen_ = true;
      self->eos_.notify_all();
    }
  }

  static void OnAudioError(OH_AVCodec *codec, int32_t error_code, void *user_data) {
    auto *self = static_cast<VideoRecorder *>(user_data);
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->failed_ = true;
    self->error_code_ = 2000 + error_code;
    OH_LOG_WARN(LOG_APP, "AAC encoder error %{public}d", error_code);
    self->eos_.notify_all();
  }

  static void OnAudioStreamChanged(OH_AVCodec *codec, OH_AVFormat *format, void *user_data) {}

  static void OnAudioNeedInputBuffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *user_data) {
    auto *self = static_cast<VideoRecorder *>(user_data);
    std::lock_guard<std::mutex> lock(self->mutex_);
    if (self->audio_encoder_ != codec) {
      return;
    }
    self->audio_inputs_.push_back({index, buffer});
    self->FeedAudioLocked();
  }

  static void OnAudioNewOutputBuffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *user_data) {
    auto *self = static_cast<VideoRecorder *>(user_data);
    std::lock_guard<std::mutex> lock(self->mutex_);
    if (self->audio_encoder_ != codec) {
      return;
    }
    OH_AVCodecBufferAttr attr = {};
    OH_AVBuffer_GetBufferAttr(buffer, &attr);
    if (self->muxer_ != nullptr && self->audio_track_ >= 0 && attr.size > 0) {
      const bool is_frame = (attr.flags & AVCODEC_BUFFER_FLAGS_CODEC_DATA) == 0;
      bool write = true;
      if (is_frame) {
        // The encoder's own timestamps do not carry the input ones through,
        // so each frame is placed by its position in the PCM stream: the
        // arrival time of the first sample plus the samples since. Same zero
        // point as the video track; sound captured before the first video
        // frame has no picture to go with and is dropped.
        const int64_t pts = self->audio_first_pts_us_ +
                            static_cast<int64_t>(self->audio_emitted_) * kAacFrameSamples * 1000000 /
                                self->audio_.sample_rate;
        self->audio_emitted_++;
        if (self->frames_out_ == 0 || pts < self->first_pts_us_) {
          self->audio_before_video_++;
          write = false;
        } else {
          OH_AVCodecBufferAttr rebased = attr;
          rebased.pts = pts - self->first_pts_us_;
          OH_AVBuffer_SetBufferAttr(buffer, &rebased);
        }
      }
      if (write) {
        const OH_AVErrCode wrote =
            OH_AVMuxer_WriteSampleBuffer(self->muxer_, static_cast<uint32_t>(self->audio_track_), buffer);
        if (wrote == AV_ERR_OK && is_frame) {
          self->audio_out_++;
        } else if (wrote != AV_ERR_OK && self->error_code_ == 0) {
          self->error_code_ = 3000 + static_cast<int32_t>(wrote);
          OH_LOG_WARN(LOG_APP, "muxer rejected an audio sample: %{public}d (size %{public}d flags %{public}u)",
                      static_cast<int>(wrote), attr.size, attr.flags);
        }
      }
    }
    OH_AudioCodec_FreeOutputBuffer(codec, index);
    if ((attr.flags & AVCODEC_BUFFER_FLAGS_EOS) != 0) {
      self->audio_eos_seen_ = true;
      self->eos_.notify_all();
    }
  }

  struct AudioInput {
    uint32_t index;
    OH_AVBuffer *buffer;
  };

  std::mutex mutex_;
  std::condition_variable eos_;
  OH_AVCodec *encoder_ = nullptr;
  OH_AVCodec *audio_encoder_ = nullptr;
  AudioTrackConfig audio_;
  int32_t audio_track_ = -1;
  std::deque<AudioInput> audio_inputs_;
  std::vector<uint8_t> pcm_;
  int64_t pcm_pts_us_ = 0;
  uint64_t pcm_in_bytes_ = 0;
  uint64_t pcm_dropped_bytes_ = 0;
  int32_t pcm_peak_ = 0;
  int64_t audio_out_ = 0;
  int64_t audio_fed_ = 0;
  int64_t audio_emitted_ = 0;
  int64_t audio_first_pts_us_ = 0;
  int64_t audio_before_video_ = 0;
  bool audio_eos_requested_ = false;
  bool audio_eos_pending_ = false;
  bool audio_eos_seen_ = false;
  OHNativeWindow *input_ = nullptr;
  bool external_producer_ = false;
  OH_AVMuxer *muxer_ = nullptr;
  int32_t track_ = -1;
  int fd_ = -1;
  std::string path_;
  // OH_AVCODEC_MIMETYPE_VIDEO_AVC or _HEVC (static strings from the SDK).
  const char *video_mime_ = OH_AVCODEC_MIMETYPE_VIDEO_AVC;
  int32_t width_ = 0;
  int32_t height_ = 0;
  int64_t started_at_us_ = 0;
  int64_t frames_in_ = 0;
  int64_t frames_out_ = 0;
  int64_t first_pts_us_ = 0;
  int64_t last_pts_us_ = 0;
  int64_t rejected_ = 0;
  bool eos_seen_ = false;
  bool failed_ = false;
  int32_t error_code_ = 0;
  int writers_ = 0;
  std::condition_variable writers_idle_;
  bool logged_failure_ = false;
};

}  // namespace flutter_ffi_uvc

#endif  // FLUTTER_FFI_UVC_RECORDER_H_
