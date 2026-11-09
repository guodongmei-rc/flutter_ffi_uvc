// Host tests for the native frame assembler. Run with test/run_native_tests.sh.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../../src/main/cpp/uvc_frame_assembler.h"

using flutter_ffi_uvc::UvcFrameAssembler;

namespace {

constexpr uint8_t kEoh = 0x80;
constexpr uint8_t kErr = 0x40;
constexpr uint8_t kEof = 0x02;
constexpr uint8_t kFid = 0x01;

using Bytes = std::vector<uint8_t>;
using Frames = std::vector<Bytes>;

int g_failures = 0;
std::string g_test;

void Expect(bool ok, const char *what, int line) {
  if (!ok) {
    g_failures++;
    std::printf("FAIL %s (line %d): %s\n", g_test.c_str(), line, what);
  }
}
#define EXPECT(condition) Expect((condition), #condition, __LINE__)

// A payload with a 2-byte header followed by `data`.
Bytes Payload(uint8_t info, const Bytes &data) {
  Bytes out = {2, static_cast<uint8_t>(info | kEoh)};
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

struct Harness {
  explicit Harness(size_t max_frame_bytes = 1024) : assembler(max_frame_bytes) {}

  void Push(const Bytes &payload) {
    assembler.Push(payload.data(), payload.size(),
                   [this](const uint8_t *data, size_t length) { frames.emplace_back(data, data + length); });
  }

  UvcFrameAssembler assembler;
  Frames frames;
};

void Test(const char *name, void (*body)()) {
  g_test = name;
  body();
}

}  // namespace

int main() {
  Test("delivers the payload data of a frame that ends with EOF", [] {
    Harness h;
    h.Push(Payload(0, {1, 2, 3}));
    h.Push(Payload(kEof, {4, 5}));
    EXPECT(h.frames == Frames({{1, 2, 3, 4, 5}}));
    EXPECT(h.assembler.stats().frames == 1);
    EXPECT(h.assembler.stats().ends_by_eof == 1);
    EXPECT(h.assembler.stats().last_frame_bytes == 5);
  });

  Test("delivers a frame on FID toggle when the camera never sends EOF", [] {
    Harness h;
    h.Push(Payload(0, {1, 2}));
    h.Push(Payload(kFid, {3}));
    h.Push(Payload(0, {4}));
    EXPECT(h.frames == Frames({{1, 2}, {3}}));
    EXPECT(h.assembler.stats().ends_by_fid_toggle == 2);
  });

  Test("does not deliver an empty frame when FID toggles right after EOF", [] {
    Harness h;
    h.Push(Payload(kEof, {1}));
    h.Push(Payload(kFid | kEof, {2}));
    EXPECT(h.frames == Frames({{1}, {2}}));
    EXPECT(h.assembler.stats().ends_by_fid_toggle == 0);
  });

  Test("ignores empty packets and header-only payloads", [] {
    Harness h;
    h.assembler.Push(nullptr, 0, [](const uint8_t *, size_t) {});
    h.Push(Payload(0, {}));
    h.Push(Payload(kEof, {1}));
    EXPECT(h.frames == Frames({{1}}));
  });

  Test("drops a frame during which the transport lost a packet", [] {
    Harness h;
    h.Push(Payload(0, {1}));
    h.assembler.MarkPacketLost();
    h.Push(Payload(kEof, {2}));
    h.Push(Payload(kFid | kEof, {3}));
    EXPECT(h.frames == Frames({{3}}));
    EXPECT(h.assembler.stats().damaged_frames == 1);
    EXPECT(h.assembler.stats().lost_packets == 1);
  });

  Test("drops a frame carrying a payload with the error bit", [] {
    Harness h;
    h.Push(Payload(kErr, {1}));
    h.Push(Payload(kEof, {2}));
    h.Push(Payload(kFid | kEof, {3}));
    EXPECT(h.frames == Frames({{3}}));
    EXPECT(h.assembler.stats().damaged_frames == 1);
    EXPECT(h.assembler.stats().error_bit_packets == 1);
  });

  Test("drops a frame carrying a payload whose header is longer than the packet", [] {
    Harness h;
    h.Push({12, kEoh, 0, 0});
    h.Push(Payload(kEof, {2}));
    h.Push(Payload(kFid | kEof, {3}));
    EXPECT(h.frames == Frames({{3}}));
    EXPECT(h.assembler.stats().bogus_packets == 1);
  });

  Test("drops a frame that grows past the size limit and recovers on the next one", [] {
    Harness h(4);
    h.Push(Payload(0, {1, 2, 3}));
    h.Push(Payload(kEof, {4, 5}));
    h.Push(Payload(kFid | kEof, {6}));
    EXPECT(h.frames == Frames({{6}}));
    EXPECT(h.assembler.stats().oversized_frames == 1);
  });

  Test("reset discards a partly assembled frame", [] {
    Harness h;
    h.Push(Payload(0, {1}));
    h.assembler.Reset();
    h.Push(Payload(kEof, {2}));
    EXPECT(h.frames == Frames({{2}}));
  });

  if (g_failures != 0) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("native assembler tests passed\n");
  return 0;
}
