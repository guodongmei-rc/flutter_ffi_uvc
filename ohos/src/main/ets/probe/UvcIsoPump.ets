// Isochronous receive pump used by the stage 0 probe.
//
// Keeps N usbManager.usbSubmitTransfer() requests in flight, resubmits each
// one from its callback, and counts frames from the UVC payload headers. No
// frame data is copied or decoded: the only question here is whether the
// ArkTS isochronous path can sustain the stream.
//
// Behaviour the SDK does not document is measured and logged, not assumed:
// where packets sit in the returned buffer, whether a buffer can be reused
// across submissions, and what the per-packet descriptors contain.

import { usbManager, BusinessError } from '@kit.BasicServicesKit';
import {
  UvcFrameCounter, IsoLayoutProbe, ISO_LAYOUT_PACKED, ISO_LAYOUT_STRIDE, ISO_LAYOUT_UNKNOWN,
} from '../uvc/UvcPayload';

const STATUS_COMPLETED: number = 0;
const STATUS_CANCELED: number = 3;
const STATUS_NO_DEVICE: number = 5;

/**
 * The packet layout is decided once this many transfers and this many
 * non-empty packets have been examined. A camera that is slow to start sends
 * empty packets first, so sampling continues up to the transfer limit.
 */
const LAYOUT_SAMPLE_TRANSFERS: number = 64;
const LAYOUT_SAMPLE_PACKETS: number = 200;
const LAYOUT_SAMPLE_TRANSFER_LIMIT: number = 2000;
/** Completed callbacks logged field by field at the start of a run. */
const VERBOSE_CALLBACKS: number = 5;
const STALL_SECONDS: number = 6;
const MAX_CONSECUTIVE_FAILURES: number = 200;

export class UvcIsoPumpConfig {
  endpointAddress: number = 0;
  /** Bytes reserved per packet: the alt setting's capacity per (micro)frame. */
  bytesPerPacket: number = 0;
  numTransfers: number = 8;
  packetsPerTransfer: number = 128;
  /** Numeric value of usbManager.UsbTransferFlags (0..3). */
  transferFlags: number = 0;
  transferTimeoutMs: number = 5000;
  durationSec: number = 300;
  minFps: number = 25;
  /**
   * Stop as soon as the system turns out to reserve fewer bytes per packet
   * than the alt setting sends, so the caller can pick an alt that fits.
   */
  stopOnCapacityMismatch: boolean = false;
}

export function hexBytes(buf: Uint8Array, offset: number, length: number): string {
  let out = '';
  const end = Math.min(buf.length, offset + length);
  for (let i = offset; i < end; i++) {
    out += buf[i].toString(16).padStart(2, '0');
    if (i + 1 < end) {
      out += ' ';
    }
  }
  return out;
}

function bump(map: Map<number, number>, key: number): void {
  const current = map.get(key);
  map.set(key, current === undefined ? 1 : current + 1);
}

function describeCounts(map: Map<number, number>): string {
  if (map.size === 0) {
    return 'none';
  }
  const parts: string[] = [];
  map.forEach((count: number, key: number) => {
    parts.push(`${key}x${count}`);
  });
  return parts.join(' ');
}

export function transferFlagsFromNumber(value: number): usbManager.UsbTransferFlags {
  switch (value) {
    case 1:
      return usbManager.UsbTransferFlags.USB_TRANSFER_FREE_BUFFER;
    case 2:
      return usbManager.UsbTransferFlags.USB_TRANSFER_FREE_TRANSFER;
    case 3:
      return usbManager.UsbTransferFlags.USB_TRANSFER_ADD_ZERO_PACKET;
    default:
      return usbManager.UsbTransferFlags.USB_TRANSFER_SHORT_NOT_OK;
  }
}

export class UvcIsoPump {
  private pipe: usbManager.USBDevicePipe;
  private config: UvcIsoPumpConfig;
  private log: (line: string) => void;
  private onStats: (line: string) => void;

  private running: boolean = false;
  private stopReason: string = '';
  private finish: ((passed: boolean) => void) | null = null;
  private timer: number = -1;

