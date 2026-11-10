// N-API surface of the HarmonyOS native code. The work itself is in the
// headers: uvc_stream.h (USB transfers, frame hand-off), uvc_frame_assembler.h
// (payloads to frames), uvc_surface.h (JPEG decode and drawing) and
// uvc_recorder.h (MP4 recording).
//
// Every function here is called from the ArkTS thread that owns the camera
// session. Streams and windows are referred to by small integer ids rather
// than pointers, so a stale id from ArkTS cannot reach freed memory.

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <unistd.h>

#include <hilog/log.h>
#include <napi/native_api.h>

#include "uvc_stream.h"
#include "uvc_surface.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0001
#define LOG_TAG "FlutterFfiUvc"

namespace {

using flutter_ffi_uvc::IsoStreamConfig;
using flutter_ffi_uvc::PreviewWindow;
using flutter_ffi_uvc::StreamSession;
using flutter_ffi_uvc::StreamStats;

constexpr size_t kMaxArguments = 12;

std::mutex g_streams_mutex;
std::map<int32_t, std::shared_ptr<StreamSession>> g_streams;
int32_t g_next_stream_id = 1;

struct Arguments {
  size_t count = kMaxArguments;
  napi_value values[kMaxArguments] = {nullptr};
};

Arguments GetArguments(napi_env env, napi_callback_info info) {
  Arguments arguments;
  napi_get_cb_info(env, info, &arguments.count, arguments.values, nullptr, nullptr);
  return arguments;
}

// Reads argument `index` as an integer; false when it is missing or not a number.
bool GetInt(napi_env env, const Arguments &arguments, size_t index, int64_t *value) {
  return index < arguments.count && napi_get_value_int64(env, arguments.values[index], value) == napi_ok;
}

bool GetWindowId(napi_env env, const Arguments &arguments, size_t index, uint64_t *id) {
  bool lossless = false;
  return index < arguments.count &&
         napi_get_value_bigint_uint64(env, arguments.values[index], id, &lossless) == napi_ok;
}

napi_value Number(napi_env env, double value) {
  napi_value result = nullptr;
  napi_create_double(env, value, &result);
  return result;
}

napi_value Undefined(napi_env env) {
  napi_value result = nullptr;
  napi_get_undefined(env, &result);
  return result;
}

std::shared_ptr<StreamSession> FindStream(int64_t id) {
  std::lock_guard<std::mutex> lock(g_streams_mutex);
  auto found = g_streams.find(static_cast<int32_t>(id));
  return found == g_streams.end() ? nullptr : found->second;
}

// Preview windows ----------------------------------------------------------------

// createWindow(surfaceId: string): bigint
napi_value CreateWindow(napi_env env, napi_callback_info info) {
  const Arguments arguments = GetArguments(env, info);
  uint64_t window_id = 0;
  char text[32] = {0};
  size_t length = 0;
  if (arguments.count >= 1 &&
      napi_get_value_string_utf8(env, arguments.values[0], text, sizeof(text), &length) == napi_ok && length > 0) {
    char *end = nullptr;
    errno = 0;
    const uint64_t surface_id = strtoull(text, &end, 10);
    if (errno == 0 && end != text && *end == '\0') {
      window_id = flutter_ffi_uvc::CreatePreviewWindow(surface_id);
      if (window_id == 0) {
        OH_LOG_ERROR(LOG_APP, "opening surface %{public}s for writing failed", text);
      }
    }
  }
  napi_value result = nullptr;
  napi_create_bigint_uint64(env, window_id, &result);
  return result;
}

// destroyWindow(window: bigint): void
napi_value DestroyWindow(napi_env env, napi_callback_info info) {
  const Arguments arguments = GetArguments(env, info);
  uint64_t window_id = 0;
  if (GetWindowId(env, arguments, 0, &window_id)) {
    flutter_ffi_uvc::DestroyPreviewWindow(window_id);
  }
  return Undefined(env);
}

// USB requests -------------------------------------------------------------------

// usbClaimInterface(fd, interfaceNumber): number
napi_value UsbClaimInterface(napi_env env, napi_callback_info info) {
  const Arguments arguments = GetArguments(env, info);
  int64_t fd = -1;
  int64_t interface_number = 0;
  if (!GetInt(env, arguments, 0, &fd) || !GetInt(env, arguments, 1, &interface_number)) {
    return Number(env, -EINVAL);
  }
  return Number(env, flutter_ffi_uvc::UsbClaimInterface(static_cast<int>(fd), static_cast<unsigned int>(interface_number)));
}

// usbReleaseInterface(fd, interfaceNumber): number
napi_value UsbReleaseInterface(napi_env env, napi_callback_info info) {
  const Arguments arguments = GetArguments(env, info);
  int64_t fd = -1;
  int64_t interface_number = 0;
  if (!GetInt(env, arguments, 0, &fd) || !GetInt(env, arguments, 1, &interface_number)) {
    return Number(env, -EINVAL);
  }
  return Number(env,
                flutter_ffi_uvc::UsbReleaseInterface(static_cast<int>(fd), static_cast<unsigned int>(interface_number)));
}

// usbSetInterface(fd, interfaceNumber, altSetting): number
napi_value UsbSetInterface(napi_env env, napi_callback_info info) {
  const Arguments arguments = GetArguments(env, info);
  int64_t fd = -1;
  int64_t interface_number = 0;
  int64_t alt_setting = 0;
  if (!GetInt(env, arguments, 0, &fd) || !GetInt(env, arguments, 1, &interface_number) ||
      !GetInt(env, arguments, 2, &alt_setting)) {
    return Number(env, -EINVAL);
  }
  return Number(env, flutter_ffi_uvc::UsbSetInterface(static_cast<int>(fd), static_cast<unsigned int>(interface_number),
                                                      static_cast<unsigned int>(alt_setting)));
}

// usbClearHalt(fd, endpoint): number
napi_value UsbClearHalt(napi_env env, napi_callback_info info) {
  const Arguments arguments = GetArguments(env, info);
  int64_t fd = -1;
  int64_t endpoint = 0;
  if (!GetInt(env, arguments, 0, &fd) || !GetInt(env, arguments, 1, &endpoint)) {
    return Number(env, -EINVAL);
  }
  return Number(env, flutter_ffi_uvc::UsbClearHalt(static_cast<int>(fd), static_cast<unsigned int>(endpoint)));
}

// usbControlTransfer(fd, requestType, request, value, index, data: ArrayBuffer, timeoutMs): number
napi_value UsbControlTransfer(napi_env env, napi_callback_info info) {
  const Arguments arguments = GetArguments(env, info);
  int64_t fd = -1;
  int64_t request_type = 0;
  int64_t request = 0;
  int64_t value = 0;
  int64_t index = 0;
  int64_t timeout_ms = 0;
  void *data = nullptr;
  size_t length = 0;
  if (!GetInt(env, arguments, 0, &fd) || !GetInt(env, arguments, 1, &request_type) ||
      !GetInt(env, arguments, 2, &request) || !GetInt(env, arguments, 3, &value) ||
      !GetInt(env, arguments, 4, &index) || arguments.count < 7 ||
      napi_get_arraybuffer_info(env, arguments.values[5], &data, &length) != napi_ok ||
      !GetInt(env, arguments, 6, &timeout_ms) || length > UINT16_MAX) {
    return Number(env, -EINVAL);
  }
  return Number(env, flutter_ffi_uvc::UsbControlTransfer(
                         static_cast<int>(fd), static_cast<uint8_t>(request_type), static_cast<uint8_t>(request),
                         static_cast<uint16_t>(value), static_cast<uint16_t>(index), static_cast<uint8_t *>(data),
                         static_cast<uint16_t>(length), static_cast<unsigned int>(timeout_ms)));
}

// usbClose(fd): void
napi_value UsbClose(napi_env env, napi_callback_info info) {
  const Arguments arguments = GetArguments(env, info);
  int64_t fd = -1;
  if (GetInt(env, arguments, 0, &fd) && fd >= 0) {
    close(static_cast<int>(fd));
  }
  return Undefined(env);
}

// Streams ------------------------------------------------------------------------

// streamStart(fd, endpoint, bytesPerPacket, maxFrameBytes, window: bigint, codec, width, height, bulk): number
napi_value StreamStart(napi_env env, napi_callback_info info) {
  const Arguments arguments = GetArguments(env, info);
  int64_t fd = -1;
  int64_t endpoint = 0;
  int64_t bytes_per_packet = 0;
  int64_t max_frame_bytes = 0;
  uint64_t window_id = 0;
  int64_t codec = 0;
  int64_t width = 0;
  int64_t height = 0;
  int64_t bulk = 0;
  if (!GetInt(env, arguments, 0, &fd) || !GetInt(env, arguments, 1, &endpoint) ||
      !GetInt(env, arguments, 2, &bytes_per_packet) || !GetInt(env, arguments, 3, &max_frame_bytes) ||
      !GetWindowId(env, arguments, 4, &window_id) || !GetInt(env, arguments, 5, &codec) ||
      !GetInt(env, arguments, 6, &width) || !GetInt(env, arguments, 7, &height) ||
      !GetInt(env, arguments, 8, &bulk) || bytes_per_packet <= 0 || max_frame_bytes <= 0 || codec < 0 || codec > 5) {
    return Number(env, -EINVAL);
  }
  IsoStreamConfig config;
  config.fd = static_cast<int>(fd);
  config.endpoint = static_cast<uint8_t>(endpoint);
  config.bytes_per_packet = static_cast<uint32_t>(bytes_per_packet);
  config.max_frame_bytes = static_cast<size_t>(max_frame_bytes);
  config.codec = static_cast<flutter_ffi_uvc::StreamCodec>(codec);
  config.width = static_cast<int32_t>(width);
  config.height = static_cast<int32_t>(height);
  config.bulk = bulk != 0;

  auto session = std::make_shared<StreamSession>();
  const int ret = session->Start(config, window_id != 0 ? flutter_ffi_uvc::FindWindow(window_id) : nullptr);
  if (ret != 0) {
    return Number(env, ret);
  }
  std::lock_guard<std::mutex> lock(g_streams_mutex);
  const int32_t id = g_next_stream_id++;
  g_streams[id] = session;
  return Number(env, id);
}

// streamSetWindow(stream, window: bigint): void
napi_value StreamSetWindow(napi_env env, napi_callback_info info) {
  const Arguments arguments = GetArguments(env, info);
  int64_t stream_id = 0;
  uint64_t window_id = 0;
  if (GetInt(env, arguments, 0, &stream_id) && GetWindowId(env, arguments, 1, &window_id)) {
    std::shared_ptr<StreamSession> session = FindStream(stream_id);
    if (session != nullptr) {
      session->SetWindow(window_id != 0 ? flutter_ffi_uvc::FindWindow(window_id) : nullptr);
    }
  }
  return Undefined(env);
}

// streamStop(stream): void. Blocks until the transfers are cancelled.
napi_value StreamStop(napi_env env, napi_callback_info info) {
  const Arguments arguments = GetArguments(env, info);
  int64_t stream_id = 0;
  if (GetInt(env, arguments, 0, &stream_id)) {
    std::shared_ptr<StreamSession> session;
    {
      std::lock_guard<std::mutex> lock(g_streams_mutex);
      auto found = g_streams.find(static_cast<int32_t>(stream_id));
      if (found != g_streams.end()) {
        session = found->second;
        g_streams.erase(found);
      }
    }
    if (session != nullptr) {
      session->Stop();
    }
  }
  return Undefined(env);
}

// streamStats(stream): number[], in the order documented in index.d.ts.
napi_value StreamStatsOf(napi_env env, napi_callback_info info) {
  const Arguments arguments = GetArguments(env, info);
  napi_value result = nullptr;
  napi_create_array(env, &result);
  int64_t stream_id = 0;
  std::shared_ptr<StreamSession> session = GetInt(env, arguments, 0, &stream_id) ? FindStream(stream_id) : nullptr;
  if (session == nullptr) {
    return result;
  }
  const StreamStats stats = session->Stats();
  uint32_t index = 0;
  const auto push = [&](double value) { napi_set_element(env, result, index++, Number(env, value)); };
  push(stats.running ? 1 : 0);
  push(stats.stop_errno);
  push(static_cast<double>(stats.urbs));
  push(static_cast<double>(stats.packets));
  push(static_cast<double>(stats.bad_packets));
  push(static_cast<double>(stats.empty_packets));
  push(static_cast<double>(stats.submit_failures));
  push(static_cast<double>(stats.assembler.frames));
  push(static_cast<double>(stats.assembler.ends_by_eof));
  push(static_cast<double>(stats.assembler.ends_by_fid_toggle));
  push(static_cast<double>(stats.assembler.damaged_frames + stats.assembler.oversized_frames));
  push(static_cast<double>(stats.assembler.last_frame_bytes));
  push(static_cast<double>(stats.invalid_jpeg));
  push(static_cast<double>(stats.shown));
  push(static_cast<double>(stats.decode_failures));
  push(static_cast<double>(stats.dropped));
  push(static_cast<double>(stats.decode_ms_total));
  push(static_cast<double>(stats.show_ms_total));
  push(static_cast<double>(stats.first_frame_ms));
  push(stats.gap_avg_ms);
  push(stats.gap_p95_ms);
  push(stats.gap_max_ms);
  push(static_cast<double>(stats.elapsed_ms));
  for (uint64_t count : stats.input_gaps) {
    push(static_cast<double>(count));
  }
  for (uint64_t count : stats.shown_gaps) {
    push(static_cast<double>(count));
  }
  push(stats.yuv ? 1 : 0);
  push(stats.starved);
  push(stats.waiting_keyframe);
  return result;
}

// streamLatestJpeg(stream): ArrayBuffer | undefined — the last complete frame as the camera sent it.
napi_value StreamLatestJpeg(napi_env env, napi_callback_info info) {
  const Arguments arguments = GetArguments(env, info);
  int64_t stream_id = 0;
  std::shared_ptr<StreamSession> session = GetInt(env, arguments, 0, &stream_id) ? FindStream(stream_id) : nullptr;
  if (session == nullptr) {
    return Undefined(env);
  }
  const std::vector<uint8_t> jpeg = session->LatestJpeg();
  if (jpeg.empty()) {
    return Undefined(env);
  }
  void *data = nullptr;
  napi_value result = nullptr;
  if (napi_create_arraybuffer(env, jpeg.size(), &data, &result) != napi_ok || data == nullptr) {
    return Undefined(env);
  }
  memcpy(data, jpeg.data(), jpeg.size());
  return result;
}

// recordStart(stream, path, fps, bitRate): number — 0 or a negative error code.
napi_value RecordStart(napi_env env, napi_callback_info info) {
  const Arguments arguments = GetArguments(env, info);
  int64_t stream_id = 0;
  int64_t fps = 0;
  int64_t bitrate = 0;
  char path[1024] = {0};
  size_t length = 0;
  if (!GetInt(env, arguments, 0, &stream_id) || arguments.count < 4 ||
      napi_get_value_string_utf8(env, arguments.values[1], path, sizeof(path), &length) != napi_ok || length == 0 ||
      !GetInt(env, arguments, 2, &fps) || !GetInt(env, arguments, 3, &bitrate) || fps <= 0 || bitrate <= 0) {
    return Number(env, -EINVAL);
  }
  std::shared_ptr<StreamSession> session = FindStream(stream_id);
  if (session == nullptr) {
    return Number(env, flutter_ffi_uvc::kErrorRecorderNoFrames);
  }
  return Number(env, session->StartRecording(path, static_cast<int32_t>(fps), bitrate));
}

// recordStop(stream): number — frames written, or a negative error code.
napi_value RecordStop(napi_env env, napi_callback_info info) {
  const Arguments arguments = GetArguments(env, info);
  int64_t stream_id = 0;
  std::shared_ptr<StreamSession> session = GetInt(env, arguments, 0, &stream_id) ? FindStream(stream_id) : nullptr;
  if (session == nullptr) {
    return Number(env, flutter_ffi_uvc::kErrorRecorderNoFrames);
  }
  return Number(env, static_cast<double>(session->StopRecording()));
}

napi_value Init(napi_env env, napi_value exports) {
  napi_property_descriptor descriptors[] = {
      {"createWindow", nullptr, CreateWindow, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"destroyWindow", nullptr, DestroyWindow, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"usbClaimInterface", nullptr, UsbClaimInterface, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"usbReleaseInterface", nullptr, UsbReleaseInterface, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"usbSetInterface", nullptr, UsbSetInterface, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"usbClearHalt", nullptr, UsbClearHalt, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"usbControlTransfer", nullptr, UsbControlTransfer, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"usbClose", nullptr, UsbClose, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"streamStart", nullptr, StreamStart, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"streamSetWindow", nullptr, StreamSetWindow, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"streamStop", nullptr, StreamStop, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"streamStats", nullptr, StreamStatsOf, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"streamLatestJpeg", nullptr, StreamLatestJpeg, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"recordStart", nullptr, RecordStart, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"recordStop", nullptr, RecordStop, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_define_properties(env, exports, sizeof(descriptors) / sizeof(descriptors[0]), descriptors);
  return exports;
}

napi_module preview_module = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "flutter_ffi_uvc_preview",
    .nm_priv = nullptr,
    .reserved = {nullptr},
};

}  // namespace

extern "C" __attribute__((constructor)) void RegisterFlutterFfiUvcPreviewModule(void) {
  napi_module_register(&preview_module);
}
