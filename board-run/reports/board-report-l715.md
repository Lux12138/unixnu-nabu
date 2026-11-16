# 上板报告 l715（一次性临时功能轮：把 Buddy/Setup 完成位写进内嵌 plist）

> 本报告是**上板前**门禁文件；本会话未运行 adb/fastboot、未上板、未写
> `E:\nabu\logs\rounds\l715.json`。只读模板 `E:\nabu\hold\l714\` 六文件 sha
> 前后未变（payload `da7df504`）。按 `E:\nabu\AGENTS.md` 0.5 / 0.6 / 0.7 / 0.9 /
> 0.10 编写，格式照 `E:\nabu\state\board-report-l714.md`；六节标题按
> `E:\nabu\tmp\board_report_check.py` 的精确字符串落盘。
> 全部证据目录：`E:\nabu\tmp\l715-build-20261111\`。

## 0. 用户授权与性质

**用户自动授权（2026-11-11）**：「自动推进、临时插帧/数据补丁、不停」；
`E:\nabu\state\HANDOVER-20261110-l696.md` §7aa/§7ad/§7ae 记边界：**只用板上内核已有机制**、
**一次性临时轮、用完回滚、不进默认 args**、零新增 printf/IOLog/计数器/探针、
零重编内核、零安全策略放宽、不碰 `boot_a`；全套门禁 + 六问板报。
本轮**不改源码、不重编、零新增探针**：以 `hold/l714` 的 kernel `d9b5a174` 为只读模板，
只对其内嵌 `com.apple.purplebuddy.plist` C 串做 333 B → 333 B 同长数据补丁
（加 `<key>buddyComplete</key><true/>`），`unixnu.toml`/args 一个字节不改。
设备上板由用户执行并判读；判读后一次性组合即作废、回滚 `hold/l700` / `hold/l714`。

## 1. 唯一问题（六问之一）

**把 Buddy/Setup 完成位写进内核内嵌 plist 后，l714 的 PreBoard 白页能否交还屏幕、SpringBoard 出桌面？**

基线（l714，`sheng-mig=1` + 隐藏 BootUI overlay）：DataMigration 杀旧 SpringBoard
pid27、重启 pid179、打开 **PreBoard pid239**；os_log t=4.392
`System shell "com.apple.PreBoard" finished startup after 2.63s`、t=4.393
`finishedStartup:YES` / `Dismissing overlay`；`screen.bgrx` sha `e39720f261142f79`
是系统自绘白页（1,021,128 白 + 2,397 黑 + 少量灰阶），frames `ncpy=3`。
新墙：`dasd` 反复 `Requires Buddy Complete Policy ... buddyComplete == 0`、
`AXSpringBoardServer` 仍 `No such process`、PreBoard 没把屏幕交还 SpringBoard 桌面。

**判据（全官方已有输出、零新探针）**：os_log `buddyComplete == 1` / 不再 `== 0`；
`AXSpringBoardServer` 不再 `No such process`；`finishedStartup` 对 **SpringBoard** 而非
PreBoard 出现；`screen.bgrx` 从白页 `e39720f2…` 变成桌面内容；frames 继续 `ncpy>0`。
若仍白页 ⇒ 键/域不对或另有 Buddy 状态源。运行期像素/策略结果静态不能判
⇒ 满足 `AGENTS.md` 0.5。

## 2. 已闭合到哪（六问之二，日志优先 + §7ae 完整链 / DSC 键名证据 / chain）

1. **写路径/内容源板侧实证**：`E:\nabu\logs\rounds\l706.json` 全白 `e1547326…`（GOP 写路径活）；
   `l709`/`l711` splash 多色 `00e62fe8…`（板上主动绘制活）；
   `state/board-report-l710.md` §2：`kern_exec.c:8508-8572` 的 323 B `nabu_fb[]` plist +
   六个 `fb_prefs[]` 路径 + 四个 backboardd 域 `keepIfPresent=0` 每轮强制覆盖，
   `BKHideAppleLogoOnLaunch=true` 经 `Not showing the boot UI overlay/underlay` 实证生效，
   `l710/l712` 隐藏后纯黑 `112b94b6…`、frames 全 0。`l713` 否证 `nabu-autolog=1`。
2. **§7ae 完整链**：`E:\nabu\logs\rounds\l714.json`、
   `E:\nabu\state\HANDOVER-20261110-l696.md` §7ae.2/§7ae.3、
   `E:\nabu\logs\l714-v1.jsonl`（117,521 行）：`sheng-mig=1` 触发
   `Trial Initialization: Deferred due to pending Data Migration`（t=2.368）→
   backboardd `start Data Migration...` / `STOP WATCHDOG for com.apple.SpringBoard` /
   `Received request to open alternate system app "com.apple.PreBoard"`（t=2.640）→
   杀 pid27、重启 pid179（t=2.661）→ PreBoard pid239 spawn / `launchSucceeded` /
   `Alternate system shell connected` / `checkInAndWaitForDataMigration:NO`（t=4.340）→
   t=4.392 `finished startup after 2.63s` → t=4.393 `systemShellDidFinishLaunching` /
   `updateOverlay: pid:239 ... finishedStartup:YES` / `Dismissing overlay`；
   PreBoard 有 `PBASecureWindow`、FBScene 前台。屏 `e39720f2…`、`ncpy=3`。
   **同时** 27 条 `buddyComplete == 0`（`com.apple.duetactivityscheduler` /
   `/usr/libexec/dasd` / category `scoring`）、`AXSpringBoardServer` 持续
   `No such process`。§7ae.3 明写：内核已内嵌 `com.apple.purplebuddy.plist`
   （`SetupDone` / `SetupFinishedAllSteps` / `SetupVersion=11`）与
   `com.apple.springboard.plist`（`BKNoWatchdogs` / `SBDisable*`）、5/5 写成功读回一致，
   **但 `buddyComplete` 键本身从没写进任何 plist**；Qoder 历史 `SetupDone=true 仍无 UI`
   属旧 BootUI 状态，条件已变，可在新 PreBoard 状态重测。
3. **第一件事：定点 DSC 键名/域（只读，别猜）**。原始 hex+ascii 与全库普查：
   `E:\nabu\tmp\l715-build-20261111\dsc-key-evidence.txt`；结论：
   `E:\nabu\tmp\l715-build-20261111\key-name-conclusion.txt`。
   - `/root/dsc/dyld_shared_cache_arm64.01@0x396a389`、`.11@0x699f426` 前后 300 B
     都是 `_bucketSize/_buddyComplete/_buddyConfigured/_buddySession` 等 **ObjC ivar
     字符串池**；`.15@0x579f181` 前后是 **DAS activity 属性池**
     （`clientName/buddyComplete/userIdentifier/runOnAppFG/widgetBudgetID`），
     同簇 `.15@0x57a43de` = `TB,N,V_requiresBuddyComplete`、
     `@0x57a8088` = `requiresBuddyComplete`、`@0x57a8fbc` = `setRequiresBuddyComplete:`。
   - 全 DSC 字面量普查：小写 `buddyComplete` **仅 3 处**（全在上述三片）；
     `Requires Buddy Complete Policy` 0、`buddyComplete == 0` 0（os_log 运行期拼出）；
     `BuddyComplete` 61 处多为 `_pendingBuddyCompleteBlocks` /
     `xpcWatchBuddyCompletedSetupSteps` 等符号。
   ⇒ **`buddyComplete` 是 DAS policy 内部状态/变量名，不是 CFPreferences key、
     也不是 Darwin notification 名（强证据）**。
   - **可写 plist 键证据**：`.13@0x2006398` 域 `com.apple.purplebuddy` 与
     `@0x200716b com.apple.purplebuddy.setupdone`、`@0x200721f SetupFinishedAllSteps`、
     `@0x2007235 SetupDone`、`@0x2007261 SetupVersion` 同簇；`.25@0x34c8b2e`
     `com.apple.purplebuddy` 紧邻 `@0x34c8b44 TVBuddyCompletedSuccessfully`、
     `@0x34c8b61 SetupDone`；`.13@0x2002907` 日志格式串
     `BYSetupAssistantNeedsToRun is YES because !setupDone` 直接证明 Setup Assistant
     读 `setupDone`（**`SetupDone`/`SetupFinishedAllSteps` 是官方域内键，强**）。
     `BYSetupAssistantNeedsToRun` 在 `.15@0x39a4ec3` 是
     `BOOL _BYSetupAssistantNeedsToRun(void)` 的 **dlsym 函数名**、不是 plist 键（中-强反证）；
     `buddyComplete=true` / `BuddyComplete=true` 作 plist 键 **无静态证据（弱）**。
     `.13@0x2002581` `... is NO due to ForceNoBuddy.` / `@0x200748f ForceNoBuddy` 邻
     `apple-internal-install` 属内部编队分支，非本板已有写入路径。
   - **证据不足以把 `buddyComplete == 0` 映射成某个官方 plist 键** ⇒ 按用户指示取
     **最保守、板上已有写入路径**的方案：在 `com.apple.purplebuddy.plist` 现有 XML
     里同长加 `<key>buddyComplete</key><true/>`（键名逐字照 os_log 变量名），
     不动其它语义/域/路径/args；既有 `SetupDone/SetupFinishedAllSteps` 已 true 一并保留，
     以便区分「官方完成位已 true 但 DAS 仍 0」与「追加键后变 1」。
4. **板上已有写入路径**：`kern_exec.c:8297-8305` 定义 `nabu_pb[]`、
   `:8344-8359` 的 `setup_skip[]` 把它写到
   `/private/var/mobile/Library/Preferences/com.apple.purplebuddy.plist`（域 `'m'`）、
   `:8369-8397` 每轮 `0600/501:501` 强制写 + `nabu: skip-read` 全量读回；
   kernel 路径字面量 @`0x11162a`。l701/l710/l714 三个同长数据补丁先例（l710 板侧
   os_log 实证隐藏键生效），零重编、零新探针、5/5 写成功读回一致。
5. **包侧链**：`hold/l715` 四文件逐字节 = `l714`；kernel 同长补丁 122 B 全部落在
   `0x21f52..0x21fd2`（目标 plist `0x21f2b..0x22077` 内）；payload 仅
   `KERNEL~1MAC` 变（`d9b5a174` → K715），`KERNEL~1TRU` / `UNIXNU~1TOM` /
   其余全 SAME；`ACCESS_READ` mmap + 192 MiB 流式复制 + `mcopy -D o` 覆盖长名
   （绝不 mmap 输入 payload 可写，l695p 教训）。证据
   `assemble.log` / `selfcheck-final.txt` / `diff-bytes.txt`。

**改前 / 改后 plist 全文**（`plist-before.txt` / `plist-after.txt`，各 333 B）：

```
改前: <?xml version="1.0" encoding="UTF-8"?>
      <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
      <plist version="1.0"><dict><key>SetupDone</key><true/><key>SetupFinishedAllSteps</key><true/><key>SetupVersion</key><integer>11</integer><key>daemonizeNeedsToRun</key><false/></dict></plist>
