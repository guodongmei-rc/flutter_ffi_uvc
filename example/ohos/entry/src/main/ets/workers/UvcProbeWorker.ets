// Runs the same probe session as the page, but on a Worker thread. Whether
// usbManager is usable here at all is one of the stage 0 questions; every
// outcome, including a thrown error, is posted back to the page's log.
import { ErrorEvent, MessageEvents, ThreadWorkerGlobalScope, worker } from '@kit.ArkTS';
import { UvcProbeSession } from 'flutter_ffi_uvc/src/main/ets/probe/UvcProbeSession';
import {
  ProbeWorkerCommand, ProbeWorkerEvent,
  PROBE_CMD_RUN, PROBE_CMD_STOP, PROBE_EVENT_DONE, PROBE_EVENT_LOG, PROBE_EVENT_STATS,
} from '../uvcprobe/ProbeWorkerMessages';

const workerPort: ThreadWorkerGlobalScope = worker.workerPort;
let session: UvcProbeSession | null = null;

function post(kind: string, text: string): void {
  const event: ProbeWorkerEvent = { kind: kind, text: text };
  try {
    workerPort.postMessage(event);
  } catch (e) {
    // The page has gone away; there is nobody left to report to.
  }
}

function finish(current: UvcProbeSession, verdict: string): void {
  current.close();
  session = null;
  post(PROBE_EVENT_DONE, verdict);
}

workerPort.onmessage = (event: MessageEvents): void => {
  const command = event.data as ProbeWorkerCommand;
  if (command.cmd === PROBE_CMD_STOP) {
    if (session !== null) {
      session.stop();
    }
    return;
  }
  if (command.cmd !== PROBE_CMD_RUN || session !== null) {
    return;
  }
  const current = new UvcProbeSession(
    (line: string): void => post(PROBE_EVENT_LOG, line),
    (line: string): void => post(PROBE_EVENT_STATS, line));
  session = current;
  post(PROBE_EVENT_LOG, 'worker: probe session created on the Worker thread');
  current.run(command.options)
    .then((passed: boolean): void => finish(current, passed ? 'PASS' : 'FAIL'))
    .catch((error: Error): void => {
      post(PROBE_EVENT_LOG, `worker: run rejected: ${error.message}`);
      finish(current, 'FAIL');
    });
};

workerPort.onerror = (event: ErrorEvent): void => {
  post(PROBE_EVENT_LOG, `worker: uncaught error: ${event.message}`);
};
