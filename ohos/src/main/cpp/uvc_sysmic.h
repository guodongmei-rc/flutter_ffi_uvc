// The camera's microphone through the system audio framework: the kernel's
// USB audio driver does the isochronous streaming and the PCM is read from an
// OHAudio capturer.
//
// Mirrors the Android side (UsbMicCapture.kt). It is the fallback for a
// camera whose direct capture (uvc_uac.h) is starved: userspace usbfs
// isochronous reads only get a fraction of the nominal rate on some hosts,
// while the kernel path delivers the full stream. Needs the application's
// ohos.permission.MICROPHONE; without it the capturer cannot be started.

#ifndef FLUTTER_FFI_UVC_SYSMIC_H_
#define FLUTTER_FFI_UVC_SYSMIC_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

#include <hilog/log.h>
#include <ohaudio/native_audio_device_base.h>
#include <ohaudio/native_audio_routing_manager.h>
#include <ohaudio/native_audiocapturer.h>
#include <ohaudio/native_audiostream_base.h>
#include <ohaudio/native_audiostreambuilder.h>

#include "uvc_recorder.h"
#include "uvc_surface.h"

namespace flutter_ffi_uvc {

class SystemMicCapture {
 public:
  ~SystemMicCapture() { Stop(); }

  // Whether the system lists the camera's microphone as an audio input and
  // records from it by default. A USB audio input qualifies, and so does an
  // input carrying the camera's USB product name: some systems report the
  // camera under a device type of their own. There is no way to pin a
  // capturer to a device on the supported API levels, so an input the system
  // would not record from does not count — the phone's own microphone must
  // never end up in a camera recording.
  static bool FindUsbInput(const std::string &product_name) {
    OH_AudioRoutingManager *routing = nullptr;
    if (OH_AudioManager_GetAudioRoutingManager(&routing) != AUDIOCOMMON_RESULT_SUCCESS || routing == nullptr) {
      return false;
    }
    uint32_t usb_id = 0;
    bool found = false;
    OH_AudioDeviceDescriptorArray *inputs = nullptr;
    if (OH_AudioRoutingManager_GetDevices(routing, AUDIO_DEVICE_FLAG_INPUT, &inputs) == AUDIOCOMMON_RESULT_SUCCESS &&
        inputs != nullptr) {
      for (uint32_t i = 0; i < inputs->size && !found; i++) {
        if (IsUsbInput(inputs->descriptors[i], product_name)) {
          found = OH_AudioDeviceDescriptor_GetDeviceId(inputs->descriptors[i], &usb_id) == AUDIOCOMMON_RESULT_SUCCESS;
        }
      }
      OH_AudioRoutingManager_ReleaseDevices(routing, inputs);
    }
    if (!found) {
      OH_LOG_INFO(LOG_APP, "system microphone: no USB audio input listed");
      return false;
    }
    bool preferred = false;
    OH_AudioDeviceDescriptorArray *route = nullptr;
    if (OH_AudioRoutingManager_GetPreferredInputDevice(routing, AUDIOSTREAM_SOURCE_TYPE_MIC, &route) ==
            AUDIOCOMMON_RESULT_SUCCESS &&
        route != nullptr) {
      for (uint32_t i = 0; i < route->size; i++) {
        uint32_t id = 0;
        if (OH_AudioDeviceDescriptor_GetDeviceId(route->descriptors[i], &id) == AUDIOCOMMON_RESULT_SUCCESS &&
            id == usb_id) {
          preferred = true;
        }
      }
      OH_AudioRoutingManager_ReleaseDevices(routing, route);
    }
    if (!preferred) {
      OH_LOG_WARN(LOG_APP, "system microphone: USB audio input %{public}u is not the one recordings use", usb_id);
    }
    return preferred;
  }

  // Opens a capturer at the first sample rate the system accepts and starts
  // it. With `await_first_data` the call then waits up to kFirstDataWaitMs
  // for PCM, as Android does the first time: the system may list the
  // microphone of a camera that sends no audio. Returns 0 or a negative
  // error code: kErrorRecorderNoUsbMic, kErrorRecorderMicrophoneStart
  // (also without the microphone permission), kErrorRecorderMicrophoneSilent.
  int32_t Start(const std::string &product_name, bool await_first_data) {
    if (capturer_ != nullptr) {
      return kErrorRecorderBusy;
    }
    if (!FindUsbInput(product_name)) {
      return kErrorRecorderNoUsbMic;
    }
    for (const int32_t rate : kCandidateRates) {
      capturer_ = Open(rate);
      if (capturer_ != nullptr) {
        break;
      }
    }
    if (capturer_ == nullptr) {
      OH_LOG_WARN(LOG_APP, "system microphone: no capturer could be created");
      return kErrorRecorderMicrophoneStart;
    }
    int32_t rate = 0;
    OH_AudioCapturer_GetSamplingRate(capturer_, &rate);
    sample_rate_ = rate;
    const OH_AudioStream_Result started = OH_AudioCapturer_Start(capturer_);
    if (started != AUDIOSTREAM_SUCCESS || sample_rate_ <= 0) {
      OH_LOG_WARN(LOG_APP, "system microphone: start failed: %{public}d (rate %{public}d)",
                  static_cast<int>(started), sample_rate_);
      Stop();
      return kErrorRecorderMicrophoneStart;
    }
    if (await_first_data) {
      std::unique_lock<std::mutex> lock(mutex_);
      const bool delivered =
          data_.wait_for(lock, std::chrono::milliseconds(kFirstDataWaitMs), [this] { return bytes_ > 0; });
      lock.unlock();
      if (!delivered) {
        OH_LOG_WARN(LOG_APP, "system microphone: nothing delivered in %{public}d ms", kFirstDataWaitMs);
        Stop();
        return kErrorRecorderMicrophoneSilent;
      }
    }
    OH_LOG_INFO(LOG_APP, "system microphone: capturing at %{public}d Hz, %{public}d ch", sample_rate_, kChannels);
    return 0;
  }

