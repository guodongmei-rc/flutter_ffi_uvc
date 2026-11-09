// Stage 0 probe: can a HarmonyOS app drive a UVC camera through usbManager?
//
// Walks the whole path once -- enumerate, authorise, read descriptors, claim
// the video interfaces, PROBE/COMMIT, select an alt setting, receive
// isochronous data -- and logs every return value along the way. It answers
// questions the SDK documentation leaves open, so nothing here is treated as
// known to work until a device run says so.
//
// Thread-agnostic on purpose: the same class runs on the UI thread and inside
// a Worker, which is how "does usbManager work in a Worker" gets answered.

import { usbManager, BusinessError } from '@kit.BasicServicesKit';
import { fileIo } from '@kit.CoreFileKit';
import {
  UvcDeviceInfo, UvcMode, UvcStreamInterface, UvcAltSetting,
  parseUvcDescriptors, listModes, defaultModeIndex, selectAltSetting, selectAltSettingWithin,
} from '../uvc/UvcDescriptor';
import {
  UvcStreamCtrl, streamCtrlLength, encodeStreamCtrl, decodeStreamCtrl, describeStreamCtrl,
  UVC_SET_CUR, UVC_GET_CUR, UVC_REQ_TYPE_SET, UVC_REQ_TYPE_GET,
  UVC_VS_PROBE_CONTROL, UVC_VS_COMMIT_CONTROL,
} from '../uvc/UvcStreamControl';
import { UvcIsoPump, UvcIsoPumpConfig, hexBytes, transferFlagsFromNumber } from './UvcIsoPump';

const USB_CLASS_VIDEO: number = 14;
const CONTROL_TIMEOUT_MS: number = 1000;
const SIZE_TEST_TIMEOUT_MS: number = 3000;
const SIZE_TEST_PACKET_COUNTS: number[] = [1, 8, 16, 32, 64, 128, 256, 512, 1024];

export class UvcProbeOptions {
  /** Index into modeLabels(); -1 picks the first mode matching width x height. */
  modeIndex: number = -1;
  width: number = 1280;
  height: number = 720;
  numTransfers: number = 8;
  packetsPerTransfer: number = 128;
  /** Numeric value of usbManager.UsbTransferFlags (0..3). */
  transferFlags: number = 0;
  transferTimeoutMs: number = 5000;
  durationSec: number = 300;
  minFps: number = 25;
  /** Streaming alt setting to select; 0 picks the smallest one that carries the negotiated payload. */
  altSetting: number = 0;
}

function errorText(e: Object): string {
  const err = e as BusinessError;
  return `code=${err.code} message=${err.message}`;
}

export class UvcProbeSession {
  private log: (line: string) => void;
  private onStats: (line: string) => void;

  private device: usbManager.USBDevice | null = null;
  private pipe: usbManager.USBDevicePipe | null = null;
  private info: UvcDeviceInfo | null = null;
  private modes: UvcMode[] = [];
  private claimed: usbManager.USBInterface[] = [];

  private stream: UvcStreamInterface | null = null;
  private alt: UvcAltSetting | null = null;
  private streamingAltSet: boolean = false;
  private pump: UvcIsoPump | null = null;

  constructor(log: (line: string) => void, onStats: (line: string) => void) {
    this.log = log;
    this.onStats = onStats;
  }

  isOpen(): boolean {
    return this.pipe !== null;
  }

  isStreaming(): boolean {
    return this.pump !== null && this.pump.isRunning();
  }

  modeLabels(): string[] {
    const labels: string[] = [];
    for (let i = 0; i < this.modes.length; i++) {
      labels.push(this.modes[i].label);
    }
    return labels;
  }

  defaultMode(width: number, height: number): number {
    return defaultModeIndex(this.modes, width, height);
  }

