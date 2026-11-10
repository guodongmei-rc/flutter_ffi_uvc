// One UVC camera session: enumerate, authorise, open, negotiate a mode and run
// the preview.
//
// usbManager is only used to find the camera, obtain the user's permission and
// get a file descriptor for it. From there the device is driven through that
// descriptor by the native code in src/main/cpp: interface claims, the
// PROBE/COMMIT requests, and the stream itself (transfers, frame assembly,
// JPEG decoding, drawing), none of which runs on this thread. The reasons for
// leaving usbManager's own transfer API are in src/main/cpp/uvc_stream.h; the
// measurements behind them were taken with the stage 0 probe (../probe).
//
// This is the HarmonyOS counterpart of the shared native session the Android
// side keeps in libuvc, and the request sequence follows libuvc. Only MJPEG
// over isochronous endpoints is handled so far.

import { usbManager, BusinessError } from '@kit.BasicServicesKit';
import { fileIo } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import native from 'libflutter_ffi_uvc_preview.so';
import {
  UvcDeviceInfo, UvcMode, UvcStreamInterface, UVC_KIND_MJPEG,
  parseUvcDescriptors, listModes, selectAltSetting, describeInterfaces,
} from '../uvc/UvcDescriptor';
import { UacCaptureInfo, probeUacCapture, describeUacCapture } from '../uvc/UacDescriptor';
import {
  UvcStreamCtrl, streamCtrlLength, encodeStreamCtrl, decodeStreamCtrl, describeStreamCtrl,
  UVC_SET_CUR, UVC_GET_CUR, UVC_REQ_TYPE_SET, UVC_REQ_TYPE_GET,
  UVC_VS_PROBE_CONTROL, UVC_VS_COMMIT_CONTROL,
} from '../uvc/UvcStreamControl';

const DOMAIN: number = 0x0001;
const TAG: string = 'FlutterFfiUvc';

const USB_CLASS_VIDEO: number = 14;
const CONTROL_TIMEOUT_MS: number = 1000;
/** Frames are never allowed to grow past this, whatever the camera reports. */
const MIN_FRAME_LIMIT_BYTES: number = 4 * 1024 * 1024;
const VERIFY_POLL_MS: number = 20;
const NO_WINDOW: bigint = BigInt(0);

const ENODEV: number = 19;

// Indices into the array returned by the native streamStats(); see index.d.ts.
const STAT_RUNNING: number = 0;
const STAT_STOP_ERRNO: number = 1;
const STAT_URBS: number = 2;
const STAT_PACKETS: number = 3;
const STAT_BAD_PACKETS: number = 4;
const STAT_EMPTY_PACKETS: number = 5;
const STAT_SUBMIT_FAILURES: number = 6;
const STAT_FRAMES: number = 7;
const STAT_ENDS_BY_EOF: number = 8;
const STAT_ENDS_BY_FID_TOGGLE: number = 9;
const STAT_INCOMPLETE_FRAMES: number = 10;
const STAT_LAST_FRAME_BYTES: number = 11;
const STAT_INVALID_JPEG: number = 12;
const STAT_SHOWN: number = 13;
const STAT_DECODE_FAILURES: number = 14;
const STAT_DROPPED: number = 15;
const STAT_DECODE_MS: number = 16;
const STAT_SHOW_MS: number = 17;
const STAT_FIRST_FRAME_MS: number = 18;
const STAT_GAP_AVG_MS: number = 19;
const STAT_GAP_P95_MS: number = 20;
const STAT_GAP_MAX_MS: number = 21;
const STAT_ELAPSED_MS: number = 22;
const STAT_INPUT_GAPS: number = 23;
const STAT_SHOWN_GAPS: number = 28;
const STAT_GAP_BUCKETS: number = 5;
const STAT_YUV: number = 33;
const STAT_COUNT: number = 34;

// Error codes shared with the Dart API (uvc_error_t in libuvc).
export const UVC_OK: number = 0;
export const UVC_ERROR_IO: number = -1;
export const UVC_ERROR_ACCESS: number = -3;
export const UVC_ERROR_NO_DEVICE: number = -4;
export const UVC_ERROR_BUSY: number = -6;
export const UVC_ERROR_NOT_SUPPORTED: number = -12;
export const UVC_ERROR_INVALID_DEVICE: number = -50;
export const UVC_ERROR_INVALID_MODE: number = -51;

/** uvc_frame_format value the Dart API uses for MJPEG modes. */
export const UVC_FRAME_FORMAT_MJPEG: number = 7;

export interface UvcDeviceSummary {
  deviceId: number;
  deviceName: string;
  vendorId: number;
  productId: number;
  productName: string;
  manufacturerName: string;
  serialNumber: string;
  hasPermission: boolean;
}

