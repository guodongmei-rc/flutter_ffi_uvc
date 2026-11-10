# Native half of the HarmonyOS camera path: receives the UVC stream on the
# device's file descriptor, assembles and decodes the frames and writes them
# into the Flutter texture's surface. Device discovery, permission and mode
# negotiation are driven from ArkTS (src/main/ets/camera).
cmake_minimum_required(VERSION 3.5.0)
project(flutter_ffi_uvc_preview)

# The headers rely on C++17 (inline variables, implicitly-inline static
# constexpr members). Without this the toolchain defaults to C++14, which
# only links at -O2 where the constants happen to be folded; debug (-O0)
# builds fail with undefined symbols.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(flutter_ffi_uvc_preview SHARED uvc_preview.cpp)
target_link_libraries(flutter_ffi_uvc_preview PUBLIC
  libace_napi.z.so
  libnative_window.so
  libnative_buffer.so
  libnative_media_core.so
  libnative_media_codecbase.so
  libnative_media_venc.so
  libnative_media_avmuxer.so
  libimage_source.so
  libpixelmap.so
  libhilog_ndk.z.so
)
