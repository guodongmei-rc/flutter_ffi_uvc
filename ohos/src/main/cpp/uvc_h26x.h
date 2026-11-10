// H.264 / H.265 camera streams: the camera's frames are Annex B access units,
// handed to the system's hardware video decoder, which renders them straight
// into the preview texture's surface. Nothing is decoded on the CPU.
//
// Mirrors the Android side (src/h26x_decoder.c): the decoder is created once
// the parameter sets (SPS/PPS, plus VPS for H.265) have been seen and a window
// is attached, and feeding starts at a keyframe. The window may come and go;
// the decoder is torn down without one and rebuilt at the next keyframe.
//
// Recording works the way Android does it too: a second decoder instance
// renders into the encoder's input surface, so the file is re-encoded rather
// than a copy of the camera's stream (uvc_stream.h wires that up).

#ifndef FLUTTER_FFI_UVC_H26X_H_
#define FLUTTER_FFI_UVC_H26X_H_

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include <hilog/log.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avformat.h>
#include <native_window/external_window.h>

#include "uvc_nal.h"
#include "uvc_surface.h"

namespace flutter_ffi_uvc {

// Decoder -----------------------------------------------------------------------

struct H26xDecoderStats {
  uint64_t fed = 0;
  // Access units skipped because no input buffer was free in time.
  uint64_t starved = 0;
  uint64_t rendered = 0;
  uint64_t errors = 0;
  int32_t last_error = 0;
};

// One hardware decoder instance in surface mode: access units in, pictures
// rendered to the surface it was created with. Everything is asynchronous;
// the callbacks only move buffer indices around.
class H26xDecoder {
 public:
  ~H26xDecoder() { Destroy(); }

