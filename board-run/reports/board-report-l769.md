# board-report l769（链闭合后唯一一次确认轮）

## 1. 唯一问题
在 l750 真锁屏基线上，仅把 `com.apple.springboard.plist` 的 `MCForceShowCloudConfigurationUI=false` 等长替换为 `SBDontLockAfterCrash=true`，SpringBoard 开机 startup transition 是否由 `destination=0 / SBStartupTransitionToLockOut` 改为 `destination=1 / SBStartupTransitionToHome`，并在 PNG 中露出 app 图标？本轮不调手势、不改 args、不改 DSC、不写 boot 分区。

## 2. 已闭合到哪
- 日志优先：全仓 CDC 与 l730 os_log 中 `SBDontLockAfterCrash` 0 次；l730/l733 均有 `SBStartupTransitionToLockOut` + `Boot{ForceLockKey:1}`，全轮 PNG 无 app 图标。日志对「首次预写该键后 destination 是否变化」沉默。
- l768 纠错：`SBDontLockEver` 域/键正确，但 `_completeStartupAfterMainSceneConnect:` @`0x1cfa5d230-0x1cfa5d3b0` 先读 `SpringBoard._dontLockEver` 并构造 context，偏好初始化写该 ivar 在更晚的 `0x1cfa5e0d0`，故 l768 NO-GO。
- l769 域/键：`SBBootDefaults._bindAndRegisterDefaults` @`0x1900a6c98` 把 `dontLockAfterCrash` 绑定 CFString `SBDontLockAfterCrash`（`0x1e6d20c90` → `0x1900fb56b`）；类继承 `SBAbstractSpringBoardDefaultDomain`，`init` @`0x19006f89c` 固定 `com.apple.springboard`。`SBIdleTimerDefaults` 同机制键 `SBDisableAutoDim` / `SBDontDimOrLockOnAC` 已在 l730 L179386/L56580 生效，是运行时正控。
- factory 消费：`_SBSystemAppMain` @`0x1cfa37be8` 在 UIApplicationMain 前无条件取 bootDefaults；`_completeStartupAfterMainSceneConnect:` @`0x1cfa5d25c/0x1cfa5d270` 注入 context；`SBStartupTransitionFactory` @`0x1cfda5ad0` 唯一读取 `dontLockAfterCrash`，@`0x1cfda5ae0/ae4` 位或进 `w8`，@`0x1cfda5aec` `w9=w8|hasPasscodeSet`，@`0x1cfda5b00` `csel w2,w8,w10,ne`。前置门全 0 时 `w8=1 → dest=1=SBStartupTransitionToHome`。旧 l823 把 shouldNeverLock 错当 bad-flags 的模型已作废；l866 为准。
- 正向终点：`SBStartupTransitionToHome` @`0x1cfde6d88` 走 `mainWorkspace → requestTransitionWithOptions:builder:validator:`，不调 `lockUIFromSource:withOptions:completion:`；`SBStartupTransitionToLockOut` @`0x1cfe61dec` 才在 `0x1cfe61fd8` 构造 `Boot{ForceLockKey:1}` 并上锁。
- 运行期量已收敛：① 4.645s startup context 的 `isInLostMode/isBlocked/isAuthenticated/requiresSetup/applicationLaunchURL` 等是否让 factory 走到尾段；② 首次反事实键 `SBDontLockAfterCrash=true` 在真机 cfprefsd/SBBootDefaults 的解析结果。DSC setter 上游与偏好时序均已挖到底，静态和日志不能再判；l730 `ToLockOut` 同时相容 bad-flags 短路与尾段 `w8=0`，不能区分。
- 包级单变量：l750 kernel `9011f295242fea5b` → l769 `234eeb09f31e1814`，仅 `0x22495..0x224c1` 44B / 1 簇差异；471B plist / `plistlib` / `setup_skip[3]` 指针 `0xfffffff007026366` 与 len=471 全保持，blob sha `6280de7b764303a8`。payload `496f583ea5c2bf43` 内仅 kernel DIFF，ADT `ddb096bf` / gpuaddon `950a3968` / trustcache `a4a6bae6` / scratch `3cd3d0d3` 全 SAME；外部 boot `ead4b531` / ADT / addon / trust 与 l750 全同；args 700 chars / 39 tokens identical，sha `ee42777c0c3a802e`，`sheng-smp=1` + `nabu-wdt-arm-delay=0` 保持。
- 板上独有标记：CDC `skip-read i=3 ... keys=...SBDontLockAfterCrash...`（全仓 CDC 0 次）。
- 链文档：`E:\nabu\state\static-chain-l755.md` §4d/§4f-补/§4g、`E:\nabu\state\l769-chain-gate.md` ④/⑤b。