  private transferLength: number = 0;
  private buffers: Uint8Array[] = [];
  private inFlightParams: (usbManager.UsbDataTransferParams | null)[] = [];
  private reuseBuffers: boolean = true;
  private inFlight: number = 0;

  // Layout detection runs first; frame counting starts once it has finished.
  private layoutProbe: IsoLayoutProbe = new IsoLayoutProbe();
  private layoutDecided: boolean = false;
  private layoutPacked: boolean = false;
  private layoutSamples: number = 0;
  private packetLengths: number[] = [];
  /** Bytes the system reserved per packet, as reported in the packet descriptors; 0 until seen. */
  private reportedStride: number = 0;
  private unevenPacketLengths: boolean = false;
  private capacityMismatch: boolean = false;

  private counter: UvcFrameCounter = new UvcFrameCounter();
  private measureStartMs: number = 0;
  private lastTickMs: number = 0;
  private lastFrames: number = 0;
  private lastBytes: number = 0;
  private lastCallbacks: number = 0;
  private fpsPerSecond: number[] = [];
  private secondsWithoutCallbacks: number = 0;

  private callbacks: number = 0;
  private wireBytes: number = 0;
  private packets: number = 0;
  private badPackets: number = 0;
  private overrunPackets: number = 0;
  private transfersWithoutDescs: number = 0;
  private canceledAfterStop: number = 0;
  private consecutiveFailures: number = 0;
  private lastCallbackMs: number = 0;
  private maxCallbackGapMs: number = 0;
  private verboseLeft: number = VERBOSE_CALLBACKS;
  private transferStatus: Map<number, number> = new Map<number, number>();
  private packetStatus: Map<number, number> = new Map<number, number>();
  private callbackErrors: Map<number, number> = new Map<number, number>();
  private submitErrors: Map<number, number> = new Map<number, number>();
  private cancelErrors: Map<number, number> = new Map<number, number>();

  constructor(
    pipe: usbManager.USBDevicePipe, config: UvcIsoPumpConfig,
    log: (line: string) => void, onStats: (line: string) => void,
  ) {
    this.pipe = pipe;
    this.config = config;
    this.log = log;
    this.onStats = onStats;
  }

  isRunning(): boolean {
    return this.running;
  }

  /** Bytes the system reserved per packet; 0 when no completed transfer reported it. */
  reportedPacketBytes(): number {
    return this.reportedStride;
  }

  /** True when the run ended early because the alt setting's packets cannot fit. */
  stoppedForCapacityMismatch(): boolean {
    return this.capacityMismatch;
  }

  /** Runs until the configured duration elapses or the pump stops; resolves with the verdict. */
  start(): Promise<boolean> {
    const config = this.config;
    this.transferLength = config.bytesPerPacket * config.packetsPerTransfer;
    this.log(
      `pump start: endpoint=0x${config.endpointAddress.toString(16)} transfers=${config.numTransfers} ` +
        `packets/transfer=${config.packetsPerTransfer} bytes/packet=${config.bytesPerPacket} ` +
        `length=${this.transferLength} flags=${config.transferFlags} timeout=${config.transferTimeoutMs}ms ` +
        `duration=${config.durationSec}s`);

    this.running = true;
    this.buffers = [];
    this.inFlightParams = [];
    for (let slot = 0; slot < config.numTransfers; slot++) {
      this.buffers.push(new Uint8Array(this.transferLength));
      this.inFlightParams.push(null);
    }
    for (let i = 0; i < config.packetsPerTransfer; i++) {
      this.packetLengths.push(0);
    }

    return new Promise<boolean>((resolve: (passed: boolean) => void) => {
      this.finish = resolve;
      for (let slot = 0; slot < config.numTransfers; slot++) {
        this.submit(slot);
      }
      if (this.inFlight === 0) {
        this.stop('no transfer could be submitted');
        return;
      }
      this.lastTickMs = Date.now();
      this.timer = setInterval(() => {
        this.tick();
      }, 1000);
    });
  }