  /** Enumerates, authorises, connects, reads descriptors and claims the video interfaces. */
  async open(): Promise<boolean> {
    if (this.pipe !== null) {
      this.log('open: already open');
      return true;
    }
    const device = this.findVideoDevice();
    if (device === null) {
      return false;
    }
    this.device = device;

    try {
      const had = usbManager.hasRight(device.name);
      this.log(`hasRight(${device.name}) = ${had}  (false means the system will ask the user now)`);
      if (!had) {
        const granted = await usbManager.requestRight(device.name);
        this.log(`requestRight = ${granted}`);
        if (!granted) {
          return false;
        }
      }
    } catch (e) {
      this.log(`hasRight/requestRight threw ${errorText(e)}`);
      return false;
    }

    let pipe: usbManager.USBDevicePipe;
    try {
      pipe = usbManager.connectDevice(device);
    } catch (e) {
      this.log(`connectDevice threw ${errorText(e)}`);
      return false;
    }
    if (!pipe) {
      this.log('connectDevice returned no pipe');
      return false;
    }
    this.pipe = pipe;
    this.log(`connectDevice ok: busNum=${pipe.busNum} devAddress=${pipe.devAddress}`);

    // The research note assumed apps get no raw file descriptor; the SDK does
    // declare this call, so record what it returns on a real device.
    try {
      const fd = usbManager.getFileDescriptor(pipe);
      this.log(`getFileDescriptor = ${fd}`);
      this.describeFd(fd);
    } catch (e) {
      this.log(`getFileDescriptor threw ${errorText(e)}`);
    }

    if (!this.readDescriptors(pipe)) {
      return false;
    }
    return this.claimVideoInterfaces(pipe, device);
  }

  /**
   * Checks whether the descriptor is a usbfs node this process can read. A
   * usable descriptor would allow a native libusb/libuvc stack
   * (libusb_wrap_sys_device), which is not tied to the packet size and
   * transfer limits measured on the usbManager path. ioctl access cannot be
   * tested from ArkTS and still needs a native check.
   */
  private describeFd(fd: number): void {
    if (fd < 0) {
      return;
    }
    try {
      const stat = fileIo.statSync(fd);
      this.log(
        `  fd stat: charDevice=${stat.isCharacterDevice()} mode=0${stat.mode.toString(8)} ` +
          `uid=${stat.uid} gid=${stat.gid} size=${stat.size}`);
    } catch (e) {
      this.log(`  fd stat threw ${errorText(e)}`);
    }
    try {
      // usbfs returns the device descriptor followed by the configurations.
      const buffer = new ArrayBuffer(64);
      const read = fileIo.readSync(fd, buffer, { offset: 0 });
      this.log(`  fd read(64 bytes at 0) = ${read}: ${hexBytes(new Uint8Array(buffer), 0, 18)}`);
    } catch (e) {
      this.log(`  fd read threw ${errorText(e)}`);
    }
  }

  private findVideoDevice(): usbManager.USBDevice | null {
    let devices: Array<Readonly<usbManager.USBDevice>>;
    try {
      devices = usbManager.getDevices();
    } catch (e) {
      this.log(`getDevices threw ${errorText(e)}`);
      return null;
    }
    if (!devices) {
      this.log('getDevices returned nothing');
      return null;
    }
    this.log(`getDevices: ${devices.length} device(s)`);
    let found: usbManager.USBDevice | null = null;
    for (let d = 0; d < devices.length; d++) {
      const device = devices[d] as usbManager.USBDevice;
      this.log(
        `device[${d}] name=${device.name} vid=0x${device.vendorId.toString(16)} ` +
          `pid=0x${device.productId.toString(16)} class=${device.clazz} product="${device.productName}"`);
      // Full structure, so the log shows whether alt settings are listed as
      // separate USBInterface entries and what maxPacketSize holds.
      try {
        this.log(`device[${d}] json=${JSON.stringify(device)}`);
      } catch (e) {
        this.log(`device[${d}] could not be serialised`);
      }
      if (found === null && this.interfacesOf(device, USB_CLASS_VIDEO).length > 0) {
        found = device;
      }
    }
    if (found === null) {
      this.log('no device with a video class (14) interface');
    }
    return found;
  }

  private interfacesOf(device: usbManager.USBDevice, clazz: number): usbManager.USBInterface[] {
    const out: usbManager.USBInterface[] = [];
    const configs = device.configs;
    if (!configs) {
      return out;
    }
    for (let c = 0; c < configs.length; c++) {
      const interfaces = configs[c].interfaces;
      if (!interfaces) {
        continue;
      }
      for (let i = 0; i < interfaces.length; i++) {
        if (interfaces[i].clazz === clazz) {
          out.push(interfaces[i]);
        }
      }
    }
    return out;
  }