## 3. 为什么离线不行
静态已给出「内核预写 → cfprefsd 域 → SBBootDefaults 键 → factory 尾段 → ToHome → 不主动上锁」的代码链和包的唯一变量，但 4.645s 的 startup context 运行期初始状态、以及反事实键首次被 cfprefsd/SBBootDefaults 解析为 true 的结果，是日志与 DSC 都无法产生的**新状态**。按 AGENTS §0.5，这只允许做一次「验证静态结论在真机上是否成立」的确认轮，不是逐参数试包。

## 4. 失败也有信息
- CDC `skip-read i=3 keys=...SBDontLockAfterCrash...` 出现但 os_log 仍 `SBStartupTransitionToLockOut` / `Boot{ForceLockKey:1}` ⇒ 键已注入但 factory 被前置门短路或偏好解析未生效，回静态按失败点闭链；
- 出现 `SBStartupTransitionToHome` / 无 `ForceLockKey` / `UI Locked:NO` 但 PNG 仍锁屏壁纸 ⇒ destination 已改但 UI 呈现还有第二道墙；
- PNG 出现 app 图标且无锁屏时钟/Home 条 ⇒ 桌面目标达成；
- 无 CDC / 无帧 / 早死 ⇒ 按最后 CDC 帧与 NABULOG 判读，回滚 l750/l730，不继续调参。

## 5. 唯一变量与回滚点
唯一变量 = payload 内 `kernel.macho` 的 44B 等长 plist 键替换；l750→l769 其余五文件、args、ADT、boot、addon、trust 逐字节相同。回滚 = `hold/l750`（真锁屏基线）/ `hold/l730`（用户目验基线）；恢复只认用户点名 `backup-boot_a-20261103.img`。

## 6. 恢复预案
只读探针：prep 时 `adb devices` 见 `f91fa664 recovery`，`fastboot devices` 为空；真启动前用 `l769-run.py` 重新查 `slot-unbootable:a/b` 与 `current-slot`，若 `yes` 先 `fastboot -s f91fa664 set_active a` 再复查；fastboot 不出现或槽位不可判则中止。允许的唯一写入顺序 = `fastboot -s f91fa664 flash cust E:/nabu/hold/l769/payload.img` → `fastboot -s f91fa664 flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin`（`flash cust` 会清 `boot-recovery`，必须 re-arm LAST，镜像 4096B 以 `boot-recovery` 开头，sha `b9f0f5a538242bf5`）→ 再查槽位 → `fastboot -s f91fa664 boot E:/nabu/hold/l769/boot-merge-r3.img`。boot_a 保持 TWRP，**任何 `erase` / `flash boot*` 禁止**；除 cust/misc 外不写任何分区。CDC 捕获进程在 boot 前启动，按 USB VID_1209:PID_316D 动态等待 UniXNU 枚举后的串口（COM3 仅作回退），从首次成功打开起采 600s 并每 10s 周期落盘；首次固定 COM3 派发因端口尚未枚举被门禁安全中止，未发生任何设备写入，详见 `l769-first-launch-gate-fail.md`；`sheng-smp=1` 保持 `nabu-wdt-arm-delay=0`，args 无显式 `nabu-wdt-pet`，不假设 pet=600。若早死/USB 无响应，软件侧不强拉，等人工按键回 TWRP。
