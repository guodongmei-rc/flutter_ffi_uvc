// Decoding JPEG frames and showing them in the surface behind a Flutter
// texture.
//
// The JPEG is decoded by the system image decoder and its pixels are copied
// row by row into a buffer requested from the texture's surface, which the
// Flutter engine's external-texture path then consumes. Decoding a 1080p
// frame took about 15 ms on the device this was written against, and copying
// it as RGBA about 20 ms (the decoder's output sits in graphics memory, which
// is slow to read back), so callers run the two steps on worker threads and
// prefer NV12, which is a third of the bytes.

#ifndef FLUTTER_FFI_UVC_SURFACE_H_
#define FLUTTER_FFI_UVC_SURFACE_H_

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include <linux/dma-buf.h>

#include <hilog/log.h>
#include <multimedia/image_framework/image/image_source_native.h>
#include <multimedia/image_framework/image/pixelmap_native.h>
#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0001
#define LOG_TAG "FlutterFfiUvc"

namespace flutter_ffi_uvc {

inline constexpr int32_t kErrorBadArguments = -1;
inline constexpr int32_t kErrorRequestBuffer = -2;
inline constexpr int32_t kErrorMapBuffer = -3;
inline constexpr int32_t kErrorFlushBuffer = -4;
inline constexpr int32_t kErrorWindowClosed = -5;
inline constexpr int32_t kErrorDecode = -6;
// Not an error: a newer frame had already been shown.
inline constexpr int32_t kResultSuperseded = 1;
inline constexpr int kFenceTimeoutMs = 100;
inline constexpr size_t kBytesPerPixel = 4;

// Not an error either: the buffers are not laid out the way the YUV path
// expects, so the caller falls back to RGBA.
inline constexpr int32_t kResultYuvUnavailable = 2;

struct MappedBuffer {
  const BufferHandle *handle;
  int fd;
  size_t size;
  void *address;
};

inline constexpr size_t kMaxMappings = 8;

struct PreviewWindow {
  // Guards everything below: frames are written on worker threads while
  // destroyWindow() runs on the ArkTS thread.
  std::mutex mutex;
  OHNativeWindow *window = nullptr;
  int32_t width = 0;
  int32_t height = 0;
  int32_t format = 0;
  // Sequence number of the newest frame written, so that two frames decoded
  // in parallel cannot be shown out of order.
  uint64_t shown_sequence = 0;
  // Mappings of the surface's buffers, kept across frames. The surface cycles
  // through a handful of buffers; mapping one afresh for every frame made the
  // copy several times slower, because each page then faults in again.
  std::vector<MappedBuffer> mappings;
};

inline void UnmapAll(PreviewWindow *preview) {
  for (const MappedBuffer &mapping : preview->mappings) {
    munmap(mapping.address, mapping.size);
  }
  preview->mappings.clear();
}

// Returns the mapping of `handle`, creating it on first use; nullptr on failure.
inline uint8_t *MapBuffer(PreviewWindow *preview, const BufferHandle *handle) {
  const size_t size = static_cast<size_t>(handle->size);
  for (const MappedBuffer &mapping : preview->mappings) {
    if (mapping.handle == handle && mapping.fd == handle->fd && mapping.size == size) {
      return static_cast<uint8_t *>(mapping.address);
    }
  }
  if (preview->mappings.size() >= kMaxMappings) {
    // More buffers than a surface normally cycles through: start over rather
    // than let stale mappings pile up.
    UnmapAll(preview);
  }
  void *address = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, handle->fd, 0);
  if (address == MAP_FAILED) {
    return nullptr;
  }
  preview->mappings.push_back({handle, handle->fd, size, address});
  return static_cast<uint8_t *>(address);
}

