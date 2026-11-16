# 上板报告 l714（一次性临时功能轮：`nabu-autolog=1` → `sheng-mig=1` 重 arm DataMigration）

> 本报告是**上板前**门禁文件；本会话未运行 adb/fastboot、未上板、未写
> `E:\nabu\logs\rounds\l714.json`。只读模板 `E:\nabu\hold\l713\` 六文件 sha
> 前后未变（payload `3821aa4c`）。按 `E:\nabu\AGENTS.md` 0.5 / 0.6 / 0.7 / 0.9 /
> 0.10 编写，格式照 `E:\nabu\state\board-report-l713.md`；六节标题按
> `E:\nabu\tmp\board_report_check.py` 的精确字符串落盘（本包实测通过）。
> 全部证据目录：`E:\nabu\tmp\l714-build-20261111\`。

## 0. 用户授权与性质

**用户自动授权（2026-11-11）**：「自动推进、临时插帧/数据补丁、不停」；
`E:\nabu\state\HANDOVER-20261110-l696.md` §7aa/§7ad 记边界：**只用板上内核已有机制**、
**一次性临时轮、用完回滚、不进默认 args**、零新增 printf/IOLog/计数器/探针、
零重编内核、零安全策略放宽、不碰 `boot_a`；全套门禁 + 六问板报。
本轮**不改源码、不改内核字节、零新增探针**：kernel 直接继承 l710/l711/l712/l713
的 `d9b5a174`（同长 `nabu_fb` plist 数据补丁），只对 payload 内 `unixnu.toml`
做 args token 原位替换。设备上板由用户执行并判读；判读后一次性组合即作废回滚。

## 1. 唯一问题（六问之一）

**把已否证的 `nabu-autolog=1` 原位替换成板上内核已有的 `sheng-mig=1`（重 arm
`/private/var/datamigrator/migrate` 哨兵与 `com.apple.migration.plist` 的
`forceMigrationOnNextReboot` / `runSetupAssistant`），能否让 SpringBoard 过
`finishedStartup` / 注册 `AXSpringBoardServer` / 出现 explicit screen owner，
或在隐藏 BootUI overlay 后显示系统自己的进度/迁移页？**

判据全为官方已有输出、零新增探针：CDC `sheng-mig: sentinel/prefs/err/readback`；
`nabu-display: frames` 是否从全 0 变 `ncpy>0`；`screen.bgrx` 是否不再是纯黑
`112b94b6…`；os_log `Migration complete` / `finishedStartup` /
`screen owner is now pid` / `Dismissing boot logo`。
**静态只能证消费者在，重 arm 的运行期结果必须上板 ⇒ 满足 `AGENTS.md` 0.5。**

## 2. 已闭合到哪（六问之二，日志优先 + §7ad/l713 全链 / chain）

1. **GOP 写路径活**：`E:\nabu\logs\rounds\l706.json` 全白 `e1547326…`；
   **板上主动绘制活**：`l709` / `l711` splash 多色 `00e62fe8…`。
2. **隐藏键链板侧实证**：`E:\nabu\state\board-report-l710.md` §2：
   `kern_exec.c:8520-8572` 的 323 B `nabu_fb[]` plist、六个 `fb_prefs[]` 路径、
   四个 backboardd 域 `keepIfPresent=0` 强制覆盖、每轮写；`/root/dsc/dyld_shared_cache_arm64.05`
   里 `com.apple.backboardd @0x1c25f9a`、`BKHideAppleLogoOnLaunch @0x1c263ec`。
   `l710-v1.jsonl` / `l712-v1.jsonl` 实测 `Not showing the boot UI overlay/underlay`，
   `Presenting overlay` 0 条。kernel `d9b5a174` 逐字节继承未改。
3. **§7ad 断点（l712/l713）**：`E:\nabu\logs\l712-v1.jsonl`（124,695 行）、
   `E:\nabu\logs\rounds\l713.json`：SpringBoard pid27 前台、`FBSystemShellInitialize`、
   backboardd `checkInAndWaitForDataMigration` / `START WATCHDOG` 已完成；但
   `AXSpringBoardServer` 持续 `No such process`、`finishedStartup` /
   `screen owner is now pid` / `Dismissing boot logo` 全 0、`No explicit screen owner`；
   隐藏 BootUI 后 `screen.bgrx` = `112b94b6…` 纯黑、`nabu-display: frames` 全 0。
   **l713 已 DECISIVE-NEGATIVE 否证 `nabu-autolog=1`**：只管 misc/panic 自 arm，
   不推 SpringBoard；隐藏 overlay 后仍无任何系统帧顶上。
4. **唯一变量静态（板上内核已有机制）**：`E:\nabu\hold\l713\kernel.macho`
   含 `sheng-mig @0x111b4f`、`/private/var/datamigrator/migrate`、
   `sheng-mig: sentinel=%d prefs=%d err=%d` / `sheng-mig: readback rc=%d fsz=%llu`
   官方格式串、内嵌 plist `forceMigrationOnNextReboot=true` /
   `runSetupAssistant=true`；源码 `kern_exec.c:8602-8611,8624-8629` 为
   `PE_parse_boot_argn("sheng-mig")` 门控的 re-arm：建目录 → 写空 migrate 哨兵 →
   写 `com.apple.migration.plist`。
5. **日志核对（不伪造闭环）**：指定 grep 集合仅 `E:\nabu\logs\rounds\l616.json`
   四行命中（旧内核 `0377bb00`：`sheng-mig=1` → `sentinel=0 prefs=0 err=0` /
   `readback rc=0 fsz=0` → datamigrator t=123.6 s → PreBoard t=225 s 迁移进度 UI），
   **近期轮次未否证该 boot-arg**；板报 l690-l699 的「PreBoard 未启用/已排除」不是
   `sheng-mig` 的否定。诚实声明：当前隐藏键基线 os_log 已有
   `DMPerformMigrationIfNeeded` / `Migration complete (Elapsed time: 0.00 seconds)`，
   §7c/§6b 又记旧 published 状态 `PUIProgressWindow` / `Migration failed` /
   keybag-keychain 失败，且 `l659`/`l632` 记「去掉 sheng-mig ⇒ 跳过 PreBoard、
   t≈99.3 s 直起 SpringBoard 交 5-6 帧」。这些都不是在
   `d9b5a174` + 隐藏 BootUI + **重 arm 哨兵** 这一组合下对 `finishedStartup` /
   explicit screen owner 的直接否证 ⇒ 本轮是针对性可证伪单变量。
6. **包侧链**：`hold/l714` 五文件逐字节 = `l713`，payload 仅 `UNIXNU~1TOM` 变
   （1114 → 1111 B、args 688/38 → 685/38），cluster 12688 整簇替换 + 目录项 size，
   `ACCESS_READ` mmap + 192 MiB 流式复制，绝不 mmap 输入 payload 可写（l695p 教训）。
   证据 `E:\nabu\tmp\l714-build-20261111\build.log` / `assemble.log` / `selfcheck-final.txt`。

## 3. 为什么离线不行（六问之三，why_board / AGENTS.md 0.5）

静态能证：§2 全链、抽回 kernel == `d9b5a174` 且含
`<key>BKHideAppleLogoOnLaunch</key><true/>` 与 `sheng-mig @0x111b4f`、
抽回 toml 逐 token 比（37/37 同序同字节、685/38 ≤700、added/dropped 与声明一致）、
toml 外其余 payload 内文件与 l713 全 SAME、关键开关逐项全在。
但**缺的量只能运行期得到**：哨兵是否被 datamigrator 消费、
`DMPerformMigrationIfNeeded` 是否从 0.00 s 变长、PreBoard/PUIProgressWindow
是否被拉起、SpringBoard 是否走完 `finishedStartup` / 注册
`AXSpringBoardServer` / 成为 explicit screen owner、隐藏 BootUI 后哪一路向 GOP
提交帧——都只有真机 CDC / os_log / `screen.bgrx` 能给出。
**静态只证消费者与字面量在，不能证运行期状态** ⇒ 属 `AGENTS.md` 0.5 允许的
「验证静态结论在真机上是否成立的那一次判定 / 日志与静态都无法产生的新状态」；
失败也有可判读信息（见 §4）。

## 4. 失败也有信息（六问之四，三判读 + 观察项）

判据**全是官方已有输出，零新增 printf/IOLog/计数器/探针**。

| 结果 | 判读 |
|---|---|
| **A**：`sheng-mig:` 输出正常 + `finishedStartup` / `screen owner is now pid` / `Dismissing boot logo` 出现 + `AXSpringBoardServer` 不再 `No such process` + `ncpy>0` + 屏非纯黑 | **重 arm 越过了断点**：DataMigration 重 arm 推 SpringBoard 走完自身初始化，系统 UI 顶上隐藏的 BootUI，写路径/overlay/内容源三条线合拢 |
| **B**：只见 `PreBoard` / `PUIProgressWindow` / `DMPerformMigrationIfNeeded` 耗时 >0 等系统进度页，`finishedStartup` 仍 0、屏仍纯黑 | **重 arm 只选中迁移 UI，未给 SpringBoard explicit owner**：哨兵被消费但断点在 SpringBoard 更早依赖，收窄到 `CanvasSizes`/`OpenByName`、`data_ark`/激活态、`default.metallib` Fault、keybag/keychain |
| **C**：仍纯黑 `112b94b6…` / frames 全 0 / `Migration complete` 0.00 s / 上述 os_log 全 0 | **migration 不是该断点**：按失败排除 `sheng-mig=1` 作为推进开关，继续查 SpringBoard 更早初始化依赖 |
| 出现 `00e62fe8…` splash 多色屏 | 替换/残留异常，对账 boot-args 回显与 `nabu-state-ring` 应为 0 |

**观察项**（全部现有通道）：CDC `sheng-mig: sentinel/prefs/err/readback`、
`nabu-display: frames xfer nswap nsurf sbytes nok ncpy nret`、
`nabu: scanout hand-off online`、`nabu-gpu-addon: mounted/target/control/status`、
boot-args 回显、panic / unencrypted panic / `buf_bread_err16`；
os_log `DMPerformMigrationIfNeeded` / `Migration complete` / `finishedStartup` /
`screen owner is now pid` / `AXSpringBoardServer` / `No explicit screen owner` /
`Not showing the boot UI overlay`；
`screen.bgrx` sha / 直方图（对比 `112b94b6…` 纯黑、`00e62fe8…` splash、
`ca18b728…`、`e1547326…` 全白）。

## 5. 唯一变量与回滚点（六问之五）

**唯一变量 = payload 内 `EFI/UNIXNU/UNIXNU~1TOM`（`unixnu.toml`）的 args token
原位替换**：`nabu-autolog=1`（14 B）→ `sheng-mig=1`（11 B）。

**args 预算（精确实测，非猜测）**：l713 `688/38` → **`685/38 ≤700`**，净 -3 B，
token 数不变 38。`added=['sheng-mig=1']`、`dropped=['nabu-autolog=1']`，
其余 **37 token 同序同字节**；toml `1114 → 1111 B`、sha8 `a038c673 → 47bd52ca`。
未超 700，无需再缩短任何已否证 token。
**逐项核关键开关全在**：`offline-legacy-transfer=1`、`t733-managed-rt-normal=1`、
`sheng-fb-gpu=1`、`sheng-gpu-addon=1`、`sheng-smp=1` + `nabu-wdt-arm-delay=0`、
`-apfs_shared_datavolume`、`nabu-fbcon=0`、`nabu-log=1`、`nabu-panic-restart=2`、
全部 `offline-legacy-validate/retire/gop/pm/phase/register/mobile-graphics-load`；
无 `nabu-state-ring`、无 `nabu-splash`、无 `nabu-autolog`。隐藏键所在 kernel
文件**一个字节未改**、随文件继承。

**六文件 sha256 前 8**（完整值见 `l714-sha8.txt`）：

| 文件 | l713 sha8 | **l714 sha8** | 说明 |
|---|---|---|---|
| kernel.macho | `d9b5a174` | **`d9b5a174`** | 逐字节相同，含隐藏键补丁 + `sheng-mig` |
| kernel.macho.adt | `78a6f7e9` | `78a6f7e9` | 逐字节相同 |
| gpu-addon.bin | `950a3968` | `950a3968` | 逐字节相同 |
| owner-trust.bundle | `a4a6bae6` | `a4a6bae6` | 逐字节相同 |
| boot-merge-r3.img | `ead4b531` | `ead4b531` | 逐字节相同 |
| payload.img | `3821aa4c` | **`da7df504`** | 只 `unixnu.toml` 变 |

payload 手术见 `build_l714.py`：输入 `hold/l713/payload.img` 只读并以 `mmap
ACCESS_READ` 打开（**绝不 mmap 输入 payload 可写**），192 MiB `copyfileobj`
流式复制到新输出；`cluster 12688` **整簇替换**（slack 全零）、目录项 size
`1114 → 1111`；`assemble_l714.py` 把六文件放进**新建**
`E:\nabu\hold\l714\`，中间产物在**新建** `E:\nabu\tmp\l714-build-20261111\`。
抽回 7 个 inner file：仅 `UNIXNU~1TOM` CHANGED，`KERNEL~1MAC` == `d9b5a174`
（含 hide 键、`sheng-mig` 官方格式串），其余全 SAME（`selfcheck-final.txt`）。
**`hold/l713/` 六文件 sha 前后未变（payload `3821aa4c`），未污染。**

- **回滚基线**：`E:\nabu\hold\l700\`（kernel `efcb4c34`，已验证 4 分钟窗口）；
  **上一包 / 只读模板** `E:\nabu\hold\l713\`（kernel `d9b5a174`、
  payload `3821aa4c`，本会话六文件 sha 前后未变）。
- **一次性、用完回滚**：`sheng-mig=1` + 隐藏键 + 无 banner 时钟组合只属于 l714，
  **不进默认 args**；判读完成后刷回 `hold/l700/payload.img` 或
  `hold/l713/payload.img`。真正还原只由用户当场点名文件。

## 6. 恢复预案（六问之六，AGENTS.md 0.10 / 0.7 / 0.9 / 附注）

**`AGENTS.md` 0.10 绝对红线：不碰 `boot_a`**（绝不 `fastboot flash boot` /
`flash boot_a`；这块板没有独立 recovery 分区，boot_a 里是 TWRP）。
流程（**仅文字，本会话不执行**；顺序不可换）：

```
recovery arm misc                                             # 不碰 boot_a
fastboot flash cust hold/l714/payload.img
fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin   # 最后才重 arm misc
fastboot boot hold/l714/boot-merge-r3.img                     # 临时加载，不写任何分区
```

`E:\nabu\dwc3\build\misc-bootrecovery.bin`（4096 B）；设备 `TWRP recovery f91fa664`。
**`arm-delay=0`**（`sheng-smp=1` 硬性配对，`AGENTS.md` 0.7），咬点 **380-410 s**，
观察完整 **4 分钟窗口**（`AGENTS.md:57-64`），早 panic 也有重置路径；历史
`sheng-mig=1` 的 datamigrator t=123.6 s / PreBoard t=225 s 落在窗口内。
若 fastboot 全失败：**人工长按电源 10 秒**。回滚见 §5。

**门禁（本会话实测，全部 PASS）**：

| 项 | 结果 | 证据（绝对路径） |
|---|---|---|
| ① 构建 + 模板只读 | 新 tmp 目录 / 新 `hold/l714/`；五文件逐字节同 l713；payload 只 toml 变；整簇替换 + 目录项 size；`ACCESS_READ` / 192 MiB 流式；l713 六文件 sha 前后未变（payload `3821aa4c`） | `E:\nabu\tmp\l714-build-20261111\build_l714.py`、`assemble_l714.py`、`build.log`、`assemble.log`、`selfcheck-final.txt`、`l713-before-sha256.txt`、`l713-after-sha256.txt` |
| ② `fsck.fat -n` + `audit_bootability.py hold/l714 hold/l713 --expected=d9b5a174…` | l713/l714 均 `10 files, 67550/97913 clusters` rc=0；→ **`OK (0 fail, 0 warn)`** RC=0（PRELINK 47/47、ncmds、payload 体积不变、kernel sha 匹配） | `gate1-fsck.txt`、`gate1-bootability.txt`、`gate1-run-final.txt` |
| ③ `audit_dual.py --tag l714` | 逗号分隔真实 sha `kernel=d9b5a174,payload=da7df504,adt=78a6f7e9,addon=950a3968,trust=a4a6bae6,boot=ead4b531,args=685/38` → **`7 items compared, 7 match, 0 differ`** | `gate2-audit_dual.txt`、`measured-values.txt`、`E:\nabu\state\audit-l714.json` |
| ④ `tmp/audit_args.py l713 l714` | `688/38 → 685/38`（≤700）；**`added=['sheng-mig=1']`、`dropped=['nabu-autolog=1']`** → **`RESULT: OK`** rc=0；其余 37 token 同序同字节、关键开关全在 | `gate3-audit_args.txt`、`selfcheck-final.txt` |
| ⑤ Windows Python 3.12.10 `precheck.py` | `run_precheck_l714.py` 驱动 `--pkg E:/nabu/hold/l714 --baseline E:/nabu/hold/l713`，声明 added/dropped → `stamp written: state/precheck.json` rc=0；八字段齐、`token_diff=ok`；`precheck_check.py` → **`[gate] OK`** rc=0，`dual audit OK: audit-l714.json (7 items)` | `gate4-precheck.txt`、`gate4-fields.txt`、`gate4-precheck_check-final.txt`、`E:\nabu\state\precheck.json` |
| ⑥ 六问板报 | 本文件；`board_report_check.py l714` → `[report] OK` 6/6 节 | `E:\nabu\state\board-report-l714.md` |
| 自检 | 抽回 toml 逐 token 比（37/37 同序同字节、≤700）；抽回 kernel `d9b5a174` 且含隐藏键 + `sheng-mig @0x111b4f` + 官方 sentinel/readback 格式串；toml 外其余 payload 内文件与 l713 全 SAME；六文件 sha8 见 §5；`hold/l713/` 未污染 | `selfcheck.py`、`selfcheck-final.txt`、`l714-sha8.txt`、`extract-l714\` |

**零新探针 / 零重编 / 零安全放宽 / 零源码改动 / 零 boot_a 触碰**；
kernel 仍是 l710 同长数据补丁（323 B → 323 B、`cmp -l` 仅 123 字节落在
`0x226d3..0x22753`、`ncmds=25`、文件长 125628416 不变），
**APFS nop `1f2003d5` 未动**；本轮**未运行 adb/fastboot、未上板、
未写 `E:\nabu\logs\rounds\l714.json`**。

**fc76 未消费与双审计偏离**：编译侧 fc76 自 **11-06 14:57** 未消费；
`audit_dual.py --tag l714` 的 `--peer` / `--mine` **两侧均为板侧实测**
（7/7 equal，真实 sha 见 §5），照 `E:\nabu\state\board-report-l713.md` 注明：
偏离 `AGENTS.md:36-47` 0.6 的「编译侧 vs 板侧」原意；本轮零重编、无编译侧
成果可比对。

## 7. 证据路径（全部绝对路径）

- 构建与自检：`E:\nabu\tmp\l714-build-20261111\`（`build_l714.py`、
  `assemble_l714.py`、`selfcheck.py`、`run_precheck_l714.py`、
  `payload.img`、`build.log`、`assemble.log`、`selfcheck-final.txt`、
  `l713-before-sha256.txt`、`l713-after-sha256.txt`、`gate1-fsck.txt`、
  `gate1-bootability.txt`、`gate2-audit_dual.txt`、`gate3-audit_args.txt`、
  `gate4-precheck.txt`、`gate4-precheck_check-final.txt`、`l714-sha8.txt`、
  `measured-values.txt`、`extract-l714\`）
- 戳与板报：`E:\nabu\state\audit-l714.json`、`E:\nabu\state\precheck.json`、
  `E:\nabu\state\board-report-l714.md`
- 包：`E:\nabu\hold\l714\`（六文件）；只读模板 `E:\nabu\hold\l713\`；
  回滚 `E:\nabu\hold\l700\`；隐藏键来源 `E:\nabu\hold\l710\`
- 链：`E:\nabu\state\HANDOVER-20261110-l696.md` §7aa/§7ac/§7ad、
  `E:\nabu\logs\rounds\l713.json`、`E:\nabu\logs\rounds\l616.json`、
  `E:\nabu\logs\l712-v1.jsonl`、`E:\nabu\state\board-report-l710.md` /
  `-l711.md` / `-l712.md` / `-l713.md`、`E:\nabu\AGENTS.md`、
  `kern_exec.c:8520-8572` / `:8602-8611` / `:8624-8629`

**结论：全部门禁 PASS ⇒ 可否上板：可**（上板由用户执行并判读；本会话未执行）。
