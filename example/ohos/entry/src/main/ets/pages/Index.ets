
import common from '@ohos.app.ability.common';
import Want from '@ohos.app.ability.Want';
import { BusinessError } from '@ohos.base';
import hilog from '@ohos.hilog';
import { FlutterPage } from '@ohos/flutter_ohos'

let storage = LocalStorage.getShared()
const EVENT_BACK_PRESS = 'EVENT_BACK_PRESS'

@Entry(storage)
@Component
struct Index {
  private context = getContext(this) as common.UIAbilityContext
  @LocalStorageLink('viewId') viewId: string = "";

  build() {
    Stack({ alignContent: Alignment.TopEnd }) {
      FlutterPage({ viewId: this.viewId })
      // Entry to the stage 0 UVC probe, which runs in its own ability and
      // does not go through Flutter.
      Button('UVC 探针')
        .fontSize(12)
        .height(32)
        .margin({ top: 44, right: 12 })
        .onClick(() => {
          const want: Want = {
            bundleName: this.context.abilityInfo.bundleName,
            abilityName: 'UvcProbeAbility'
          }
          this.context.startAbility(want).catch((error: BusinessError) => {
            hilog.error(0x0001, 'UvcProbe', 'startAbility failed: %{public}d %{public}s', error.code, error.message)
          })
        })
    }
  }

  onBackPress(): boolean {
    this.context.eventHub.emit(EVENT_BACK_PRESS)
    return true
  }
}