/** PCM format of the device's UAC microphone, as `queryAudioInterface` reports it. */
export interface UvcAudioSummary {
  sampleRate: number;
  channels: number;
  bitsPerSample: number;
}

export interface UvcModeSummary {
  format: number;
  formatName: string;
  width: number;
  height: number;
  fps: number;
}

export interface UvcPreviewRequest {
  format: number;
  width: number;
  height: number;
  fps: number;
  consecutiveValidFrames: number;
  timeoutMs: number;
  /** Only require one shown frame instead of consecutive valid ones. */
  sequenceOnly: boolean;
}

export interface UvcPreviewOutcome {
  success: boolean;
  validFrameCount: number;
  consecutiveValidFrames: number;
  errorCount: number;
  elapsedMs: number;
  lastError: string;
  nativeErrorCode: number;
  /**
   * Frame rate the device committed to, which may differ from the one
   * requested: UVC lets the device rewrite the PROBE values and we commit
   * what it answered. 0 when nothing was negotiated.
   */
  negotiatedFps: number;
}

export interface UvcSessionStats {
  sequence: number;
  inputFrameCount: number;
  deliveredFrameCount: number;
  decodeSuccessCount: number;
  decodeFailureCount: number;
  /** Frames replaced by a newer one while the decoders were busy. */
  callbackLockDropCount: number;
  /** Frames dropped because the transport lost or flagged one of their packets. */
  undersizedFrameCount: number;
  invalidMjpegCount: number;
  inputFps: number;
  deliveredFps: number;
  avgInterFrameGapMs: number;
  p95InterFrameGapMs: number;
  maxInterFrameGapMs: number;
  firstFrameLatencyMs: number;
  elapsedMs: number;
}

export interface UvcSessionListener {
  onStreamError(message: string): void;

  /** The preview ended without a stopPreview() call, e.g. the camera was unplugged. */
  onPreviewStopped(reason: string): void;
}

function errorText(e: Object): string {
  const err = e as BusinessError;
  return `code=${err.code} message=${err.message}`;
}

/** Describes a negative error code returned by the native side. */
function errnoText(result: number): string {
  switch (-result) {
    case 20:
      return 'a recording is already running';
    case 21:
      return 'the video encoder failed';
    case 22:
      return 'the MP4 muxer failed';
    case 23:
      return 'the output file could not be created';
    case 24:
      return 'no frame has been decoded yet';
    case 25:
      return 'the decoded frames are not NV12, which the encoder needs';
    case 12:
      return 'out of USB transfer memory (ENOMEM)';
    case 13:
      return 'access denied (EACCES)';
    case 16:
      return 'device or interface busy (EBUSY)';
    case ENODEV:
      return 'device disconnected (ENODEV)';
    case 110:
      return 'timed out (ETIMEDOUT)';
    default:
      return `errno ${-result}`;
  }
}

function deviceIdOf(device: usbManager.USBDevice): number {
  return device.busNum * 1000 + device.devAddress;
}

/** Whether the device exposes a USB video class interface. */
export function hasVideoInterface(device: usbManager.USBDevice): boolean {
  const configs = device.configs;
  if (!configs) {
    return false;
  }
  for (let c = 0; c < configs.length; c++) {
    const interfaces = configs[c].interfaces;
    if (!interfaces) {
      continue;
    }
    for (let i = 0; i < interfaces.length; i++) {
      if (interfaces[i].clazz === USB_CLASS_VIDEO) {
        return true;
      }
    }
  }
  return false;
}

/** The device as the Dart side describes it; `hasPermission` is looked up when `askRight` is set. */
export function summarizeDevice(device: usbManager.USBDevice, askRight: boolean): UvcDeviceSummary {
  let hasRight = false;
  if (askRight) {
    try {
      hasRight = usbManager.hasRight(device.name);
    } catch (e) {
      hasRight = false;
    }
  }
  const summary: UvcDeviceSummary = {
    deviceId: deviceIdOf(device),
    deviceName: device.name,
    vendorId: device.vendorId,
    productId: device.productId,
    productName: device.productName,
    manufacturerName: device.manufacturerName,
    serialNumber: device.serial,
    hasPermission: hasRight
  };
  return summary;
}

function fpsOf(mode: UvcMode): number {
  return mode.interval > 0 ? Math.round(10000000 / mode.interval) : 0;
}

/** File name for a capture, from the current time: UVC_20261110_093012_345. */
function captureName(): string {
  const now = new Date();
  const pad = (value: number, width: number): string => value.toString().padStart(width, '0');
  return `UVC_${now.getFullYear()}${pad(now.getMonth() + 1, 2)}${pad(now.getDate(), 2)}_` +
    `${pad(now.getHours(), 2)}${pad(now.getMinutes(), 2)}${pad(now.getSeconds(), 2)}_${pad(now.getMilliseconds(), 3)}`;
}

