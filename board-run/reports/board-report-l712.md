# 上板报告 l712（一次性临时功能轮：隐藏 BootUI Apple Logo + banner 时钟 + 恢复系统真实 transfer 帧）

> 本报告是**上板前**门禁文件；本会话未运行 adb/fastboot、未上板、未写
> `E:\nabu\logs\rounds\l712.json`，`E:\nabu\hold\l711\` 只读且六文件 sha 前后未变
> （payload `f3d1672a`）。按 `E:\nabu\AGENTS.md` 0.5 / 0.6 / 0.7 / 0.9 / 0.10 编写；
> 格式照 `E:\nabu\state\board-report-l709.md` / `-l710.md` / `-l711.md`。
> 全部证据目录：`E:\nabu\tmp\l712-build-20261111\`。

## 0. 用户授权与性质

**用户自动授权（2026-11-11）**：「自动推进、临时插帧/插针、义无反顾向前、不要等指示」；
`E:\nabu\state\HANDOVER-20261110-l696.md` §7aa 记授权边界：只用**板上内核已有机制**、
一次性临时轮、**用完回滚、不进默认 args**、零新增 printf/IOLog/计数器/探针、
零重编内核、零安全放宽、不碰 `boot_a`；全套门禁 + 六问板报。

本轮**不改源码、不改内核字节、不放宽任何策略**：kernel 直接继承 l710/l711 的
`d9b5a174`（同长 `nabu_fb` plist 数据补丁），只对 payload 内 `unixnu.toml` 做
args token 原位替换。设备上板由用户执行并判读。

## 1. 唯一问题（六问之一）

**隐藏 BootUI Apple Logo 后，恢复 `offline-legacy-transfer=1` 的系统真实帧在
`screen.bgrx` 里到底是什么？**

本轮**保留隐藏键**（l710 内核 `d9b5a174` 的 `BKHideAppleLogoOnLaunch=true`）、
**保留 `nabu-state-ring=10` banner 时钟**（用于 `NABUSTATE1` / 状态读数），
**把 `offline-legacy-transfer=1` 加回来**让系统自己的 5 次真实 surface 拷贝回来，
并按 args 预算**删掉 `nabu-splash=1`**，使 splash 不再写入、系统帧成为唯一内容源。
这是把「写路径 / overlay / SpringBoard 未完成」三条线合拢的决定性一轮。
`screen.bgrx` 是硬判据；运行期像素静态不能判 ⇒ 满足 `AGENTS.md` 0.5。

## 2. 已闭合到哪（六问之二，日志优先 + 静态链）

1. **GOP 写路径活**：§7aa.1 / `E:\nabu\logs\rounds\l706.json`：`sheng-fb-fill=4`
   ⇒ 屏全白 `e1547326…`（1,024,000 个 `(255,255,255,255)`）。
2. **Logo 是谁画的 / 5 帧真实拷贝**：§7aa.2/.3 / `l707.json`：`nabu-splash=1` +
   `offline-legacy-transfer=1` ⇒ `frames ncpy=5`，`transfer[5]!=0` 让 splash
   stand down；屏仍是 backboardd 的 `BKDisplayRenderOverlayAppleLogo`
   `ca18b728…`。`NativeLegacyProbe.cpp:861-893` 明写 `transfer[5]` 是「real CPU
   copy plus dsb」才加的计数；`NativeLegacyDisplay.cpp:329-334` 定义 frames 字段。
   §7ab 全量 l707 os_log 119,428 行：两次 `Presenting overlay`、
   `Dismissing boot logo` / `finishedStartup` / `Not showing ...` 全 0；§7ac：
   SpringBoard pid27 前台但 `finishedStartup=0`，`No explicit screen owner` 4 条。
3. **时钟是 l709 的决定性变量**：`E:\nabu\logs\rounds\l708.json` 删
   `offline-legacy-transfer=1` ⇒ frames 全 0、屏纯黑 `112b94b6…`；
   `l709.json` 加 `nabu-state-ring=10` 恢复 `NABUSTATE1` banner ⇒
   `native_splash_update()`（`NativeUfs.cpp:3542`，banner 由 `nabu-state-ring`
   的 `nabuLogThread` 每 >=10 s 触发，`:4092-4101`）真正被调用，屏
   sha `00e62fe873668aba`，`nonblack=1024000`：`(22,18,15,255)` 背景
   1,006,600、`(255,212,42,255)` 中心金标 5,400、`(255,255,255,255)` 进度格
   12,000；CDC 806,664 B；frames 全 0；无 panic。
4. **隐藏键已被板侧实证**：`E:\nabu\state\board-report-l710.md` §2 记静态链
   （`kern_exec.c:8520-8572` 的 323 B `nabu_fb[]` plist、`:8537-8544` 六个
   `fb_prefs[]` 路径、`:8539-8543` 四个 backboardd 域 `keepIfPresent=0`
   强制覆盖、`:8565` 每轮写；板上字面量在 `hold/l707/kernel.macho`
   `@0x226ac..0x227ee`；`/root/dsc/dyld_shared_cache_arm64.05` 的
   `com.apple.backboardd @0x1c25f9a`、`BKHideAppleLogoOnLaunch @0x1c263ec`、
   `hideAppleLogoOnLaunch @0x1c263d6`；BackBoard uuidtext 官方 hide 格式串）。
   l710 os_log `E:\nabu\logs\l710-v1.jsonl` 实测
   `addOverlay(1-init 2): Not showing the boot UI overlay because we were told to hide it`
   与 `addUnderlay: Not showing the boot UI underlay ...`（共 4 条 hide 串）；
   `Presenting overlay: <BKDisplayRenderOverlayAppleLogo>` **0 条**；
   `E:\nabu\logs\l710-diag-tree\screen.bgrx` sha `112b94b6b55a348f`
   （1,024,000 像素全 `(0,0,0,255)`）、`l710-cdc.bin` frames 全 0
   ⇒ 键生效，但缺 banner 时钟 / transfer 内容顶上，屏纯黑。
5. **干净基线成立**：`E:\nabu\logs\l711-diag-tree\screen.bgrx` 与 `l709` 的
   `screen.bgrx` **逐字节相同** `00e62fe873668aba72d893c4fcd5e154f916854d8c11722ff122c9aeefecb00f`
   （l711 = l710 kernel + 把 `offline-legacy-transfer=1` 换成 `nabu-state-ring=10`，
   payload `f3d1672a`，680/38）。隐藏键与 splash 时钟互不干扰。
6. **两轮从未合并**：现有日志只有「有 Logo + 有 transfer + 无时钟」(l707)、
   「有 Logo + 有时钟 + 无 transfer」(l709/l711)、「隐藏 Logo + 无时钟 +
   无 transfer 帧」(l710) 三个状态；「隐藏键 + banner 时钟 + 恢复 transfer +
   删 splash」是日志与静态都无法产生的新状态。

## 3. 为什么离线不行（六问之三，`AGENTS.md` 0.5）

静态能证：l706 的 GOP 写路径、l707 的 `transfer[5]` stand-down 与 `ncpy=5`
真实拷贝、l709 的 `NABUSTATE1` banner → `native_splash_update()` →
`00e62fe8…` 传导链、l710 的 plist 强制覆盖写路径 / 键与域绑定 / 官方
`Not showing the boot UI overlay` 格式串与板侧 os_log 实证、l711 干净基线；
l712 五文件逐字节同 l711、抽回 kernel == `d9b5a174` 且含
`<key>BKHideAppleLogoOnLaunch</key><true/>`、抽回 toml 逐 token 比
（37/37 同序同字节、`692/38 ≤700`）、toml 外其余 payload 内文件与 l711 全 SAME、
关键开关逐项全在。

但「隐藏 BootUI overlay 后，恢复 `offline-legacy-transfer=1` 的 5 次真实
surface CPU 拷贝在无 splash 写入、有 banner 时钟的条件下，最终把 GOP 写成
什么像素」是运行期事实：拷贝源 surface 的内容（BootUI overlay 残留 / 空 /
SpringBoard 桌面层）、hide 键对 backboardd overlay 初始化与 framebuffer
所有权 / cfprefsd 注册时机的影响、transfer 帧与 splash/blank swap 的覆盖竞争、
SpringBoard `finishedStartup` 是否推进，都只有真机的 `screen.bgrx` 像素与
os_log / CDC 时序能给出。合并态属「日志与静态都无法产生的新状态」，
是 0.5 允许的「验证静态结论在真机上是否成立的那一次判定」。

## 4. 失败也有信息（六问之四，三判读）

| 结果 | 判读 |
|---|---|
| 出现**非 Logo、非纯黑、非 splash** 多色内容 | **BootUI 是遮挡，系统帧终于露出，接近真实 UI**；写路径 + overlay + 内容源三条线合拢 |
| **纯黑 `112b94b6…` / Logo `ca18b728…`** | **系统 5 帧本身是空/Logo**，墙在 SpringBoard `finishedStartup` / 内容源；下一刀核 `native_splash_state[0..3]`、surface 源地址/stride、SpringBoard 初始化 |
| 仍是 `00e62fe8…` splash 多色屏 | **splash 残留或替换未生效**（对照 args 回显：`nabu-splash=1` 应为 0、`offline-legacy-transfer=1` 应为 1）；回到 `l711` 板报对账 |

**os_log 判据**：`Not showing the boot UI overlay` 应仍在；
`Presenting overlay: <BKDisplayRenderOverlayAppleLogo>` 应为 **0**；
`addUnderlay: Not showing ...` / `No explicit screen owner` 一并核。

**观察项（全部板上已有输出，零新增 printf/IOLog/计数器/探针）**：
`screen.bgrx` sha / 直方图（对比 `00e62fe8…`、`112b94b6…`、`ca18b728…`、
`e1547326…`）；os_log `Not showing the boot UI overlay` /
`Not showing the boot UI underlay` / `Presenting overlay: <BKDisplayRenderOverlayAppleLogo>` /
`No explicit screen owner` / `Dismissing boot logo`；CDC
`nabu-display: frames xfer nswap nsurf sbytes nok ncpy nret`（**预期 `ncpy=5`**）、
`nabu: scanout hand-off online`、`nabu-gpu-addon:`、boot-args 回显、
panic / unencrypted panic / `buf_bread_err16`。

## 5. 唯一变量与回滚（六问之五）

**唯一变量 = payload 内 `UNIXNU~1TOM`（`unixnu.toml`）的 args token 原位替换**：
`nabu-splash=1` → `offline-legacy-transfer=1`。
即 **l711 的隐藏键 kernel + banner 时钟 + 恢复真实 transfer 帧、且删掉 splash 写入**；
与 l711 的差别只在这一个 token。

**args 预算（精确实测，非猜测；用户预估 693，实测 692）**：l711 `680/38`；
直接追加 `" offline-legacy-transfer=1"` = **26 B** 会到 `706/39`（超 700），
故必须合法替换。把 `nabu-splash=1`（**13 B**）原位替换成
`offline-legacy-transfer=1`（**25 B**），净 **+12 B** ⇒
**`692/38 ≤700`**，token 数不变 38（用户预估 `693/38` 与实测差 1 字符，
以 `audit_args.py` / `build_l712.py` 实测为准：13→25 是 +12 而非 +13）。
`added=['offline-legacy-transfer=1']`、`dropped=['nabu-splash=1']`，
其余 **37 token 同序同字节**；toml `1106 → 1118 B`、sha8 `ed7ee193 → 0dab34fe`。
模板里没有 `sheng-vsync` / `sheng-build-tag` 可缩，故无需动其它 token。

**逐项核关键开关全在**：`nabu-state-ring=10`、`t733-managed-rt-normal=1`、
`sheng-fb-gpu=1`、`sheng-gpu-addon=1`、`sheng-smp=1` + `nabu-wdt-arm-delay=0`、
`-apfs_shared_datavolume`、`nabu-fbcon=0`、`nabu-log=1`、
`offline-legacy-validate/retire/gop/pm/phase/register`、
`offline-legacy-transfer=1`；`nabu-splash=1` 已删。隐藏键所在 kernel 文件
**一个字节未改**、随文件继承。

**六文件 sha256 前 8**（完整值见 `l712-sha8.txt`）：

| 文件 | l711 sha8 | **l712 sha8** | 说明 |
|---|---|---|---|
| kernel.macho | `d9b5a174` | **`d9b5a174`** | 逐字节相同，含隐藏键补丁 |
| kernel.macho.adt | `78a6f7e9` | `78a6f7e9` | 逐字节相同 |
| gpu-addon.bin | `950a3968` | `950a3968` | 逐字节相同 |
| owner-trust.bundle | `a4a6bae6` | `a4a6bae6` | 逐字节相同 |
| boot-merge-r3.img | `ead4b531` | `ead4b531` | 逐字节相同 |
| payload.img | `f3d1672a` | **`38a58279`** | 只 `unixnu.toml` 变 |

payload 手术见 `build_l712.py`：输入 `hold/l711/payload.img` 只读并以
`mmap ACCESS_READ` 打开（**绝不 mmap 输入 payload 可写**，l695p 教训），
192 MiB `copyfileobj` 流式复制到新输出；`cluster 12688` **整簇替换**
（slack 全零）、目录项 size `1106 → 1118`；`assemble_l712.py` 把六文件
放进**新建** `E:\nabu\hold\l712\`，中间产物在**新建**
`E:\nabu\tmp\l712-build-20261111\`。抽回 7 个 inner file：仅
`UNIXNU~1TOM` CHANGED，`KERNEL~1MAC` == `d9b5a174`（含 hide 键），
其余全 SAME（`selfcheck.txt`）。**`hold/l711/` 六文件 sha 前后未变
（payload `f3d1672a`），未污染**。

- **回滚基线**：`E:\nabu\hold\l700\`（kernel `efcb4c34`，已验证 4 分钟窗口）；
  **上一包 / 只读模板** `E:\nabu\hold\l711\`（kernel `d9b5a174`、
  payload `f3d1672a`，本会话六文件 sha 前后未变，见 `selfcheck.txt` 模板完整性段）。
- **一次性、用完回滚**：`offline-legacy-transfer=1` + 隐藏键 + banner 时钟
  + 无 splash 组合只属于 l712，**不进默认 args**；判读完成后刷回
  `hold/l700/payload.img` 或 `hold/l711/payload.img`。真正还原只由用户当场点名文件。

## 6. 恢复预案 / 门禁 / 附注（六问之六）

**`AGENTS.md` 0.10 绝对红线：不碰 `boot_a`**（绝不 `fastboot flash boot` /
`flash boot_a`；这块板没有独立 recovery 分区，boot_a 里是 TWRP）。
流程（**仅文字，本会话不执行**；顺序不可换）：

```
recovery arm misc                                             # 不碰 boot_a
fastboot flash cust hold/l712/payload.img
fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin   # 最后才重 arm misc
fastboot boot hold/l712/boot-merge-r3.img                     # 临时加载，不写任何分区
```

`E:\nabu\dwc3\build\misc-bootrecovery.bin`（4096 B）；设备 `TWRP recovery f91fa664`。
`arm-delay=0`（`sheng-smp=1` 硬性配对，`AGENTS.md` 0.7），咬点 **380-410 s**，
观察完整 **4 分钟窗口**（`AGENTS.md:57-64`），早 panic 也有重置路径。
若 fastboot 全失败：人工长按电源 10 秒。回滚见 §5。

**门禁（本会话实测，全部 PASS）**：

| 项 | 结果 | 证据（绝对路径） |
|---|---|---|
| ① 构建 + 模板只读 | 新 tmp 目录 / 新 `hold/l712/`；五文件逐字节同 l711；payload 只 toml 变；整簇替换 + 目录项 size；`ACCESS_READ` / 192 MiB 流式；l711 六文件 sha 前后未变（payload `f3d1672a`） | `E:\nabu\tmp\l712-build-20261111\build_l712.py`、`assemble_l712.py`、`build.log`、`assemble.log`、`selfcheck.txt` |
| ② `fsck.fat -n` + `audit_bootability.py hold/l712 hold/l711 --expected=d9b5a174…` | l712/l711 均 `10 files, 67550/97913 clusters` rc=0；→ **`OK (0 fail, 0 warn)`** RC=0（PRELINK 47/47、ncmds、payload 体积不变、kernel sha 匹配） | `gate1-fsck.txt`、`gate1-bootability.txt` |
| ③ `audit_dual.py --tag l712` | 逗号分隔真实 sha `kernel=d9b5a174,payload=38a58279,adt=78a6f7e9,addon=950a3968,trust=a4a6bae6,boot=ead4b531,args=692/38` → **`7 items compared, 7 match, 0 differ`** | `gate2-audit_dual.txt`、`E:\nabu\state\audit-l712.json` |
| ④ `tmp/audit_args.py l711 l712` | `680/38 → 692/38`（≤700）；**`added=['offline-legacy-transfer=1']`、`dropped=['nabu-splash=1']`** → **`RESULT: OK`** rc=0；其余 37 token 同序同字节、关键开关全在 | `gate3-audit_args.txt`、`selfcheck.txt` |
| ⑤ Windows Python 3.12.10 `precheck.py` | `run_precheck_l712.py` 驱动 `--pkg E:/nabu/hold/l712 --baseline E:/nabu/hold/l711`，声明 added/dropped → `stamp written: state/precheck.json` rc=0；八字段齐、`token_diff=ok`；`precheck_check.py` → **`[gate] OK`** rc=0，`dual audit OK: audit-l712.json (7 items)` | `gate4-precheck.txt`、`gate4-precheck_check.txt`、`E:\nabu\state\precheck.json` |
| ⑥ 六问板报 | 本文件 | `E:\nabu\state\board-report-l712.md` |
| 自检 | 抽回 toml 逐 token 比（37/37 同序同字节、≤700）；抽回 kernel `d9b5a174` 且含隐藏键；toml 外其余 payload 内文件与 l711 全一致；六文件 sha8 见 §5；`hold/l711/` 未污染 | `selfcheck.py`、`selfcheck.txt`、`l712-sha8.txt`、`extract-l712\` |

**零新探针 / 零重编 / 零安全放宽 / 零源码改动 / 零 boot_a 触碰**；
kernel 仍是 l710 同长数据补丁（323 B → 323 B、`cmp -l` 仅 123 字节落在
`0x226d3..0x22753`、`ncmds=25`、文件长 125628416 不变），
**APFS nop `1f2003d5` 未动**；本轮**未运行 adb/fastboot、未上板、
未写 `E:\nabu\logs\rounds\l712.json`**。

**AGENTS.md 0.10**：`recovery arm misc` → `fastboot flash cust hold/l712/payload.img` →
**最后** `fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin` →
`fastboot boot hold/l712/boot-merge-r3.img`；`arm-delay=0`、咬点 **380-410 s**、
完整 **4 分钟窗口**，不碰 `boot_a`（详见 §6）。

**fc76 未消费与双审计两侧板侧实测偏离**：编译侧 fc76 自 **11-06 14:57** 未消费；
`audit_dual.py --tag l712` 的 `--peer` / `--mine` **两侧均为板侧实测**
（7/7 equal，真实 sha 见 §5），照 `E:\nabu\state\board-report-l711.md` 注明：
偏离 `AGENTS.md:36-47` 0.6 的「编译侧 vs 板侧」原意；本轮零重编、无编译侧
成果可比对。

## 7. 证据路径（全部绝对路径）

- 构建与自检：`E:\nabu\tmp\l712-build-20261111\`（`build_l712.py`、
  `assemble_l712.py`、`selfcheck.py`、`run_precheck_l712.py`、
  `payload.img`、`build.log`、`assemble.log`、`selfcheck.txt`、
  `gate1-fsck.txt`、`gate1-bootability.txt`、`gate2-audit_dual.txt`、
  `gate3-audit_args.txt`、`gate4-precheck.txt`、`gate4-precheck_check.txt`、
  `l712-sha8.txt`、`extract-l712\`）
- 戳与板报：`E:\nabu\state\audit-l712.json`、`E:\nabu\state\precheck.json`、
  `E:\nabu\state\board-report-l712.md`
- 包：`E:\nabu\hold\l712\`（六文件）；只读模板 `E:\nabu\hold\l711\`；
  回滚 `E:\nabu\hold\l700\`；隐藏键来源 `E:\nabu\hold\l710\`
- 板上证据：`E:\nabu\logs\rounds\l706.json` / `l707.json` / `l708.json` /
  `l709.json`、`E:\nabu\logs\l710-v1.jsonl`、`E:\nabu\logs\l710-cdc.bin`、
  `E:\nabu\logs\l710-diag-tree\screen.bgrx`、
  `E:\nabu\logs\l711-diag-tree\screen.bgrx`、`E:\nabu\logs\l711-cdc.bin`、
  `E:\nabu\state\HANDOVER-20261110-l696.md` §7aa / §7ab / §7ac、
  `E:\nabu\state\board-report-l709.md` / `-l710.md` / `-l711.md`
- 静态链：`/root/merge/out-v6/nabu/ipados-17.7.11-21H461/nabu/tree/.cache/xnu-work/bsd/kern/kern_exec.c:8520-8572`、
  `/root/merge/UniXNU-nabu/drivers/NativeUfs/NativeUfs.cpp:3542` / `:4092-4101`、
  `NativeLegacyProbe.cpp:861-893`、`NativeLegacyDisplay.cpp:329-334` / `:73-94`、
  `/root/dsc/dyld_shared_cache_arm64.05`、`E:\nabu\AGENTS.md`

**结论：全部门禁 PASS ⇒ 可否上板：可**（上板由用户执行并判读；本会话未执行）。