  stop(reason: string): void {
    if (!this.running) {
      return;
    }
    this.running = false;
    this.stopReason = reason;
    if (this.timer !== -1) {
      clearInterval(this.timer);
      this.timer = -1;
    }
    for (let slot = 0; slot < this.inFlightParams.length; slot++) {
      const params = this.inFlightParams[slot];
      if (params === null) {
        continue;
      }
      try {
        usbManager.usbCancelTransfer(params);
      } catch (e) {
        bump(this.cancelErrors, (e as BusinessError).code);
      }
    }
    const passed = this.capacityMismatch ? false : this.summarize();
    const finish = this.finish;
    this.finish = null;
    if (finish !== null) {
      finish(passed);
    }
  }

  private submit(slot: number): void {
    if (!this.running) {
      return;
    }
    if (this.trySubmit(slot, this.reuseBuffers)) {
      return;
    }
    if (this.reuseBuffers && this.trySubmit(slot, false)) {
      this.reuseBuffers = false;
      this.log('NOTE: resubmitting with the same buffer failed but a fresh buffer worked; ' +
        'allocating a new buffer per transfer from now on');
    }
  }

  private trySubmit(slot: number, reuse: boolean): boolean {
    if (!reuse) {
      this.buffers[slot] = new Uint8Array(this.transferLength);
    }
    const buffer = this.buffers[slot];
    const params: usbManager.UsbDataTransferParams = {
      devPipe: this.pipe,
      flags: transferFlagsFromNumber(this.config.transferFlags),
      endpoint: this.config.endpointAddress,
      type: usbManager.UsbEndpointTransferType.TRANSFER_TYPE_ISOCHRONOUS,
      timeout: this.config.transferTimeoutMs,
      length: this.transferLength,
      callback: (err: BusinessError, data: usbManager.SubmitTransferCallback): void => {
        this.onTransfer(slot, buffer, err, data);
      },
      userData: new Uint8Array(0),
      buffer: buffer,
      isoPacketCount: this.config.packetsPerTransfer
    };
    try {
      usbManager.usbSubmitTransfer(params);
    } catch (e) {
      const code = (e as BusinessError).code;
      if (!this.submitErrors.has(code)) {
        this.log(`usbSubmitTransfer threw code=${code} message=${(e as BusinessError).message}`);
      }
      bump(this.submitErrors, code);
      return false;
    }
    this.inFlightParams[slot] = params;
    this.inFlight++;
    return true;
  }

  private onTransfer(
    slot: number, buffer: Uint8Array, err: BusinessError, data: usbManager.SubmitTransferCallback,
  ): void {
    this.inFlight--;
    this.inFlightParams[slot] = null;
    this.callbacks++;

    const now = Date.now();
    if (this.lastCallbackMs !== 0 && now - this.lastCallbackMs > this.maxCallbackGapMs) {
      this.maxCallbackGapMs = now - this.lastCallbackMs;
    }
    this.lastCallbackMs = now;

    if (!this.running) {
      this.canceledAfterStop++;
      return;
    }

    let failed = false;
    if (err && err.code) {
      if (!this.callbackErrors.has(err.code)) {
        this.log(`transfer callback error code=${err.code} message=${err.message}`);
      }
      bump(this.callbackErrors, err.code);
      failed = true;
    }

    let status = -1;
    if (data) {
      status = data.status;
      bump(this.transferStatus, status);
      if (this.verboseLeft > 0) {
        this.verboseLeft--;
        this.logCallback(buffer, data);
      }
      if (status === STATUS_COMPLETED) {
        this.processTransfer(buffer, data);
      } else {
        failed = true;
      }
    } else if (!failed) {
      this.log('transfer callback delivered neither an error nor data');
      failed = true;
    }

    if (status === STATUS_NO_DEVICE) {
      this.stop('device disconnected (TRANSFER_NO_DEVICE)');
      return;
    }
    if (failed) {
      this.consecutiveFailures++;
      if (this.consecutiveFailures > MAX_CONSECUTIVE_FAILURES) {
        this.stop(`${MAX_CONSECUTIVE_FAILURES} consecutive failed transfers`);
        return;
      }
    } else {
      this.consecutiveFailures = 0;
    }

    this.submit(slot);
    if (this.inFlight === 0) {
      this.stop('every transfer failed to resubmit');
    }
  }