function ensureDirectory(path: string): void {
  if (!fileIo.accessSync(path)) {
    fileIo.mkdirSync(path, true);
  }
}

function emptyStats(): number[] {
  const stats: number[] = [];
  for (let i = 0; i < STAT_COUNT; i++) {
    stats.push(0);
  }
  return stats;
}

export class UvcCameraSession {
  private listener: UvcSessionListener;

  private device: usbManager.USBDevice | null = null;
  private pipe: usbManager.USBDevicePipe | null = null;
  /** Descriptor from usbManager.getFileDescriptor(); owned and closed here. */
  private fd: number = -1;
  private info: UvcDeviceInfo | null = null;
  private modes: UvcMode[] = [];
  /** UAC capture interface of the open device; null when it has no microphone. */
  private audio: UacCaptureInfo | null = null;
  private claimedInterfaces: number[] = [];

  private stream: UvcStreamInterface | null = null;
  private streamingAltSet: boolean = false;
  /** Native stream id; 0 while nothing is streaming. */
  private streamId: number = 0;
  private window: bigint = NO_WINDOW;
  /** Mode of the running preview, for the recording's frame rate. */
  private previewMode: UvcMode | null = null;
  /** dwFrameInterval the device committed to for the current/last preview. */
  private negotiatedInterval: number = 0;
  /** Path of the MP4 being recorded; '' when not recording. */
  private recordingPath: string = '';

  /** Bumped whenever the preview is started, stopped or the device is closed. */
  private epoch: number = 0;
  private previewing: boolean = false;
  private errorMessage: string = '';

  /** Counters of the running stream, or of the last one after it stopped. */
  private lastStats: number[] = emptyStats();
  /** Frames shown by earlier streams, so the sequence never goes backwards. */
  private sequenceBase: number = 0;
  private reportedFailures: number = 0;
  private loggedStats: number[] = emptyStats();

  constructor(listener: UvcSessionListener) {
    this.listener = listener;
  }

  lastError(): string {
    return this.errorMessage;
  }

  isOpen(): boolean {
    return this.pipe !== null;
  }

  isPreviewing(): boolean {
    return this.previewing;
  }

  // ---------------------------------------------------------------------------
  // Devices
  // ---------------------------------------------------------------------------

  listDevices(): UvcDeviceSummary[] {
    const out: UvcDeviceSummary[] = [];
    const devices = this.videoDevices();
    for (let i = 0; i < devices.length; i++) {
      out.push(summarizeDevice(devices[i], true));
    }
    return out;
  }

  private videoDevices(): usbManager.USBDevice[] {
    const out: usbManager.USBDevice[] = [];
    let devices: Array<Readonly<usbManager.USBDevice>>;
    try {
      devices = usbManager.getDevices();
    } catch (e) {
      this.errorMessage = `getDevices failed: ${errorText(e)}`;
      return out;
    }
    if (!devices) {
      return out;
    }
    for (let i = 0; i < devices.length; i++) {
      const device = devices[i] as usbManager.USBDevice;
      if (hasVideoInterface(device)) {
        out.push(device);
      }
    }
    return out;
  }

