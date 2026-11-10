/**
 * USB Audio Class capture probe over the raw configuration descriptor.
 *
 * Mirrors the Android side (`src/uac_audio.c`, `uac_audio_probe_all`): a UVC
 * camera with a microphone exposes it as a separate AudioStreaming interface
 * (class 1 / subclass 2) next to the video interfaces. The first alternate
 * setting carrying a non-zero isochronous IN endpoint is the capture
 * candidate. Its PCM format comes from the UAC1 FORMAT_TYPE I descriptor;
 * UAC2 devices do not state the rate there, so it is estimated from the
 * packet size exactly as the Android probe does. Pure parsing, no USB I/O.
 */

const UAC_CLASS_AUDIO = 1;
const UAC_SUBCLASS_STREAMING = 2;
const DT_INTERFACE = 4;
const DT_ENDPOINT = 5;
const DT_CS_INTERFACE = 0x24;
const AS_GENERAL = 0x01;
const AS_FORMAT_TYPE = 0x02;
const FORMAT_TYPE_I = 1;

const STANDARD_RATES: number[] = [
  8000, 11025, 12000, 16000, 22050, 24000, 32000,
  44100, 48000, 64000, 88200, 96000, 176400, 192000,
];

export class UacCaptureInfo {
  interfaceNumber: number = 0;
  altSetting: number = 0;
  endpointAddress: number = 0;
  endpointInterval: number = 0;
  packetSize: number = 0;
  channels: number = 0;
  bitsPerSample: number = 0;
  sampleRate: number = 0;
  /** bTerminalLink from AS_GENERAL; 0 when absent. */
  terminalLink: number = 0;
}

function u16(raw: Uint8Array, offset: number): number {
  return raw[offset] | (raw[offset + 1] << 8);
}

function u24(raw: Uint8Array, offset: number): number {
  return raw[offset] | (raw[offset + 1] << 8) | (raw[offset + 2] << 16);
}

/** Snaps a rate estimate to a standard rate within ~3 %, or 0. */
function snapSampleRate(estimate: number): number {
  for (let i = 0; i < STANDARD_RATES.length; i++) {
    const rate = STANDARD_RATES[i];
    if (Math.abs(estimate - rate) <= Math.floor(rate / 32)) {
      return rate;
    }
  }
  return 0;
}

/** High-speed isochronous packets per second for an endpoint bInterval. */
function packetsPerSecond(interval: number): number {
  let exponent = interval > 0 ? interval - 1 : 0;
  if (exponent > 4) {
    exponent = 4;
  }
  return 8000 >> exponent;
}

/**
 * Fills in whatever the descriptors left out: bits default to 16; channels
 * and rate are derived from the packet size when missing (a synchronous IN
 * endpoint carries rate / packetsPerSecond frames per packet).
 */
function estimateFormat(info: UacCaptureInfo): void {
  if (info.bitsPerSample <= 0) {
    info.bitsPerSample = 16;
  }
  if (info.channels > 0 && info.sampleRate > 0) {
    return;
  }
  const bytesPerSample = Math.floor((info.bitsPerSample + 7) / 8);
  const perSecond = packetsPerSecond(info.endpointInterval);
  const candidates: number[] = info.channels > 0 ? [info.channels] : [2, 1];
  for (let i = 0; i < candidates.length; i++) {
    const frameBytes = candidates[i] * bytesPerSample;
    if (frameBytes <= 0 || info.packetSize % frameBytes !== 0) {
      continue;
    }
    const snapped = snapSampleRate((info.packetSize / frameBytes) * perSecond);
    if (snapped > 0) {
      info.channels = candidates[i];
      info.sampleRate = snapped;
      return;
    }
  }
  if (info.channels <= 0) {
    info.channels = 2;
  }
  if (info.sampleRate <= 0) {
    info.sampleRate = 48000;
  }
}

/**
 * Every AudioStreaming alternate setting with a usable isochronous IN
 * endpoint, in descriptor order. Empty when the device has no microphone.
 */
export function listUacCaptureCandidates(raw: Uint8Array): UacCaptureInfo[] {
  const found: UacCaptureInfo[] = [];
  let offset = 0;
  let inStreaming = false;
  let current: UacCaptureInfo | null = null;
  // Format fields of the alt setting being walked, applied to its endpoint.
  let channels = 0;
  let bits = 0;
  let rate = 0;
  let terminalLink = 0;
  while (offset + 2 <= raw.length) {
    const length = raw[offset];
    const type = raw[offset + 1];
    if (length < 2 || offset + length > raw.length) {
      break;
    }
    if (type === DT_INTERFACE && length >= 9) {
      inStreaming = raw[offset + 5] === UAC_CLASS_AUDIO && raw[offset + 6] === UAC_SUBCLASS_STREAMING;
      current = null;
      channels = 0;
      bits = 0;
      rate = 0;
      terminalLink = 0;
      if (inStreaming) {
        current = new UacCaptureInfo();
        current.interfaceNumber = raw[offset + 2];
        current.altSetting = raw[offset + 3];
      }
    } else if (type === DT_CS_INTERFACE && inStreaming && raw[offset + 2] === AS_GENERAL && length >= 4) {
      terminalLink = raw[offset + 3];
    } else if (type === DT_CS_INTERFACE && inStreaming && raw[offset + 2] === AS_FORMAT_TYPE && length >= 6 &&
      raw[offset + 3] === FORMAT_TYPE_I) {
      if (length >= 11) {
        // UAC1 Type I: bNrChannels(4) bSubframeSize(5) bBitResolution(6) bSamFreqType(7) tSamFreq(8..).
        channels = raw[offset + 4];
        bits = raw[offset + 6];
        rate = u24(raw, offset + 8);
      } else {
        // UAC2 Type I: bSubslotSize(4) bBitResolution(5); rate lives in the clock source.
        bits = raw[offset + 5];
      }
    } else if (type === DT_ENDPOINT && inStreaming && current !== null && length >= 7) {
      const address = raw[offset + 2];
      const attributes = raw[offset + 3];
      const packetSize = u16(raw, offset + 4) & 0x7ff;
      if ((attributes & 0x03) === 0x01 && (address & 0x80) !== 0 && packetSize > 0) {
        const info = new UacCaptureInfo();
        info.interfaceNumber = current.interfaceNumber;
        info.altSetting = current.altSetting;
        info.endpointAddress = address;
        info.endpointInterval = raw[offset + 6];
        info.packetSize = packetSize;
        info.channels = channels;
        info.bitsPerSample = bits;
        info.sampleRate = rate;
        info.terminalLink = terminalLink;
        estimateFormat(info);
        found.push(info);
      }
    }
    offset += length;
  }
  return found;
}

/** The capture interface Android would record from, or null without one. */
export function probeUacCapture(raw: Uint8Array): UacCaptureInfo | null {
  const all = listUacCaptureCandidates(raw);
  return all.length > 0 ? all[0] : null;
}

export function describeUacCapture(info: UacCaptureInfo): string {
  return `interface=${info.interfaceNumber} alt=${info.altSetting} ep=0x${info.endpointAddress.toString(16)} ` +
    `packet=${info.packetSize} interval=${info.endpointInterval} rate=${info.sampleRate} ` +
    `ch=${info.channels} bits=${info.bitsPerSample} terminal_link=${info.terminalLink}`;
}
