// Stage 0 UVC probe UI. Drives UvcProbeSession from the plugin, either on this
// thread or inside a Worker, and mirrors every log line to the screen, hilog
// and a file in the app sandbox (the camera occupies the USB port during a
// run, so a wired hdc session is usually not available to read hilog live).
import { common } from '@kit.AbilityKit';
import { ErrorEvent, MessageEvents, worker } from '@kit.ArkTS';
import { LengthMetrics } from '@kit.ArkUI';
import { fileIo } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { UvcProbeOptions, UvcProbeSession } from 'flutter_ffi_uvc';
import {
  ProbeWorkerCommand, ProbeWorkerEvent,
  PROBE_CMD_RUN, PROBE_CMD_STOP, PROBE_EVENT_DONE, PROBE_EVENT_LOG, PROBE_EVENT_STATS,
} from '../uvcprobe/ProbeWorkerMessages';

const DOMAIN: number = 0x0001;
const TAG: string = 'UvcProbe';
const MAX_LOG_LINES: number = 400;
const WORKER_SCRIPT: string = 'entry/ets/workers/UvcProbeWorker.ets';

function pad(value: number, width: number): string {
  return value.toString().padStart(width, '0');
}

function timestamp(): string {
  const now = new Date();
  return `${pad(now.getHours(), 2)}:${pad(now.getMinutes(), 2)}:${pad(now.getSeconds(), 2)}.` +
    `${pad(now.getMilliseconds(), 3)}`;
}

function parseField(text: string, fallback: number): number {
  const value = parseInt(text);
  return isNaN(value) || value < 0 ? fallback : value;
}

@Entry
@Component
struct UvcProbePage {
  @State logText: string = '';
  @State statsLine: string = '';
  @State status: string = '未打开';
  @State modeOptions: SelectOption[] = [];
  @State modeIndex: number = 0;
  @State busy: boolean = false;
  @State logPath: string = '';
  @State numTransfers: string = '8';
  @State packetsPerTransfer: string = '128';
  @State transferFlags: string = '0';
  @State durationSec: string = '300';
  @State altSetting: string = '0';

  private lines: string[] = [];
  private session: UvcProbeSession | null = null;
  private probeWorker: worker.ThreadWorker | null = null;
  private logFile: fileIo.File | null = null;
  private scroller: Scroller = new Scroller();

  aboutToAppear(): void {
    const context = getContext(this) as common.UIAbilityContext;
    const path = `${context.filesDir}/uvc_probe_${Date.now()}.log`;
    try {
      this.logFile = fileIo.openSync(path, fileIo.OpenMode.CREATE | fileIo.OpenMode.READ_WRITE);
      this.logPath = path;
    } catch (e) {
      this.logPath = '(日志文件创建失败，仅屏幕和 hilog)';
    }
    this.appendLog(`log file: ${this.logPath}`);
  }

  aboutToDisappear(): void {
    this.stopWorker();
    if (this.session !== null) {
      this.session.close();
      this.session = null;
    }
    if (this.logFile !== null) {
      try {
        fileIo.closeSync(this.logFile);
      } catch (e) {
        hilog.warn(DOMAIN, TAG, 'closing the log file failed');
      }
      this.logFile = null;
    }
  }

  private appendLog(line: string): void {
    const stamped = `${timestamp()} ${line}`;
    hilog.info(DOMAIN, TAG, '%{public}s', stamped);
    if (this.logFile !== null) {
      try {
        fileIo.writeSync(this.logFile.fd, `${stamped}\n`);
      } catch (e) {
        hilog.warn(DOMAIN, TAG, 'writing the log file failed');
      }
    }
    this.lines.push(stamped);
    if (this.lines.length > MAX_LOG_LINES) {
      this.lines.splice(0, this.lines.length - MAX_LOG_LINES);
    }
    this.logText = this.lines.join('\n');
    this.scroller.scrollEdge(Edge.Bottom);
  }