  /** Opens the device with `deviceId` from listDevices(). Resolves with UVC_OK or an error code. */
  async open(deviceId: number): Promise<number> {
    this.close();
    const epoch = this.epoch;
    let device: usbManager.USBDevice | null = null;
    const devices = this.videoDevices();
    for (let i = 0; i < devices.length; i++) {
      if (deviceIdOf(devices[i]) === deviceId) {
        device = devices[i];
      }
    }
    if (device === null) {
      return this.failWith(UVC_ERROR_NO_DEVICE, `no UVC device with id ${deviceId}`);
    }

    try {
      if (!usbManager.hasRight(device.name)) {
        const granted = await usbManager.requestRight(device.name);
        if (epoch !== this.epoch) {
          return this.failWith(UVC_ERROR_BUSY, 'open was superseded by another request');
        }
        if (!granted) {
          return this.failWith(UVC_ERROR_ACCESS, 'USB access was not granted');
        }
      }
    } catch (e) {
      return this.failWith(UVC_ERROR_ACCESS, `requesting USB access failed: ${errorText(e)}`);
    }

    let pipe: usbManager.USBDevicePipe;
    try {
      pipe = usbManager.connectDevice(device);
    } catch (e) {
      return this.failWith(UVC_ERROR_IO, `connectDevice failed: ${errorText(e)}`);
    }
    if (!pipe) {
      return this.failWith(UVC_ERROR_IO, 'connectDevice returned no pipe');
    }
    this.device = device;
    this.pipe = pipe;

    let raw: Uint8Array;
    try {
      this.fd = usbManager.getFileDescriptor(pipe);
      raw = usbManager.getRawDescriptor(pipe);
    } catch (e) {
      this.close();
      return this.failWith(UVC_ERROR_IO, `reading the device through the USB service failed: ${errorText(e)}`);
    }
    if (this.fd < 0 || !raw) {
      this.close();
      return this.failWith(UVC_ERROR_NOT_SUPPORTED, 'the system did not provide a descriptor for the device');
    }
    const info = parseUvcDescriptors(raw);
    const interfaces = describeInterfaces(raw);
    for (let i = 0; i < interfaces.length; i++) {
      hilog.info(DOMAIN, TAG, 'descriptor: %{public}s', interfaces[i]);
    }
    const all = listModes(info);
    const usable: UvcMode[] = [];
    for (let i = 0; i < all.length; i++) {
      if (all[i].kind === UVC_KIND_MJPEG && all[i].width > 0 && all[i].height > 0 && all[i].interval > 0) {
        usable.push(all[i]);
      }
    }
    if (usable.length === 0) {
      this.close();
      return this.failWith(
        UVC_ERROR_INVALID_DEVICE,
        `the device reports ${all.length} mode(s), none of them MJPEG; other formats are not supported yet`);
    }
    this.info = info;
    this.modes = usable;
    this.audio = probeUacCapture(raw);
    if (this.audio !== null) {
      hilog.info(DOMAIN, TAG, 'uac capture: %{public}s', describeUacCapture(this.audio));
    } else {
      hilog.info(DOMAIN, TAG, 'uac capture: no AudioStreaming interface; recordings stay video-only');
    }

    // The interfaces are claimed on our own descriptor, never through
    // usbManager: a claim made by the USB service would lock this one out.
    const wanted: number[] = [];
    if (info.controlInterfaceNumber >= 0) {
      wanted.push(info.controlInterfaceNumber);
    }
    for (let s = 0; s < info.streams.length; s++) {
      wanted.push(info.streams[s].interfaceNumber);
    }
    for (let i = 0; i < wanted.length; i++) {
      const result = native.usbClaimInterface(this.fd, wanted[i]);
      if (result < 0) {
        this.close();
        return this.failWith(UVC_ERROR_BUSY, `claiming interface ${wanted[i]} failed: ${errnoText(result)}`);
      }
      this.claimedInterfaces.push(wanted[i]);
    }
    hilog.info(DOMAIN, TAG, 'opened %{public}s: bcdUVC=0x%{public}s, %{public}d MJPEG mode(s)',
      device.name, info.bcdUVC.toString(16), usable.length);
    this.errorMessage = '';
    return UVC_OK;
  }

  supportedModes(): UvcModeSummary[] {
    const out: UvcModeSummary[] = [];
    for (let i = 0; i < this.modes.length; i++) {
      const mode = this.modes[i];
      const summary: UvcModeSummary = {
        format: UVC_FRAME_FORMAT_MJPEG,
        formatName: UVC_KIND_MJPEG,
        width: mode.width,
        height: mode.height,
        fps: fpsOf(mode)
      };
      out.push(summary);
    }
    return out;
  }

  /**
   * The microphone format Android's probe would pick, or null when the open
   * device has no USB Audio Class capture interface (or nothing is open).
   * Capturing from it is not implemented on HarmonyOS yet; this only answers
   * whether the camera has one.
   */
  audioInterface(): UvcAudioSummary | null {
    const audio = this.audio;
    if (this.fd < 0 || audio === null) {
      return null;
    }
    const summary: UvcAudioSummary = {
      sampleRate: audio.sampleRate,
      channels: audio.channels,
      bitsPerSample: audio.bitsPerSample
    };
    return summary;
  }

  close(): void {
    this.stopPreview();
    if (this.fd >= 0) {
      for (let i = 0; i < this.claimedInterfaces.length; i++) {
        native.usbReleaseInterface(this.fd, this.claimedInterfaces[i]);
      }
      native.usbClose(this.fd);
    }
    const pipe = this.pipe;
    if (pipe !== null) {
      try {
        usbManager.closePipe(pipe);
      } catch (e) {
        // The device may already be gone.
      }
    }
    this.claimedInterfaces = [];
    this.fd = -1;
    this.pipe = null;
    this.device = null;
    this.info = null;
    this.modes = [];
    this.audio = null;
    this.stream = null;
  }

  // ---------------------------------------------------------------------------
  // Preview
  // ---------------------------------------------------------------------------

  /** The window frames are drawn into; NO_WINDOW (0n) decodes without drawing. */
  setPreviewWindow(window: bigint): void {
    this.window = window;
    if (this.streamId > 0) {
      native.streamSetWindow(this.streamId, window);
    }
  }

