// UVC descriptor parsing for the raw bytes returned by usbManager.getRawDescriptor().
//
// Pure data code: no usbManager, no ArkUI. Field offsets follow the libuvc
// parsers bundled with this package (src/libuvc/src/device.c), which stay the
// reference for the Android path.

const DT_INTERFACE: number = 0x04;
const DT_ENDPOINT: number = 0x05;
const DT_CS_INTERFACE: number = 0x24;
const DT_SS_ENDPOINT_COMPANION: number = 0x30;

const CLASS_VIDEO: number = 0x0e;
const SUBCLASS_VIDEO_CONTROL: number = 0x01;
const SUBCLASS_VIDEO_STREAMING: number = 0x02;

const VC_HEADER: number = 0x01;
const VS_INPUT_HEADER: number = 0x01;
const VS_FORMAT_UNCOMPRESSED: number = 0x04;
const VS_FRAME_UNCOMPRESSED: number = 0x05;
const VS_FORMAT_MJPEG: number = 0x06;
const VS_FRAME_MJPEG: number = 0x07;
const VS_FORMAT_FRAME_BASED: number = 0x10;
const VS_FRAME_FRAME_BASED: number = 0x11;

export const UVC_KIND_MJPEG: string = 'MJPEG';
export const UVC_KIND_UNCOMPRESSED: string = 'UNCOMPRESSED';
export const UVC_KIND_FRAME_BASED: string = 'FRAME_BASED';

export class UvcFrameDesc {
  frameIndex: number = 0;
  width: number = 0;
  height: number = 0;
  defaultInterval: number = 0;
  /** Discrete intervals in 100 ns units; empty when the frame is continuous. */
  intervals: number[] = [];
  minInterval: number = 0;
  maxInterval: number = 0;
  intervalStep: number = 0;
  /** Zero for frame-based frames, which do not carry this field. */
  maxVideoFrameBufferSize: number = 0;
}

export class UvcFormatDesc {
  kind: string = '';
  formatIndex: number = 0;
  /** 'MJPG' for MJPEG, otherwise the first four GUID bytes when printable. */
  fourcc: string = '';
  frames: UvcFrameDesc[] = [];
}

export class UvcAltSetting {
  alternateSetting: number = 0;
  endpointAddress: number = 0;
  attributes: number = 0;
  /** Raw descriptor value, multiplier bits included. */
  wMaxPacketSize: number = 0;
  /** Bytes the endpoint can carry per (micro)frame. */
  bytesPerPacket: number = 0;
  isochronous: boolean = false;
}

export class UvcStreamInterface {
  interfaceNumber: number = 0;
  /** From the VS input header; zero if the header was not found. */
  endpointAddress: number = 0;
  formats: UvcFormatDesc[] = [];
  altSettings: UvcAltSetting[] = [];
}

export class UvcDeviceInfo {
  /** Zero when no VideoControl header was found. */
  bcdUVC: number = 0;
  controlInterfaceNumber: number = -1;
  streams: UvcStreamInterface[] = [];
  warnings: string[] = [];
}

export class UvcMode {
  streamInterfaceNumber: number = 0;
  kind: string = '';
  fourcc: string = '';
  formatIndex: number = 0;
  frameIndex: number = 0;
  width: number = 0;
  height: number = 0;
  interval: number = 0;
  maxVideoFrameBufferSize: number = 0;
  label: string = '';
}

function u16(raw: Uint8Array, offset: number): number {
  return raw[offset] | (raw[offset + 1] << 8);
}

function u32(raw: Uint8Array, offset: number): number {
  return (raw[offset] | (raw[offset + 1] << 8) | (raw[offset + 2] << 16) | (raw[offset + 3] << 24)) >>> 0;
}

function fourccFromGuid(raw: Uint8Array, offset: number): string {
  let out = '';
  for (let i = 0; i < 4; i++) {
    const c = raw[offset + i];
    if (c < 0x20 || c > 0x7e) {
      return '';
    }
    out += String.fromCharCode(c);
  }
  return out;
}

function streamFor(info: UvcDeviceInfo, interfaceNumber: number): UvcStreamInterface {
  for (let i = 0; i < info.streams.length; i++) {
    if (info.streams[i].interfaceNumber === interfaceNumber) {
      return info.streams[i];
    }
  }
  const stream = new UvcStreamInterface();
  stream.interfaceNumber = interfaceNumber;
  info.streams.push(stream);
  return stream;
}