  int32_t sample_rate() const { return sample_rate_; }
  int32_t channels() const { return kChannels; }

  void SetSink(PcmSink *sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = sink;
  }

  void Stop() {
    OH_AudioCapturer *capturer = capturer_;
    capturer_ = nullptr;
    if (capturer == nullptr) {
      return;
    }
    // Stop() returns once no read callback is running any more.
    OH_AudioCapturer_Stop(capturer);
    OH_AudioCapturer_Release(capturer);
    std::lock_guard<std::mutex> lock(mutex_);
    OH_LOG_INFO(LOG_APP, "system microphone: stopped after %{public}llu bytes",
                static_cast<unsigned long long>(bytes_));
    bytes_ = 0;
  }

 private:
  static bool IsUsbInput(OH_AudioDeviceDescriptor *device, const std::string &product_name) {
    OH_AudioDevice_Type type = AUDIO_DEVICE_TYPE_INVALID;
    if (OH_AudioDeviceDescriptor_GetDeviceType(device, &type) != AUDIOCOMMON_RESULT_SUCCESS) {
      return false;
    }
    if (type == AUDIO_DEVICE_TYPE_USB_HEADSET || type == AUDIO_DEVICE_TYPE_USB_DEVICE) {
      return true;
    }
    if (type == AUDIO_DEVICE_TYPE_MIC || product_name.empty()) {
      return false;
    }
    // Owned by the descriptor; must not be freed here.
    char *name = nullptr;
    return OH_AudioDeviceDescriptor_GetDeviceName(device, &name) == AUDIOCOMMON_RESULT_SUCCESS && name != nullptr &&
           product_name == name;
  }

  // Same format as Android asks AudioRecord for: mono 16-bit PCM from the
  // plain microphone source.
  OH_AudioCapturer *Open(int32_t rate) {
    OH_AudioStreamBuilder *builder = nullptr;
    if (OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_CAPTURER) != AUDIOSTREAM_SUCCESS ||
        builder == nullptr) {
      return nullptr;
    }
    OH_AudioCapturer *capturer = nullptr;
    OH_AudioCapturer_Callbacks callbacks = {};
    callbacks.OH_AudioCapturer_OnReadData = OnReadData;
    if (OH_AudioStreamBuilder_SetSamplingRate(builder, rate) == AUDIOSTREAM_SUCCESS &&
        OH_AudioStreamBuilder_SetChannelCount(builder, kChannels) == AUDIOSTREAM_SUCCESS &&
        OH_AudioStreamBuilder_SetSampleFormat(builder, AUDIOSTREAM_SAMPLE_S16LE) == AUDIOSTREAM_SUCCESS &&
        OH_AudioStreamBuilder_SetEncodingType(builder, AUDIOSTREAM_ENCODING_TYPE_RAW) == AUDIOSTREAM_SUCCESS &&
        OH_AudioStreamBuilder_SetCapturerInfo(builder, AUDIOSTREAM_SOURCE_TYPE_MIC) == AUDIOSTREAM_SUCCESS &&
        OH_AudioStreamBuilder_SetCapturerCallback(builder, callbacks, this) == AUDIOSTREAM_SUCCESS) {
      const OH_AudioStream_Result generated = OH_AudioStreamBuilder_GenerateCapturer(builder, &capturer);
      if (generated != AUDIOSTREAM_SUCCESS) {
        OH_LOG_WARN(LOG_APP, "system microphone: capturer refused at %{public}d Hz: %{public}d", rate,
                    static_cast<int>(generated));
        capturer = nullptr;
      }
    }
    OH_AudioStreamBuilder_Destroy(builder);
    return capturer;
  }

  // Called on the capturer's own thread. The PCM is stamped with its arrival
  // time, the clock the video frames carry too.
  static int32_t OnReadData(OH_AudioCapturer *capturer, void *user_data, void *buffer, int32_t length) {
    auto *self = static_cast<SystemMicCapture *>(user_data);
    if (buffer == nullptr || length <= 0) {
      return 0;
    }
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->bytes_ += static_cast<uint64_t>(length);
    if (self->sink_ != nullptr) {
      self->sink_->WritePcm(static_cast<const uint8_t *>(buffer), static_cast<size_t>(length), NowMicros());
    }
    self->data_.notify_all();
    return 0;
  }

  static constexpr int32_t kChannels = 1;
  // Same order as Android: 48 kHz first, USB audio devices almost always
  // support it, and the fewer resampling steps the better.
  static constexpr int32_t kCandidateRates[] = {48000, 44100, 32000, 24000, 16000};
  // Same as Android's USB_MIC_FIRST_DATA_WAIT_MS.
  static constexpr int kFirstDataWaitMs = 1000;

  OH_AudioCapturer *capturer_ = nullptr;
  int32_t sample_rate_ = 0;
  std::mutex mutex_;
  std::condition_variable data_;
  PcmSink *sink_ = nullptr;
  uint64_t bytes_ = 0;
};

}  // namespace flutter_ffi_uvc

#endif  // FLUTTER_FFI_UVC_SYSMIC_H_