  private readDescriptors(pipe: usbManager.USBDevicePipe): boolean {
    let raw: Uint8Array;
    try {
      raw = usbManager.getRawDescriptor(pipe);
    } catch (e) {
      this.log(`getRawDescriptor threw ${errorText(e)}`);
      return false;
    }
    if (!raw) {
      this.log('getRawDescriptor returned nothing');
      return false;
    }
    this.log(`getRawDescriptor: ${raw.length} bytes`);
    for (let offset = 0; offset < raw.length; offset += 32) {
      this.log(`  raw[${offset.toString().padStart(4, '0')}] ${hexBytes(raw, offset, 32)}`);
    }

    const info = parseUvcDescriptors(raw);
    this.info = info;
    this.modes = listModes(info);
    this.log(
      `parsed: bcdUVC=0x${info.bcdUVC.toString(16)} controlInterface=${info.controlInterfaceNumber} ` +
        `streamingInterfaces=${info.streams.length} modes=${this.modes.length}`);
    for (let i = 0; i < info.warnings.length; i++) {
      this.log(`  parse warning: ${info.warnings[i]}`);
    }
    for (let s = 0; s < info.streams.length; s++) {
      const stream = info.streams[s];
      this.log(`  VS interface ${stream.interfaceNumber} endpoint=0x${stream.endpointAddress.toString(16)}`);
      for (let a = 0; a < stream.altSettings.length; a++) {
        const alt = stream.altSettings[a];
        this.log(
          `    alt ${alt.alternateSetting}: endpoint=0x${alt.endpointAddress.toString(16)} ` +
            `attributes=0x${alt.attributes.toString(16)} wMaxPacketSize=${alt.wMaxPacketSize} ` +
            `bytesPerPacket=${alt.bytesPerPacket} isochronous=${alt.isochronous}`);
      }
    }
    for (let m = 0; m < this.modes.length; m++) {
      this.log(`  mode[${m}] ${this.modes[m].label}`);
    }
    if (this.modes.length === 0) {
      this.log('no usable format/frame descriptors found');
      return false;
    }
    return true;
  }

  private claimVideoInterfaces(pipe: usbManager.USBDevicePipe, device: usbManager.USBDevice): boolean {
    const interfaces = this.interfacesOf(device, USB_CLASS_VIDEO);
    const seen: number[] = [];
    let streamingClaimed = false;
    for (let i = 0; i < interfaces.length; i++) {
      const iface = interfaces[i];
      if (seen.indexOf(iface.id) >= 0) {
        continue;
      }
      seen.push(iface.id);
      let result = -1;
      try {
        result = usbManager.claimInterface(pipe, iface, false);
        this.log(`claimInterface(id=${iface.id} subClass=${iface.subClass}, force=false) = ${result}`);
        if (result !== 0) {
          result = usbManager.claimInterface(pipe, iface, true);
          this.log(`claimInterface(id=${iface.id} subClass=${iface.subClass}, force=true) = ${result}`);
        }
      } catch (e) {
        this.log(`claimInterface(id=${iface.id}) threw ${errorText(e)}`);
      }
      if (result === 0) {
        this.claimed.push(iface);
        if (iface.subClass === 2) {
          streamingClaimed = true;
        }
      }
    }
    if (!streamingClaimed) {
      this.log('no VideoStreaming interface could be claimed');
    }
    return streamingClaimed;
  }

  private async control(
    pipe: usbManager.USBDevicePipe, requestType: number, request: number, selector: number,
    interfaceNumber: number, data: Uint8Array,
  ): Promise<number> {
    const params: usbManager.USBDeviceRequestParams = {
      bmRequestType: requestType,
      bRequest: request,
      wValue: selector << 8,
      wIndex: interfaceNumber,
      wLength: data.length,
      data: data
    };
    return usbManager.usbControlTransfer(pipe, params, CONTROL_TIMEOUT_MS);
  }

