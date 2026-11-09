import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  UvcStreamCtrl, streamCtrlLength, encodeStreamCtrl, decodeStreamCtrl,
} from '../uvc/UvcStreamControl.ts';

test('uses 26 bytes before UVC 1.1, 34 for 1.1 and 48 from 1.5', () => {
  assert.equal(streamCtrlLength(0x0100), 26);
  assert.equal(streamCtrlLength(0x010a), 26);
  assert.equal(streamCtrlLength(0x0110), 34);
  assert.equal(streamCtrlLength(0x0150), 48);
});

test('falls back to 26 bytes when bcdUVC is unknown', () => {
  assert.equal(streamCtrlLength(0), 26);
});

test('encodes the probe block little-endian at the libuvc offsets', () => {
  const ctrl = new UvcStreamCtrl();
  ctrl.bmHint = 1;
  ctrl.bFormatIndex = 2;
  ctrl.bFrameIndex = 3;
  ctrl.dwFrameInterval = 333333;
  ctrl.dwMaxVideoFrameSize = 0x01020304;
  ctrl.dwMaxPayloadTransferSize = 3072;
  const buf = encodeStreamCtrl(ctrl, 26);
  assert.equal(buf.length, 26);
  assert.deepEqual(Array.from(buf.subarray(0, 8)), [1, 0, 2, 3, 0x15, 0x16, 0x05, 0x00]);
  assert.deepEqual(Array.from(buf.subarray(18, 22)), [0x04, 0x03, 0x02, 0x01]);
  assert.deepEqual(Array.from(buf.subarray(22, 26)), [0x00, 0x0c, 0x00, 0x00]);
});

test('encodes the UVC 1.1 tail only in the 34-byte form', () => {
  const ctrl = new UvcStreamCtrl();
  ctrl.dwClockFrequency = 48000000;
  ctrl.bmFramingInfo = 3;
  ctrl.bPreferredVersion = 1;
  ctrl.bMinVersion = 1;
  ctrl.bMaxVersion = 1;
  const buf = encodeStreamCtrl(ctrl, 34);
  assert.equal(buf.length, 34);
  assert.deepEqual(Array.from(buf.subarray(26, 34)), [0x00, 0x6c, 0xdc, 0x02, 3, 1, 1, 1]);
});

test('leaves the UVC 1.5 extension zeroed in the 48-byte form', () => {
  const buf = encodeStreamCtrl(new UvcStreamCtrl(), 48);
  assert.equal(buf.length, 48);
  assert.ok(buf.subarray(34).every((b) => b === 0));
});

test('decodes what it encoded', () => {
  const ctrl = new UvcStreamCtrl();
  ctrl.bmHint = 1;
  ctrl.bFormatIndex = 1;
  ctrl.bFrameIndex = 2;
  ctrl.dwFrameInterval = 166666;
  ctrl.wKeyFrameRate = 30;
  ctrl.wPFrameRate = 4;
  ctrl.wCompQuality = 5000;
  ctrl.wCompWindowSize = 7;
  ctrl.wDelay = 33;
  ctrl.dwMaxVideoFrameSize = 1843200;
  ctrl.dwMaxPayloadTransferSize = 3072;
  ctrl.dwClockFrequency = 48000000;
  ctrl.bmFramingInfo = 3;
  ctrl.bPreferredVersion = 1;
  ctrl.bMinVersion = 1;
  ctrl.bMaxVersion = 2;
  assert.deepEqual(decodeStreamCtrl(encodeStreamCtrl(ctrl, 34), 34), ctrl);
});

test('decodes values above 2^31 as unsigned', () => {
  const buf = new Uint8Array(26);
  buf.set([0xff, 0xff, 0xff, 0xff], 18);
  assert.equal(decodeStreamCtrl(buf, 26).dwMaxVideoFrameSize, 4294967295);
});

test('decodes only the bytes the device actually returned', () => {
  const full = new UvcStreamCtrl();
  full.dwMaxPayloadTransferSize = 3072;
  full.dwClockFrequency = 48000000;
  const buf = encodeStreamCtrl(full, 34);
  const decoded = decodeStreamCtrl(buf, 26);
  assert.equal(decoded.dwMaxPayloadTransferSize, 3072);
  assert.equal(decoded.dwClockFrequency, 0);
});