function readIntervals(raw: Uint8Array, offset: number, intervalType: number, frame: UvcFrameDesc): void {
  if (intervalType === 0) {
    frame.minInterval = u32(raw, offset);
    frame.maxInterval = u32(raw, offset + 4);
    frame.intervalStep = u32(raw, offset + 8);
    return;
  }
  for (let i = 0; i < intervalType; i++) {
    frame.intervals.push(u32(raw, offset + i * 4));
  }
}

/** Bytes needed after offset 26 for the interval table of a frame descriptor. */
function intervalTableSize(intervalType: number): number {
  return intervalType === 0 ? 12 : intervalType * 4;
}

function parseStreamingBlock(
  raw: Uint8Array, offset: number, length: number, stream: UvcStreamInterface, info: UvcDeviceInfo,
): void {
  const subtype = raw[offset + 2];
  const tooShort = (needed: number): boolean => {
    if (length >= needed) {
      return false;
    }
    info.warnings.push(
      `VS descriptor subtype 0x${subtype.toString(16)} at offset ${offset} too short: ` +
        `bLength ${length}, need ${needed}`);
    return true;
  };

  if (subtype === VS_INPUT_HEADER) {
    if (tooShort(7)) {
      return;
    }
    stream.endpointAddress = raw[offset + 6] & 0x8f;
    return;
  }

  if (subtype === VS_FORMAT_MJPEG) {
    if (tooShort(4)) {
      return;
    }
    const format = new UvcFormatDesc();
    format.kind = UVC_KIND_MJPEG;
    format.formatIndex = raw[offset + 3];
    format.fourcc = 'MJPG';
    stream.formats.push(format);
    return;
  }

  if (subtype === VS_FORMAT_UNCOMPRESSED || subtype === VS_FORMAT_FRAME_BASED) {
    if (tooShort(21)) {
      return;
    }
    const format = new UvcFormatDesc();
    format.kind = subtype === VS_FORMAT_UNCOMPRESSED ? UVC_KIND_UNCOMPRESSED : UVC_KIND_FRAME_BASED;
    format.formatIndex = raw[offset + 3];
    format.fourcc = fourccFromGuid(raw, offset + 5);
    stream.formats.push(format);
    return;
  }

  const isFrameBased = subtype === VS_FRAME_FRAME_BASED;
  if (subtype !== VS_FRAME_UNCOMPRESSED && subtype !== VS_FRAME_MJPEG && !isFrameBased) {
    return;
  }
  if (stream.formats.length === 0) {
    info.warnings.push(`VS frame descriptor at offset ${offset} appears before any format descriptor`);
    return;
  }
  if (tooShort(26)) {
    return;
  }
  // Frame-based frames carry bFrameIntervalType at 21 and no buffer size;
  // uncompressed and MJPEG frames carry it at 25 after dwMaxVideoFrameBufferSize.
  const intervalType = raw[offset + (isFrameBased ? 21 : 25)];
  if (tooShort(26 + intervalTableSize(intervalType))) {
    return;
  }
  const frame = new UvcFrameDesc();
  frame.frameIndex = raw[offset + 3];
  frame.width = u16(raw, offset + 5);
  frame.height = u16(raw, offset + 7);
  if (isFrameBased) {
    frame.defaultInterval = u32(raw, offset + 17);
  } else {
    frame.maxVideoFrameBufferSize = u32(raw, offset + 17);
    frame.defaultInterval = u32(raw, offset + 21);
  }
  readIntervals(raw, offset + 26, intervalType, frame);
  stream.formats[stream.formats.length - 1].frames.push(frame);
}

/**
 * Walks a descriptor blob (device descriptor optional, any number of
 * configurations) and collects what is needed to negotiate and receive a
 * UVC stream. Never throws on malformed input; problems land in `warnings`.
 */