  /** PROBE, COMMIT and alt setting selection for one mode. */
  private async negotiate(modeIndex: number, forcedAlt: number): Promise<boolean> {
    const pipe = this.pipe;
    const info = this.info;
    if (pipe === null || info === null || modeIndex < 0 || modeIndex >= this.modes.length) {
      this.log(`negotiate: not open or bad mode index ${modeIndex}`);
      return false;
    }
    const mode = this.modes[modeIndex];
    let stream: UvcStreamInterface | null = null;
    for (let s = 0; s < info.streams.length; s++) {
      if (info.streams[s].interfaceNumber === mode.streamInterfaceNumber) {
        stream = info.streams[s];
      }
    }
    if (stream === null) {
      return false;
    }
    this.stream = stream;
    this.log(`negotiate: ${mode.label} on VS interface ${stream.interfaceNumber}`);

    const length = streamCtrlLength(info.bcdUVC);
    const wanted = new UvcStreamCtrl();
    wanted.bmHint = 1;
    wanted.bFormatIndex = mode.formatIndex;
    wanted.bFrameIndex = mode.frameIndex;
    wanted.dwFrameInterval = mode.interval;

    let answer: Uint8Array = new Uint8Array(length);
    let answered = 0;
    try {
      const sent = await this.control(pipe, UVC_REQ_TYPE_SET, UVC_SET_CUR, UVC_VS_PROBE_CONTROL,
        stream.interfaceNumber, encodeStreamCtrl(wanted, length));
      this.log(`SET_CUR(PROBE, ${length} bytes) = ${sent}`);
      if (sent < 0) {
        return false;
      }
      answered = await this.control(pipe, UVC_REQ_TYPE_GET, UVC_GET_CUR, UVC_VS_PROBE_CONTROL,
        stream.interfaceNumber, answer);
      this.log(`GET_CUR(PROBE, ${length} bytes) = ${answered}: ${hexBytes(answer, 0, length)}`);
      if (answered < 0) {
        return false;
      }
    } catch (e) {
      this.log(`PROBE control transfer threw ${errorText(e)}`);
      return false;
    }

    const ctrl = decodeStreamCtrl(answer, answered >= 26 ? answered : length);
    this.log(`  device proposes: ${describeStreamCtrl(ctrl)}`);
    if (ctrl.bFormatIndex !== mode.formatIndex || ctrl.bFrameIndex !== mode.frameIndex) {
      this.log('  WARN: device answered with a different format/frame than requested');
    }
    if (ctrl.dwMaxPayloadTransferSize === 0) {
      this.log('  device reported dwMaxPayloadTransferSize=0; cannot choose an alt setting');
      return false;
    }

    try {
      // Commit exactly the bytes the device proposed.
      const committed = await this.control(pipe, UVC_REQ_TYPE_SET, UVC_SET_CUR, UVC_VS_COMMIT_CONTROL,
        stream.interfaceNumber, answer);
      this.log(`SET_CUR(COMMIT) = ${committed}`);
      if (committed < 0) {
        return false;
      }
    } catch (e) {
      this.log(`COMMIT control transfer threw ${errorText(e)}`);
      return false;
    }

    let alt: UvcAltSetting | null = null;
    if (forcedAlt > 0) {
      for (let a = 0; a < stream.altSettings.length; a++) {
        if (stream.altSettings[a].alternateSetting === forcedAlt && stream.altSettings[a].isochronous) {
          alt = stream.altSettings[a];
        }
      }
      if (alt === null) {
        this.log(`alt setting ${forcedAlt} was requested but is not an isochronous alt of this interface`);
        return false;
      }
      this.log(
        `  alt setting forced to ${forcedAlt} (${alt.bytesPerPacket} bytes per packet, ` +
          `negotiated payload ${ctrl.dwMaxPayloadTransferSize})`);
    } else {
      alt = selectAltSetting(stream.altSettings, ctrl.dwMaxPayloadTransferSize);
    }
    if (alt === null) {
      this.log(
        `no isochronous alt setting carries ${ctrl.dwMaxPayloadTransferSize} bytes per packet ` +
          '(bulk-only cameras are outside this probe)');
      return false;
    }
    this.alt = alt;
    return this.setAlt(pipe, stream.interfaceNumber, alt.alternateSetting);
  }

