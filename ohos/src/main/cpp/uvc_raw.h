// Uncompressed (YUV) camera streams: each assembled frame is a picture in
// YUY2, UYVY or NV12 layout. It is repacked to NV12 on the CPU (a 640x480
// frame is well under a millisecond) and written into the preview window and
// the recorder the same way decoded MJPEG frames are; photos are encoded to
// JPEG from the latest NV12 picture by the system image packer.
//
// USB 2.0 limits these formats to small modes (1080p YUY2 would be 4 MB per
// frame at ~24 MB/s of bulk bandwidth), so they are not optimised further.

#ifndef FLUTTER_FFI_UVC_RAW_H_
#define FLUTTER_FFI_UVC_RAW_H_

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <hilog/log.h>
#include <multimedia/image_framework/image/image_packer_native.h>
#include <multimedia/image_framework/image/pixelmap_native.h>

#include "uvc_nal.h"
#include "uvc_recorder.h"
#include "uvc_surface.h"

namespace flutter_ffi_uvc {

// One picture in NV12 with tightly packed rows.
struct Nv12Picture {
  int32_t width = 0;
  int32_t height = 0;
  std::vector<uint8_t> bytes;

  size_t LumaBytes() const { return static_cast<size_t>(width) * static_cast<size_t>(height); }
  FramePixels Pixels() const {
    FramePixels pixels;
    pixels.yuv = true;
    pixels.width = width;
    pixels.height = height;
    pixels.plane0 = bytes.data();
    pixels.stride0 = static_cast<size_t>(width);
    pixels.plane1 = bytes.data() + LumaBytes();
    pixels.stride1 = static_cast<size_t>(width);
    return pixels;
  }
};

// Bytes one frame of `codec` takes for a `width` x `height` picture.
inline size_t RawFrameBytes(StreamCodec codec, int32_t width, int32_t height) {
  const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
  return codec == StreamCodec::kNv12 ? pixels * 3 / 2 : pixels * 2;
}

// Repacks one frame into `out`. Packed 4:2:2 sources (YUY2: Y U Y V, UYVY:
// U Y V Y) keep the chroma of every even row; NV12 is copied. Returns false
// when `length` is not a whole frame.
inline bool RepackToNv12(StreamCodec codec, const uint8_t *data, size_t length, int32_t width, int32_t height,
                         Nv12Picture *out) {
  if (width <= 0 || height <= 0 || length != RawFrameBytes(codec, width, height)) {
    return false;
  }
  out->width = width;
  out->height = height;
  const size_t luma_bytes = out->LumaBytes();
  out->bytes.resize(luma_bytes + luma_bytes / 2);
  uint8_t *luma = out->bytes.data();
  uint8_t *chroma = luma + luma_bytes;
  if (codec == StreamCodec::kNv12) {
    std::memcpy(luma, data, out->bytes.size());
    return true;
  }
  const size_t row_bytes = static_cast<size_t>(width) * 2;
  const size_t y_offset = codec == StreamCodec::kYuy2 ? 0 : 1;
  const size_t c_offset = codec == StreamCodec::kYuy2 ? 1 : 0;
  for (int32_t row = 0; row < height; row++) {
    const uint8_t *source = data + static_cast<size_t>(row) * row_bytes;
    uint8_t *y = luma + static_cast<size_t>(row) * static_cast<size_t>(width);
    for (int32_t x = 0; x < width; x++) {
      y[x] = source[static_cast<size_t>(x) * 2 + y_offset];
    }
    if ((row & 1) == 0) {
      uint8_t *uv = chroma + static_cast<size_t>(row / 2) * static_cast<size_t>(width);
      for (int32_t x = 0; x < width; x += 2) {
        uv[x] = source[static_cast<size_t>(x) * 2 + c_offset];          // U
        uv[x + 1] = source[static_cast<size_t>(x) * 2 + 2 + c_offset];  // V
      }
    }
  }
  return true;
}

// Encodes an NV12 picture to JPEG with the system image packer; empty on failure.
inline std::vector<uint8_t> EncodeNv12ToJpeg(const Nv12Picture &picture, int32_t quality) {
  std::vector<uint8_t> jpeg;
  if (picture.bytes.empty()) {
    return jpeg;
  }
  OH_Pixelmap_InitializationOptions *options = nullptr;
  OH_PixelmapNative *pixelmap = nullptr;
  OH_ImagePackerNative *packer = nullptr;
  OH_PackingOptions *packing = nullptr;
  if (OH_PixelmapInitializationOptions_Create(&options) == IMAGE_SUCCESS) {
    OH_PixelmapInitializationOptions_SetWidth(options, static_cast<uint32_t>(picture.width));
    OH_PixelmapInitializationOptions_SetHeight(options, static_cast<uint32_t>(picture.height));
    OH_PixelmapInitializationOptions_SetSrcPixelFormat(options, PIXEL_FORMAT_NV12);
    OH_PixelmapInitializationOptions_SetPixelFormat(options, PIXEL_FORMAT_NV12);
    // CreatePixelmap copies the bytes; the const_cast only satisfies its signature.
    OH_PixelmapNative_CreatePixelmap(const_cast<uint8_t *>(picture.bytes.data()), picture.bytes.size(), options,
                                     &pixelmap);
    OH_PixelmapInitializationOptions_Release(options);
  }
  if (pixelmap != nullptr && OH_ImagePackerNative_Create(&packer) == IMAGE_SUCCESS &&
      OH_PackingOptions_Create(&packing) == IMAGE_SUCCESS) {
    char mime[] = "image/jpeg";
    Image_MimeType type = {mime, sizeof(mime) - 1};
    OH_PackingOptions_SetMimeType(packing, &type);
    OH_PackingOptions_SetQuality(packing, static_cast<uint32_t>(quality));
    // A JPEG never exceeds the NV12 bytes by much; leave room for headers.
    jpeg.resize(picture.bytes.size() + 64 * 1024);
    size_t size = jpeg.size();
    const Image_ErrorCode packed = OH_ImagePackerNative_PackToDataFromPixelmap(packer, packing, pixelmap, jpeg.data(), &size);
    if (packed == IMAGE_SUCCESS && size > 0) {
      jpeg.resize(size);
    } else {
      OH_LOG_WARN(LOG_APP, "JPEG packing failed: %{public}d", static_cast<int>(packed));
      jpeg.clear();
    }
  }
  if (packing != nullptr) {
    OH_PackingOptions_Release(packing);
  }
  if (packer != nullptr) {
    OH_ImagePackerNative_Release(packer);
  }
  if (pixelmap != nullptr) {
    OH_PixelmapNative_Release(pixelmap);
  }
  return jpeg;
}

}  // namespace flutter_ffi_uvc

#endif  // FLUTTER_FFI_UVC_RAW_H_