export function parseUvcDescriptors(raw: Uint8Array): UvcDeviceInfo {
  const info = new UvcDeviceInfo();
  let offset = 0;
  let curNumber = -1;
  let curAlt = 0;
  let curClass = 0;
  let curSubClass = 0;

  while (offset + 2 <= raw.length) {
    const length = raw[offset];
    const type = raw[offset + 1];
    if (length < 2) {
      info.warnings.push(`descriptor at offset ${offset} has bLength ${length}; parsing stopped`);
      break;
    }
    if (offset + length > raw.length) {
      info.warnings.push(
        `descriptor at offset ${offset} (bLength ${length}) runs past the end (${raw.length}); parsing stopped`);
      break;
    }

    if (type === DT_INTERFACE && length >= 9) {
      curNumber = raw[offset + 2];
      curAlt = raw[offset + 3];
      curClass = raw[offset + 5];
      curSubClass = raw[offset + 6];
      if (curClass === CLASS_VIDEO && curSubClass === SUBCLASS_VIDEO_STREAMING) {
        streamFor(info, curNumber);
      }
    } else if (type === DT_ENDPOINT && length >= 7) {
      if (curClass === CLASS_VIDEO && curSubClass === SUBCLASS_VIDEO_STREAMING) {
        const alt = new UvcAltSetting();
        alt.alternateSetting = curAlt;
        alt.endpointAddress = raw[offset + 2];
        alt.attributes = raw[offset + 3];
        alt.wMaxPacketSize = u16(raw, offset + 4);
        alt.bytesPerPacket = (alt.wMaxPacketSize & 0x07ff) * (((alt.wMaxPacketSize >> 11) & 3) + 1);
        alt.isochronous = (alt.attributes & 0x03) === 0x01;
        streamFor(info, curNumber).altSettings.push(alt);
      }
    } else if (type === DT_SS_ENDPOINT_COMPANION) {
      if (curClass === CLASS_VIDEO && curSubClass === SUBCLASS_VIDEO_STREAMING) {
        info.warnings.push(
          `SuperSpeed endpoint companion at offset ${offset} is not applied; bytesPerPacket may be wrong`);
      }
    } else if (type === DT_CS_INTERFACE && length >= 3 && curClass === CLASS_VIDEO) {
      if (curSubClass === SUBCLASS_VIDEO_CONTROL) {
        if (raw[offset + 2] === VC_HEADER && length >= 5) {
          info.bcdUVC = u16(raw, offset + 3);
          info.controlInterfaceNumber = curNumber;
        }
      } else if (curSubClass === SUBCLASS_VIDEO_STREAMING) {
        parseStreamingBlock(raw, offset, length, streamFor(info, curNumber), info);
      }
    }

    offset += length;
  }
  return info;
}

/**
 * Smallest isochronous alt setting able to carry `maxPayloadTransferSize`
 * bytes per (micro)frame, or null when none can.
 */
export function selectAltSetting(alts: UvcAltSetting[], maxPayloadTransferSize: number): UvcAltSetting | null {
  let best: UvcAltSetting | null = null;
  for (let i = 0; i < alts.length; i++) {
    const alt = alts[i];
    if (!alt.isochronous || alt.bytesPerPacket < maxPayloadTransferSize) {
      continue;
    }
    if (best === null || alt.bytesPerPacket < best.bytesPerPacket) {
      best = alt;
    }
  }
  return best;
}

/**
 * Largest isochronous alt setting whose per-packet size does not exceed
 * `capacity`. Used when the host reserves fewer bytes per packet than the alt
 * setting chosen from the negotiated payload size would send.
 */
export function selectAltSettingWithin(alts: UvcAltSetting[], capacity: number): UvcAltSetting | null {
  let best: UvcAltSetting | null = null;
  for (let i = 0; i < alts.length; i++) {
    const alt = alts[i];
    if (!alt.isochronous || alt.bytesPerPacket > capacity) {
      continue;
    }
    if (best === null || alt.bytesPerPacket > best.bytesPerPacket) {
      best = alt;
    }
  }
  return best;
}

/** One entry per format, frame and frame interval, in descriptor order. */
export function listModes(info: UvcDeviceInfo): UvcMode[] {
  const modes: UvcMode[] = [];
  for (let s = 0; s < info.streams.length; s++) {
    const stream = info.streams[s];
    for (let f = 0; f < stream.formats.length; f++) {
      const format = stream.formats[f];
      for (let r = 0; r < format.frames.length; r++) {
        const frame = format.frames[r];
        const intervals = frame.intervals.length > 0 ? frame.intervals : [frame.defaultInterval];
        for (let i = 0; i < intervals.length; i++) {
          const mode = new UvcMode();
          mode.streamInterfaceNumber = stream.interfaceNumber;
          mode.kind = format.kind;
          mode.fourcc = format.fourcc;
          mode.formatIndex = format.formatIndex;
          mode.frameIndex = frame.frameIndex;
          mode.width = frame.width;
          mode.height = frame.height;
          mode.interval = intervals[i];
          mode.maxVideoFrameBufferSize = frame.maxVideoFrameBufferSize;
          const name = format.kind === UVC_KIND_MJPEG ? UVC_KIND_MJPEG : (format.fourcc || format.kind);
          const fps = intervals[i] > 0 ? (10000000 / intervals[i]).toFixed(1) : '?';
          mode.label = `${name} ${frame.width}x${frame.height} @${fps}fps ` +
            `(fmt ${format.formatIndex} / frame ${frame.frameIndex})`;
          modes.push(mode);
        }
      }
    }
  }
  return modes;
}