  private setAlt(pipe: usbManager.USBDevicePipe, interfaceNumber: number, alternateSetting: number): boolean {
    const device = this.device;
    if (device === null) {
      return false;
    }
    const interfaces = this.interfacesOf(device, USB_CLASS_VIDEO);
    let target: usbManager.USBInterface | null = null;
    let base: usbManager.USBInterface | null = null;
    for (let i = 0; i < interfaces.length; i++) {
      if (interfaces[i].id !== interfaceNumber) {
        continue;
      }
      if (base === null) {
        base = interfaces[i];
      }
      if (interfaces[i].alternateSetting === alternateSetting) {
        target = interfaces[i];
      }
    }
    if (target === null && base !== null) {
      this.log(`  getDevices() lists no entry for alt ${alternateSetting}; passing a copy with that alternateSetting`);
      const copy: usbManager.USBInterface = {
        id: base.id,
        protocol: base.protocol,
        clazz: base.clazz,
        subClass: base.subClass,
        alternateSetting: alternateSetting,
        name: base.name,
        endpoints: base.endpoints
      };
      target = copy;
    }
    if (target === null) {
      this.log(`  interface ${interfaceNumber} not present in getDevices()`);
      return false;
    }
    try {
      const result = usbManager.setInterface(pipe, target);
      this.log(`setInterface(id=${interfaceNumber}, alt=${alternateSetting}) = ${result}`);
      if (result === 0) {
        this.streamingAltSet = alternateSetting !== 0;
      }
      return result === 0;
    } catch (e) {
      this.log(`setInterface threw ${errorText(e)}`);
      return false;
    }
  }

  private async ensureNegotiated(options: UvcProbeOptions): Promise<boolean> {
    if (!(await this.open())) {
      return false;
    }
    const modeIndex = options.modeIndex >= 0 ? options.modeIndex : this.defaultMode(options.width, options.height);
    return this.negotiate(modeIndex, options.altSetting);
  }

  /** Negotiates and receives the stream; resolves with the verdict when the run ends. */
  async run(options: UvcProbeOptions): Promise<boolean> {
    if (this.isStreaming()) {
      this.log('run: already streaming');
      return false;
    }
    if (!(await this.ensureNegotiated(options))) {
      return false;
    }
    const pipe = this.pipe;
    const stream = this.stream;
    let alt = this.alt;
    if (pipe === null || stream === null || alt === null) {
      return false;
    }
    // With an automatic alt setting the first attempt only finds out how many
    // bytes per packet the system reserves; a forced one runs as requested.
    let pump = this.createPump(pipe, alt, options, options.altSetting === 0);
    let passed = await pump.start();
    if (pump.stoppedForCapacityMismatch()) {
      const capacity = pump.reportedPacketBytes();
      const fitting = selectAltSettingWithin(stream.altSettings, capacity);
      this.idleStream();
      if (fitting === null) {
        this.log(`no isochronous alt setting fits the ${capacity} bytes per packet the system reserves`);
        this.pump = null;
        return false;
      }
      this.log(
        `alt ${alt.alternateSetting} sends up to ${alt.bytesPerPacket} bytes per packet but the system reserves ` +
          `${capacity}; restarting on alt ${fitting.alternateSetting} (${fitting.bytesPerPacket} bytes per packet)`);
      alt = fitting;
      this.alt = fitting;
      if (!this.setAlt(pipe, stream.interfaceNumber, fitting.alternateSetting)) {
        this.pump = null;
        return false;
      }
      pump = this.createPump(pipe, fitting, options, false);
      passed = await pump.start();
    }
    this.pump = null;
    this.idleStream();
    return passed;
  }

  private createPump(
    pipe: usbManager.USBDevicePipe, alt: UvcAltSetting, options: UvcProbeOptions, stopOnCapacityMismatch: boolean,
  ): UvcIsoPump {
    const config = new UvcIsoPumpConfig();
    config.endpointAddress = alt.endpointAddress;
    config.bytesPerPacket = alt.bytesPerPacket;
    config.numTransfers = options.numTransfers;
    config.packetsPerTransfer = options.packetsPerTransfer;
    config.transferFlags = options.transferFlags;
    config.transferTimeoutMs = options.transferTimeoutMs;
    config.durationSec = options.durationSec;
    config.minFps = options.minFps;
    config.stopOnCapacityMismatch = stopOnCapacityMismatch;
    const pump = new UvcIsoPump(pipe, config, this.log, this.onStats);
    this.pump = pump;
    return pump;
  }

