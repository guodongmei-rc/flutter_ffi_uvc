// Flutter plugin entry for HarmonyOS. Bridges the Dart UvcCamera API to the
// usbManager-based camera session; see lib/src/ohos/ohos_uvc_camera.dart for
// the Dart side of this channel.

import {
  Any,
  EventChannel,
  EventSink,
  FlutterPlugin,
  FlutterPluginBinding,
  MethodCall,
  MethodCallHandler,
  MethodChannel,
  MethodResult,
  StreamHandler,
} from '@ohos/flutter_ohos';
import { hilog } from '@kit.PerformanceAnalysisKit';
import {
  UvcCameraSession, UvcDeviceSummary, UvcModeSummary, UvcPreviewOutcome, UvcPreviewRequest,
  UvcSessionListener, UvcSessionStats,
} from '../../camera/UvcCameraSession';
import { NO_WINDOW, UvcPreviewRenderer } from '../../camera/UvcPreviewRenderer';

const METHOD_CHANNEL: string = 'flutter_ffi_uvc/ohos';
const EVENT_CHANNEL: string = 'flutter_ffi_uvc/ohos_events';
const STATS_INTERVAL_MS: number = 250;

function numberArg(call: MethodCall, key: string, fallback: number): number {
  const value: Any = call.argument(key);
  return typeof value === 'number' ? value as number : fallback;
}