  // Creates and starts the decoder for `codec` rendering into `window`.
  // `csd` is the parameter sets, pushed first as codec data. Returns 0 or a
  // negative error code.
  int32_t Start(StreamCodec codec, int32_t width, int32_t height, OHNativeWindow *window,
                const std::vector<uint8_t> &csd) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (decoder_ != nullptr || window == nullptr) {
      return kErrorBadArguments;
    }
    const char *mime = codec == StreamCodec::kH265 ? OH_AVCODEC_MIMETYPE_VIDEO_HEVC : OH_AVCODEC_MIMETYPE_VIDEO_AVC;
    decoder_ = OH_VideoDecoder_CreateByMime(mime);
    if (decoder_ == nullptr) {
      OH_LOG_WARN(LOG_APP, "no hardware decoder for %{public}s", mime);
      return kErrorDecode;
    }
    OH_AVCodecCallback callback = {OnError, OnStreamChanged, OnNeedInputBuffer, OnNewOutputBuffer};
    OH_AVFormat *format = OH_AVFormat_Create();
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, width);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, height);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
    const bool ok = OH_VideoDecoder_RegisterCallback(decoder_, callback, this) == AV_ERR_OK &&
                    OH_VideoDecoder_Configure(decoder_, format) == AV_ERR_OK &&
                    OH_VideoDecoder_SetSurface(decoder_, window) == AV_ERR_OK &&
                    OH_VideoDecoder_Prepare(decoder_) == AV_ERR_OK && OH_VideoDecoder_Start(decoder_) == AV_ERR_OK;
    OH_AVFormat_Destroy(format);
    if (!ok) {
      OH_LOG_WARN(LOG_APP, "decoder setup failed for %{public}s %{public}dx%{public}d", mime, width, height);
      OH_VideoDecoder_Destroy(decoder_);
      decoder_ = nullptr;
      return kErrorDecode;
    }
    csd_ = csd;
    csd_pending_ = !csd.empty();
    return 0;
  }

  bool Running() {
    std::lock_guard<std::mutex> lock(mutex_);
    return decoder_ != nullptr && !failed_;
  }

  // Redirects the pictures to another surface. Returns false if the decoder
  // refused, in which case the caller rebuilds it.
  bool SetSurface(OHNativeWindow *window) {
    std::lock_guard<std::mutex> lock(mutex_);
    return decoder_ != nullptr && window != nullptr && OH_VideoDecoder_SetSurface(decoder_, window) == AV_ERR_OK;
  }

  // Queues one access unit. Waits briefly for a free input buffer; returns
  // false (and counts a starved frame) when none turns up.
  bool Feed(const uint8_t *data, size_t length, int64_t pts_us, bool keyframe) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (decoder_ == nullptr || failed_) {
      return false;
    }
    if (csd_pending_) {
      // The parameter sets go in on their own, flagged as codec data.
      if (!PushLocked(lock, csd_.data(), csd_.size(), pts_us, AVCODEC_BUFFER_FLAGS_CODEC_DATA)) {
        starved_++;
        return false;
      }
      csd_pending_ = false;
    }
    if (!PushLocked(lock, data, length, pts_us, keyframe ? AVCODEC_BUFFER_FLAGS_SYNC_FRAME : 0)) {
      starved_++;
      return false;
    }
    fed_++;
    return true;
  }

  // Stops and frees the decoder. Pictures still inside are discarded.
  void Destroy() {
    OH_AVCodec *decoder = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      decoder = decoder_;
      decoder_ = nullptr;
      inputs_.clear();
    }
    if (decoder != nullptr) {
      // Stop/Destroy wait for the callbacks, which take the mutex: no lock here.
      OH_VideoDecoder_Stop(decoder);
      OH_VideoDecoder_Destroy(decoder);
    }
  }

  H26xDecoderStats Stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    H26xDecoderStats stats;
    stats.fed = fed_;
    stats.starved = starved_;
    stats.rendered = rendered_;
    stats.errors = errors_;
    stats.last_error = last_error_;
    return stats;
  }

  // Called (from the codec's thread) for every picture rendered.
  void SetOnRendered(std::function<void(int64_t pts_us)> on_rendered) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_rendered_ = std::move(on_rendered);
  }

 private:
  struct Input {
    uint32_t index;
    OH_AVBuffer *buffer;
  };

  bool PushLocked(std::unique_lock<std::mutex> &lock, const uint8_t *data, size_t length, int64_t pts_us,
                  uint32_t flags) {
    if (!input_ready_.wait_for(lock, std::chrono::milliseconds(kInputWaitMs),
                               [this] { return decoder_ == nullptr || failed_ || !inputs_.empty(); })) {
      return false;
    }
    if (decoder_ == nullptr || failed_) {
      return false;
    }
    const Input input = inputs_.front();
    inputs_.pop_front();
    uint8_t *destination = OH_AVBuffer_GetAddr(input.buffer);
    const int32_t capacity = OH_AVBuffer_GetCapacity(input.buffer);
    if (destination == nullptr || capacity < static_cast<int32_t>(length)) {
      OH_LOG_WARN(LOG_APP, "decoder input buffer too small: %{public}d < %{public}zu", capacity, length);
      // Hand the buffer back empty so the decoder does not lose it.
      OH_AVCodecBufferAttr attr = {pts_us, 0, 0, 0};
      OH_AVBuffer_SetBufferAttr(input.buffer, &attr);
      OH_VideoDecoder_PushInputBuffer(decoder_, input.index);
      return false;
    }
    std::memcpy(destination, data, length);
    OH_AVCodecBufferAttr attr = {pts_us, static_cast<int32_t>(length), 0, flags};
    OH_AVBuffer_SetBufferAttr(input.buffer, &attr);
    if (OH_VideoDecoder_PushInputBuffer(decoder_, input.index) != AV_ERR_OK) {
      errors_++;
      return false;
    }
    return true;
  }

  static void OnError(OH_AVCodec *codec, int32_t error_code, void *user_data) {
    auto *self = static_cast<H26xDecoder *>(user_data);
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->errors_++;
    self->last_error_ = error_code;
    self->failed_ = true;
    OH_LOG_WARN(LOG_APP, "video decoder error %{public}d", error_code);
    self->input_ready_.notify_all();
  }

  static void OnStreamChanged(OH_AVCodec *codec, OH_AVFormat *format, void *user_data) {}

  static void OnNeedInputBuffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *user_data) {
    auto *self = static_cast<H26xDecoder *>(user_data);
    std::lock_guard<std::mutex> lock(self->mutex_);
    if (self->decoder_ != codec) {
      return;
    }
    self->inputs_.push_back({index, buffer});
    self->input_ready_.notify_one();
  }

  static void OnNewOutputBuffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *user_data) {
    auto *self = static_cast<H26xDecoder *>(user_data);
    std::function<void(int64_t)> on_rendered;
    int64_t pts_us = 0;
    {
      std::lock_guard<std::mutex> lock(self->mutex_);
      if (self->decoder_ != codec) {
        return;
      }
      OH_AVCodecBufferAttr attr = {};
      if (buffer != nullptr) {
        OH_AVBuffer_GetBufferAttr(buffer, &attr);
      }
      pts_us = attr.pts;
      if ((attr.flags & AVCODEC_BUFFER_FLAGS_EOS) != 0) {
        OH_VideoDecoder_FreeOutputBuffer(codec, index);
        return;
      }
      if (OH_VideoDecoder_RenderOutputBuffer(codec, index) != AV_ERR_OK) {
        self->errors_++;
        return;
      }
      self->rendered_++;
      on_rendered = self->on_rendered_;
    }
    if (on_rendered) {
      on_rendered(pts_us);
    }
  }

  static constexpr int kInputWaitMs = 40;

  std::mutex mutex_;
  std::condition_variable input_ready_;
  OH_AVCodec *decoder_ = nullptr;
  std::deque<Input> inputs_;
  std::vector<uint8_t> csd_;
  bool csd_pending_ = false;
  bool failed_ = false;
  uint64_t fed_ = 0;
  uint64_t starved_ = 0;
  uint64_t rendered_ = 0;
  uint64_t errors_ = 0;
  int32_t last_error_ = 0;
  std::function<void(int64_t)> on_rendered_;
};

}  // namespace flutter_ffi_uvc

#endif  // FLUTTER_FFI_UVC_H26X_H_