  stop(): void {
    if (this.pump !== null) {
      this.pump.stop('stopped by user');
    }
  }

  /**
   * Submits single isochronous transfers of growing packet counts to find out
   * whether usbManager caps the length of one transfer.
   */
  async probeTransferSizes(options: UvcProbeOptions): Promise<void> {
    if (this.isStreaming()) {
      this.log('size test: stop the stream first');
      return;
    }
    if (!(await this.ensureNegotiated(options))) {
      return;
    }
    const pipe = this.pipe;
    const alt = this.alt;
    if (pipe === null || alt === null) {
      return;
    }
    this.log(`size test: bytes/packet=${alt.bytesPerPacket} flags=${options.transferFlags}`);
    for (let i = 0; i < SIZE_TEST_PACKET_COUNTS.length; i++) {
      const packets = SIZE_TEST_PACKET_COUNTS[i];
      const outcome = await this.singleTransfer(pipe, alt, packets, options);
      this.log(`  packets=${packets} length=${packets * alt.bytesPerPacket}: ${outcome}`);
    }
    this.idleStream();
  }

  private singleTransfer(
    pipe: usbManager.USBDevicePipe, alt: UvcAltSetting, packets: number, options: UvcProbeOptions,
  ): Promise<string> {
    return new Promise<string>((resolve: (outcome: string) => void) => {
      let settled = false;
      const length = packets * alt.bytesPerPacket;
      const buffer = new Uint8Array(length);
      const params: usbManager.UsbDataTransferParams = {
        devPipe: pipe,
        flags: transferFlagsFromNumber(options.transferFlags),
        endpoint: alt.endpointAddress,
        type: usbManager.UsbEndpointTransferType.TRANSFER_TYPE_ISOCHRONOUS,
        timeout: options.transferTimeoutMs,
        length: length,
        callback: (err: BusinessError, data: usbManager.SubmitTransferCallback): void => {
          if (settled) {
            return;
          }
          settled = true;
          if (err && err.code) {
            resolve(`callback error code=${err.code} message=${err.message}`);
          } else if (data) {
            const descs = data.isoPacketDescs ? data.isoPacketDescs.length : -1;
            resolve(`status=${data.status} actualLength=${data.actualLength} isoPacketDescs=${descs}`);
          } else {
            resolve('callback without error or data');
          }
        },
        userData: new Uint8Array(0),
        buffer: buffer,
        isoPacketCount: packets
      };
      try {
        usbManager.usbSubmitTransfer(params);
      } catch (e) {
        settled = true;
        resolve(`usbSubmitTransfer threw ${errorText(e)}`);
        return;
      }
      setTimeout(() => {
        if (settled) {
          return;
        }
        settled = true;
        try {
          usbManager.usbCancelTransfer(params);
        } catch (e) {
          resolve(`no callback within ${SIZE_TEST_TIMEOUT_MS}ms; usbCancelTransfer threw ${errorText(e)}`);
          return;
        }
        resolve(`no callback within ${SIZE_TEST_TIMEOUT_MS}ms; cancelled`);
      }, SIZE_TEST_TIMEOUT_MS);
    });
  }

  /** Puts the streaming interface back on alt 0 so the camera stops sending. */
  private idleStream(): void {
    const pipe = this.pipe;
    const stream = this.stream;
    if (pipe !== null && stream !== null && this.streamingAltSet) {
      this.setAlt(pipe, stream.interfaceNumber, 0);
      this.streamingAltSet = false;
    }
  }

  close(): void {
    this.stop();
    const pipe = this.pipe;
    if (pipe === null) {
      return;
    }
    this.idleStream();
    for (let i = 0; i < this.claimed.length; i++) {
      try {
        const result = usbManager.releaseInterface(pipe, this.claimed[i]);
        this.log(`releaseInterface(id=${this.claimed[i].id}) = ${result}`);
      } catch (e) {
        this.log(`releaseInterface threw ${errorText(e)}`);
      }
    }
    try {
      this.log(`closePipe = ${usbManager.closePipe(pipe)}`);
    } catch (e) {
      this.log(`closePipe threw ${errorText(e)}`);
    }
    this.claimed = [];
    this.pipe = null;
    this.device = null;
    this.info = null;
    this.modes = [];
    this.stream = null;
    this.alt = null;
  }
}
