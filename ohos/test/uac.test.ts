import { test } from 'node:test';
import assert from 'node:assert/strict';
import { probeUacCapture, listUacCaptureCandidates } from '../uvc/UacDescriptor.ts';
import { runcamLikeDescriptor, le16, endpointDescriptor } from './fixture.ts';

function withLength(body: number[]): number[] {
  return [body.length + 1, ...body];
}

function audioInterface(num: number, alt: number, numEndpoints: number, subClass: number): number[] {
  return withLength([0x04, num, alt, numEndpoints, 0x01, subClass, 0x00, 0]);
}

/** UAC1 AS_GENERAL: bTerminalLink, bDelay, wFormatTag (1 = PCM). */
function asGeneral(terminalLink: number): number[] {
  return withLength([0x24, 0x01, terminalLink, 1, ...le16(1)]);
}

/** UAC1 FORMAT_TYPE I with discrete sample rates. */
function formatTypeI(channels: number, bits: number, rates: number[]): number[] {
  const body = [0x24, 0x02, 0x01, channels, bits / 8, bits, rates.length];
  for (const rate of rates) {
    body.push(rate & 0xff, (rate >> 8) & 0xff, (rate >> 16) & 0xff);
  }
  return withLength(body);
}

/** Appends a UAC1 microphone (control + streaming interfaces) to the camera. */
function cameraWithMicrophone(channels: number, bits: number, rate: number, packetSize: number): Uint8Array {
  const video = runcamLikeDescriptor();
  const audio: number[] = [
    ...audioInterface(2, 0, 0, 0x01),
    ...withLength([0x24, 0x01, 0x00, 0x01, ...le16(9 + 1), 1, 3]), // AC header pointing at interface 3
    ...audioInterface(3, 0, 0, 0x02),
    ...audioInterface(3, 1, 1, 0x02),
    ...asGeneral(2),
    ...formatTypeI(channels, bits, [rate]),
    ...endpointDescriptor(0x84, 0x05, packetSize, 4),
  ];
  const out = new Uint8Array(video.length + audio.length);
  out.set(video, 0);
  out.set(audio, video.length);
  return out;
}

test('a camera with only video interfaces has no capture candidate', () => {
  assert.equal(probeUacCapture(runcamLikeDescriptor()), null);
  assert.deepEqual(listUacCaptureCandidates(runcamLikeDescriptor()), []);
});

test('reads the UAC1 Type I format of the microphone streaming interface', () => {
  const info = probeUacCapture(cameraWithMicrophone(1, 16, 48000, 96));
  assert.ok(info);
  assert.equal(info.interfaceNumber, 3);
  assert.equal(info.altSetting, 1);
  assert.equal(info.endpointAddress, 0x84);
  assert.equal(info.packetSize, 96);
  assert.equal(info.channels, 1);
  assert.equal(info.bitsPerSample, 16);
  assert.equal(info.sampleRate, 48000);
  assert.equal(info.terminalLink, 2);
});

test('zero-bandwidth alt 0 is skipped and the stereo format is kept', () => {
  const info = probeUacCapture(cameraWithMicrophone(2, 16, 44100, 180));
  assert.ok(info);
  assert.equal(info.altSetting, 1);
  assert.equal(info.channels, 2);
  assert.equal(info.sampleRate, 44100);
});

test('estimates the rate from the packet size when the descriptor omits it', () => {
  // Same layout but with the Type I descriptor truncated to the UAC2 shape
  // (no rate table): 16-bit stereo at 48 kHz over 1 ms packets is 192 bytes.
  const video = runcamLikeDescriptor();
  const audio: number[] = [
    ...audioInterface(3, 1, 1, 0x02),
    ...withLength([0x24, 0x02, 0x01, 2, 16]),
    ...endpointDescriptor(0x84, 0x05, 192, 4),
  ];
  const raw = new Uint8Array(video.length + audio.length);
  raw.set(video, 0);
  raw.set(audio, video.length);
  const info = probeUacCapture(raw);
  assert.ok(info);
  assert.equal(info.bitsPerSample, 16);
  assert.equal(info.channels, 2);
  assert.equal(info.sampleRate, 48000);
});