  /**
   * Starts streaming `request`'s mode and resolves once enough frames were
   * decoded and shown, or with a failure once the timeout elapses. A failed
   * start leaves the preview stopped.
   */
  async startPreview(request: UvcPreviewRequest): Promise<UvcPreviewOutcome> {
    this.stopPreview();
    this.epoch++;
    const epoch = this.epoch;
    const startedAt = Date.now();
    this.lastStats = emptyStats();
    this.loggedStats = emptyStats();
    this.reportedFailures = 0;

    const info = this.info;
    const fd = this.fd;
    if (fd < 0 || info === null) {
      return this.outcome(false, startedAt, UVC_ERROR_NO_DEVICE, 'no device is open');
    }
    let mode: UvcMode | null = null;
    for (let i = 0; i < this.modes.length; i++) {
      const candidate = this.modes[i];
      if (request.format === UVC_FRAME_FORMAT_MJPEG && candidate.width === request.width &&
        candidate.height === request.height && fpsOf(candidate) === request.fps) {
        mode = candidate;
        break;
      }
    }
    if (mode === null) {
      return this.outcome(false, startedAt, UVC_ERROR_INVALID_MODE,
        `mode ${request.width}x${request.height}@${request.fps} (format ${request.format}) is not available`);
    }
    let stream: UvcStreamInterface | null = null;
    for (let s = 0; s < info.streams.length; s++) {
      if (info.streams[s].interfaceNumber === mode.streamInterfaceNumber) {
        stream = info.streams[s];
      }
    }
    if (stream === null) {
      return this.outcome(false, startedAt, UVC_ERROR_INVALID_MODE, 'the mode has no streaming interface');
    }
    this.stream = stream;

    this.negotiatedInterval = 0;
    const ctrl = this.negotiate(fd, info, stream, mode);
    if (ctrl === null) {
      return this.outcome(false, startedAt, UVC_ERROR_IO, this.errorMessage);
    }
    this.negotiatedInterval = ctrl.dwFrameInterval;
    const alt = selectAltSetting(stream.altSettings, ctrl.dwMaxPayloadTransferSize);
    if (alt === null) {
      return this.outcome(false, startedAt, UVC_ERROR_NOT_SUPPORTED,
        `no isochronous alt setting carries ${ctrl.dwMaxPayloadTransferSize} bytes per packet ` +
          '(bulk cameras are not supported yet)');
    }
    const altResult = native.usbSetInterface(fd, stream.interfaceNumber, alt.alternateSetting);
    if (altResult < 0) {
      return this.outcome(false, startedAt, UVC_ERROR_IO,
        `selecting alt setting ${alt.alternateSetting} failed: ${errnoText(altResult)}`);
    }
    this.streamingAltSet = true;

    const frameLimit = Math.max(ctrl.dwMaxVideoFrameSize, mode.maxVideoFrameBufferSize, MIN_FRAME_LIMIT_BYTES);
    const streamId = native.streamStart(fd, alt.endpointAddress, alt.bytesPerPacket, frameLimit, this.window);
    if (streamId <= 0) {
      this.idleStream();
      return this.outcome(false, startedAt, UVC_ERROR_IO, `starting the stream failed: ${errnoText(streamId)}`);
    }
    this.streamId = streamId;
    this.previewMode = mode;
    this.previewing = true;
    hilog.info(DOMAIN, TAG, 'streaming on alt %{public}d (%{public}d bytes per packet)',
      alt.alternateSetting, alt.bytesPerPacket);

    const needed = Math.max(1, request.consecutiveValidFrames);
    const verified = await this.waitForFrames(streamId, epoch, needed, request.sequenceOnly,
      Math.max(1, request.timeoutMs));
    if (epoch !== this.epoch) {
      return this.outcome(false, startedAt, UVC_ERROR_BUSY, 'the preview was stopped while it was starting');
    }
    if (!verified) {
      const stats = native.streamStats(streamId);
      let message = `no valid frame within ${request.timeoutMs} ms`;
      if (stats.length >= STAT_COUNT) {
        message = stats[STAT_RUNNING] === 0
          ? `the stream stopped: ${errnoText(-stats[STAT_STOP_ERRNO])}`
          : `${message} (received ${stats[STAT_FRAMES]}, shown ${stats[STAT_SHOWN]}, ` +
            `undecodable ${stats[STAT_DECODE_FAILURES] + stats[STAT_INVALID_JPEG]})`;
      }
      const result = this.outcome(false, startedAt, UVC_OK, message);
      this.stopPreview();
      this.errorMessage = message;
      return result;
    }
    hilog.info(DOMAIN, TAG, 'preview running: %{public}dx%{public}d@%{public}d after %{public}d ms',
      mode.width, mode.height, fpsOf(mode), Date.now() - startedAt);
    return this.outcome(true, startedAt, UVC_OK, '');
  }

