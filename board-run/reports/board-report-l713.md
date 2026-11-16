# 上板报告 l713（一次性临时功能轮：首次在 published 状态开 `nabu-autolog=1`）

> 本报告是**上板前**门禁文件；本会话未运行 adb/fastboot、未上板、未写
> `E:\nabu\logs\rounds\l713.json`。只读模板 `E:\nabu\hold\l712\` 六文件 sha
> 前后未变（payload `38a58279`）。按 `E:\nabu\AGENTS.md` 0.5 / 0.6 / 0.7 / 0.9 /
> 0.10 编写，格式照 `E:\nabu\state\board-report-l712.md`。
> 全部证据目录：`E:\nabu\tmp\l713-build-20261111\`。

## 0. 用户授权与性质

**用户自动授权（2026-11-11）**：「自动推进、临时插帧/数据补丁、义无反顾向前、不要停下来」；
`E:\nabu\state\HANDOVER-20261110-l696.md` §7aa 记边界：只用**板上内核已有机制**、
一次性临时轮、**用完回滚、不进默认 args**、零新增 printf/IOLog/计数器/探针、
零重编内核、零安全放宽、不碰 `boot_a`；全套门禁 + 六问板报。
本轮**不改源码、不改内核字节、不放宽任何策略**：kernel 直接继承 l710/l711/l712 的
`d9b5a174`（同长 `nabu_fb` plist 数据补丁），只对 payload 内 `unixnu.toml` 做
args token 原位替换。设备上板由用户执行并判读。

## 1. 唯一问题（六问之一）

**开 `nabu-autolog=1` 能否推 SpringBoard 过 `finishedStartup` / 注册
`AXSpringBoardServer` / 成为 explicit screen owner，从而让隐藏 BootUI 后出现系统自己的帧？**

l712 已把断点钉死：隐藏键生效、`Presenting overlay` 消失后屏纯黑、frames 全 0，
SpringBoard pid27 在跑但没走完自身初始化。`nabu-state-ring=10` 只是 banner 诊断
时钟，本轮不靠 splash；`nabu-autolog` 是板上已有的 SpringBoard 启动/自动登录推进
开关（用户 hypothesis），从未在当前 published 状态试过。硬判据是 `screen.bgrx`
与官方 os_log / CDC 输出；运行期像素静态不能判 ⇒ 满足 `AGENTS.md` 0.5。

## 2. 已闭合到哪（六问之二，日志优先 + §7ad 全链 / chain）

1. **GOP 写路径活**：`E:\nabu\logs\rounds\l706.json`：`sheng-fb-fill=4` ⇒ 全屏白
   `e1547326…`（1,024,000 个 `(255,255,255,255)`）。
2. **板上主动绘制活**：`E:\nabu\logs\rounds\l709.json` / `l711-diag-tree\screen.bgrx`：
   `nabu-state-ring=10` 的 `NABUSTATE1` banner 时钟（`NativeUfs.cpp:3542` /
   `:4092-4101`）调 `native_splash_update()` ⇒ 多色屏
   `00e62fe873668aba…`（近黑蓝 1,006,600 + 金标 5,400 + 白进度格 12,000 px）。
3. **l707 的 `ncpy=5` 是 BootUI overlay 自己的 5 次 surface 拷贝**，不是桌面帧；
   `NativeLegacyProbe.cpp:861-893` / `NativeLegacyDisplay.cpp:329-334`。
4. **隐藏键链已闭合且板侧实证**：`E:\nabu\state\board-report-l710.md` §2：
   `kern_exec.c:8520-8572` 的 323 B `nabu_fb[]` plist、六个 `fb_prefs[]` 路径、
   四个 backboardd 域 `keepIfPresent=0` 强制覆盖、每轮写；板上字面量在
   `hold/l707/kernel.macho @0x226ac..0x227ee`；`/root/dsc/dyld_shared_cache_arm64.05`
   里 `com.apple.backboardd @0x1c25f9a`、`BKHideAppleLogoOnLaunch @0x1c263ec`、
   `hideAppleLogoOnLaunch @0x1c263d6`；BackBoard uuidtext 官方 hide 格式串。
   `E:\nabu\logs\l710-v1.jsonl` / `l712-v1.jsonl` 实测
   `Not showing the boot UI overlay/underlay because we were told to hide it`（各 4 条）、
   `Presenting overlay: <BKDisplayRenderOverlayAppleLogo>` **0 条**。
5. **l712 断点（§7ac / §7ad）**：`E:\nabu\logs\l712-v1.jsonl` 124,695 行：
   SpringBoard pid27 前台、`FBSystemShellInitialize`、backboardd
   `checkInAndWaitForDataMigration` / `START WATCHDOG` 都完成；但主可执行 uuidtext
   仅 73 B / **自身 os_log 0 行**、`AXSpringBoardServer` 22 次查询伴随
   `No such process`（全文件 1,290 条）、`finishedStartup` /
   `screen owner is now pid` / `systemShellDidFinishLaunching` /
   `Dismissing boot logo` 全 **0**、`No explicit screen owner` 4 条；
   `E:\nabu\logs\l712-diag-tree\screen.bgrx` sha `112b94b6b55a348f…` 纯黑、
   `E:\nabu\logs\l712-cdc.bin` `nabu-display: frames ... ncpy=0` 全 0。
   ⇒ 断点 = **SpringBoard 自身初始化没走完**，显示/GOP/overlay 机制全部可用。
6. **唯一变量静态**：`E:\nabu\hold\l712\kernel.macho` 已有字面量
   `nabu-autolog @0x18035b`（1 处）与 `NativeUfs: args hang=%u panic=%u autolog=%u ...`
   回显格式 `@0x181d7a`（`autolog=%u @0x181d9b`）；源码
   `/root/merge/UniXNU-nabu/drivers/NativeUfs/NativeUfs.cpp:2137-2139`
   `PE_parse_boot_argn("nabu-autolog") -> if(!autolog) return 0;` 位于
   `native_ufs_panic_recovery()`（misc `boot-recovery` 自 arm），`:4628-4637` 是
   args 回显；`E:\nabu\notes\AUTOLOG.md` / `notes\HANDBOOK.md` 记其历史语义为
   「启动早期预写 misc boot-recovery」。`E:\nabu\state\board-report-l696.md`..`-l705.md`
   逐轮明记「本轮 args 无 `nabu-autolog`」⇒ **当前 published 状态从未开过**。
   **诚实声明（chain 不伪造闭环）**：静态侧**没有**从 `nabu-autolog` 到
   SpringBoard `finishedStartup` / `AXSpringBoardServer` 的已证传导边；用户把它
   视作 SpringBoard 启动/自动登录推进开关，本轮正是用一次可证伪的运行期单变量
   判定检验该 hypothesis，失败即按判据收窄到更早的初始化依赖。
7. **包侧链**：`hold/l713` 五文件逐字节 = `l712`，payload 仅 `UNIXNU~1TOM` 变
   （1118 → 1114 B、args 692/38 → 688/38、其余 37 token 同序同字节），
   cluster 12688 整簇替换 + 目录项 size，`ACCESS_READ` mmap + 192 MiB 流式。
   证据 `E:\nabu\tmp\l713-build-20261111\build.log` / `assemble.log` / `selfcheck.txt`。

## 3. 为什么离线不行（六问之三，why_board）

静态能证：§2 全链、l713 五文件逐字节同 l712、抽回 kernel == `d9b5a174` 且含
`<key>BKHideAppleLogoOnLaunch</key><true/>` 与 `nabu-autolog @0x18035b`、
抽回 toml 逐 token 比（37/37 同序同字节、688/38 ≤700、added/dropped 与声明一致）、
toml 外其余 payload 内文件与 l712 全 SAME、关键开关逐项全在。
但「当前 published 状态首次开 `nabu-autolog=1` 后，SpringBoard 是否走完
`finishedStartup` / 注册 `AXSpringBoardServer` / 成为 explicit screen owner，
从而让隐藏 BootUI 后 GOP 出现系统自己的帧」是**运行期事实**：该 arg 在板上内核
的解析点、misc 自 arm 与 iOS 用户态启动时序的交互、cfprefsd / launchd /
SpringBoard 初始化依赖是否被间接推进，都只有真机 `os_log` / CDC /
`screen.bgrx` 能给出。历史轮次从未在 published 状态开过，日志与静态都无法
产生这个新状态 ⇒ 属 `AGENTS.md` 0.5 允许的「验证静态结论在真机上是否成立的
那一次判定」。失败也有信息（见 §4）。

## 4. 失败也有信息（六问之四，三判读）

判据**全是官方已有输出，零新增 printf/IOLog/计数器/探针**：os_log
`AXSpringBoardServer` 不再 `No such process`；`finishedStartup` /
`screen owner is now pid` / `systemShellDidFinishLaunching` /
`Dismissing boot logo` 任一出现；SpringBoard 自身 os_log 行数从 0 变正；
`nabu-display: frames` 从全 0 变 `ncpy>0`；`screen.bgrx` 不再是纯黑
`112b94b6…`。

| 结果 | 判读 |
|---|---|
| 上述判据出现 + 屏非纯黑 | **`nabu-autolog=1` 推进了 SpringBoard 断点**，系统桌面层顶上隐藏的 BootUI；写路径 / overlay / 内容源三条线合拢 |
| **仍纯黑 `112b94b6…` / frames 全 0 / 上述 os_log 全 0** | **autolog 不推进该断点**；按失败收窄到 SpringBoard 更早的初始化依赖（`CanvasSizes`/`OpenByName`、`data_ark`/激活态、`default.metallib` Fault、已写入的 `SetupDone`/`BKNoWatchdogs` 之外的落点），并排除该 arg 作为推进开关 |
| 仍是 `00e62fe8…` splash 多色屏 | 替换未生效 / splash 残留，对账 args 回显（`nabu-state-ring=10` 应为 0、`nabu-autolog=1` 应为 1）与 `NativeUfs: args ... autolog=1` |

**观察项**：os_log 关键字计数（`AXSpringBoardServer`、`No such process`、
`finishedStartup`、`screen owner is now pid`、`systemShellDidFinishLaunching`、
`Dismissing boot logo`、`No explicit screen owner`、`Not showing the boot UI
overlay`、`Presenting overlay`）+ SpringBoard 主可执行 uuidtext / 自身 os_log 行数；
CDC `NativeUfs: args ... autolog=1` 回显、`nabu-display: frames xfer nswap nsurf
sbytes nok ncpy nret`、`nabu: scanout hand-off online`、`nabu-gpu-addon:`、
boot-args 回显、panic / unencrypted panic / `buf_bread_err16`；
`screen.bgrx` sha / 直方图（对比 `112b94b6…`、`00e62fe8…`、`ca18b728…`、
`e1547326…`）。

## 5. 唯一变量与回滚（六问之五）

**唯一变量 = payload 内 `EFI/UNIXNU/UNIXNU~1TOM`（`unixnu.toml`）的 args token
原位替换**：`nabu-state-ring=10` → `nabu-autolog=1`。理由：`nabu-state-ring` 只是
banner 诊断时钟、本轮不靠 splash；`nabu-autolog` 是板上已有、当前 published
状态从未开过的 SpringBoard 启动/自动登录推进开关。

**args 预算（精确实测，非猜测）**：l712 `692/38`；`nabu-state-ring=10` **18 B**
→ `nabu-autolog=1` **14 B**，净 **-4 B** ⇒ **`688/38 ≤700`**，token 数不变 38。
`added=['nabu-autolog=1']`、`dropped=['nabu-state-ring=10']`，
其余 **37 token 同序同字节**；toml `1118 → 1114 B`、sha8 `0dab34fe → a038c673`。
未超 700，**无需再缩短任何已否证 token**。

**逐项核关键开关全在**：`offline-legacy-transfer=1`、`t733-managed-rt-normal=1`、
`sheng-fb-gpu=1`、`sheng-gpu-addon=1`、`sheng-smp=1` + `nabu-wdt-arm-delay=0`、
`-apfs_shared_datavolume`、`nabu-fbcon=0`、`nabu-log=1`、`nabu-panic-restart=2`、
全部 `offline-legacy-validate/retire/gop/pm/phase/register`；`nabu-splash=1` 仍无。
隐藏键所在 kernel 文件**一个字节未改**、随文件继承。

**六文件 sha256 前 8**（完整值见 `l713-sha8.txt`）：

| 文件 | l712 sha8 | **l713 sha8** | 说明 |
|---|---|---|---|
| kernel.macho | `d9b5a174` | **`d9b5a174`** | 逐字节相同，含隐藏键补丁 |
| kernel.macho.adt | `78a6f7e9` | `78a6f7e9` | 逐字节相同 |
| gpu-addon.bin | `950a3968` | `950a3968` | 逐字节相同 |
| owner-trust.bundle | `a4a6bae6` | `a4a6bae6` | 逐字节相同 |
| boot-merge-r3.img | `ead4b531` | `ead4b531` | 逐字节相同 |
| payload.img | `38a58279` | **`3821aa4c`** | 只 `unixnu.toml` 变 |

payload 手术见 `build_l713.py`：输入 `hold/l712/payload.img` 只读并以 `mmap
ACCESS_READ` 打开（**绝不 mmap 输入 payload 可写**，l695p 教训），192 MiB
`copyfileobj` 流式复制到新输出；`cluster 12688` **整簇替换**（slack 全零）、
目录项 size `1118 → 1114`；`assemble_l713.py` 把六文件放进**新建**
`E:\nabu\hold\l713\`，中间产物在**新建** `E:\nabu\tmp\l713-build-20261111\`。
抽回 7 个 inner file：仅 `UNIXNU~1TOM` CHANGED，`KERNEL~1MAC` == `d9b5a174`
（含 hide 键、`nabu-autolog` 字面量），其余全 SAME（`selfcheck.txt`）。
**`hold/l712/` 六文件 sha 前后未变（payload `38a58279`），未污染。**

- **回滚基线**：`E:\nabu\hold\l700\`（kernel `efcb4c34`，已验证 4 分钟窗口）；
  **上一包 / 只读模板** `E:\nabu\hold\l712\`（kernel `d9b5a174`、
  payload `38a58279`，本会话六文件 sha 前后未变，见 `selfcheck.txt`）。
- **一次性、用完回滚**：`nabu-autolog=1` + 隐藏键 + 无 banner 时钟组合只属于
  l713，**不进默认 args**；判读完成后刷回 `hold/l700/payload.img` 或
  `hold/l712/payload.img`。真正还原只由用户当场点名文件。

## 6. 恢复预案 / 门禁 / 附注（六问之六）

**`AGENTS.md` 0.10 绝对红线：不碰 `boot_a`**（绝不 `fastboot flash boot` /
`flash boot_a`；这块板没有独立 recovery 分区，boot_a 里是 TWRP）。
流程（**仅文字，本会话不执行**；顺序不可换）：

```
recovery arm misc                                             # 不碰 boot_a
fastboot flash cust hold/l713/payload.img
fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin   # 最后才重 arm misc
fastboot boot hold/l713/boot-merge-r3.img                     # 临时加载，不写任何分区
```

`E:\nabu\dwc3\build\misc-bootrecovery.bin`（4096 B）；设备 `TWRP recovery f91fa664`。
`arm-delay=0`（`sheng-smp=1` 硬性配对，`AGENTS.md` 0.7），咬点 **380-410 s**，
观察完整 **4 分钟窗口**（`AGENTS.md:57-64`），早 panic 也有重置路径。
`nabu-autolog=1` 可能让内核在 panic 时自写 misc `boot-recovery`，仍严格按上述
人工顺序 arm / 最后重 arm，不据此省略任何一步。若 fastboot 全失败：人工长按
电源 10 秒。回滚见 §5。

**门禁（本会话实测，全部 PASS）**：

| 项 | 结果 | 证据（绝对路径） |
|---|---|---|
| ① 构建 + 模板只读 | 新 tmp 目录 / 新 `hold/l713/`；五文件逐字节同 l712；payload 只 toml 变；整簇替换 + 目录项 size；`ACCESS_READ` / 192 MiB 流式；l712 六文件 sha 前后未变（payload `38a58279`） | `E:\nabu\tmp\l713-build-20261111\build_l713.py`、`assemble_l713.py`、`build.log`、`assemble.log`、`selfcheck.txt`、`l712-before-sha256.txt` |
| ② `fsck.fat -n` + `audit_bootability.py hold/l713 hold/l712 --expected=d9b5a174…` | l712/l713 均 `10 files, 67550/97913 clusters` rc=0；→ **`OK (0 fail, 0 warn)`** RC=0（PRELINK 47/47、ncmds、payload 体积不变、kernel sha 匹配） | `gate1-fsck.txt`、`gate1-bootability.txt` |
| ③ `audit_dual.py --tag l713` | 逗号分隔真实 sha `kernel=d9b5a174,payload=3821aa4c,adt=78a6f7e9,addon=950a3968,trust=a4a6bae6,boot=ead4b531,args=688/38` → **`7 items compared, 7 match, 0 differ`** | `gate2-audit_dual.txt`、`E:\nabu\state\audit-l713.json` |
| ④ `tmp/audit_args.py l712 l713` | `692/38 → 688/38`（≤700）；**`added=['nabu-autolog=1']`、`dropped=['nabu-state-ring=10']`** → **`RESULT: OK`** rc=0；其余 37 token 同序同字节、关键开关全在 | `gate3-audit_args.txt`、`selfcheck.txt` |
| ⑤ Windows Python 3.12.10 `precheck.py` | `run_precheck_l713.py` 驱动 `--pkg E:/nabu/hold/l713 --baseline E:/nabu/hold/l712`，声明 added/dropped → `stamp written: state/precheck.json` rc=0；八字段齐、`token_diff=ok`；`precheck_check.py` → **`[gate] OK`** rc=0，`dual audit OK: audit-l713.json (7 items)` | `gate4-precheck.txt`、`gate4-precheck_check.txt`、`E:\nabu\state\precheck.json` |
| ⑥ 六问板报 | 本文件 | `E:\nabu\state\board-report-l713.md` |
| 自检 | 抽回 toml 逐 token 比（37/37 同序同字节、≤700）；抽回 kernel `d9b5a174` 且含隐藏键 + `nabu-autolog @0x18035b`；toml 外其余 payload 内文件与 l712 全一致；六文件 sha8 见 §5；`hold/l712/` 未污染 | `selfcheck.py`、`selfcheck.txt`、`l713-sha8.txt`、`extract-l713\` |

**零新探针 / 零重编 / 零安全放宽 / 零源码改动 / 零 boot_a 触碰**；
kernel 仍是 l710 同长数据补丁（323 B → 323 B、`cmp -l` 仅 123 字节落在
`0x226d3..0x22753`、`ncmds=25`、文件长 125628416 不变），
**APFS nop `1f2003d5` 未动**；本轮**未运行 adb/fastboot、未上板、
未写 `E:\nabu\logs\rounds\l713.json`**。

**fc76 未消费与双审计两侧板侧实测偏离**：编译侧 fc76 自 **11-06 14:57** 未消费；
`audit_dual.py --tag l713` 的 `--peer` / `--mine` **两侧均为板侧实测**
（7/7 equal，真实 sha 见 §5），照 `E:\nabu\state\board-report-l712.md` 注明：
偏离 `AGENTS.md:36-47` 0.6 的「编译侧 vs 板侧」原意；本轮零重编、无编译侧
成果可比对。

## 7. 证据路径（全部绝对路径）

- 构建与自检：`E:\nabu\tmp\l713-build-20261111\`（`build_l713.py`、
  `assemble_l713.py`、`selfcheck.py`、`run_precheck_l713.py`、
  `payload.img`、`build.log`、`assemble.log`、`selfcheck.txt`、
  `l712-before-sha256.txt`、`gate1-fsck.txt`、`gate1-bootability.txt`、
  `gate2-audit_dual.txt`、`gate3-audit_args.txt`、`gate4-precheck.txt`、
  `gate4-precheck_check.txt`、`l713-sha8.txt`、`extract-l713\`）
- 戳与板报：`E:\nabu\state\audit-l713.json`、`E:\nabu\state\precheck.json`、
  `E:\nabu\state\board-report-l713.md`
- 包：`E:\nabu\hold\l713\`（六文件）；只读模板 `E:\nabu\hold\l712\`；
  回滚 `E:\nabu\hold\l700\`；隐藏键来源 `E:\nabu\hold\l710\`
- 板上证据：`E:\nabu\logs\rounds\l706.json` / `l707.json` / `l708.json` /
  `l709.json`、`E:\nabu\logs\l710-v1.jsonl`、`E:\nabu\logs\l712-v1.jsonl`、
  `E:\nabu\logs\l712-cdc.bin`、`E:\nabu\logs\l712-diag-tree\screen.bgrx`、
  `E:\nabu\logs\l709-diag-tree\screen.bgrx`、`E:\nabu\logs\l711-diag-tree\screen.bgrx`、
  `E:\nabu\state\HANDOVER-20261110-l696.md` §7aa / §7ab / §7ac / §7ad、
  `E:\nabu\state\board-report-l709.md` / `-l710.md` / `-l711.md` / `-l712.md`、
  `E:\nabu\notes\AUTOLOG.md` / `notes\HANDBOOK.md`
- 静态链：`/root/merge/UniXNU-nabu/drivers/NativeUfs/NativeUfs.cpp:2137-2139`
  / `:4628-4637`、`:3542` / `:4092-4101`、
  `/root/merge/out-v6/nabu/ipados-17.7.11-21H461/nabu/tree/.cache/xnu-work/bsd/kern/kern_exec.c:8520-8572`、
  `NativeLegacyProbe.cpp:861-893`、`NativeLegacyDisplay.cpp:329-334`、
  `/root/dsc/dyld_shared_cache_arm64.05`、`E:\nabu\AGENTS.md`

**结论：全部门禁 PASS ⇒ 可否上板：可**（上板由用户执行并判读；本会话未执行）。
