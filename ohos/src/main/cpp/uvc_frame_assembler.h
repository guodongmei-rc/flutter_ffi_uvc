// Assembles complete video frames from UVC payloads.
//
// Plain C++ with no platform dependencies, so it is also compiled and tested
// on the development machine (test/native). The header handling follows
// _uvc_process_payload() in the bundled libuvc (src/libuvc/src/stream.c): a
// frame ends at the EOF bit, or at a FID toggle for cameras that never set
// EOF, and a payload flagged ERR spoils the frame it belongs to.
//
// This runs in native code because it touches every received packet: doing
// the same copies from ArkTS kept the thread that services the USB transfers
// busy for more than half of every second on a 1080p stream.

#ifndef FLUTTER_FFI_UVC_FRAME_ASSEMBLER_H_
#define FLUTTER_FFI_UVC_FRAME_ASSEMBLER_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace flutter_ffi_uvc {

struct UvcFrameAssemblerStats {
  // Frames handed to the frame callback.
  uint64_t frames = 0;
  // Frame ends seen as an EOF bit, and as a FID toggle without one (an EOF
  // packet went missing, or the camera never sets the bit).
  uint64_t ends_by_eof = 0;
  uint64_t ends_by_fid_toggle = 0;
  // Frames dropped because a payload was lost, malformed or flagged ERR.
  uint64_t damaged_frames = 0;
  // Frames dropped because they exceeded the size limit.
  uint64_t oversized_frames = 0;
  uint64_t error_bit_packets = 0;
  uint64_t bogus_packets = 0;
  uint64_t lost_packets = 0;
  uint64_t last_frame_bytes = 0;
};

// Collects payload data into frames and hands each undamaged frame to the
// callback. Frames that lost a packet, carried an error or malformed header,
// or outgrew the size limit are counted and dropped rather than delivered
// incomplete.
class UvcFrameAssembler {
 public:
  // Receives a completed frame; the bytes are only valid during the call.
  using FrameCallback = std::function<void(const uint8_t *data, size_t length)>;

  explicit UvcFrameAssembler(size_t max_frame_bytes) : max_frame_bytes_(max_frame_bytes) {
    work_.reserve(max_frame_bytes < kInitialCapacity ? max_frame_bytes : kInitialCapacity);
  }

  const UvcFrameAssemblerStats &stats() const { return stats_; }

  // Feeds one payload of `length` bytes.
  void Push(const uint8_t *data, size_t length, const FrameCallback &on_frame) {
    if (length == 0) {
      return;
    }
    const size_t header_length = data[0];
    if (length < 2 || header_length < 2 || header_length > length) {
      stats_.bogus_packets++;
      damaged_ = true;
      return;
    }
    const uint8_t info = data[1];
    if ((info & kHeaderErr) != 0) {
      stats_.error_bit_packets++;
      damaged_ = true;
      return;
    }
    const uint8_t fid = info & kHeaderFid;
    if (fid != fid_ && HasPendingFrame()) {
      stats_.ends_by_fid_toggle++;
      FinishFrame(on_frame);
    }
    fid_ = fid;

    const size_t data_length = length - header_length;
    if (data_length > 0) {
      Append(data + header_length, data_length);
    }
    if ((info & kHeaderEof) != 0 && HasPendingFrame()) {
      stats_.ends_by_eof++;
      FinishFrame(on_frame);
    }
  }

  // Records a packet the transport reported as failed.
  void MarkPacketLost() {
    stats_.lost_packets++;
    damaged_ = true;
  }

  // Discards the frame being assembled, e.g. when the stream restarts.
  void Reset() {
    work_.clear();
    fid_ = 0;
    damaged_ = false;
    oversized_ = false;
  }

 private:
  static constexpr uint8_t kHeaderFid = 0x01;
  static constexpr uint8_t kHeaderEof = 0x02;
  static constexpr uint8_t kHeaderErr = 0x40;
  static constexpr size_t kInitialCapacity = 256 * 1024;

  bool HasPendingFrame() const { return !work_.empty() || damaged_ || oversized_; }

  void Append(const uint8_t *data, size_t length) {
    if (oversized_) {
      return;
    }
    if (work_.size() + length > max_frame_bytes_) {
      oversized_ = true;
      return;
    }
    work_.insert(work_.end(), data, data + length);
  }

  void FinishFrame(const FrameCallback &on_frame) {
    const bool damaged = damaged_;
    const bool oversized = oversized_;
    damaged_ = false;
    oversized_ = false;
    if (oversized) {
      stats_.oversized_frames++;
    } else if (damaged) {
      stats_.damaged_frames++;
    } else if (!work_.empty()) {
      stats_.frames++;
      stats_.last_frame_bytes = work_.size();
      on_frame(work_.data(), work_.size());
    }
    work_.clear();
  }

  size_t max_frame_bytes_;
  std::vector<uint8_t> work_;
  UvcFrameAssemblerStats stats_;
  uint8_t fid_ = 0;
  bool damaged_ = false;
  bool oversized_ = false;
};

}  // namespace flutter_ffi_uvc

#endif  // FLUTTER_FFI_UVC_FRAME_ASSEMBLER_H_
