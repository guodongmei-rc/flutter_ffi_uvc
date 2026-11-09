// Messages between UvcProbePage and UvcProbeWorker. The session type is
// imported by file path, not through the plugin's index, so the Worker does
// not load the Flutter engine bindings the plugin class depends on.
import { UvcProbeOptions } from 'flutter_ffi_uvc/src/main/ets/probe/UvcProbeSession';

export const PROBE_CMD_RUN: string = 'run';
export const PROBE_CMD_STOP: string = 'stop';

export const PROBE_EVENT_LOG: string = 'log';
export const PROBE_EVENT_STATS: string = 'stats';
export const PROBE_EVENT_DONE: string = 'done';

export interface ProbeWorkerCommand {
  cmd: string;
  options: UvcProbeOptions;
}

export interface ProbeWorkerEvent {
  kind: string;
  text: string;
}
