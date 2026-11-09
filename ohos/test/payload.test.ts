import { test } from 'node:test';
import assert from 'node:assert/strict';
import { UvcFrameCounter, looksLikeUvcHeader, IsoLayoutProbe } from '../uvc/UvcPayload.ts';

const EOH = 0x80;
const ERR = 0x40;
const EOF = 0x02;

/** A payload with a 2-byte header followed by `dataLen` bytes of data. */
function payload(info: number, dataLen: number): Uint8Array {
  const buf = new Uint8Array(2 + dataLen);
  buf[0] = 2;
  buf[1] = info | EOH;
  return buf;
}

function push(counter: UvcFrameCounter, buf: Uint8Array): void {
  counter.push(buf, 0, buf.length);
}

test('counts a frame when the EOF bit arrives', () => {
  const counter = new UvcFrameCounter();
  push(counter, payload(0, 100));
  assert.equal(counter.frames, 0);
  push(counter, payload(EOF, 50));
  assert.equal(counter.frames, 1);
  assert.equal(counter.framesByEof, 1);
  assert.equal(counter.lastFrameBytes, 150);
});

test('counts a frame on FID toggle when the camera never sends EOF', () => {
  const counter = new UvcFrameCounter();
  push(counter, payload(0, 100));
  push(counter, payload(0, 100));
  push(counter, payload(1, 100));
  assert.equal(counter.frames, 1);
  assert.equal(counter.framesByFidToggle, 1);
  assert.equal(counter.lastFrameBytes, 200);
});

test('does not double count a frame that ends with EOF and then toggles FID', () => {
  const counter = new UvcFrameCounter();
  push(counter, payload(EOF, 100));
  push(counter, payload(1 | EOF, 100));
  assert.equal(counter.frames, 2);
  assert.equal(counter.framesByFidToggle, 0);
});

test('excludes header bytes from the payload byte total', () => {
  const counter = new UvcFrameCounter();
  const buf = new Uint8Array(12 + 30);
  buf[0] = 12;
  buf[1] = EOH | 0x0c;
  push(counter, buf);
  assert.equal(counter.payloadBytes, 30);
});

test('ignores empty packets', () => {
  const counter = new UvcFrameCounter();
  counter.push(new Uint8Array(8), 0, 0);
  assert.equal(counter.emptyPackets, 1);
  assert.equal(counter.payloadBytes, 0);
});

test('flags the frame as damaged when a payload has the error bit', () => {
  const counter = new UvcFrameCounter();
  push(counter, payload(0, 100));
  push(counter, payload(ERR, 100));
  push(counter, payload(EOF, 100));
  assert.equal(counter.errorBitPackets, 1);
  assert.equal(counter.frames, 1);
  assert.equal(counter.damagedFrames, 1);
  assert.equal(counter.lastFrameBytes, 200);
});

test('flags the frame as damaged when the header is longer than the packet', () => {
  const counter = new UvcFrameCounter();
  const bogus = new Uint8Array([12, EOH, 0, 0]);
  push(counter, bogus);
  push(counter, payload(EOF, 10));
  assert.equal(counter.bogusPackets, 1);
  assert.equal(counter.damagedFrames, 1);
});

test('flags the frame as damaged when the transport reports a lost packet', () => {
  const counter = new UvcFrameCounter();
  push(counter, payload(0, 100));
  counter.markPacketLost();
  push(counter, payload(EOF, 100));
  assert.equal(counter.damagedFrames, 1);
});

test('a clean frame after a damaged one is not counted as damaged', () => {
  const counter = new UvcFrameCounter();
  push(counter, payload(ERR, 100));
  push(counter, payload(EOF, 100));
  push(counter, payload(1 | EOF, 100));
  assert.equal(counter.frames, 2);
  assert.equal(counter.damagedFrames, 1);
});

test('tracks the smallest and largest frame sizes', () => {
  const counter = new UvcFrameCounter();
  push(counter, payload(EOF, 300));
  push(counter, payload(1 | EOF, 100));
  push(counter, payload(EOF, 200));
  assert.equal(counter.minFrameBytes, 100);
  assert.equal(counter.maxFrameBytes, 300);
});

test('reads a payload at an offset inside a larger transfer buffer', () => {
  const counter = new UvcFrameCounter();
  const buf = new Uint8Array(64);
  buf[32] = 2;
  buf[33] = EOH | EOF;
  counter.push(buf, 32, 10);
  assert.equal(counter.frames, 1);
  assert.equal(counter.lastFrameBytes, 8);
});

