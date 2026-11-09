# Native half of the HarmonyOS camera path: receives the UVC stream on the
# device's file descriptor, assembles and decodes the frames and writes them
# into the Flutter texture's surface. Device discovery, permission and mode
# negotiation are driven from ArkTS (src/main/ets/camera).
cmake_minimum_required(VERSION 3.5.0)
project(flutter_ffi_uvc_preview)

add_library(flutter_ffi_uvc_preview SHARED uvc_preview.cpp)
target_link_libraries(flutter_ffi_uvc_preview PUBLIC
  libace_napi.z.so
  libnative_window.so
  libnative_buffer.so
  libimage_source.so
  libpixelmap.so
  libhilog_ndk.z.so
)
