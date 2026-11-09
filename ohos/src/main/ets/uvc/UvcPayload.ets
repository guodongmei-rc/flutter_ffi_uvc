// UVC payload header handling for counting frames without assembling them.
//
// Header bits follow _uvc_process_payload() in the bundled libuvc
// (src/libuvc/src/stream.c): bit 0 FID, bit 1 EOF, bit 6 ERR; bit 7 is the
// end-of-header marker from the UVC payload header definition.

const HEADER_FID: number = 0x01;
const HEADER_EOF: number = 0x02;
const HEADER_PTS: number = 0x04;
const HEADER_SCR: number = 0x08;
const HEADER_ERR: number = 0x40;
const HEADER_EOH: number = 0x80;

export const ISO_LAYOUT_STRIDE: string = 'stride';
export const ISO_LAYOUT_PACKED: string = 'packed';
/** Both placements explain the data equally well, e.g. every packet was full. */
export const ISO_LAYOUT_INDISTINGUISHABLE: string = 'indistinguishable';
export const ISO_LAYOUT_UNKNOWN: string = 'unknown';

/**
 * Counts frames from a sequence of UVC payloads. A frame ends at the EOF bit,
 * or at a FID toggle for cameras that never set EOF.
 */
export class UvcFrameCounter {
  frames: number = 0;
  framesByEof: number = 0;
  framesByFidToggle: number = 0;
  /** Frames during which a payload was lost, malformed or flagged ERR. */
  damagedFrames: number = 0;
  errorBitPackets: number = 0;
  bogusPackets: number = 0;
  emptyPackets: number = 0;
  lostPackets: number = 0;
  /** Image bytes only, payload headers excluded. */
  payloadBytes: number = 0;
  lastFrameBytes: number = 0;
  minFrameBytes: number = 0;
  maxFrameBytes: number = 0;

  private fid: number = 0;
  private gotBytes: number = 0;
  private damaged: boolean = false;

  /** Feeds one payload: `length` bytes of `buf` starting at `offset`. */
  push(buf: Uint8Array, offset: number, length: number): void {
    if (length === 0) {
      this.emptyPackets++;
      return;
    }
    const headerLength = buf[offset];
    if (headerLength < 2 || headerLength > length) {
      this.bogusPackets++;
      this.damaged = true;
      return;
    }
    const info = buf[offset + 1];
    if ((info & HEADER_ERR) !== 0) {
      this.errorBitPackets++;
      this.damaged = true;
      return;
    }
    const fid = info & HEADER_FID;
    if (fid !== this.fid && this.gotBytes > 0) {
      this.framesByFidToggle++;
      this.finishFrame();
    }
    this.fid = fid;

    const dataLength = length - headerLength;
    this.gotBytes += dataLength;
    this.payloadBytes += dataLength;
    if ((info & HEADER_EOF) !== 0 && this.gotBytes > 0) {
      this.framesByEof++;
      this.finishFrame();
    }
  }

  /** Records a packet the transport reported as failed. */
  markPacketLost(): void {
    this.lostPackets++;
    this.damaged = true;
  }

  private finishFrame(): void {
    this.frames++;
    if (this.damaged) {
      this.damagedFrames++;
    }
    this.damaged = false;
    this.lastFrameBytes = this.gotBytes;
    if (this.minFrameBytes === 0 || this.gotBytes < this.minFrameBytes) {
      this.minFrameBytes = this.gotBytes;
    }
    if (this.gotBytes > this.maxFrameBytes) {
      this.maxFrameBytes = this.gotBytes;
    }
    this.gotBytes = 0;
  }
}

/**
 * Heuristic: does `length` bytes at `offset` start with a UVC payload header?
 * Requires a header length that fits the packet, plus either the
 * end-of-header bit or -- for cameras that never set it -- PTS/SCR flags whose
 * field sizes add up to exactly the stated header length.
 */
export function looksLikeUvcHeader(buf: Uint8Array, offset: number, length: number): boolean {
  if (length < 2 || offset + 2 > buf.length) {
    return false;
  }
  const headerLength = buf[offset];
  if (headerLength < 2 || headerLength > length) {
    return false;
  }
  const info = buf[offset + 1];
  if ((info & HEADER_EOH) !== 0) {
    return true;
  }
  const hasPts = (info & HEADER_PTS) !== 0;
  const hasScr = (info & HEADER_SCR) !== 0;
  if (!hasPts && !hasScr) {
    return false;
  }
  return headerLength === 2 + (hasPts ? 4 : 0) + (hasScr ? 6 : 0);
}

/**
 * Works out where each isochronous packet sits inside a completed transfer
 * buffer. libusb places packet i at i * stride; whether the HarmonyOS
 * usbManager does the same is not documented, so both placements are scored
 * against real data instead of assumed.
 */
export class IsoLayoutProbe {
  /** Non-empty packets examined. */
  checked: number = 0;
  strideValid: number = 0;
  packedValid: number = 0;

  /**
   * @param stride bytes reserved per packet (transfer length / packet count)
   * @param actualLengths actual length of every packet in the transfer
   */
  observe(buf: Uint8Array, stride: number, actualLengths: number[]): void {
    let packedOffset = 0;
    for (let i = 0; i < actualLengths.length; i++) {
      const length = actualLengths[i];
      if (length > 0) {
        this.checked++;
        if (looksLikeUvcHeader(buf, i * stride, length)) {
          this.strideValid++;
        }
        if (looksLikeUvcHeader(buf, packedOffset, length)) {
          this.packedValid++;
        }
      }
      packedOffset += length;
    }
  }

  /** A layout is accepted only when it explains at least 90% of the non-empty packets. */
  verdict(): string {
    const strideWins = this.strideValid >= this.packedValid;
    const best = strideWins ? this.strideValid : this.packedValid;
    if (this.checked === 0 || best * 10 < this.checked * 9) {
      return ISO_LAYOUT_UNKNOWN;
    }
    if (this.strideValid === this.packedValid) {
      return ISO_LAYOUT_INDISTINGUISHABLE;
    }
    return strideWins ? ISO_LAYOUT_STRIDE : ISO_LAYOUT_PACKED;
  }
}
