// Host for the stage 0 UVC probe page. Kept apart from EntryAbility so the
// probe never depends on, or disturbs, the Flutter engine.
import { UIAbility } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0x0001;
const TAG: string = 'UvcProbe';

export default class UvcProbeAbility extends UIAbility {
  onWindowStageCreate(windowStage: window.WindowStage): void {
    windowStage.loadContent('pages/UvcProbePage', (err: BusinessError): void => {
      if (err && err.code) {
        hilog.error(DOMAIN, TAG, 'loadContent failed: %{public}d %{public}s', err.code, err.message);
        return;
      }
      // A five-minute run must not be cut short by the screen timing out.
      windowStage.getMainWindowSync().setWindowKeepScreenOn(true)
        .catch((error: BusinessError): void => {
          hilog.warn(DOMAIN, TAG, 'setWindowKeepScreenOn failed: %{public}d', error.code);
        });
    });
  }
}