  /**
   * Bytes between consecutive packets in a completed buffer. The system sizes
   * the packets itself and reports that size in each descriptor's `length`;
   * on a real device it was the first alt setting's wMaxPacketSize, not
   * transfer length / packet count, so the reported value wins.
   */
  private strideOf(descs: Array<Readonly<usbManager.UsbIsoPacketDescriptor>>): number {
    const reported = descs.length > 0 ? descs[0].length : 0;
    if (reported <= 0) {
      return this.config.bytesPerPacket;
    }
    if (reported !== this.reportedStride) {
      this.reportedStride = reported;
      if (reported !== this.config.bytesPerPacket) {
        this.log(
          `NOTE: the system reports ${reported} bytes per packet, the alt setting carries ` +
            `${this.config.bytesPerPacket}; using ${reported} as the stride` +
            (reported < this.config.bytesPerPacket ? ' (larger payloads cannot fit and will fail)' : ''));
      }
      if (reported < this.config.bytesPerPacket && this.config.stopOnCapacityMismatch) {
        this.capacityMismatch = true;
        this.stop('packet capacity mismatch');
      }
    }
    if (!this.unevenPacketLengths) {
      for (let i = 1; i < descs.length; i++) {
        if (descs[i].length !== reported) {
          this.unevenPacketLengths = true;
          this.log(`WARN: packet descriptors report uneven lengths (${reported} vs ${descs[i].length} at ${i})`);
          break;
        }
      }
    }
    return reported;
  }

  private logCallback(buffer: Uint8Array, data: usbManager.SubmitTransferCallback): void {
    const descs = data.isoPacketDescs;
    const count = descs ? descs.length : -1;
    this.log(`callback: status=${data.status} actualLength=${data.actualLength} isoPacketDescs=${count}`);
    if (!descs) {
      return;
    }
    let shown = 0;
    let packedOffset = 0;
    for (let i = 0; i < descs.length && shown < 3; i++) {
      const desc = descs[i];
      if (desc.actualLength > 0 || i === 0) {
        const strideOffset = i * this.strideOf(descs);
        this.log(
          `  packet[${i}] length=${desc.length} actualLength=${desc.actualLength} status=${desc.status} ` +
            `@stride ${strideOffset}: ${hexBytes(buffer, strideOffset, 12)} | ` +
            `@packed ${packedOffset}: ${hexBytes(buffer, packedOffset, 12)}`);
        shown++;
      }
      packedOffset += desc.actualLength;
    }
  }

  private processTransfer(buffer: Uint8Array, data: usbManager.SubmitTransferCallback): void {
    const descs = data.isoPacketDescs;
    if (!descs || descs.length === 0) {
      // No per-packet information: the only thing left to try is treating the
      // whole transfer as one payload.
      this.transfersWithoutDescs++;
      this.wireBytes += data.actualLength;
      if (this.layoutDecided) {
        this.counter.push(buffer, 0, Math.min(data.actualLength, buffer.length));
      } else {
        this.decideLayout();
      }
      return;
    }

    const stride = this.strideOf(descs);
    if (!this.running) {
      return;
    }
    if (!this.layoutDecided) {
      if (this.packetLengths.length !== descs.length) {
        this.packetLengths = [];
        for (let i = 0; i < descs.length; i++) {
          this.packetLengths.push(0);
        }
      }
      for (let i = 0; i < descs.length; i++) {
        this.packetLengths[i] = descs[i].status === STATUS_COMPLETED ? descs[i].actualLength : 0;
        this.wireBytes += this.packetLengths[i];
      }
      this.layoutProbe.observe(buffer, stride, this.packetLengths);
      this.layoutSamples++;
      const enough = this.layoutSamples >= LAYOUT_SAMPLE_TRANSFERS &&
        this.layoutProbe.checked >= LAYOUT_SAMPLE_PACKETS;
      if (enough || this.layoutSamples >= LAYOUT_SAMPLE_TRANSFER_LIMIT) {
        this.decideLayout();
      }
      return;
    }

    let packedOffset = 0;
    for (let i = 0; i < descs.length; i++) {
      const desc = descs[i];
      const length = desc.actualLength;
      const offset = this.layoutPacked ? packedOffset : i * stride;
      packedOffset += length;
      this.packets++;
      if (desc.status !== STATUS_COMPLETED) {
        this.badPackets++;
        bump(this.packetStatus, desc.status);
        this.counter.markPacketLost();
        continue;
      }
      if (offset + length > buffer.length) {
        this.overrunPackets++;
        this.counter.markPacketLost();
        continue;
      }
      this.wireBytes += length;
      this.counter.push(buffer, offset, length);
    }
  }