inline void SyncBuffer(int fd, uint64_t flags) {
  struct dma_buf_sync sync = {flags};
  ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

inline std::mutex g_windows_mutex;
inline std::map<uint64_t, std::shared_ptr<PreviewWindow>> g_windows;
inline uint64_t g_next_window_id = 1;

inline std::shared_ptr<PreviewWindow> FindWindow(uint64_t id) {
  std::lock_guard<std::mutex> lock(g_windows_mutex);
  auto found = g_windows.find(id);
  return found == g_windows.end() ? nullptr : found->second;
}

inline int64_t NowMicros() {
  struct timespec now = {0, 0};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<int64_t>(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
}

// Pixels of one decoded frame. RGBA uses plane0 only; NV12 has the luma rows
// in plane0 and the interleaved chroma rows in plane1.
struct FramePixels {
  bool yuv = false;
  int32_t width = 0;
  int32_t height = 0;
  const uint8_t *plane0 = nullptr;
  size_t stride0 = 0;
  const uint8_t *plane1 = nullptr;
  size_t stride1 = 0;
};

// Where the two planes of an NV12 buffer start and how far apart their rows
// are. False when the buffer does not describe such a layout.
struct YuvLayout {
  uint8_t *luma = nullptr;
  size_t luma_stride = 0;
  uint8_t *chroma = nullptr;
  size_t chroma_stride = 0;
};

inline bool MapYuvLayout(OH_NativeBuffer *buffer, YuvLayout *layout) {
  void *address = nullptr;
  OH_NativeBuffer_Planes planes = {};
  const int32_t mapped = buffer != nullptr ? OH_NativeBuffer_MapPlanes(buffer, &address, &planes) : -1;
  if (mapped != 0 || address == nullptr || planes.planeCount < 2) {
    OH_LOG_WARN(LOG_APP, "NV12 layout unavailable: MapPlanes=%{public}d planes=%{public}u", mapped, planes.planeCount);
    return false;
  }
  // Each plane is described by the byte distance between rows and between
  // neighbouring samples. The device this was written against reports the two
  // swapped relative to the header's field names (rowStride 2, columnStride
  // 1920 for the chroma of a 1920-wide frame), so they are told apart by size:
  // the larger one is the row distance.
  const auto row_bytes = [](const OH_NativeBuffer_Plane &plane) {
    return static_cast<size_t>(std::max(plane.rowStride, plane.columnStride));
  };
  const auto sample_bytes = [](const OH_NativeBuffer_Plane &plane) {
    return std::min(plane.rowStride, plane.columnStride);
  };
  // Semi-planar chroma is reported either as one plane or as two interleaved
  // ones (samples 2 bytes apart); it starts at the lower of the two offsets.
  uint64_t chroma_offset = planes.planes[1].offset;
  if (planes.planeCount >= 3) {
    if (sample_bytes(planes.planes[1]) != 2 || sample_bytes(planes.planes[2]) != 2) {
      OH_LOG_WARN(LOG_APP,
                  "NV12 layout unavailable: planes=%{public}u p0=%{public}llu/%{public}u/%{public}u "
                  "p1=%{public}llu/%{public}u/%{public}u p2=%{public}llu/%{public}u/%{public}u",
                  planes.planeCount, (unsigned long long)planes.planes[0].offset, planes.planes[0].rowStride,
                  planes.planes[0].columnStride, (unsigned long long)planes.planes[1].offset,
                  planes.planes[1].rowStride, planes.planes[1].columnStride,
                  (unsigned long long)planes.planes[2].offset, planes.planes[2].rowStride,
                  planes.planes[2].columnStride);
      return false;
    }
    chroma_offset = std::min(planes.planes[1].offset, planes.planes[2].offset);
  }
  auto *base = static_cast<uint8_t *>(address);
  layout->luma = base + planes.planes[0].offset;
  layout->luma_stride = row_bytes(planes.planes[0]);
  layout->chroma = base + chroma_offset;
  layout->chroma_stride = row_bytes(planes.planes[1]);
  return layout->luma_stride > 0 && layout->chroma_stride > 0;
}

// Copies one frame into a buffer requested from the window and queues it for
// display.
inline int32_t WriteFrame(PreviewWindow *preview, uint64_t sequence, const FramePixels &pixels) {
  std::lock_guard<std::mutex> lock(preview->mutex);
  OHNativeWindow *window = preview->window;
  if (window == nullptr) {
    return kErrorWindowClosed;
  }
  if (sequence <= preview->shown_sequence) {
    return kResultSuperseded;
  }
  const int32_t format = pixels.yuv ? NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP : NATIVEBUFFER_PIXEL_FMT_RGBA_8888;
  if (preview->width != pixels.width || preview->height != pixels.height || preview->format != format) {
    OH_NativeWindow_NativeWindowHandleOpt(window, SET_FORMAT, format);
    OH_NativeWindow_NativeWindowHandleOpt(
        window, SET_USAGE,
        static_cast<uint64_t>(NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE |
                              NATIVEBUFFER_USAGE_MEM_DMA | NATIVEBUFFER_USAGE_HW_TEXTURE));
    OH_NativeWindow_NativeWindowHandleOpt(window, SET_BUFFER_GEOMETRY, pixels.width, pixels.height);
    if (pixels.yuv) {
      // JPEG stores full-range BT.601.
      OH_NativeWindow_SetColorSpace(window, OH_COLORSPACE_BT601_EBU_FULL);
    }
    preview->width = pixels.width;
    preview->height = pixels.height;
    preview->format = format;
    // The surface allocates new buffers for the new size or format.
    UnmapAll(preview);
  }

  OHNativeWindowBuffer *buffer = nullptr;
  int fence_fd = -1;
  int32_t ret = OH_NativeWindow_NativeWindowRequestBuffer(window, &buffer, &fence_fd);
  if (ret != 0 || buffer == nullptr) {
    return kErrorRequestBuffer;
  }
  if (fence_fd >= 0) {
    // The previous reader of this buffer may still be using it.
    struct pollfd wait = {fence_fd, POLLIN, 0};
    poll(&wait, 1, kFenceTimeoutMs);
    close(fence_fd);
  }

  BufferHandle *handle = OH_NativeWindow_GetBufferHandleFromNative(buffer);
  if (handle == nullptr || handle->fd < 0 || handle->size <= 0 || handle->stride <= 0) {
    OH_NativeWindow_NativeWindowAbortBuffer(window, buffer);
    return kErrorMapBuffer;
  }

  if (pixels.yuv) {
    OH_NativeBuffer *target_buffer = nullptr;
    YuvLayout target;
    if (OH_NativeBuffer_FromNativeWindowBuffer(buffer, &target_buffer) != 0 || !MapYuvLayout(target_buffer, &target) ||
        target.luma_stride < static_cast<size_t>(pixels.width) ||
        target.chroma_stride < static_cast<size_t>(pixels.width)) {
      OH_LOG_WARN(LOG_APP, "NV12 surface buffer unusable: strides %{public}zu/%{public}zu", target.luma_stride,
                  target.chroma_stride);
      OH_NativeWindow_NativeWindowAbortBuffer(window, buffer);
      return kResultYuvUnavailable;
    }
    SyncBuffer(handle->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    const size_t row_bytes = static_cast<size_t>(pixels.width);
    for (int32_t y = 0; y < pixels.height; y++) {
      memcpy(target.luma + static_cast<size_t>(y) * target.luma_stride,
             pixels.plane0 + static_cast<size_t>(y) * pixels.stride0, row_bytes);
    }
    for (int32_t y = 0; y < pixels.height / 2; y++) {
      memcpy(target.chroma + static_cast<size_t>(y) * target.chroma_stride,
             pixels.plane1 + static_cast<size_t>(y) * pixels.stride1, row_bytes);
    }
  } else {
    uint8_t *target = MapBuffer(preview, handle);
    if (target == nullptr) {
      OH_NativeWindow_NativeWindowAbortBuffer(window, buffer);
      return kErrorMapBuffer;
    }
    SyncBuffer(handle->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    const size_t row_bytes = static_cast<size_t>(pixels.width) * kBytesPerPixel;
    const size_t stride = static_cast<size_t>(handle->stride);
    const size_t copy_bytes = row_bytes < stride ? row_bytes : stride;
    const int32_t rows = pixels.height < handle->height ? pixels.height : handle->height;
    for (int32_t y = 0; y < rows; y++) {
      const size_t target_offset = static_cast<size_t>(y) * stride;
      if (target_offset + copy_bytes > static_cast<size_t>(handle->size)) {
        break;
      }
      memcpy(target + target_offset, pixels.plane0 + static_cast<size_t>(y) * pixels.stride0, copy_bytes);
    }
  }
  SyncBuffer(handle->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);

  Region region = {nullptr, 0};
  ret = OH_NativeWindow_NativeWindowFlushBuffer(window, buffer, -1, region);
  if (ret != 0) {
    return kErrorFlushBuffer;
  }
  preview->shown_sequence = sequence;
  return 0;
}

// One decoded frame: the decoder's PixelMap and where its pixels are. Must be
// passed to ReleaseFrame().
struct DecodedFrame {
  OH_PixelmapNative *pixelmap = nullptr;
  // Graphics buffer behind an NV12 PixelMap; holds a reference.
  OH_NativeBuffer *buffer = nullptr;
  // Whether OH_PixelmapNative_AccessPixels() is outstanding.
  bool accessed = false;
  FramePixels pixels;
};

inline void ReleaseFrame(DecodedFrame *frame) {
  if (frame->pixelmap != nullptr && frame->accessed) {
    OH_PixelmapNative_UnaccessPixels(frame->pixelmap);
  }
  if (frame->buffer != nullptr) {
    OH_NativeBuffer_Unreference(frame->buffer);
  }
  if (frame->pixelmap != nullptr) {
    OH_PixelmapNative_Release(frame->pixelmap);
  }
  *frame = DecodedFrame();
}

// Decodes one JPEG frame with the system decoder. The hardware decoder
// handles one frame at a time (a second, concurrent request fails and falls
// back to a much slower software decode), so call this from a single thread.
//
// With `yuv` the decoder is asked for NV12, its native output: a 1080p frame
// is then 3 MB to copy instead of 8 MB of RGBA. Returns 0, kErrorDecode, or
// kResultYuvUnavailable when NV12 cannot be used here and the caller should
// retry with RGBA.
inline int32_t DecodeFrame(const uint8_t *jpeg, size_t jpeg_bytes, bool yuv, DecodedFrame *frame) {
  OH_ImageSourceNative *source = nullptr;
  OH_DecodingOptions *options = nullptr;
  OH_Pixelmap_ImageInfo *info = nullptr;
  int32_t result = kErrorDecode;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t row_stride = 0;
  *frame = DecodedFrame();

  if (OH_ImageSourceNative_CreateFromData(const_cast<uint8_t *>(jpeg), jpeg_bytes, &source) != IMAGE_SUCCESS ||
      source == nullptr) {
    goto done;
  }
  if (OH_DecodingOptions_Create(&options) != IMAGE_SUCCESS || options == nullptr) {
    goto done;
  }
  OH_DecodingOptions_SetPixelFormat(options, yuv ? PIXEL_FORMAT_NV12 : PIXEL_FORMAT_RGBA_8888);
  {
    const Image_ErrorCode decoded = OH_ImageSourceNative_CreatePixelmap(source, options, &frame->pixelmap);
    if (decoded != IMAGE_SUCCESS || frame->pixelmap == nullptr) {
      if (yuv) {
        OH_LOG_WARN(LOG_APP, "NV12 decode unavailable: CreatePixelmap=%{public}d", static_cast<int>(decoded));
        result = kResultYuvUnavailable;
      }
      goto done;
    }
  }
  if (OH_PixelmapImageInfo_Create(&info) != IMAGE_SUCCESS || info == nullptr ||
      OH_PixelmapNative_GetImageInfo(frame->pixelmap, info) != IMAGE_SUCCESS) {
    goto done;
  }
  OH_PixelmapImageInfo_GetWidth(info, &width);
  OH_PixelmapImageInfo_GetHeight(info, &height);
  OH_PixelmapImageInfo_GetRowStride(info, &row_stride);
  if (width == 0 || height == 0) {
    goto done;
  }
  frame->pixels.yuv = yuv;
  frame->pixels.width = static_cast<int32_t>(width);
  frame->pixels.height = static_cast<int32_t>(height);
  if (yuv) {
    // The plane layout is only known for a decoder output that lives in a
    // graphics buffer; the hardware decoder's does.
    YuvLayout layout;
    const Image_ErrorCode got = OH_PixelmapNative_GetNativeBuffer(frame->pixelmap, &frame->buffer);
    if (got != IMAGE_SUCCESS || !MapYuvLayout(frame->buffer, &layout) || layout.luma_stride < width ||
        layout.chroma_stride < width) {
      OH_LOG_WARN(LOG_APP, "NV12 decoder output unusable: GetNativeBuffer=%{public}d strides %{public}zu/%{public}zu",
                  static_cast<int>(got), layout.luma_stride, layout.chroma_stride);
      result = kResultYuvUnavailable;
      goto done;
    }
    frame->pixels.plane0 = layout.luma;
    frame->pixels.stride0 = layout.luma_stride;
    frame->pixels.plane1 = layout.chroma;
    frame->pixels.stride1 = layout.chroma_stride;
  } else {
    void *address = nullptr;
    if (row_stride < width * kBytesPerPixel ||
        OH_PixelmapNative_AccessPixels(frame->pixelmap, &address) != IMAGE_SUCCESS || address == nullptr) {
      goto done;
    }
    frame->accessed = true;
    frame->pixels.plane0 = static_cast<const uint8_t *>(address);
    frame->pixels.stride0 = row_stride;
  }
  result = 0;

done:
  if (info != nullptr) {
    OH_PixelmapImageInfo_Release(info);
  }
  if (options != nullptr) {
    OH_DecodingOptions_Release(options);
  }
  // The decoded pixels no longer depend on the compressed data.
  if (source != nullptr) {
    OH_ImageSourceNative_Release(source);
  }
  if (result != 0) {
    ReleaseFrame(frame);
  }
  return result;
}

// Opens the producer side of the surface with this id. Returns a window id,
// or 0 when the surface could not be opened.
inline uint64_t CreatePreviewWindow(uint64_t surface_id) {
  OHNativeWindow *window = nullptr;
  const int32_t ret = OH_NativeWindow_CreateNativeWindowFromSurfaceId(surface_id, &window);
  if (ret != 0 || window == nullptr) {
    return 0;
  }
  auto preview = std::make_shared<PreviewWindow>();
  preview->window = window;
  std::lock_guard<std::mutex> lock(g_windows_mutex);
  const uint64_t id = g_next_window_id++;
  g_windows[id] = preview;
  return id;
}

inline void DestroyPreviewWindow(uint64_t id) {
  std::shared_ptr<PreviewWindow> preview;
  {
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    auto found = g_windows.find(id);
    if (found == g_windows.end()) {
      return;
    }
    preview = found->second;
    g_windows.erase(found);
  }
  // Waits for a frame that is being written; later ones see a closed window.
  std::lock_guard<std::mutex> lock(preview->mutex);
  UnmapAll(preview.get());
  if (preview->window != nullptr) {
    OH_NativeWindow_DestroyNativeWindow(preview->window);
    preview->window = nullptr;
  }
}

}  // namespace flutter_ffi_uvc

#endif  // FLUTTER_FFI_UVC_SURFACE_H_