test('recognises a plausible UVC payload header', () => {
  assert.equal(looksLikeUvcHeader(new Uint8Array([2, EOH]), 0, 2), true);
  assert.equal(looksLikeUvcHeader(new Uint8Array([12, EOH | 0x0c, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]), 0, 12), true);
});

test('rejects headers that are too short, too long or lack the end-of-header bit', () => {
  assert.equal(looksLikeUvcHeader(new Uint8Array([1, EOH]), 0, 2), false);
  assert.equal(looksLikeUvcHeader(new Uint8Array([12, EOH, 0, 0]), 0, 4), false);
  assert.equal(looksLikeUvcHeader(new Uint8Array([2, 0x00]), 0, 2), false);
  assert.equal(looksLikeUvcHeader(new Uint8Array([2, EOH]), 0, 1), false);
});

test('accepts a header without the end-of-header bit when its length matches the PTS/SCR flags', () => {
  // Seen on a real camera: 12-byte header, info 0x0d (FID | PTS | SCR), no EOH.
  assert.equal(looksLikeUvcHeader(new Uint8Array([12, 0x0d, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]), 0, 12), true);
  assert.equal(looksLikeUvcHeader(new Uint8Array([6, 0x04, 0, 0, 0, 0]), 0, 6), true);
});

test('rejects a header without the end-of-header bit whose length contradicts its flags', () => {
  assert.equal(looksLikeUvcHeader(new Uint8Array([12, 0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]), 0, 12), false);
  assert.equal(looksLikeUvcHeader(new Uint8Array([0x11, 0x11, 0x11, 0x11]), 0, 100), false);
});

/** Lays packets out at fixed `stride` offsets, the libusb convention. */
function strideBuffer(stride: number, lengths: number[]): Uint8Array {
  const buf = new Uint8Array(stride * lengths.length);
  buf.fill(0x11);
  lengths.forEach((len, i) => {
    if (len > 0) {
      buf[i * stride] = 2;
      buf[i * stride + 1] = EOH;
    }
  });
  return buf;
}

/** Lays packets out back to back with no gaps. */
function packedBuffer(stride: number, lengths: number[]): Uint8Array {
  const buf = new Uint8Array(stride * lengths.length);
  buf.fill(0x11);
  let offset = 0;
  for (const len of lengths) {
    if (len > 0) {
      buf[offset] = 2;
      buf[offset + 1] = EOH;
    }
    offset += len;
  }
  return buf;
}

test('scores a stride-laid-out transfer as stride', () => {
  const probe = new IsoLayoutProbe();
  const lengths = [100, 60, 0, 100];
  probe.observe(strideBuffer(128, lengths), 128, lengths);
  assert.equal(probe.strideValid, 3);
  assert.equal(probe.packedValid, 1);
  assert.equal(probe.checked, 3);
  assert.equal(probe.verdict(), 'stride');
});

test('scores a back-to-back transfer as packed', () => {
  const probe = new IsoLayoutProbe();
  const lengths = [100, 60, 0, 100];
  probe.observe(packedBuffer(128, lengths), 128, lengths);
  assert.equal(probe.packedValid, 3);
  assert.equal(probe.strideValid, 1);
  assert.equal(probe.verdict(), 'packed');
});

test('gives no verdict when neither layout looks like UVC headers', () => {
  const probe = new IsoLayoutProbe();
  const buf = new Uint8Array(512);
  probe.observe(buf, 128, [100, 100, 100, 100]);
  assert.equal(probe.verdict(), 'unknown');
});

test('reports the layouts as indistinguishable when every packet is full', () => {
  const probe = new IsoLayoutProbe();
  const lengths = [128, 128];
  probe.observe(strideBuffer(128, lengths), 128, lengths);
  assert.equal(probe.strideValid, 2);
  assert.equal(probe.packedValid, 2);
  assert.equal(probe.verdict(), 'indistinguishable');
});

test('gives no verdict before any non-empty packet was seen', () => {
  const probe = new IsoLayoutProbe();
  probe.observe(new Uint8Array(256), 128, [0, 0]);
  assert.equal(probe.checked, 0);
  assert.equal(probe.verdict(), 'unknown');
});