改后: <?xml version="1.0" encoding="UTF-8"?>
                                                                        <plist version="1.0"><dict><key>buddyComplete</key><true/><key>SetupDone</key><true/><key>SetupFinishedAllSteps</key><true/><key>SetupVersion</key><integer>11</integer><key>daemonizeNeedsToRun</key><false/></dict></plist>
```
（103 B DOCTYPE → 72 B 无害 XML 空白 + 31 B 新键；两者均 `ET.fromstring` 通过。）

## 3. 为什么离线不行（六问之三，why_board / AGENTS.md 0.5）

静态能证：§2 全链、DSC 键名/域判定、`kern_exec.c` 写入路径与 5/5 读回一致、
K715 同长补丁 122 B 全在目标 plist 内、文件长 125628416 不变、NUL @`0x22078` 未动、
APFS nop @`0x6cd2f68` 仍 `1f2003d5`、Mach-O `ncmds=25` 且无 `LC_CODE_SIGNATURE`、
`BKHideAppleLogoOnLaunch` 仍在 `nabu_fb @0x2272c`、抽回 `KERNEL~1MAC` == K715、
`KERNEL~1TRU` == `a4a6bae6`、toml 与 l714 逐字节同、args `685/38` 且 added/dropped 均空、
其余四文件逐字节同 l714、`hold/l714` 未污染。

**但缺的量只能运行期得到**：cfprefsd 缓存/域归属、`budd` 是否读该文件、
DAS policy 的 `buddyComplete` 输入源（可能来自 `com.apple.purplebuddy.setupdone`
通知或 budd XPC 而非这个 plist）、PreBoard→SpringBoard 交接时序、
`AXSpringBoardServer` 是否注册、`screen.bgrx` 是否变桌面、`ncpy` 是否继续 >0——
全只有真机 os_log / CDC / `screen.bgrx` 能给出。l714 的 27 条 os_log 只证 `== 0`，
**零条**证「写过完成位后如何变化」。
⇒ 属 `AGENTS.md` 0.5 允许的「验证静态结论在真机上是否成立的那一次判定 /
日志与静态都无法产生的新状态」；失败也有可判读信息（§4）。

## 4. 失败也有信息（六问之四，三判读 + 观察项）

| 结果 | 判读 |
|---|---|
| **A**：`buddyComplete == 1`、不再 `== 0`；`AXSpringBoardServer` 不再 `No such process`；`finishedStartup` 出现在 SpringBoard；`screen.bgrx` 从 `e39720f2…` 白页变桌面内容、frames `ncpy>0` | **完成位被 Buddy/DAS 采信**：PreBoard 交还屏幕、SpringBoard 出桌面，Buddy/Setup 完成位是该断点的修复点 |
| **B**：屏仍白页 `e39720f2…`，但 `buddyComplete == 1` / `Requires Buddy Complete Policy` 不再 `Must Not Proceed` | **DAS 策略已过、断点后移**：PreBoard→SpringBoard 交接或另有 Buddy 状态源未满足；收窄到 `AXSpringBoardServer` 注册 / explicit screen owner / SpringBoard 自身初始化 |
| **C**：仍白页且 27 条 `buddyComplete == 0` 不变、`AXSpringBoardServer` 仍 `No such process` | **键/域不对或状态源不在该 plist**：`buddyComplete` 是内部变量，DAS 输入可能来自 budd XPC / `com.apple.purplebuddy.setupdone` 通知 / `purplebuddy.sentinel` / `BYSetupAssistantNeedsToRun` / `ForceNoBuddy` 分支；按失败排除「plists 写入完成位」路线 |
| 出现 `112b94b6…` 纯黑、`00e62fe8…` splash 多色或 `e1547326…` 测试全白 | 包/回显异常：对账 boot-args 回显、`nabu-state-ring`/`nabu-splash`/`sheng-fb-fill` 均应为 0，核对 `unixnu.toml` 未变 |

**观察项（全部现有通道、零新探针）**：
os_log `buddyComplete` / `Requires Buddy Complete Policy` / `PreBoard` /
`SpringBoard` / `AXSpringBoardServer` / `finishedStartup` / `Dismissing overlay` /
`systemShellDidFinishLaunching` / `Not showing the boot UI overlay`；
`screen.bgrx` sha 与直方图（对比 `e39720f2…` 白页、`112b94b6…` 纯黑、
`00e62fe8…` splash、`e1547326…` 全白）；CDC `nabu-display: frames xfer/nswap/nsurf/
sbytes/nok/ncpy/nret`、`nabu: fbprefs ok=6/6 kept=0`、
`nabu: skip-setup prefs_m=5/5` / `nabu: skip-read ... eq=6/6`、
`nabu: scanout hand-off online`、`nabu-gpu-addon: mounted/target/control/status`、
boot-args 回显、`panic` / `unencrypted panic` / `buf_bread_err16`。

## 5. 唯一变量与回滚点（六问之五）

**唯一变量 = `kernel.macho` 内嵌 `nabu_pb[]` plist 的同长数据补丁**（333 B → 333 B，
`cmp -l` 122 B 差异全部落在 `0x21f52..0x21fd2`，即目标 plist `0x21f2b..0x22077` 内）；
payload 内 `KERNEL~1.MAC` 用 `mcopy -D o` 覆盖长名/短名同步到 K715。
**args 一个字节不改**：`685/38 <=700`，`added=[]` / `dropped=[]`，38/38 token 同序同字节，
`unixnu.toml` 1111 B、sha8 `47bd52ca` 与 l714 逐字节相同。
`sheng-mig=1`、`offline-legacy-transfer=1`、`BKHideAppleLogoOnLaunch=true`、
`sheng-smp=1 + nabu-wdt-arm-delay=0` 等全部继承。

**六文件 sha256 前 8**（完整值见 `l715-sha8.txt`）：

| 文件 | l714 sha8 | **l715 sha8** | 说明 |
|---|---|---|---|
| kernel.macho | `d9b5a174` | **`dbed74dc`** | 唯一变更；K715 全 sha `dbed74dcefd9d54904d8afd5563c10a120f37b7f05c8eb6b85389707b5289c87` |
| kernel.macho.adt | `78a6f7e9` | `78a6f7e9` | 逐字节相同 |
| gpu-addon.bin | `950a3968` | `950a3968` | 逐字节相同 |
| owner-trust.bundle | `a4a6bae6` | `a4a6bae6` | 逐字节相同；payload 内 `KERNEL~1.TRU` 与它逐字节一致 |
| boot-merge-r3.img | `ead4b531` | `ead4b531` | 逐字节相同 |
| payload.img | `da7df504` | **`7f258a54`** | 仅 `KERNEL~1MAC` 变；`UNIXNU~1TOM` 与 l714 逐字节同 |

**自检**：`cmp -l` 122 B（独立 `cmp -l | wc -l` 复核）、文件长 125628416 不变、
APFS nop `1f2003d5` 未动、`ncmds=25` 无代码签名、`BKHideAppleLogoOnLaunch` 计数 1 且
在 `nabu_fb @0x2272c`、原四键全在、抽回 `KERNEL~1MAC` == K715、
`KERNEL~1TRU` == `owner-trust.bundle a4a6bae6`、toml 逐字节同 l714；
`fsck.fat -n` 双包 `10 files`；PRELINK 47/47、payload 体积 201326592 不变。
**`hold/l714/` 六文件 sha 前后未变（payload `da7df504`），未污染**
（`l714-before-sha256.txt` 与 `l714-after-sha256.txt` diff 为空）。

- **回滚基线**：`E:\nabu\hold\l700\`（kernel `efcb4c34`）；
  **上一包 / 只读模板** `E:\nabu\hold\l714\`（kernel `d9b5a174`、payload `da7df504`）。
- **一次性、用完回滚**：`buddyComplete=true` + `sheng-mig=1` + 隐藏键组合只属于 l715，
  **不进默认 args**；判读完成后刷回 `hold/l700/payload.img` 或 `hold/l714/payload.img`。
  真正还原只由用户当场点名文件。

## 6. 恢复预案（六问之六，AGENTS.md 0.10 / 0.7 / 0.9 / 附注）

**`AGENTS.md` 0.10 绝对红线：不碰 `boot_a`**（绝不 `fastboot flash boot` /
`flash boot_a`；这块板没有独立 recovery 分区，boot_a 里是 TWRP）。
流程（**仅文字，本会话不执行**；顺序不可换）：

```
recovery arm misc                                             # 不碰 boot_a
flash cust hold/l715/payload.img
fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin   # 最后才重 arm misc
fastboot boot hold/l715/boot-merge-r3.img                     # 临时加载，不写任何分区
```

`E:\nabu\dwc3\build\misc-bootrecovery.bin`（4096 B）；设备 `TWRP recovery f91fa664`。
**`arm-delay=0`**（`sheng-smp=1` 硬性配对，`AGENTS.md` 0.7），咬点 **380-410 s**，
观察完整 **4 分钟窗口**（`AGENTS.md:57-64`），早 panic 也有重置路径；
l714 的 DataMigration t=2.640 / PreBoard finishedStartup t=4.392 均在窗口内。
若 fastboot 全失败：**人工长按电源 10 秒**。回滚见 §5。

**门禁（本会话实测，全部 PASS）**：

| 项 | 结果 | 证据（绝对路径） |
|---|---|---|
| ① 构建 + 模板只读 | 新 tmp 目录 / 新 `hold/l715/`；四文件逐字节同 l714；payload 只 `KERNEL~1MAC` 变；`ACCESS_READ` mmap + 192 MiB 流式 + `mcopy -D o`；l714 六文件 sha 前后未变（`da7df504`） | `E:\nabu\tmp\l715-build-20261111\patch_kernel_l715.py`、`assemble_l715.py`、`patch.log`、`assemble.log`、`selfcheck-final.txt`、`l714-before-sha256.txt`、`l714-after-sha256.txt` |
| ② `fsck.fat -n` + `audit_bootability.py hold/l715 hold/l714 --expected=<K715 全 sha>` | l714/l715 均 `10 files, 67550/97913 clusters` rc=0；→ **`OK (0 fail, 0 warn)`** RC=0（PRELINK 47/47、ncmds、payload 体积不变、仅 kernel sha 变） | `gate1-fsck.txt`、`gate1-bootability.txt`、`gate1-run-final.txt` |
| ③ `audit_dual.py --tag l715` | 逗号分隔真实 sha `kernel=dbed74dc,payload=7f258a54,adt=78a6f7e9,addon=950a3968,trust=a4a6bae6,boot=ead4b531,args=685/38` → **`7 items compared, 7 match, 0 differ`** | `gate2-audit_dual.txt`、`measured-values.txt`、`E:\nabu\state\audit-l715.json` |
| ④ `tmp/audit_args.py l714 l715` | `685/38 → 685/38`（≤700）；**`added=[]`、`dropped=[]`**、38/38 token 同序同字节 → **`RESULT: OK`** rc=0 | `gate3-audit_args.txt`、`gate23-run-final.txt`、`selfcheck-final.txt` |
| ⑤ Windows Python `precheck.py` | `run_precheck_l715.py` 驱动 `--pkg E:/nabu/hold/l715 --baseline E:/nabu/hold/l714`，八字段齐、`token_diff=ok` → `stamp written: state/precheck.json` rc=0；`precheck_check.py` → **`[gate] OK`** rc=0，`dual audit OK: audit-l715.json (7 items)` | `gate4-precheck.txt`、`gate4-fields.txt`、`gate4-precheck_check.txt`、`gate4-run-final.txt`、`E:\nabu\state\precheck.json` |
| ⑥ 六问板报 | 本文件；`board_report_check.py l715` → `[report] OK` 6/6 节 | `E:\nabu\state\board-report-l715.md` |
| DSC 键名定点（只读） | 三偏移前后 300 B hex+ascii、purplebuddy 域字符串簇、全库字面量普查；结论见 §2 | `E:\nabu\tmp\l715-build-20261111\dsc-key-evidence.txt`、`key-name-conclusion.txt` |
| 自检 | 改前/改后 plist 全文、`cmp -l` 122 B 全在目标 plist 内、长度不变、APFS nop 未动、隐藏键仍在、抽回 kernel/trust/toml、l714 未污染 | `plist-before.txt`、`plist-after.txt`、`diff-bytes.txt`、`K715.txt`、`l715-sha8.txt`、`selfcheck-final.txt`、`extract-l715\` |

**零新探针 / 零重编 / 零安全放宽 / 零源码改动 / 零 boot_a 触碰**；
**同长数据补丁**（333 B → 333 B）、**APFS nop `1f2003d5` 未动**、
**kernel 无代码签名**（`ncmds=25`，无 `LC_CODE_SIGNATURE`）；
**l701 / l710 / l714 先例**（同类同长内嵌 plist 数据补丁，l710 板侧 os_log 实证生效）。
本轮**未运行 adb/fastboot、未上板、未写 `E:\nabu\logs\rounds\l715.json`**。

**fc76 未消费与双审计偏离**：编译侧 fc76 自 **11-06 14:57** 未消费；
`audit_dual.py --tag l715` 的 `--peer` / `--mine` **两侧均为板侧实测**
（7/7 equal，真实 sha 见 §5），照 `E:\nabu\state\board-report-l714.md` 注明：
偏离 `AGENTS.md:36-47` 0.6 的「编译侧 vs 板侧」原意；本轮零重编、无编译侧成果可比对。

## 7. 证据路径（全部绝对路径）

- 构建与自检：`E:\nabu\tmp\l715-build-20261111\`（`patch_kernel_l715.py`、
  `assemble_l715.py`、`selfcheck_l715.py`、`run_gate1.py`、`run_gate23.py`、
  `run_precheck_l715.py`、`kernel.macho`、`plist-before.txt`、`plist-after.txt`、
  `diff-bytes.txt`、`K715.txt`、`dsc-key-evidence.txt`、`key-name-conclusion.txt`、
  `patch.log`、`assemble.log`、`selfcheck-final.txt`、`l715-sha8.txt`、
  `l714-before-sha256.txt`、`l714-after-sha256.txt`、`gate1-fsck.txt`、
  `gate1-bootability.txt`、`gate2-audit_dual.txt`、`gate3-audit_args.txt`、
  `gate4-precheck.txt`、`gate4-precheck_check.txt`、`measured-values.txt`、
  `extract-l715\`）
- 戳与板报：`E:\nabu\state\audit-l715.json`、`E:\nabu\state\precheck.json`、
  `E:\nabu\state\board-report-l715.md`
- 包：`E:\nabu\hold\l715\`（六文件）；只读模板 `E:\nabu\hold\l714\`；
  回滚 `E:\nabu\hold\l700\`；隐藏键来源 `E:\nabu\hold\l710\`
- 链：`E:\nabu\state\HANDOVER-20261110-l696.md` §7ad/§7ae、
  `E:\nabu\logs\rounds\l714.json`、`E:\nabu\logs\l714-v1.jsonl`、
  `E:\nabu\state\board-report-l710.md` / `-l712.md` / `-l714.md`、
  `/root/dsc/dyld_shared_cache_arm64.01|.11|.13|.15|.25`、
  `E:\nabu\AGENTS.md`、`kern_exec.c:8297-8305` / `:8344-8359` / `:8369-8397` /
  `:8508-8572` / `:8602-8629`

**结论：全部门禁 PASS ⇒ 可否上板：可**（上板由用户执行并判读；本会话未执行）。