  private decideLayout(): void {
    const probe = this.layoutProbe;
    const verdict = probe.verdict();
    this.layoutPacked = verdict === ISO_LAYOUT_PACKED;
    this.layoutDecided = true;
    this.log(
      `packet layout: verdict=${verdict} (stride ${probe.strideValid}/${probe.checked}, ` +
        `packed ${probe.packedValid}/${probe.checked} packets start with a UVC header) ` +
        `-> using ${this.layoutPacked ? ISO_LAYOUT_PACKED : ISO_LAYOUT_STRIDE}`);
    if (verdict === ISO_LAYOUT_UNKNOWN) {
      this.log('WARN: packet layout undetermined; frame counts below assume the stride layout and may be wrong');
    }
    // Measurement starts here so the sampling window does not skew the rates.
    this.counter = new UvcFrameCounter();
    this.wireBytes = 0;
    this.measureStartMs = Date.now();
    this.lastTickMs = this.measureStartMs;
    this.lastFrames = 0;
    this.lastBytes = 0;
    this.lastCallbacks = this.callbacks;
  }

  private tick(): void {
    if (!this.running) {
      return;
    }
    const now = Date.now();
    const callbacksNow = this.callbacks - this.lastCallbacks;
    this.lastCallbacks = this.callbacks;
    if (callbacksNow === 0) {
      this.secondsWithoutCallbacks++;
      if (this.secondsWithoutCallbacks >= STALL_SECONDS) {
        this.stop(`no transfer callback for ${STALL_SECONDS}s`);
        return;
      }
    } else {
      this.secondsWithoutCallbacks = 0;
    }
    if (!this.layoutDecided) {
      this.onStats(`sampling packet layout: ${this.layoutSamples} transfers, ` +
        `${this.layoutProbe.checked} non-empty packets, callbacks=${this.callbacks}`);
      this.lastTickMs = now;
      return;
    }

    const elapsedMs = Math.max(1, now - this.lastTickMs);
    this.lastTickMs = now;
    const frames = this.counter.frames - this.lastFrames;
    const bytes = this.wireBytes - this.lastBytes;
    this.lastFrames = this.counter.frames;
    this.lastBytes = this.wireBytes;
    const fps = frames * 1000 / elapsedMs;
    this.fpsPerSecond.push(fps);

    const seconds = Math.round((now - this.measureStartMs) / 1000);
    const line = `t=${seconds}s fps=${fps.toFixed(1)} rate=${(bytes * 1000 / elapsedMs / 1024).toFixed(0)}kB/s ` +
      `callbacks=${callbacksNow} badPackets=${this.badPackets} errBit=${this.counter.errorBitPackets} ` +
      `bogus=${this.counter.bogusPackets} damagedFrames=${this.counter.damagedFrames} ` +
      `inFlight=${this.inFlight} gapMax=${this.maxCallbackGapMs}ms frame=${this.counter.lastFrameBytes}B`;
    this.onStats(line);
    if (seconds % 10 === 0) {
      this.log(line);
    }
    if (now - this.measureStartMs >= this.config.durationSec * 1000) {
      this.stop('duration reached');
    }
  }

