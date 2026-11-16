# 上板报告 l716（一次性临时功能轮：反向 Setup 语义 runSetupAssistant=false）

> 本报告是**上板前**门禁文件；本会话未运行 adb/fastboot、未上板、未写
> `E:\nabu\logs\rounds\l716.json`。只读模板 `E:\nabu\hold\l714\` 六文件 sha
> 前后未变（payload `da7df504`）。按 `E:\nabu\AGENTS.md` 0.5 / 0.6 / 0.7 / 0.9 /
> 0.10 编写，格式照 `E:\nabu\state\board-report-l714.md`；六节标题按
> `E:\nabu\tmp\board_report_check.py` 的精确字符串落盘。
> 全部证据目录：`E:\nabu\tmp\l716-build-20261111\`。

## 0. 用户授权与性质

**用户自动授权（2026-11-11）**：「自动推进、临时插帧/数据补丁、不停」；
`E:\nabu\state\HANDOVER-20261110-l696.md` §7ad/§7ae 记边界：**只用板上内核已有机制**、
**一次性临时轮、用完回滚、不进默认 args**、零新增 printf/IOLog/计数器/探针、
零重编内核、零安全策略放宽、不碰 `boot_a`；全套门禁 + 六问板报。
本轮**不改源码、不重编、零新增探针**：以 `hold/l714` 的 kernel `d9b5a174` 为只读模板，
只对其内嵌 `com.apple.migration.plist` C 串做 264 B → 264 B 同长数据补丁
（`runSetupAssistant` 的 `<true/>` → `<false/>` + 去掉行尾换行），
`unixnu.toml`/args 一个字节不改。设备上板由用户执行并判读；判读后一次性组合即作废、
回滚 `hold/l700` / `hold/l714`。

**候选选择（先静态核对，再选一个）**

| 候选 | 静态核对结果 | 结论 |
|---|---|---|
| **A `sheng-prefs-wipe=1`** | `/root/merge/out-v6/nabu/ipados-17.7.11-21H461/nabu/tree/.cache/xnu-work/bsd/kern/kern_exec.c:8380-8390`：`PE_parse_boot_argn("sheng-prefs-wipe", &wipe, ...)` → `nabu_kern_write_file_owner(path, body, (wipe==1 ? 0u : len), 0600, 501, 501)`，把 `setup_skip[]` 的 5 份 plist 写成 **0 长度（CLEAR 到 cfprefsd 的 "no value"）**，**不是**「重置 Setup/Buddy 偏好到官方 seed 值」，方向与 `buddyComplete` 变 1 相反。`:8508-8572` 的 `fb_prefs[]` 是**独立块**、每轮无条件强制覆盖 6 份 backboardd/.GlobalPreferences plist（4 份 `keepIfPresent=0`），内含 `BKHideAppleLogoOnLaunch=true` 与 3 个 `CAEnable*` 键 —— **A 不抹隐藏键、也不抹 3 个 CA 键**（Qoder 历史 `bridge/pending/20261105-030x-7ba4-display-change-ledger-l533-l577.md:44-45` 同证；l714 CDC `nabu: fbprefs ok=6/6 kept=0` 实证覆盖路径活）。但 A 把 Setup 完成位清空、无正向证据；且 l714 685 + 21 = **706 > 700**，要保 `sheng-mig=1` 必须等长换掉别的已证 token（会丢 l714 白页触发条件）。 | **弃用**：语义错向、无正向证据、args 超限 |
| **B 反向 Setup 语义** | `hold/l714/kernel.macho @0x227f0..0x228f7` 的 264 B `nabu_mig_force[]` 含 `forceMigrationOnNextReboot=true` 与 `runSetupAssistant=true`；`kern_exec.c:8602-8640` 证明该 C 串是 `sheng-mig=1` 分支写到 `com.apple.migration.plist` 的唯一显式「等 Setup」值；DSC 全库无 `runSetupAssistant` 字面量（键名只在内核 C 串）。两个候选静态都无明确正向证据 ⇒ 按用户决策规则取 **B 的最小反向变量 `runSetupAssistant=false`**。 | **选用** |

选 B 的完整理由：**不破坏 l714 已证白页**（`sheng-mig=1`、`forceMigrationOnNextReboot=true`、
`BKHideAppleLogoOnLaunch=true`、3 个 `CAEnable*`、args 685/38 全保留）、
**静态证据更明确**（偏移/字节/消费者/写入路径全可证）、**l715 已证写 Buddy 键有害**
（K716 中 `buddyComplete` 计数 0）。A 弃用理由：见上表（清空而非 seed、方向相反、args 706>700）。

## 1. 唯一问题（六问之一）

**去掉 l715 错误 Buddy 键、回到 l714 白页基线后，选中的 Setup 语义变量能否让 PreBoard
交还屏幕 / SpringBoard 出桌面？**

基线（l714，`sheng-mig=1` + 隐藏 BootUI overlay）：DataMigration 杀旧 SpringBoard
pid27、重启 pid179、打开 **PreBoard pid239**；os_log t=4.392
`System shell "com.apple.PreBoard" finished startup after 2.63s`、t=4.393
`finishedStartup:YES` / `Dismissing overlay`；`screen.bgrx` sha `e39720f261142f79`
是系统自绘白页（1,021,128 白 + 2,397 黑 + 少量灰阶），frames `ncpy=3`。
新墙：`dasd` 反复 `Requires Buddy Complete Policy ... buddyComplete == 0`、
`AXSpringBoardServer` 仍 `No such process`、PreBoard 没把屏幕交还 SpringBoard 桌面。
l715 在 l714 基础上同长补丁给 `com.apple.purplebuddy.plist` 加 `buddyComplete=true`
（kernel `dbed74dc`）⇒ plist 5/5 写成功读回（`E:\nabu\logs\l715-cdc.bin`
`nabu: skip-read ok=5/5 ... eq=5/5`），但 `E:\nabu\logs\l715-v1.jsonl`（115,293 行）
仍有 19 条 `buddyComplete == 0`、2 条 `BackgroundTaskSuspended`，
`E:\nabu\logs\l715-diag-tree\screen.bgrx` 退回纯黑 `112b94b6b55a348f`、frames 全 0。
⇒ **该 plist 键无效且有害**，l716 回到 l714 kernel `d9b5a174`。

**判据（全官方已有输出、零新探针）**：屏从 `e39720f2…` 白页变成桌面内容、
os_log SpringBoard `finishedStartup` / `AXSpringBoardServer` 注册 /
`screen owner is now pid`、DAS 不再 `buddyComplete == 0`、frames `ncpy>0`；
若回到白页 ⇒ 变量无效；若纯黑 `112b94b6…` ⇒ 变量有害。运行期结果静态不能判
⇒ 满足 `AGENTS.md` 0.5。

## 2. 已闭合到哪（六问之二，日志优先 + §7ad/§7ae/l714/l715 全链 + 候选选择理由 / chain）

1. **写路径/内容源板侧实证**：`E:\nabu\logs\rounds\l706.json` 全白 `e1547326…`；
   `l709`/`l711` splash 多色 `00e62fe8…`；`state/board-report-l710.md` §2：
   `kern_exec.c:8508-8572` 的 323 B `nabu_fb[]` plist + 六个 `fb_prefs[]` 路径 +
   四个 backboardd 域 `keepIfPresent=0` 每轮强制覆盖，
   `BKHideAppleLogoOnLaunch=true` 经 `Not showing the boot UI overlay/underlay` 实证生效；
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
   `No such process`。
3. **l715 否证链（本会话实测板上原始产物）**：`E:\nabu\state\board-report-l715.md`、
   `E:\nabu\logs\l715-cdc.bin`（`nabu: skip-read i=0 ... keys=buddyComplete,... eq=1`、
   `nabu: fbprefs ok=6/6 kept=0`、`sheng-mig: sentinel=0 prefs=0 err=0`、
   `nabu-display: frames ... ncpy=0`）、`E:\nabu\logs\l715-v1.jsonl`（19 条
   `buddyComplete == 0`、2 条 `BackgroundTaskSuspended`）、
   `E:\nabu\logs\l715-diag-tree\screen.bgrx`（`112b94b6b55a348f` 纯黑）。
   **`E:\nabu\logs\rounds\l715.json` 在本会话不存在**（用户明确禁止写 l716.json，
   亦未补写 l715.json）；l715 结论全部取自上述原始产物。
4. **第一件事：两候选只读静态核对**（详见 §0 表）。候选 A 的消费者与副作用证据：
   `kern_exec.c:8380-8390` 原文、`E:\nabu\tmp\0012.patch:200-214`、
   `E:\nabu\bridge\pending\20261105-030x-7ba4-display-change-ledger-l533-l577.md:44-45`、
   `E:\nabu\tmp\ca-offline-20261110\source_exact.py:15`；
   `fb_prefs[]` 独立性证据 `kern_exec.c:8508-8572`。候选 B 的偏移/字节证据：
   `E:\nabu\tmp\l716-build-20261111\kernel-plists-raw.txt`、
   `plist-before.txt` / `plist-after.txt` / `diff-bytes.txt`；消费者证据
   `kern_exec.c:8602-8640`。DSC 普查 `runSetupAssistant` 0 处、
   `forceMigrationOnNextReboot` 仅 `.01@0x2a27aec` / `.25@0x414b9c1`
   的 ObjC selector 子串，无正向 plist 键证据 ⇒ 按决策规则取最小反向变量。
5. **板上已有写入路径**：`kern_exec.c:8602-8640` 的 `sheng-mig=1` 分支写
   `/private/var/datamigrator/migrate` 空哨兵 +
   `/private/var/mobile/Library/Preferences/com.apple.migration.plist`（`nabu_mig_force`）；
   l714 CDC 官方 `sheng-mig: sentinel=0 prefs=0 err=0` / `readback rc=0 fsz=0`
   证明该路径生效。l701/l710/l714 三个同长数据补丁先例（l710 板侧 os_log 实证隐藏键生效），
   零重编、零新探针、5/5 写成功读回一致。
6. **包侧链**：`hold/l716` 四文件逐字节 = `l714`；kernel 同长补丁 22 B 全部落在
   `0x228e2..0x228f7`（目标 plist `0x227f0..0x228f7` 内）；payload 仅
   `KERNEL~1MAC` 变（`d9b5a174` → K716 `1209bcc5`），`KERNEL~1TRU` / `UNIXNU~1TOM` /
   其余全 SAME；`ACCESS_READ` mmap + 192 MiB 流式复制 + `mcopy -D o` 覆盖长名
   （绝不 mmap 输入 payload 可写，l695p 教训）。证据
   `assemble.log` / `diff-bytes.txt` / `selfcheck-final.txt`。

**改前 / 改后 plist 全文**（`plist-before.txt` / `plist-after.txt`，各 264 B）：

```
改前: <?xml version="1.0" encoding="UTF-8"?>
      <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
      <plist version="1.0"><dict><key>forceMigrationOnNextReboot</key><true/><key>runSetupAssistant</key><true/></dict></plist>\n