/** Index of the first mode with the given size, or 0 when none matches. */
export function defaultModeIndex(modes: UvcMode[], width: number, height: number): number {
  for (let i = 0; i < modes.length; i++) {
    if (modes[i].width === width && modes[i].height === height) {
      return i;
    }
  }
  return 0;
}

/**
 * One line per interface alternate setting in the configuration descriptor,
 * for every class (not just video): class/subclass/protocol, endpoints, and
 * for USB Audio Class streaming interfaces the PCM format they carry. Used
 * to log what a device offers, e.g. whether it has a UAC microphone.
 */
export function describeInterfaces(raw: Uint8Array): string[] {
  const lines: string[] = [];
  let offset = 0;
  let current = '';
  let inAudioStreaming = false;
  while (offset + 2 <= raw.length) {
    const length = raw[offset];
    const type = raw[offset + 1];
    if (length < 2 || offset + length > raw.length) {
      break;
    }
    if (type === 2 && length >= 9) {
      // Configuration header: compare what it announces with what we got.
      lines.push(`config: ${raw[offset + 4]} interface(s), wTotalLength=${u16(raw, offset + 2)}, ` +
        `raw bytes=${raw.length}`);
    } else if (type === 4 && length >= 9) {
      if (current.length > 0) {
        lines.push(current);
      }
      const cls = raw[offset + 5];
      const sub = raw[offset + 6];
      current = `if ${raw[offset + 2]} alt ${raw[offset + 3]}: class ${cls}/${sub}/${raw[offset + 7]}` +
        ` (${className(cls, sub)}) eps=${raw[offset + 4]}`;
      inAudioStreaming = cls === 1 && sub === 2;
    } else if (type === 5 && length >= 7) {
      const attrs = raw[offset + 3];
      const kind = ['control', 'iso', 'bulk', 'interrupt'][attrs & 3];
      current += ` ep 0x${raw[offset + 2].toString(16)} ${kind} ${u16(raw, offset + 4) & 0x7ff}B` +
        ` x${((u16(raw, offset + 4) >> 11) & 3) + 1} ival=${raw[offset + 6]}`;
    } else if (type === 0x24 && inAudioStreaming && raw[offset + 2] === 1 && length >= 7) {
      // AS_GENERAL: bTerminalLink, bDelay, wFormatTag (1 = PCM).
      current += ` fmtTag=${u16(raw, offset + 5)}`;
    } else if (type === 0x24 && inAudioStreaming && raw[offset + 2] === 2 && length >= 8) {
      // FORMAT_TYPE I: channels, subframe size, bit resolution, sample rates.
      const channels = raw[offset + 4];
      const bits = raw[offset + 6];
      const rateCount = raw[offset + 7];
      const rates: string[] = [];
      if (rateCount === 0 && length >= 14) {
        rates.push(`${u24(raw, offset + 8)}-${u24(raw, offset + 11)}`);
      } else {
        for (let i = 0; i < rateCount && offset + 8 + i * 3 + 3 <= offset + length; i++) {
          rates.push(`${u24(raw, offset + 8 + i * 3)}`);
        }
      }
      current += ` pcm ${channels}ch ${bits}bit @ ${rates.join('/')}Hz`;
    }
    offset += length;
  }
  if (current.length > 0) {
    lines.push(current);
  }
  return lines;
}

function u24(raw: Uint8Array, offset: number): number {
  return raw[offset] | (raw[offset + 1] << 8) | (raw[offset + 2] << 16);
}

function className(cls: number, sub: number): string {
  if (cls === 14) {
    return sub === 1 ? 'video control' : sub === 2 ? 'video streaming' : 'video';
  }
  if (cls === 1) {
    return sub === 1 ? 'audio control' : sub === 2 ? 'audio streaming' : 'audio';
  }
  if (cls === 3) {
    return 'HID';
  }
  return 'other';
}