  stopPreview(): void {
    this.epoch++;
    if (this.isRecording()) {
      this.stopRecording();
    }
    if (this.streamId > 0) {
      this.lastStats = this.readStats();
      native.streamStop(this.streamId);
      this.streamId = 0;
      this.sequenceBase += this.lastStats[STAT_SHOWN];
    }
    this.idleStream();
    this.previewing = false;
  }

  // ---------------------------------------------------------------------------
  // Capture
  // ---------------------------------------------------------------------------

  /** True while an MP4 is being recorded. */
  isRecording(): boolean {
    return this.recordingPath.length > 0;
  }

  /**
   * Saves the latest camera frame, as the camera sent it, as a JPEG file in
   * `directory`. Returns the path, or '' with lastError() set.
   */
  takePicture(directory: string): string {
    if (this.streamId <= 0) {
      this.errorMessage = 'no preview is running';
      return '';
    }
    const jpeg = native.streamLatestJpeg(this.streamId);
    if (jpeg === undefined) {
      this.errorMessage = 'no frame has been received yet';
      return '';
    }
    const path = `${directory}/${captureName()}.jpg`;
    try {
      ensureDirectory(directory);
      const file = fileIo.openSync(path, fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.WRITE_ONLY);
      try {
        fileIo.writeSync(file.fd, jpeg);
      } finally {
        fileIo.closeSync(file);
      }
    } catch (e) {
      this.errorMessage = `writing ${path} failed: ${errorText(e)}`;
      return '';
    }
    return path;
  }

  /**
   * Starts recording the preview to an MP4 file in `directory`. Returns the
   * path, or '' with lastError() set. `fps` 0 means the mode's frame rate.
   */
  startRecording(directory: string, fps: number, bitRate: number): string {
    if (this.streamId <= 0 || this.previewMode === null) {
      this.errorMessage = 'no preview is running';
      return '';
    }
    if (this.isRecording()) {
      this.errorMessage = 'a recording is already running';
      return '';
    }
    // Default to the rate the device actually committed to, not the one the
    // mode asked for; the camera may have rewritten it during PROBE.
    const streamingFps = this.negotiatedInterval > 0
      ? Math.round(10000000 / this.negotiatedInterval) : fpsOf(this.previewMode);
    const frameRate = fps > 0 ? fps : streamingFps;
    const mode = this.previewMode;
    // About 0.1 bit per pixel per frame; roughly what phone cameras use.
    const rate = bitRate > 0 ? bitRate : Math.max(2000000, Math.round(mode.width * mode.height * frameRate / 10));
    const path = `${directory}/${captureName()}.mp4`;
    try {
      ensureDirectory(directory);
    } catch (e) {
      this.errorMessage = `creating ${directory} failed: ${errorText(e)}`;
      return '';
    }
    const result = native.recordStart(this.streamId, path, frameRate, rate);
    if (result < 0) {
      this.errorMessage = `starting the recording failed: ${errnoText(result)}`;
      return '';
    }
    this.recordingPath = path;
    hilog.info(DOMAIN, TAG, 'recording to %{public}s at %{public}d fps, %{public}d bps', path, frameRate, rate);
    return path;
  }

  /** Finishes the recording. Returns its path, or '' with lastError() set. */
  stopRecording(): string {
    const path = this.recordingPath;
    this.recordingPath = '';
    if (path.length === 0) {
      this.errorMessage = 'no recording is running';
      return '';
    }
    const frames = this.streamId > 0 ? native.recordStop(this.streamId) : -24;
    if (frames < 0) {
      this.errorMessage = `the recording failed: ${errnoText(frames)}`;
      return '';
    }
    hilog.info(DOMAIN, TAG, 'recorded %{public}d frames to %{public}s', frames, path);
    return path;
  }

  /**
   * Notices a stream that ended on its own and frames that failed to decode,
   * and reports them to the listener. Called periodically while previewing.
   */
  poll(): void {
    if (this.streamId <= 0 || !this.previewing) {
      return;
    }
    const stats = this.readStats();
    const failures = stats[STAT_DECODE_FAILURES] + stats[STAT_INVALID_JPEG];
    if (failures > this.reportedFailures) {
      const message = `${failures - this.reportedFailures} frame(s) could not be decoded`;
      this.reportedFailures = failures;
      this.errorMessage = message;
      this.listener.onStreamError(message);
    }
    if (stats[STAT_RUNNING] === 0) {
      const reason = `the stream stopped: ${errnoText(-stats[STAT_STOP_ERRNO])}`;
      hilog.warn(DOMAIN, TAG, '%{public}s', reason);
      this.stopPreview();
      this.errorMessage = reason;
      this.listener.onStreamError(reason);
      this.listener.onPreviewStopped(reason);
    }
  }