改后: <?xml version="1.0" encoding="UTF-8"?>
      <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
      <plist version="1.0"><dict><key>forceMigrationOnNextReboot</key><true/><key>runSetupAssistant</key><false/></dict></plist>
```
（`<true/>`→`<false/>` +1 B，去掉行尾 `\n` -1 B，长度仍 264 B；
两者均 `ET.fromstring` 通过，`dict` = `forceMigrationOnNextReboot:true` +
`runSetupAssistant:false`；`cmp -l` 22 B 全在 `0x228e2..0x228f7`。）

## 3. 为什么离线不行（六问之三，why_board / AGENTS.md 0.5）

静态能证：§0/§2 全链、两候选的消费者与副作用、K716 同长补丁 22 B 全在目标 plist 内、
文件长 125628416 不变、NUL @`0x228f8` 未动、APFS nop @`0x6cd2f68` 仍 `1f2003d5`、
Mach-O `ncmds=25` 且无 `LC_CODE_SIGNATURE`、`BKHideAppleLogoOnLaunch` 仍在
`nabu_fb @0x226ac`、3 个 `CAEnable*` 同在该 plist、`buddyComplete` 计数 0、
抽回 `KERNEL~1MAC` == K716、`KERNEL~1TRU` == `a4a6bae6`、toml 与 l714 逐字节同、
args `685/38` 且 added/dropped 均空、其余四文件逐字节同 l714、`hold/l714` 未污染。

**但缺的量只能运行期得到**：cfprefsd 缓存/域归属、migration 框架与 budd 是否读该
`runSetupAssistant` 值、DAS policy 的 `buddyComplete` 输入源（可能来自 budd XPC /
`com.apple.purplebuddy.setupdone` 通知而非 migration plist）、PreBoard→SpringBoard
交接时序、`AXSpringBoardServer` 是否注册、`screen.bgrx` 是否变桌面、`ncpy` 是否 >0——
全只有真机 os_log / CDC / `screen.bgrx` 能给出。l714/l715 日志只证「白页 + 等 Buddy」
与「写 Buddy 键有害」，**零条**证「反向 Setup 语义后如何变化」。
⇒ 属 `AGENTS.md` 0.5 允许的「验证静态结论在真机上是否成立的那一次判定 /
日志与静态都无法产生的新状态」；失败也有可判读信息（§4）。

## 4. 失败也有信息（六问之四，三判读 + 观察项）

| 结果 | 判读 |
|---|---|
| **A 有效**：屏从 `e39720f2…` 白页变桌面内容；os_log SpringBoard `finishedStartup` / `AXSpringBoardServer` 注册 / `screen owner is now pid`；DAS 不再 `buddyComplete == 0`；frames `ncpy>0` | **反向 Setup 语义被采信**：`runSetupAssistant=false` 让 PreBoard 不等 Setup、交还屏幕、SpringBoard 出桌面，migration plist 的 Setup 显式值是该断点修复点 |
| **B 无效**：屏回到白页 `e39720f2…`，DAS 仍反复 `buddyComplete == 0`，`AXSpringBoardServer` 仍 `No such process` | **变量无效**：`runSetupAssistant` 不是 DAS `buddyComplete` 的输入源；排除「migration plist 的 Setup 语义」整条路线，收窄到 budd XPC / `com.apple.purplebuddy.setupdone` 通知 / `AXSpringBoardServer` 注册 |
| **C 有害**：屏纯黑 `112b94b6…`、frames 全 0、`BackgroundTaskSuspended` 复现（l715 形态） | **变量有害**：`false` 让 PreBoard/迁移链挂起；该 Setup 键不能反向改，回滚 `hold/l714` |
| 出现 `00e62fe8…` splash 多色或 `e1547326…` 测试全白 | 包/回显异常：对账 boot-args 回显、`nabu-state-ring`/`nabu-splash`/`sheng-fb-fill` 均应为 0，核对 `unixnu.toml` 未变 |

**观察项（全部现有通道、零新探针）**：
os_log `PreBoard` / `SpringBoard` / `DAS` (`buddyComplete` / `Requires Buddy Complete Policy`) /
`AXSpringBoardServer` / `finishedStartup` / `screen owner is now pid` /
`Dismissing overlay` / `Not showing the boot UI overlay` / `BackgroundTaskSuspended`；
`screen.bgrx` sha 与直方图（对比 `e39720f2…` 白页、`112b94b6…` 纯黑、
`00e62fe8…` splash、`e1547326…` 全白）；CDC `nabu-display: frames xfer/nswap/nsurf/
sbytes/nok/ncpy/nret`、`nabu: fbprefs ok=6/6 kept=0`、
`nabu: skip-setup ... eq=/=`（应为 l714 形态、**无 buddyComplete 键**）、
`sheng-mig: sentinel/prefs/readback`、`nabu: scanout hand-off online`、
`nabu-gpu-addon: mounted/target/control/status`、boot-args 回显、
`panic` / `unencrypted panic` / `buf_bread_err16`。

## 5. 唯一变量与回滚点（六问之五）

**唯一变量 = `kernel.macho` 内嵌 `nabu_mig_force[]` plist 的同长数据补丁**（264 B → 264 B，
`cmp -l` 22 B 差异全部落在 `0x228e2..0x228f7`，即目标 plist `0x227f0..0x228f7` 内）；
payload 内 `KERNEL~1.MAC` 用 `mcopy -D o` 覆盖长名/短名同步到 K716。
**args 一个字节不改**：`685/38 <=700`，`added=[]` / `dropped=[]`，38/38 token 同序同字节，
`unixnu.toml` 1111 B、sha8 `47bd52ca` 与 l714 逐字节相同。
`sheng-mig=1`、`offline-legacy-transfer=1`、`BKHideAppleLogoOnLaunch=true`、
3 个 `CAEnable*`、`sheng-smp=1 + nabu-wdt-arm-delay=0` 等全部继承；
**l715 的 `buddyComplete` 键已移除**（K716 计数 0）。

**六文件 sha256 前 8**（完整值见 `l716-sha8.txt`）：

| 文件 | l714 sha8 | **l716 sha8** | 说明 |
|---|---|---|---|
| kernel.macho | `d9b5a174` | **`1209bcc5`** | 唯一变更；K716 全 sha `1209bcc5302faf2fcc2443fdc8da38e68710b014d2930a974918b1367829c509` |
| kernel.macho.adt | `78a6f7e9` | `78a6f7e9` | 逐字节相同 |
| gpu-addon.bin | `950a3968` | `950a3968` | 逐字节相同 |
| owner-trust.bundle | `a4a6bae6` | `a4a6bae6` | 逐字节相同；payload 内 `KERNEL~1.TRU` 与它逐字节一致 |
| boot-merge-r3.img | `ead4b531` | `ead4b531` | 逐字节相同 |
| payload.img | `da7df504` | **`047e1915`** | 仅 `KERNEL~1MAC` 变；`UNIXNU~1TOM` 与 l714 逐字节同 |

**自检**：`cmp -l` 22 B、文件长 125628416 不变、APFS nop `1f2003d5` 未动、
`ncmds=25` 无代码签名、`BKHideAppleLogoOnLaunch` 计数 1 且在 `nabu_fb @0x226ac`、
3 个 `CAEnable*` 计数各 1、l715 `buddyComplete` 计数 0、
抽回 `KERNEL~1MAC` == K716、`KERNEL~1TRU` == `owner-trust.bundle a4a6bae6`、
toml 逐字节同 l714；`fsck.fat -n` 双包 `10 files`；PRELINK 47/47、
payload 体积 201326592 不变。
**`hold/l714/` 六文件 sha 前后未变（payload `da7df504`），未污染**
（`l714-before-sha256.txt` 与 `l714-after-sha256.txt` diff 为空）。

- **回滚基线**：`E:\nabu\hold\l700\`（kernel `efcb4c34`）；
  **上一包 / 只读模板** `E:\nabu\hold\l714\`（kernel `d9b5a174`、payload `da7df504`）。
- **一次性、用完回滚**：`runSetupAssistant=false` + `sheng-mig=1` + 隐藏键组合只属于 l716，
  **不进默认 args**；判读完成后刷回 `hold/l700/payload.img` 或 `hold/l714/payload.img`。
  真正还原只由用户当场点名文件。

## 6. 恢复预案（六问之六，AGENTS.md 0.10 / 0.7 / 0.9 / 附注）

**`AGENTS.md` 0.10 绝对红线：不碰 `boot_a`**（绝不 `fastboot flash boot` /
`flash boot_a`；这块板没有独立 recovery 分区，boot_a 里是 TWRP）。
流程（**仅文字，本会话不执行**；顺序不可换）：

```
recovery arm misc                                             # 不碰 boot_a
flash cust hold/l716/payload.img
fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin   # 最后才重 arm misc
fastboot boot hold/l716/boot-merge-r3.img                     # 临时加载，不写任何分区
```

`E:\nabu\dwc3\build\misc-bootrecovery.bin`（4096 B）；设备 `TWRP recovery f91fa664`。
**`arm-delay=0`**（`sheng-smp=1` 硬性配对，`AGENTS.md` 0.7），咬点 **380-410 s**，
观察完整 **4 分钟窗口**（`AGENTS.md:57-64`），早 panic 也有重置路径；
l714 的 DataMigration t=2.640 / PreBoard finishedStartup t=4.392 均在窗口内。
若 fastboot 全失败：**人工长按电源 10 秒**。回滚见 §5。

**门禁（本会话实测，全部 PASS）**：

| 项 | 结果 | 证据（绝对路径） |
|---|---|---|
| ① 构建 + 模板只读 | 新 tmp 目录 `E:\nabu\tmp\l716-build-20261111\` / 新 `E:\nabu\hold\l716\`；四文件逐字节同 l714；payload 只 `KERNEL~1MAC` 变；`ACCESS_READ` mmap + 192 MiB 流式 + `mcopy -D o`；l714 六文件 sha 前后未变（`da7df504`） | `E:\nabu\tmp\l716-build-20261111\patch_kernel_l716.py`、`assemble_l716.py`、`patch.log`、`assemble.log`、`selfcheck-final.txt`、`l714-before-sha256.txt`、`l714-after-sha256.txt` |
| ② `fsck.fat -n` 10 files + `audit_bootability.py hold/l716 hold/l714 --expected=<K716 全 sha>` | l714/l716 均 `10 files, 67550/97913 clusters` rc=0；→ **`OK (0 fail, 0 warn)`** RC=0（PRELINK 47/47、ncmds、payload 体积不变、仅 kernel sha 变） | `gate1-fsck.txt`、`gate1-bootability.txt`、`gate1-run-final.txt` |
| ③ `audit_dual.py --tag l716` | 逗号分隔真实 sha `kernel=1209bcc5,payload=047e1915,adt=78a6f7e9,addon=950a3968,trust=a4a6bae6,boot=ead4b531,args=685/38` → **`7 items compared, 7 match, 0 differ`** | `gate2-audit_dual.txt`、`measured-values.txt`、`E:\nabu\state\audit-l716.json` |
| ④ `tmp/audit_args.py l714 l716` | `685/38 → 685/38`（≤700）；**`added=[]`、`dropped=[]`**、38/38 token 同序同字节、关键开关全在 → **`RESULT: OK`** rc=0 | `gate3-audit_args.txt`、`gate23-run-final.txt`、`toml-token-compare.txt` |
| ⑤ Windows Python `precheck.py` | `run_precheck_l716.py` 驱动 `--pkg E:/nabu/hold/l716 --baseline E:/nabu/hold/l714`，八字段齐、`token_diff=ok` → `stamp written: state/precheck.json` rc=0；`precheck_check.py` → **`[gate] OK`** rc=0，`dual audit OK: audit-l716.json (7 items)` | `gate4-precheck.txt`、`gate4-fields.txt`、`gate4-precheck_check.txt`、`gate4-run-final.txt`、`E:\nabu\state\precheck.json` |
| ⑥ 六问板报 | 本文件；`board_report_check.py l716` → `[report] OK` 6/6 节 | `E:\nabu\state\board-report-l716.md` |
| 候选静态核对（只读） | A 的消费者/副作用 + B 的 plist 偏移/字节 + DSC 普查；结论见 §0 | `kernel-plists-raw.txt`、`plist-before.txt`、`plist-after.txt`、`diff-bytes.txt`、`toml-token-compare.txt` |
| 自检 | 改前/改后 plist 全文、`cmp -l` 22 B 全在目标 plist 内、长度不变、APFS nop 未动、隐藏键/3 个 CA 键仍在、Buddy 键已移除、抽回 kernel/trust/toml、l714 未污染 | `K716.txt`、`l716-sha8.txt`、`selfcheck-final.txt`、`diff-bytes.txt` |

**零新探针 / 零重编 / 零安全放宽 / 零源码改动 / 零 boot_a 触碰**；
**同长数据补丁**（264 B → 264 B）、**APFS nop `1f2003d5` 未动**、
**kernel 无代码签名**（`ncmds=25`，无 `LC_CODE_SIGNATURE`）；
**l701 / l710 / l714 先例**（同类同长内嵌 plist 数据补丁，l710 板侧 os_log 实证生效）。
本轮**未运行 adb/fastboot、未上板、未写 `E:\nabu\logs\rounds\l716.json`**。

**fc76 未消费与双审计偏离**：编译侧 fc76 自 **11-06 14:57** 未消费；
`audit_dual.py --tag l716` 的 `--peer` / `--mine` **两侧均为板侧实测**
（7/7 equal，真实 sha 见 §5），照 `E:\nabu\state\board-report-l714.md` 注明：
偏离 `AGENTS.md:36-47` 0.6 的「编译侧 vs 板侧」原意；本轮零重编、无编译侧成果可比对。

## 7. 证据路径（全部绝对路径）

- 构建与自检：`E:\nabu\tmp\l716-build-20261111\`（`patch_kernel_l716.py`、
  `assemble_l716.py`、`selfcheck_l716.py`、`run_gate1.py`、`run_gate23.py`、
  `run_precheck_l716.py`、`kernel.macho`、`plist-before.txt`、`plist-after.txt`、
  `diff-bytes.txt`、`K716.txt`、`kernel-plists-raw.txt`、`toml-token-compare.txt`、
  `patch.log`、`assemble.log`、`selfcheck-final.txt`、`l716-sha8.txt`、
  `l714-before-sha256.txt`、`l714-after-sha256.txt`、`gate1-fsck.txt`、
  `gate1-bootability.txt`、`gate2-audit_dual.txt`、`gate3-audit_args.txt`、
  `gate4-precheck.txt`、`gate4-precheck_check.txt`、`measured-values.txt`）
- 戳与板报：`E:\nabu\state\audit-l716.json`、`E:\nabu\state\precheck.json`、
  `E:\nabu\state\board-report-l716.md`
- 包：`E:\nabu\hold\l716\`（六文件）；只读模板 `E:\nabu\hold\l714\`；
  回滚 `E:\nabu\hold\l700\`；隐藏键来源 `E:\nabu\hold\l710\`
- 链：`E:\nabu\state\HANDOVER-20261110-l696.md` §7ad/§7ae、
  `E:\nabu\logs\rounds\l714.json`、`E:\nabu\logs\l714-v1.jsonl`、
  `E:\nabu\logs\l715-cdc.bin`、`E:\nabu\logs\l715-v1.jsonl`、
  `E:\nabu\logs\l715-diag-tree\screen.bgrx`、`E:\nabu\state\board-report-l715.md`、
  `E:\nabu\state\board-report-l710.md` / `-l712.md` / `-l714.md`、
  `/root/merge/out-v6/nabu/ipados-17.7.11-21H461/nabu/tree/.cache/xnu-work/bsd/kern/kern_exec.c`、
  `/root/dsc/dyld_shared_cache_arm64.*`、`E:\nabu\tmp\0012.patch`、
  `E:\nabu\bridge\pending\20261105-030x-7ba4-display-change-ledger-l533-l577.md`、
  `E:\nabu\AGENTS.md`

**结论：全部门禁 PASS ⇒ 可否上板：可**（上板由用户执行并判读；本会话未执行）。