  private ensureSession(): UvcProbeSession {
    if (this.session === null) {
      this.session = new UvcProbeSession(
        (line: string): void => this.appendLog(line),
        (line: string): void => {
          this.statsLine = line;
        });
    }
    return this.session;
  }

  private options(): UvcProbeOptions {
    const options = new UvcProbeOptions();
    options.modeIndex = this.modeOptions.length > 0 ? this.modeIndex : -1;
    options.numTransfers = Math.max(1, parseField(this.numTransfers, 8));
    options.packetsPerTransfer = Math.max(1, parseField(this.packetsPerTransfer, 128));
    options.transferFlags = Math.min(3, parseField(this.transferFlags, 0));
    options.durationSec = Math.max(1, parseField(this.durationSec, 300));
    options.altSetting = parseField(this.altSetting, 0);
    return options;
  }

  /** Runs one probe step with the buttons disabled; an unexpected throw is logged, never swallowed. */
  private async guarded(action: () => Promise<string>): Promise<void> {
    this.busy = true;
    try {
      this.status = await action();
    } catch (e) {
      this.appendLog(`unexpected exception: ${JSON.stringify(e)} ${(e as Error).message}`);
      this.status = '异常（看日志）';
    }
    this.busy = false;
  }

  private openDevice(): Promise<void> {
    return this.guarded(async (): Promise<string> => {
      const session = this.ensureSession();
      const opened = await session.open();
      if (!opened) {
        return '打开失败（看日志）';
      }
      const labels = session.modeLabels();
      const options: SelectOption[] = [];
      for (let i = 0; i < labels.length; i++) {
        const option: SelectOption = { value: labels[i] };
        options.push(option);
      }
      this.modeOptions = options;
      this.modeIndex = session.defaultMode(1280, 720);
      return '已打开';
    });
  }

  private runOnMainThread(): Promise<void> {
    return this.guarded(async (): Promise<string> => {
      this.status = '主线程收流中';
      this.appendLog('==== run on the UI thread ====');
      const passed = await this.ensureSession().run(this.options());
      return `主线程结束：${passed ? 'PASS' : 'FAIL'}`;
    });
  }

  private runInWorker(): void {
    // One connection to the device at a time: hand it over to the Worker.
    if (this.session !== null) {
      this.session.close();
    }
    this.busy = true;
    this.status = 'Worker 收流中';
    this.appendLog('==== run inside a Worker ====');
    let probeWorker: worker.ThreadWorker;
    try {
      probeWorker = new worker.ThreadWorker(WORKER_SCRIPT);
    } catch (e) {
      this.appendLog(`creating the Worker failed: ${JSON.stringify(e)}`);
      this.status = 'Worker 创建失败';
      this.busy = false;
      return;
    }
    this.probeWorker = probeWorker;
    probeWorker.onmessage = (event: MessageEvents): void => {
      const message = event.data as ProbeWorkerEvent;
      if (message.kind === PROBE_EVENT_LOG) {
        this.appendLog(message.text);
      } else if (message.kind === PROBE_EVENT_STATS) {
        this.statsLine = message.text;
      } else if (message.kind === PROBE_EVENT_DONE) {
        this.appendLog(`worker finished: ${message.text}`);
        this.status = `Worker 结束：${message.text}`;
        this.stopWorker();
        this.busy = false;
      }
    };
    probeWorker.onerror = (event: ErrorEvent): void => {
      this.appendLog(`worker error: ${event.message}`);
      this.status = 'Worker 出错（看日志）';
      this.stopWorker();
      this.busy = false;
    };
    const command: ProbeWorkerCommand = { cmd: PROBE_CMD_RUN, options: this.options() };
    try {
      probeWorker.postMessage(command);
    } catch (e) {
      this.appendLog(`posting to the Worker failed: ${JSON.stringify(e)}`);
      this.status = 'Worker 启动失败';
      this.stopWorker();
      this.busy = false;
    }
  }

  private stopWorker(): void {
    const probeWorker = this.probeWorker;
    this.probeWorker = null;
    if (probeWorker !== null) {
      try {
        probeWorker.terminate();
      } catch (e) {
        this.appendLog(`terminating the Worker failed: ${JSON.stringify(e)}`);
      }
    }
  }