/** FlutterFfiUvcPlugin **/
export default class FlutterFfiUvcPlugin implements FlutterPlugin, MethodCallHandler, StreamHandler,
UvcSessionListener {
  private channel: MethodChannel | null = null;
  private eventChannel: EventChannel | null = null;
  private events: EventSink | null = null;
  private renderer: UvcPreviewRenderer | null = null;
  private session: UvcCameraSession | null = null;
  private statsTimer: number = -1;
  private statsTicks: number = 0;
  /** Log transport and rendering counters once per second while previewing. */
  private verbose: boolean = false;
  private lastTickMs: number = 0;
  private maxTickLateMs: number = 0;

  constructor() {
  }

  getUniqueClassName(): string {
    return "FlutterFfiUvcPlugin"
  }

  onAttachedToEngine(binding: FlutterPluginBinding): void {
    const renderer = new UvcPreviewRenderer(binding.getTextureRegistry());
    this.renderer = renderer;
    this.session = new UvcCameraSession(this);
    this.channel = new MethodChannel(binding.getBinaryMessenger(), METHOD_CHANNEL);
    this.channel.setMethodCallHandler(this);
    this.eventChannel = new EventChannel(binding.getBinaryMessenger(), EVENT_CHANNEL);
    this.eventChannel.setStreamHandler(this);
  }

  onDetachedFromEngine(binding: FlutterPluginBinding): void {
    this.stopStatsTimer();
    if (this.session !== null) {
      this.session.close();
      this.session = null;
    }
    if (this.renderer !== null) {
      this.renderer.disposeAll();
      this.renderer = null;
    }
    if (this.channel !== null) {
      this.channel.setMethodCallHandler(null);
      this.channel = null;
    }
    this.eventChannel = null;
    this.events = null;
  }

  // StreamHandler --------------------------------------------------------------

  onListen(args: Any, events: EventSink): void {
    this.events = events;
  }

  onCancel(args: Any): void {
    this.events = null;
  }

  // UvcSessionListener ---------------------------------------------------------

  onStreamError(message: string): void {
    const event: Map<string, Any> = new Map<string, Any>();
    event.set('type', 'streamError');
    event.set('message', message);
    this.emit(event);
  }

  onPreviewStopped(reason: string): void {
    this.stopStatsTimer();
    this.emitStats();
    const event: Map<string, Any> = new Map<string, Any>();
    event.set('type', 'previewStopped');
    event.set('reason', reason);
    this.emit(event);
  }

  // MethodCallHandler ----------------------------------------------------------

  onMethodCall(call: MethodCall, result: MethodResult): void {
    const session = this.session;
    const renderer = this.renderer;
    if (session === null || renderer === null) {
      result.error('detached', 'the plugin is not attached to an engine', null);
      return;
    }
    switch (call.method) {
      case 'setLogLevel':
        this.verbose = call.argument('verbose') === true;
        result.success(null);
        break;
      case 'listUsbDevices':
        result.success(this.devicesToList(session.listDevices()));
        break;
      case 'openUsbDevice':
        this.openDevice(session, numberArg(call, 'deviceId', -1), result);
        break;
      case 'closeUsbDevice':
        this.stopStatsTimer();
        session.close();
        result.success(null);
        break;
      case 'startPreview':
        this.startPreview(session, call, result);
        break;
      case 'stopPreview':
        this.stopStatsTimer();
        session.stopPreview();
        this.emitStats();
        result.success(null);
        break;
      case 'createPreviewTexture':
        try {
          result.success(renderer.createTexture());
        } catch (e) {
          result.error('texture', `creating the preview texture failed: ${(e as Error).message}`, null);
        }
        break;
      case 'disposePreviewTexture': {
        const textureId = numberArg(call, 'textureId', -1);
        if (renderer.isAttached(textureId)) {
          // Stop drawing before the surface goes away.
          session.setPreviewWindow(NO_WINDOW);
        }
        renderer.disposeTexture(textureId);
        result.success(null);
        break;
      }
      case 'attachPreviewTexture': {
        const window = renderer.attach(numberArg(call, 'textureId', -1));
        if (window === NO_WINDOW) {
          result.error('texture', 'unknown preview texture id', null);
        } else {
          session.setPreviewWindow(window);
          result.success(null);
        }
        break;
      }
      default:
        result.notImplemented();
        break;
    }
  }

  private openDevice(session: UvcCameraSession, deviceId: number, result: MethodResult): void {
    session.open(deviceId).then((code: number): void => {
      const reply: Map<string, Any> = new Map<string, Any>();
      reply.set('result', code);
      reply.set('lastError', session.lastError());
      reply.set('modes', this.modesToList(session.supportedModes()));
      result.success(reply);
    }).catch((error: Error): void => {
      result.error('open', `opening the device failed: ${error.message}`, null);
    });
  }

  private startPreview(session: UvcCameraSession, call: MethodCall, result: MethodResult): void {
    const request: UvcPreviewRequest = {
      format: numberArg(call, 'format', 0),
      width: numberArg(call, 'width', 0),
      height: numberArg(call, 'height', 0),
      fps: numberArg(call, 'fps', 0),
      consecutiveValidFrames: numberArg(call, 'consecutiveValidFrames', 3),
      timeoutMs: numberArg(call, 'timeoutMs', 2000),
      sequenceOnly: call.argument('sequenceOnly') === true
    };
    this.startStatsTimer();
    session.startPreview(request).then((outcome: UvcPreviewOutcome): void => {
      if (!session.isPreviewing()) {
        this.stopStatsTimer();
      }
      this.emitStats();
      const reply: Map<string, Any> = new Map<string, Any>();
      reply.set('success', outcome.success);
      reply.set('validFrameCount', outcome.validFrameCount);
      reply.set('consecutiveValidFrames', outcome.consecutiveValidFrames);
      reply.set('errorCount', outcome.errorCount);
      reply.set('elapsedMs', outcome.elapsedMs);
      reply.set('lastError', outcome.lastError);
      reply.set('nativeErrorCode', outcome.nativeErrorCode);
      result.success(reply);
    }).catch((error: Error): void => {
      this.stopStatsTimer();
      result.error('preview', `starting the preview failed: ${error.message}`, null);
    });
  }

  // Events ---------------------------------------------------------------------

  private emit(event: Map<string, Any>): void {
    const events = this.events;
    if (events !== null) {
      events.success(event);
    }
  }

  private startStatsTimer(): void {
    if (this.statsTimer !== -1) {
      return;
    }
    this.statsTimer = setInterval((): void => {
      const now = Date.now();
      if (this.lastTickMs !== 0 && now - this.lastTickMs - STATS_INTERVAL_MS > this.maxTickLateMs) {
        this.maxTickLateMs = now - this.lastTickMs - STATS_INTERVAL_MS;
      }
      this.lastTickMs = now;
      const session = this.session;
      if (session !== null) {
        session.poll();
      }
      this.emitStats();
      this.statsTicks++;
      if (this.verbose && session !== null && this.statsTicks % 4 === 0) {
        const line = session.diagnostics();
        if (line.length > 0) {
          hilog.info(0x0001, 'FlutterFfiUvc', '%{public}s loopLate=%{public}dms', line, this.maxTickLateMs);
          this.maxTickLateMs = 0;
        }
      }
    }, STATS_INTERVAL_MS);
  }

  private stopStatsTimer(): void {
    if (this.statsTimer !== -1) {
      clearInterval(this.statsTimer);
      this.statsTimer = -1;
    }
  }

  private emitStats(): void {
    const session = this.session;
    if (session === null || this.events === null) {
      return;
    }
    const stats: UvcSessionStats = session.stats();
    const event: Map<string, Any> = new Map<string, Any>();
    event.set('type', 'stats');
    event.set('sequence', stats.sequence);
    event.set('inputFrameCount', stats.inputFrameCount);
    event.set('deliveredFrameCount', stats.deliveredFrameCount);
    event.set('decodeSuccessCount', stats.decodeSuccessCount);
    event.set('decodeFailureCount', stats.decodeFailureCount);
    event.set('callbackLockDropCount', stats.callbackLockDropCount);
    event.set('undersizedFrameCount', stats.undersizedFrameCount);
    event.set('invalidMjpegCount', stats.invalidMjpegCount);
    event.set('inputFps', stats.inputFps);
    event.set('deliveredFps', stats.deliveredFps);
    event.set('avgInterFrameGapMs', stats.avgInterFrameGapMs);
    event.set('p95InterFrameGapMs', stats.p95InterFrameGapMs);
    event.set('maxInterFrameGapMs', stats.maxInterFrameGapMs);
    event.set('firstFrameLatencyMs', stats.firstFrameLatencyMs);
    event.set('elapsedMs', stats.elapsedMs);
    this.emit(event);
  }

  // Conversions ----------------------------------------------------------------

  private devicesToList(devices: UvcDeviceSummary[]): Array<Any> {
    const out: Array<Any> = [];
    for (let i = 0; i < devices.length; i++) {
      const device = devices[i];
      const map: Map<string, Any> = new Map<string, Any>();
      map.set('deviceId', device.deviceId);
      map.set('deviceName', device.deviceName);
      map.set('vendorId', device.vendorId);
      map.set('productId', device.productId);
      map.set('productName', device.productName);
      map.set('manufacturerName', device.manufacturerName);
      map.set('serialNumber', device.serialNumber);
      map.set('hasPermission', device.hasPermission);
      out.push(map);
    }
    return out;
  }

  private modesToList(modes: UvcModeSummary[]): Array<Any> {
    const out: Array<Any> = [];
    for (let i = 0; i < modes.length; i++) {
      const mode = modes[i];
      const map: Map<string, Any> = new Map<string, Any>();
      map.set('format', mode.format);
      map.set('formatName', mode.formatName);
      map.set('width', mode.width);
      map.set('height', mode.height);
      map.set('fps', mode.fps);
      out.push(map);
    }
    return out;
  }
}
