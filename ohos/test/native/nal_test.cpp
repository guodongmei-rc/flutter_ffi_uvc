// Host tests for the Annex B NAL parsing behind the H.264/H.265 preview path.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../../src/main/cpp/uvc_nal.h"

using flutter_ffi_uvc::IsKeyframeNal;
using flutter_ffi_uvc::IsParameterSet;
using flutter_ffi_uvc::IsSliceNal;
using flutter_ffi_uvc::NalType;
using flutter_ffi_uvc::NalUnit;
using flutter_ffi_uvc::ParameterSets;
using flutter_ffi_uvc::SplitNals;
using flutter_ffi_uvc::StreamCodec;

namespace {

using Bytes = std::vector<uint8_t>;

int g_failures = 0;
std::string g_test;

void Expect(bool ok, const char *what, int line) {
  if (!ok) {
    g_failures++;
    std::printf("FAIL %s (line %d): %s\n", g_test.c_str(), line, what);
  }
}
#define EXPECT(condition) Expect((condition), #condition, __LINE__)

template <typename Body>
void Test(const char *name, Body body) {
  g_test = name;
  body();
}

// An H.264 NAL with a 4-byte start code, header for `type` and `payload`.
Bytes H264Nal(int type, const Bytes &payload, bool short_code = false) {
  Bytes out = short_code ? Bytes{0, 0, 1} : Bytes{0, 0, 0, 1};
  out.push_back(static_cast<uint8_t>(0x60 | type));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

// An H.265 NAL: two header bytes, type in bits 1..6 of the first.
Bytes H265Nal(int type, const Bytes &payload) {
  Bytes out = {0, 0, 0, 1, static_cast<uint8_t>(type << 1), 0x01};
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

Bytes Concat(const std::vector<Bytes> &parts) {
  Bytes out;
  for (const Bytes &part : parts) {
    out.insert(out.end(), part.begin(), part.end());
  }
  return out;
}

}  // namespace

int main() {
  Test("splits an access unit into its NAL units with both start code lengths", [] {
    const Bytes au = Concat({H264Nal(7, {0xaa, 0xbb}), H264Nal(8, {0xcc}, true), H264Nal(5, {1, 2, 3, 4})});
    const std::vector<NalUnit> nals = SplitNals(au.data(), au.size());
    EXPECT(nals.size() == 3);
    EXPECT(nals[0].offset == 0 && nals[0].length == 7 && nals[0].header == 4);
    EXPECT(nals[1].offset == 7 && nals[1].length == 5 && nals[1].header == 10);
    EXPECT(nals[2].offset == 12 && nals[2].length == 9 && nals[2].header == 16);
    EXPECT(NalType(StreamCodec::kH264, au[nals[0].header]) == 7);
    EXPECT(NalType(StreamCodec::kH264, au[nals[2].header]) == 5);
  });

  Test("bytes before the first start code are not a NAL unit", [] {
    const Bytes garbage = {0x12, 0x34, 0x56};
    EXPECT(SplitNals(garbage.data(), garbage.size()).empty());
    const Bytes au = Concat({{0x12}, H264Nal(1, {9})});
    const std::vector<NalUnit> nals = SplitNals(au.data(), au.size());
    EXPECT(nals.size() == 1);
    EXPECT(nals[0].offset == 1);
  });

  Test("classifies H.264 NAL types", [] {
    EXPECT(IsParameterSet(StreamCodec::kH264, 7) && IsParameterSet(StreamCodec::kH264, 8));
    EXPECT(!IsParameterSet(StreamCodec::kH264, 5));
    EXPECT(IsKeyframeNal(StreamCodec::kH264, 5) && !IsKeyframeNal(StreamCodec::kH264, 1));
    EXPECT(IsSliceNal(StreamCodec::kH264, 1) && IsSliceNal(StreamCodec::kH264, 5));
    EXPECT(!IsSliceNal(StreamCodec::kH264, 6) && !IsSliceNal(StreamCodec::kH264, 9));
  });

  Test("classifies H.265 NAL types", [] {
    const Bytes vps = H265Nal(32, {});
    EXPECT(NalType(StreamCodec::kH265, vps[4]) == 32);
    EXPECT(IsParameterSet(StreamCodec::kH265, 32) && IsParameterSet(StreamCodec::kH265, 33) &&
           IsParameterSet(StreamCodec::kH265, 34));
    EXPECT(IsKeyframeNal(StreamCodec::kH265, 19) && IsKeyframeNal(StreamCodec::kH265, 21));
    EXPECT(!IsKeyframeNal(StreamCodec::kH265, 1));
    EXPECT(IsSliceNal(StreamCodec::kH265, 1) && !IsSliceNal(StreamCodec::kH265, 39));
  });

  Test("H.264 parameter sets complete with SPS and PPS and track changes", [] {
    ParameterSets params(StreamCodec::kH264);
    const Bytes sps = H264Nal(7, {1, 2});
    const Bytes pps = H264Nal(8, {3});
    EXPECT(!params.Complete());
    EXPECT(params.Offer(7, sps.data(), sps.size()));
    EXPECT(!params.Complete());
    EXPECT(params.Offer(8, pps.data(), pps.size()));
    EXPECT(params.Complete());
    EXPECT(params.Bytes() == Concat({sps, pps}));
    const uint64_t generation = params.generation();
    // The same sets again change nothing; a different SPS does.
    EXPECT(params.Offer(7, sps.data(), sps.size()));
    EXPECT(params.generation() == generation);
    const Bytes other = H264Nal(7, {9, 9});
    EXPECT(params.Offer(7, other.data(), other.size()));
    EXPECT(params.generation() == generation + 1);
    const Bytes idr = H264Nal(5, {0});
    EXPECT(!params.Offer(5, idr.data(), idr.size()));
  });

  Test("H.265 parameter sets also need the VPS", [] {
    ParameterSets params(StreamCodec::kH265);
    const Bytes sps = H265Nal(33, {1});
    const Bytes pps = H265Nal(34, {2});
    const Bytes vps = H265Nal(32, {3});
    params.Offer(33, sps.data(), sps.size());
    params.Offer(34, pps.data(), pps.size());
    EXPECT(!params.Complete());
    params.Offer(32, vps.data(), vps.size());
    EXPECT(params.Complete());
    EXPECT(params.Bytes() == Concat({vps, sps, pps}));
  });

  if (g_failures != 0) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("native nal tests passed\n");
  return 0;
}
