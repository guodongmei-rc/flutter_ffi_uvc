// VideoStreaming PROBE / COMMIT control block.
//
// Layout and request constants follow uvc_query_stream_ctrl() in the bundled
// libuvc (src/libuvc/src/stream.c). libuvc itself stops at the 34-byte UVC 1.1
// form; the 48-byte UVC 1.5 length is sent with its extension fields zeroed.

export const UVC_SET_CUR: number = 0x01;
export const UVC_GET_CUR: number = 0x81;
export const UVC_REQ_TYPE_SET: number = 0x21;
export const UVC_REQ_TYPE_GET: number = 0xa1;
export const UVC_VS_PROBE_CONTROL: number = 0x01;
export const UVC_VS_COMMIT_CONTROL: number = 0x02;

export class UvcStreamCtrl {
  bmHint: number = 0;
  bFormatIndex: number = 0;
  bFrameIndex: number = 0;
  dwFrameInterval: number = 0;
  wKeyFrameRate: number = 0;
  wPFrameRate: number = 0;
  wCompQuality: number = 0;
  wCompWindowSize: number = 0;
  wDelay: number = 0;
  dwMaxVideoFrameSize: number = 0;
  dwMaxPayloadTransferSize: number = 0;
  dwClockFrequency: number = 0;
  bmFramingInfo: number = 0;
  bPreferredVersion: number = 0;
  bMinVersion: number = 0;
  bMaxVersion: number = 0;
}

/** Length of the PROBE / COMMIT block for a device reporting `bcdUVC`. */
export function streamCtrlLength(bcdUVC: number): number {
  if (bcdUVC >= 0x0150) {
    return 48;
  }
  if (bcdUVC >= 0x0110) {
    return 34;
  }
  return 26;
}

function put16(buf: Uint8Array, offset: number, value: number): void {
  buf[offset] = value & 0xff;
  buf[offset + 1] = (value >> 8) & 0xff;
}

function put32(buf: Uint8Array, offset: number, value: number): void {
  buf[offset] = value & 0xff;
  buf[offset + 1] = (value >> 8) & 0xff;
  buf[offset + 2] = (value >> 16) & 0xff;
  buf[offset + 3] = (value >>> 24) & 0xff;
}

function get16(buf: Uint8Array, offset: number): number {
  return buf[offset] | (buf[offset + 1] << 8);
}

function get32(buf: Uint8Array, offset: number): number {
  return (buf[offset] | (buf[offset + 1] << 8) | (buf[offset + 2] << 16) | (buf[offset + 3] << 24)) >>> 0;
}

export function encodeStreamCtrl(ctrl: UvcStreamCtrl, length: number): Uint8Array {
  const buf = new Uint8Array(length);
  put16(buf, 0, ctrl.bmHint);
  buf[2] = ctrl.bFormatIndex;
  buf[3] = ctrl.bFrameIndex;
  put32(buf, 4, ctrl.dwFrameInterval);
  put16(buf, 8, ctrl.wKeyFrameRate);
  put16(buf, 10, ctrl.wPFrameRate);
  put16(buf, 12, ctrl.wCompQuality);
  put16(buf, 14, ctrl.wCompWindowSize);
  put16(buf, 16, ctrl.wDelay);
  put32(buf, 18, ctrl.dwMaxVideoFrameSize);
  put32(buf, 22, ctrl.dwMaxPayloadTransferSize);
  if (length >= 34) {
    put32(buf, 26, ctrl.dwClockFrequency);
    buf[30] = ctrl.bmFramingInfo;
    buf[31] = ctrl.bPreferredVersion;
    buf[32] = ctrl.bMinVersion;
    buf[33] = ctrl.bMaxVersion;
  }
  return buf;
}

/**
 * Decodes the first `length` bytes of `buf`. Pass the byte count the device
 * actually returned so fields it did not send stay zero.
 */
export function decodeStreamCtrl(buf: Uint8Array, length: number): UvcStreamCtrl {
  const ctrl = new UvcStreamCtrl();
  if (length < 26 || buf.length < 26) {
    return ctrl;
  }
  ctrl.bmHint = get16(buf, 0);
  ctrl.bFormatIndex = buf[2];
  ctrl.bFrameIndex = buf[3];
  ctrl.dwFrameInterval = get32(buf, 4);
  ctrl.wKeyFrameRate = get16(buf, 8);
  ctrl.wPFrameRate = get16(buf, 10);
  ctrl.wCompQuality = get16(buf, 12);
  ctrl.wCompWindowSize = get16(buf, 14);
  ctrl.wDelay = get16(buf, 16);
  ctrl.dwMaxVideoFrameSize = get32(buf, 18);
  ctrl.dwMaxPayloadTransferSize = get32(buf, 22);
  if (length >= 34 && buf.length >= 34) {
    ctrl.dwClockFrequency = get32(buf, 26);
    ctrl.bmFramingInfo = buf[30];
    ctrl.bPreferredVersion = buf[31];
    ctrl.bMinVersion = buf[32];
    ctrl.bMaxVersion = buf[33];
  }
  return ctrl;
}

export function describeStreamCtrl(ctrl: UvcStreamCtrl): string {
  return `hint=${ctrl.bmHint} fmt=${ctrl.bFormatIndex} frame=${ctrl.bFrameIndex} ` +
    `interval=${ctrl.dwFrameInterval} maxFrame=${ctrl.dwMaxVideoFrameSize} ` +
    `maxPayload=${ctrl.dwMaxPayloadTransferSize} clock=${ctrl.dwClockFrequency} ` +
    `framing=${ctrl.bmFramingInfo}`;
}
