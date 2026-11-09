import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  parseUvcDescriptors, listModes, selectAltSetting, selectAltSettingWithin, defaultModeIndex,
} from '../uvc/UvcDescriptor.ts';
import { runcamLikeDescriptor, interfaceDescriptor, mjpegFormat, mjpegFrame } from './fixture.ts';

test('reads bcdUVC and the control interface number from the VC header', () => {
  const info = parseUvcDescriptors(runcamLikeDescriptor(0x0110));
  assert.equal(info.bcdUVC, 0x0110);
  assert.equal(info.controlInterfaceNumber, 0);
});

test('finds one streaming interface with the endpoint from the input header', () => {
  const info = parseUvcDescriptors(runcamLikeDescriptor());
  assert.equal(info.streams.length, 1);
  assert.equal(info.streams[0].interfaceNumber, 1);
  assert.equal(info.streams[0].endpointAddress, 0x81);
});

test('parses MJPEG format with its discrete frame intervals', () => {
  const info = parseUvcDescriptors(runcamLikeDescriptor());
  const mjpeg = info.streams[0].formats[0];
  assert.equal(mjpeg.kind, 'MJPEG');
  assert.equal(mjpeg.formatIndex, 1);
  assert.equal(mjpeg.frames.length, 2);
  assert.equal(mjpeg.frames[1].frameIndex, 2);
  assert.equal(mjpeg.frames[1].width, 1280);
  assert.equal(mjpeg.frames[1].height, 720);
  assert.deepEqual(mjpeg.frames[1].intervals, [333333, 166666]);
  assert.equal(mjpeg.frames[1].maxVideoFrameBufferSize, 1280 * 720 * 2);
});

test('parses a frame-based format and reports its fourcc from the GUID', () => {
  const info = parseUvcDescriptors(runcamLikeDescriptor());
  const h264 = info.streams[0].formats[1];
  assert.equal(h264.kind, 'FRAME_BASED');
  assert.equal(h264.fourcc, 'H264');
  assert.equal(h264.formatIndex, 2);
  assert.equal(h264.frames[0].width, 1280);
  assert.deepEqual(h264.frames[0].intervals, [333333]);
});

test('computes bytes per microframe including the high-bandwidth multiplier', () => {
  const alts = parseUvcDescriptors(runcamLikeDescriptor()).streams[0].altSettings;
  assert.deepEqual(alts.map((a) => a.alternateSetting), [1, 2, 3, 4]);
  assert.deepEqual(alts.map((a) => a.wMaxPacketSize), [800, 3040, 4992, 5120]);
  // 3040 = 0x0BE0 -> 992 bytes x 2 transactions; 4992 = 0x1380 -> 896 x 3;
  // 5120 = 0x1400 -> 1024 x 3 (same arithmetic as libuvc stream.c:1336).
  assert.deepEqual(alts.map((a) => a.bytesPerPacket), [800, 1984, 2688, 3072]);
  assert.ok(alts.every((a) => a.isochronous));
});

test('selects the smallest alt setting that fits the negotiated payload size', () => {
  const alts = parseUvcDescriptors(runcamLikeDescriptor()).streams[0].altSettings;
  assert.equal(selectAltSetting(alts, 800)!.alternateSetting, 1);
  assert.equal(selectAltSetting(alts, 801)!.alternateSetting, 2);
  assert.equal(selectAltSetting(alts, 3072)!.alternateSetting, 4);
});

test('returns null when no alt setting can carry the payload size', () => {
  const alts = parseUvcDescriptors(runcamLikeDescriptor()).streams[0].altSettings;
  assert.equal(selectAltSetting(alts, 3073), null);
});

test('selects the largest alt setting that fits a per-packet capacity', () => {
  const alts = parseUvcDescriptors(runcamLikeDescriptor()).streams[0].altSettings;
  assert.equal(selectAltSettingWithin(alts, 800)!.alternateSetting, 1);
  assert.equal(selectAltSettingWithin(alts, 2000)!.alternateSetting, 2);
  assert.equal(selectAltSettingWithin(alts, 100000)!.alternateSetting, 4);
});

test('returns null when every alt setting exceeds the per-packet capacity', () => {
  const alts = parseUvcDescriptors(runcamLikeDescriptor()).streams[0].altSettings;
  assert.equal(selectAltSettingWithin(alts, 799), null);
});

test('lists one mode per format, frame and interval', () => {
  const modes = listModes(parseUvcDescriptors(runcamLikeDescriptor()));
  assert.equal(modes.length, 4);
  assert.equal(modes[1].label, 'MJPEG 1280x720 @30.0fps (fmt 1 / frame 2)');
  assert.equal(modes[1].interval, 333333);
  assert.equal(modes[2].interval, 166666);
  assert.equal(modes[3].label, 'H264 1280x720 @30.0fps (fmt 2 / frame 1)');
});

test('defaults to the first mode matching the requested resolution', () => {
  const modes = listModes(parseUvcDescriptors(runcamLikeDescriptor()));
  assert.equal(defaultModeIndex(modes, 1280, 720), 1);
  assert.equal(defaultModeIndex(modes, 640, 480), 0);
});

test('stops with a warning on a zero-length descriptor instead of looping', () => {
  const good = runcamLikeDescriptor();
  const raw = new Uint8Array(good.length + 4);
  raw.set(good);
  const info = parseUvcDescriptors(raw);
  assert.equal(info.streams.length, 1);
  assert.equal(info.warnings.length, 1);
  assert.match(info.warnings[0], /bLength/);
});

test('warns about a class-specific descriptor too short for its fields', () => {
  const truncatedFrame = mjpegFrame(1, 640, 480, [333333]).slice(0, 12);
  truncatedFrame[0] = truncatedFrame.length;
  const raw = new Uint8Array([
    ...interfaceDescriptor(1, 0, 0, 0x02),
    ...mjpegFormat(1, 1),
    ...truncatedFrame,
  ]);
  const info = parseUvcDescriptors(raw);
  assert.equal(info.warnings.length, 1);
  assert.match(info.warnings[0], /too short/);
  assert.equal(info.streams[0].formats[0].frames.length, 0);
});

test('parses a bare configuration without a leading device descriptor', () => {
  const full = runcamLikeDescriptor();
  const info = parseUvcDescriptors(full.subarray(18));
  assert.equal(info.streams.length, 1);
  assert.equal(info.streams[0].formats.length, 2);
});