  /** PROBE and COMMIT for one mode; returns what the device committed to, or null. */
  private negotiate(fd: number, info: UvcDeviceInfo, stream: UvcStreamInterface, mode: UvcMode): UvcStreamCtrl | null {
    const length = streamCtrlLength(info.bcdUVC);
    const wanted = new UvcStreamCtrl();
    wanted.bmHint = 1;
    wanted.bFormatIndex = mode.formatIndex;
    wanted.bFrameIndex = mode.frameIndex;
    wanted.dwFrameInterval = mode.interval;

    const request = encodeStreamCtrl(wanted, length);
    const sent = native.usbControlTransfer(fd, UVC_REQ_TYPE_SET, UVC_SET_CUR, UVC_VS_PROBE_CONTROL << 8,
      stream.interfaceNumber, request.buffer as ArrayBuffer, CONTROL_TIMEOUT_MS);
    if (sent < 0) {
      this.errorMessage = `SET_CUR(PROBE) failed: ${errnoText(sent)}`;
      return null;
    }
    const answer: Uint8Array = new Uint8Array(length);
    const answered = native.usbControlTransfer(fd, UVC_REQ_TYPE_GET, UVC_GET_CUR, UVC_VS_PROBE_CONTROL << 8,
      stream.interfaceNumber, answer.buffer as ArrayBuffer, CONTROL_TIMEOUT_MS);
    if (answered < 0) {
      this.errorMessage = `GET_CUR(PROBE) failed: ${errnoText(answered)}`;
      return null;
    }
    const ctrl = decodeStreamCtrl(answer, answered >= 26 ? answered : length);
    hilog.info(DOMAIN, TAG, 'requested fmt=%{public}d frame=%{public}d interval=%{public}d; negotiated %{public}s',
      mode.formatIndex, mode.frameIndex, mode.interval, describeStreamCtrl(ctrl));
    if (ctrl.dwFrameInterval !== mode.interval) {
      hilog.warn(DOMAIN, TAG, 'device kept interval %{public}d instead of requested %{public}d (%{public}d fps)',
        ctrl.dwFrameInterval, mode.interval, fpsOf(mode));
    }
    if (ctrl.dwMaxPayloadTransferSize === 0) {
      this.errorMessage = 'the device reported dwMaxPayloadTransferSize=0';
      return null;
    }
    // Commit exactly the bytes the device proposed.
    const committed = native.usbControlTransfer(fd, UVC_REQ_TYPE_SET, UVC_SET_CUR, UVC_VS_COMMIT_CONTROL << 8,
      stream.interfaceNumber, answer.buffer as ArrayBuffer, CONTROL_TIMEOUT_MS);
    if (committed < 0) {
      this.errorMessage = `SET_CUR(COMMIT) failed: ${errnoText(committed)}`;
      return null;
    }
    return ctrl;
  }

  /** Puts the streaming interface back on alt 0 so the camera stops sending. */
  private idleStream(): void {
    const stream = this.stream;
    if (this.fd >= 0 && stream !== null && this.streamingAltSet) {
      native.usbSetInterface(this.fd, stream.interfaceNumber, 0);
    }
    this.streamingAltSet = false;
  }

  /**
   * Resolves true once `needed` frames in a row were shown without a decode
   * failure in between (or one frame, for `sequenceOnly`), false on timeout,
   * when the stream stops, or when the preview is restarted or stopped.
   */
  private waitForFrames(
    streamId: number, epoch: number, needed: number, sequenceOnly: boolean, timeoutMs: number,
  ): Promise<boolean> {
    return new Promise<boolean>((resolve: (ok: boolean) => void): void => {
      const deadline = Date.now() + timeoutMs;
      let streakStart = 0;
      let seenFailures = 0;
      const timer = setInterval((): void => {
        let done = false;
        let ok = false;
        if (epoch !== this.epoch) {
          done = true;
        } else {
          const stats = native.streamStats(streamId);
          if (stats.length < STAT_COUNT || stats[STAT_RUNNING] === 0) {
            done = true;
          } else {
            const shown = stats[STAT_SHOWN];
            const failures = stats[STAT_DECODE_FAILURES] + stats[STAT_INVALID_JPEG];
            if (failures > seenFailures) {
              seenFailures = failures;
              streakStart = shown;
            }
            if (sequenceOnly ? shown >= 1 : shown - streakStart >= needed) {
              done = true;
              ok = true;
            } else if (Date.now() >= deadline) {
              done = true;
            }
          }
        }
        if (done) {
          clearInterval(timer);
          resolve(ok);
        }
      }, VERIFY_POLL_MS);
    });
  }

  // Results and statistics -------------------------------------------------------