  private stopRun(): void {
    if (this.probeWorker !== null) {
      const command: ProbeWorkerCommand = { cmd: PROBE_CMD_STOP, options: new UvcProbeOptions() };
      try {
        this.probeWorker.postMessage(command);
      } catch (e) {
        this.appendLog(`posting stop to the Worker failed: ${JSON.stringify(e)}`);
      }
    }
    if (this.session !== null) {
      this.session.stop();
    }
  }

  private testTransferSizes(): Promise<void> {
    return this.guarded(async (): Promise<string> => {
      this.status = '测单次 transfer 长度';
      this.appendLog('==== single transfer size test ====');
      await this.ensureSession().probeTransferSizes(this.options());
      return '长度测试结束';
    });
  }

  private closeDevice(): void {
    if (this.session !== null) {
      this.session.close();
    }
    this.modeOptions = [];
    this.status = '已关闭';
  }

  @Builder
  numberField(label: string, value: string, onChange: (value: string) => void) {
    Column({ space: 2 }) {
      Text(label).fontSize(11).fontColor('#666666')
      TextInput({ text: value })
        .type(InputType.Number)
        .width(62)
        .height(36)
        .fontSize(14)
        .onChange(onChange)
    }
  }

  build() {
    Column({ space: 8 }) {
      Text(`UVC 阶段 0 探针 · ${this.status}`).fontSize(16).fontWeight(FontWeight.Medium)
      Text(this.logPath).fontSize(10).fontColor('#666666')

      Row({ space: 8 }) {
        this.numberField('transfer 数', this.numTransfers, (value: string): void => {
          this.numTransfers = value;
        })
        this.numberField('包/transfer', this.packetsPerTransfer, (value: string): void => {
          this.packetsPerTransfer = value;
        })
        this.numberField('flags 0-3', this.transferFlags, (value: string): void => {
          this.transferFlags = value;
        })
        this.numberField('时长 秒', this.durationSec, (value: string): void => {
          this.durationSec = value;
        })
        this.numberField('alt 0=自动', this.altSetting, (value: string): void => {
          this.altSetting = value;
        })
      }

      if (this.modeOptions.length > 0) {
        Select(this.modeOptions)
          .selected(this.modeIndex)
          .value(this.modeOptions[this.modeIndex].value)
          .font({ size: 13 })
          .onSelect((index: number): void => {
            this.modeIndex = index;
          })
      }

      Flex({ wrap: FlexWrap.Wrap, space: { main: LengthMetrics.vp(8), cross: LengthMetrics.vp(8) } }) {
        Button('打开设备').fontSize(13).enabled(!this.busy).onClick((): void => {
          this.openDevice();
        })
        Button('开始·主线程').fontSize(13).enabled(!this.busy).onClick((): void => {
          this.runOnMainThread();
        })
        Button('开始·Worker').fontSize(13).enabled(!this.busy).onClick((): void => {
          this.runInWorker();
        })
        Button('停止').fontSize(13).onClick((): void => {
          this.stopRun();
        })
        Button('测长度上限').fontSize(13).enabled(!this.busy).onClick((): void => {
          this.testTransferSizes();
        })
        Button('关闭设备').fontSize(13).enabled(!this.busy).onClick((): void => {
          this.closeDevice();
        })
      }

      Text(this.statsLine).fontSize(11).fontColor('#0A59F7').width('100%')

      Scroll(this.scroller) {
        Text(this.logText)
          .fontSize(10)
          .width('100%')
          .copyOption(CopyOptions.LocalDevice)
      }
      .align(Alignment.TopStart)
      .scrollBar(BarState.Auto)
      .width('100%')
      .layoutWeight(1)
      .backgroundColor('#F5F5F5')
    }
    .width('100%')
    .height('100%')
    .padding({ left: 12, right: 12, top: 44, bottom: 16 })
    .alignItems(HorizontalAlign.Start)
  }
}