  /** Logs the run summary and returns whether the stage 0 criterion was met. */
  private summarize(): boolean {
    const counter = this.counter;
    const elapsedSec = this.measureStartMs === 0 ? 0 : (Date.now() - this.measureStartMs) / 1000;
    const averageFps = elapsedSec > 0 ? counter.frames / elapsedSec : 0;

    // The first two seconds are excluded from the per-second minimum: the
    // camera is still ramping up and the timer has not settled.
    let minFps = -1;
    let secondsBelow = 0;
    let worstWindow = -1;
    for (let i = 2; i < this.fpsPerSecond.length; i++) {
      const fps = this.fpsPerSecond[i];
      if (minFps < 0 || fps < minFps) {
        minFps = fps;
      }
      if (fps < this.config.minFps) {
        secondsBelow++;
      }
      if (i >= 6) {
        let sum = 0;
        for (let j = i - 4; j <= i; j++) {
          sum += this.fpsPerSecond[j];
        }
        if (worstWindow < 0 || sum / 5 < worstWindow) {
          worstWindow = sum / 5;
        }
      }
    }

    let failedTransfers = 0;
    this.transferStatus.forEach((count: number, status: number) => {
      if (status !== STATUS_COMPLETED && status !== STATUS_NO_DEVICE && status !== STATUS_CANCELED) {
        failedTransfers += count;
      }
    });
    let callbackErrorCount = 0;
    this.callbackErrors.forEach((count: number) => {
      callbackErrorCount += count;
    });
    let submitErrorCount = 0;
    this.submitErrors.forEach((count: number) => {
      submitErrorCount += count;
    });
    const errors = failedTransfers + callbackErrorCount + submitErrorCount + this.badPackets +
      this.overrunPackets + counter.errorBitPackets + counter.bogusPackets;
    const ranFullDuration = this.stopReason === 'duration reached';
    const passed = ranFullDuration && averageFps >= this.config.minFps && errors === 0;

    this.log('---- pump summary ----');
    this.log(`stopped: ${this.stopReason}; measured ${elapsedSec.toFixed(1)}s of ${this.config.durationSec}s`);
    this.log(
      `frames=${counter.frames} (EOF ${counter.framesByEof}, FID toggle ${counter.framesByFidToggle}) ` +
        `damaged=${counter.damagedFrames} averageFps=${averageFps.toFixed(2)}`);
    this.log(
      `per-second fps after warm-up: min=${minFps.toFixed(1)} worst 5s average=${worstWindow.toFixed(1)} ` +
        `seconds below ${this.config.minFps}=${secondsBelow}`);
    this.log(
      `frame size: min=${counter.minFrameBytes}B max=${counter.maxFrameBytes}B; ` +
        `wire bytes=${this.wireBytes} (${elapsedSec > 0 ? (this.wireBytes / elapsedSec / 1048576).toFixed(2) : '0'} MB/s) ` +
        `payload bytes=${counter.payloadBytes}`);
    this.log(
      `transfers: callbacks=${this.callbacks} status{${describeCounts(this.transferStatus)}} ` +
        `callbackErrors{${describeCounts(this.callbackErrors)}} submitErrors{${describeCounts(this.submitErrors)}} ` +
        `cancelErrors{${describeCounts(this.cancelErrors)}} withoutDescs=${this.transfersWithoutDescs} ` +
        `maxCallbackGap=${this.maxCallbackGapMs}ms reuseBuffers=${this.reuseBuffers}`);
    this.log(
      `packets=${this.packets} bad=${this.badPackets} status{${describeCounts(this.packetStatus)}} ` +
        `overrun=${this.overrunPackets} empty=${counter.emptyPackets} errBit=${counter.errorBitPackets} ` +
        `bogusHeader=${counter.bogusPackets}`);
    this.log(
      `layout: ${this.layoutPacked ? ISO_LAYOUT_PACKED : ISO_LAYOUT_STRIDE} reportedStride=${this.reportedStride} ` +
        `(stride ${this.layoutProbe.strideValid}/${this.layoutProbe.checked}, ` +
        `packed ${this.layoutProbe.packedValid}/${this.layoutProbe.checked})`);
    this.log(
      `VERDICT: ${passed ? 'PASS' : 'FAIL'} -- rule: ran the full duration (${ranFullDuration}), ` +
        `average fps >= ${this.config.minFps} (${averageFps.toFixed(2)}), ` +
        `no errors other than NO_DEVICE/CANCELED (${errors})`);
    return passed;
  }
}