  private failWith(code: number, message: string): number {
    this.errorMessage = message;
    hilog.warn(DOMAIN, TAG, '%{public}s', message);
    return code;
  }

  private outcome(success: boolean, startedAt: number, code: number, message: string): UvcPreviewOutcome {
    if (!success) {
      this.errorMessage = message;
      hilog.warn(DOMAIN, TAG, 'preview start failed: %{public}s', message);
    }
    const stats = this.readStats();
    const failures = stats[STAT_DECODE_FAILURES] + stats[STAT_INVALID_JPEG];
    const result: UvcPreviewOutcome = {
      success: success,
      validFrameCount: stats[STAT_SHOWN],
      consecutiveValidFrames: failures === 0 ? stats[STAT_SHOWN] : 0,
      errorCount: failures,
      elapsedMs: Date.now() - startedAt,
      lastError: message,
      nativeErrorCode: code,
      negotiatedFps: this.negotiatedInterval > 0 ? Math.round(10000000 / this.negotiatedInterval) : 0
    };
    return result;
  }

  /** Counters of the running stream, or of the last one after it stopped. */
  private readStats(): number[] {
    if (this.streamId > 0) {
      const stats = native.streamStats(this.streamId);
      if (stats.length >= STAT_COUNT) {
        this.lastStats = stats;
      }
    }
    return this.lastStats;
  }

  /** One line of transport and rendering counters for the log; empty when nothing is streaming. */
  diagnostics(): string {
    if (this.streamId <= 0) {
      return '';
    }
    const stats = this.readStats();
    const previous = this.loggedStats;
    this.loggedStats = stats;
    const shown = Math.max(1, stats[STAT_SHOWN]);
    let inputGaps = '';
    let shownGaps = '';
    for (let i = 0; i < STAT_GAP_BUCKETS; i++) {
      const separator = i === 0 ? '' : '/';
      inputGaps += `${separator}${stats[STAT_INPUT_GAPS + i] - previous[STAT_INPUT_GAPS + i]}`;
      shownGaps += `${separator}${stats[STAT_SHOWN_GAPS + i] - previous[STAT_SHOWN_GAPS + i]}`;
    }
    return `transfers=${stats[STAT_URBS]} packets=${stats[STAT_PACKETS]} empty=${stats[STAT_EMPTY_PACKETS]} ` +
      `bad=${stats[STAT_BAD_PACKETS]} submitFail=${stats[STAT_SUBMIT_FAILURES]} frames=${stats[STAT_FRAMES]} ` +
      `eof=${stats[STAT_ENDS_BY_EOF]} fidToggle=${stats[STAT_ENDS_BY_FID_TOGGLE]} ` +
      `incomplete=${stats[STAT_INCOMPLETE_FRAMES]} lastFrame=${stats[STAT_LAST_FRAME_BYTES]}B ` +
      `shown=${stats[STAT_SHOWN]} decodeFail=${stats[STAT_DECODE_FAILURES]} dropped=${stats[STAT_DROPPED]} ` +
      `yuv=${stats[STAT_YUV]} decode=${(stats[STAT_DECODE_MS] / shown).toFixed(1)}ms draw=${(stats[STAT_SHOW_MS] / shown).toFixed(1)}ms ` +
      `gapsMs<=22/30/45/70/more in=${inputGaps} shown=${shownGaps}`;
  }

  /** Snapshot for the current preview, or the last one after it stopped. */
  stats(): UvcSessionStats {
    const stats = this.readStats();
    const seconds = stats[STAT_ELAPSED_MS] / 1000;
    const result: UvcSessionStats = {
      sequence: this.sequenceBase + (this.streamId > 0 ? stats[STAT_SHOWN] : 0),
      inputFrameCount: stats[STAT_FRAMES],
      deliveredFrameCount: stats[STAT_SHOWN],
      decodeSuccessCount: stats[STAT_SHOWN],
      decodeFailureCount: stats[STAT_DECODE_FAILURES],
      callbackLockDropCount: stats[STAT_DROPPED],
      undersizedFrameCount: stats[STAT_INCOMPLETE_FRAMES],
      invalidMjpegCount: stats[STAT_INVALID_JPEG],
      inputFps: seconds > 0 ? stats[STAT_FRAMES] / seconds : 0,
      deliveredFps: seconds > 0 ? stats[STAT_SHOWN] / seconds : 0,
      avgInterFrameGapMs: stats[STAT_GAP_AVG_MS],
      p95InterFrameGapMs: stats[STAT_GAP_P95_MS],
      maxInterFrameGapMs: stats[STAT_GAP_MAX_MS],
      firstFrameLatencyMs: Math.max(0, stats[STAT_FIRST_FRAME_MS]),
      elapsedMs: stats[STAT_ELAPSED_MS]
    };
    return result;
  }
}
