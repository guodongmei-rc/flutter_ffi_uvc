// Annex B NAL unit parsing for H.264 and H.265 camera streams, shared by the
// hardware-decoder path (uvc_h26x.h) and the host tests. No platform
// dependencies.

#ifndef FLUTTER_FFI_UVC_NAL_H_
#define FLUTTER_FFI_UVC_NAL_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace flutter_ffi_uvc {

// Stream codecs, as the ArkTS side passes them to streamStart(). 3..5 are
// uncompressed pictures (see uvc_raw.h).
enum class StreamCodec : int32_t { kMjpeg = 0, kH264 = 1, kH265 = 2, kYuy2 = 3, kUyvy = 4, kNv12 = 5 };

inline bool IsCompressedVideo(StreamCodec codec) {
  return codec == StreamCodec::kH264 || codec == StreamCodec::kH265;
}

inline bool IsRawVideo(StreamCodec codec) {
  return codec == StreamCodec::kYuy2 || codec == StreamCodec::kUyvy || codec == StreamCodec::kNv12;
}

// NAL parsing -------------------------------------------------------------------

struct NalUnit {
  // Start of the start code (3 or 4 bytes) and the total length including it.
  size_t offset = 0;
  size_t length = 0;
  // Offset of the NAL header byte.
  size_t header = 0;
};

// Splits an Annex B byte stream into NAL units (each with its start code).
inline std::vector<NalUnit> SplitNals(const uint8_t *data, size_t length) {
  std::vector<NalUnit> nals;
  size_t i = 0;
  while (i + 3 <= length) {
    if (data[i] == 0 && data[i + 1] == 0 && (data[i + 2] == 1 || (data[i + 2] == 0 && i + 4 <= length && data[i + 3] == 1))) {
      const size_t code = data[i + 2] == 1 ? 3 : 4;
      if (!nals.empty()) {
        nals.back().length = i - nals.back().offset;
      }
      NalUnit nal;
      nal.offset = i;
      nal.header = i + code;
      nals.push_back(nal);
      i += code;
    } else {
      i++;
    }
  }
  if (!nals.empty()) {
    nals.back().length = length - nals.back().offset;
  }
  return nals;
}

inline int NalType(StreamCodec codec, uint8_t header) {
  return codec == StreamCodec::kH265 ? (header >> 1) & 0x3f : header & 0x1f;
}

// VPS/SPS/PPS: what the decoder needs before the first picture.
inline bool IsParameterSet(StreamCodec codec, int type) {
  return codec == StreamCodec::kH265 ? (type >= 32 && type <= 34) : (type == 7 || type == 8);
}

// A picture the decoder can start from: IDR for H.264; BLA/IDR/CRA for H.265.
inline bool IsKeyframeNal(StreamCodec codec, int type) {
  return codec == StreamCodec::kH265 ? (type >= 16 && type <= 21) : type == 5;
}

// Slice data (as opposed to SEI, AUD, parameter sets, ...).
inline bool IsSliceNal(StreamCodec codec, int type) {
  return codec == StreamCodec::kH265 ? type <= 31 : (type >= 1 && type <= 5);
}

// Parameter sets collected from the stream, replaced whenever the camera
// resends them. Complete when every kind the codec needs has been seen.
class ParameterSets {
 public:
  explicit ParameterSets(StreamCodec codec) : codec_(codec) {}

  // Stores `nal` (with its start code) if it is a parameter set. Returns true
  // if it was one.
  bool Offer(int type, const uint8_t *nal, size_t length) {
    if (!IsParameterSet(codec_, type)) {
      return false;
    }
    std::vector<uint8_t> &slot = Slot(type);
    if (slot.size() != length || std::memcmp(slot.data(), nal, length) != 0) {
      slot.assign(nal, nal + length);
      generation_++;
    }
    return true;
  }

  bool Complete() const {
    if (codec_ == StreamCodec::kH265) {
      return !vps_.empty() && !sps_.empty() && !pps_.empty();
    }
    return !sps_.empty() && !pps_.empty();
  }

  // All parameter sets back to back, in decoding order.
  std::vector<uint8_t> Bytes() const {
    std::vector<uint8_t> out;
    out.insert(out.end(), vps_.begin(), vps_.end());
    out.insert(out.end(), sps_.begin(), sps_.end());
    out.insert(out.end(), pps_.begin(), pps_.end());
    return out;
  }

  // Bumps whenever a parameter set changes, so a decoder can tell its
  // configuration is stale.
  uint64_t generation() const { return generation_; }

 private:
  std::vector<uint8_t> &Slot(int type) {
    if (codec_ == StreamCodec::kH265) {
      return type == 32 ? vps_ : type == 33 ? sps_ : pps_;
    }
    return type == 7 ? sps_ : pps_;
  }

  StreamCodec codec_;
  std::vector<uint8_t> vps_;
  std::vector<uint8_t> sps_;
  std::vector<uint8_t> pps_;
  uint64_t generation_ = 0;
};

}  // namespace flutter_ffi_uvc

#endif  // FLUTTER_FFI_UVC_NAL_H_
