# 上板报告 l709（一次性临时诊断轮：只加 `nabu-state-ring=10`，恢复 `NABUSTATE1` banner 时钟）

> 本报告是**上板前**门禁文件；本会话未运行 adb/fastboot、未上板、未写 `E:\nabu\logs\rounds\l709.json`、
> 未改 `E:\nabu\hold\l708\`（只读模板，payload `fe2368bb`）、未污染 `E:\nabu\hold\l700\`。
> 按 `E:\nabu\AGENTS.md` 0.5 / 0.6 / 0.7 / 0.9 / 0.10 编写；格式照
> `E:\nabu\state\board-report-l708.md`。

## 0. 用户授权与 args 预算（≤700 硬上限，逐项实测不猜）

**用户自动授权（2026-11-11）**：「哪怕是插帧，你可以使用临时插帧，进行必要地区的插帧，义无反顾地向前推进」，
并明确「临时插帧/插针、义无反顾向前、自动推进、不要等指示」。⇒ 本轮是
**用户自动授权 + 一次性临时轮 + 用完回滚**：
**只用板上内核已有机制**（`nabu-state-ring=10` 这个已有的 `PE_parse_boot_argn` 门驱动
`nabuLogThread` 的 `NABUSTATE1` banner，banner 内已有 `native_splash_update()` 调用与
`native_splash_state[0..3]` 读数）；**用完回滚、不进默认 args**；
仍然**零新增 printf / IOLog / 计数器 / 采样 / once-only 探针**，**不改任何源码、不重编内核、
不放宽安全策略、不动 `boot_a`**。

预算（从 payload 抽回 `UNIXNU~1TOM` 实测，非推算）：

| 步骤 | 操作 | 字符 | token | ≤700 |
|---|---|---|---|---|
| 0 | l708 实测（`tmp/audit_args.py`） | **661** | **37** | PASS |
| A | **只加** ` nabu-state-ring=10`（19 B = 空格 + 18 字符） | **680** | **38** | **PASS** |
| B | l709 payload 抽回 `UNIXNU~1TOM` 复测 | **680** | **38** | **PASS** |

`added=['nabu-state-ring=10']`、`dropped=[]`；其余 **37 token 同序同字节**、零 mismatch、
toml 头尾除 args 外逐字节同（`gate3-token-by-token.txt`）。toml 1087 → 1106 B。
门禁脚本显式 `--expect-added nabu-state-ring=10`（`audit_args.py` 与 `precheck.py` 驱动都带），
**不靠裸调用默认空集合**，避免误判 FAIL。关键开关逐项实测全在：`nabu-splash=1`、`nabu-log=1`、
`sheng-fb-gpu=1`、`t733-managed-rt-normal=1`、`sheng-gpu-addon=1`、
`sheng-smp=1` + `nabu-wdt-arm-delay=0`、`-apfs_shared_datavolume`、
其余 `offline-legacy-*`（`validate/retire/gop/pm/phase/register`）全在、
`nabu-fbcon=0`、`native-ufs-{root,diagnostics}`、`-v serial=3 debug=0x14e`；
**`offline-legacy-transfer=1` 保持删除状态**（l708 的唯一变量不回退），
`sheng-fb-fill`（l706 一次性项）已消失，`nabu-gpu-observe=1`（l699 起的换出项）未动。

## 1. 唯一问题（一句话）

**恢复 `NABUSTATE1` banner 时钟后，`nabu-splash=1` 能否真正画出背景 + 240x90 中心标记 +
8 格进度条，且 `native_splash_state[0..3]` 在 banner 里变成非 0？**
若画出 ⇒ 证明 **splash 通道完整，只是缺定时调用**；之后可回到 `offline-legacy-transfer=1`
+ 直接催 SpringBoard `finishedStartup` / dismiss BootUI overlay。若仍纯黑 ⇒
`native_gop_virtual` / `native_legacy_gop[3..5]` 或 splash 写入被覆盖，下一刀查这些全局值。
`NABUSTATE1` 可能只写 RAM ring 不进 CDC 尾窗（历史 l520 丢整行、§7z.1），
但 **`screen.bgrx` 像素结果是硬判据**。运行期像素结果静态不能判 ⇒ 满足 `AGENTS.md` 0.5。

## 2. 已闭合到哪（日志优先 + 静态链，`chain` 已写进 `state/precheck.json`）

1. **l706 写路径已证**（`E:\nabu\state\HANDOVER-20261110-l696.md` §7aa.1、
   `E:\nabu\logs\rounds\l706.json`）：`sheng-fb-fill=4` 把 `screen.bgrx` 从 iBoot Logo
   `ca18b728…` 变成全屏纯白 `e1547326…`（1,024,000 个 `(255,255,255,255)`）；
   `frames xfer=5 nswap=0 nsurf=0 nok=5 ncpy=0 nret=5` ⇒ **GOP `bootMapping` 写像素 /
   扫描基址 / retire 全活**；屏不变不是写路径/映射/APFS/sandbox 问题。
2. **l707 stand-down 链已证**（§7aa.2 / §7aa.3、`E:\nabu\logs\rounds\l707.json`）：
   `nabu-splash=1` + `offline-legacy-transfer=1`，屏仍 Logo，
   `frames xfer=5 nswap=5 nsurf=2 sbytes=16384000 nok=5 ncpy=5 nret=5`。
   `/root/merge/UniXNU-nabu/drivers/GopFramebuffer/NativeLegacyDisplay.cpp:329-334` 的
   `nabu-display: frames … ncpy=%u` 第 6 字段就是 `(unsigned)native_legacy_transfer[5]`；
   `/root/merge/UniXNU-nabu/package/NativeLegacyProbe.cpp:861-893`（板上同构快照
   `E:\nabu\hold\l624-probe-in-touch\sources\NativeLegacyProbe.cpp:876-893`）明写
   `native_splash_update()` 一见 `transfer[5]!=0` 就 `native_splash_state[3]=1` stand down
   （「client has begun presenting: hand the panel over」）⇒ `ncpy=5` 证明已交出 5 帧，
   **splash 没画是按设计让位，不是通道坏**。
3. **l708 已经解除了 stand-down，却仍纯黑**（`E:\nabu\logs\rounds\l708.json`）：
   删 `offline-legacy-transfer=1` 后 CDC frames 始终
   `xfer=0 nswap=0 ncpy=0 nret=0` ⇒ `transfer[5]=0`，stand-down 不再成立；
   `nabu-splash=1` 在但没画，屏纯黑 `112b94b6…`（1,024,000 像素全 `(0,0,0,255)`）。
   历史注释 `NativeLegacyDisplay.cpp:61`「removing offline-legacy-transfer left the screen
   completely black」只是 l510 旧状态（那时无 splash 独立写路径），本轮条件不同。
4. **★ 刚查明的根因（本轮唯一变量的依据）**：
   `E:\nabu\hold\l598-touch-fullcrc\sources\NativeUfs.cpp:3542` 的
   `native_splash_update()` **只在 `native_ufs_state_block()` 的 `NABUSTATE1` banner 路径里**
   被调用；banner 由 `nabu-state-ring=<seconds>` 的 `nabuLogThread` 每 >=10 s 触发
   （同文件 `:4092-4100`：`PE_parse_boot_argn("nabu-state-ring", &cadence, …)`、
   `cadence<10` 抬到 10、`IOSleep(2000)` 累加 `elapsed>=cadence` 才出 banner）；
   `:4085-4086` 末尾还会打印 `native_splash_state[0..3]`，所以恢复时钟能直接读到
   splash stand-down / paint 状态。
   `HANDOVER-20261110-l696.md:392` 记 **l699 为换 `nabu-gpu-observe=1` 把
   `nabu-state-ring=10` 删了**；`:1535-1539` 记 **l701–l708 五轮 CDC 里
   `NABUSTATE1` / `NABUGPU1` 全 0** ⇒ banner 从未触发，splash 函数根本没被调到。
   `nabu-log=1` 已在 args；板上 `hold/l709/kernel.macho` 已有
   `nabu-state-ring @0x182f8c`、`NABUSTATE1 @0x180fbf`、`NABUGPU1 @0x1807ed/0x180835`、
   `nabu-splash @0x18af2b`、`nabu-log @0xc1289`、`native_splash_state @0x55c3975/0x5874c2b`，
   且 `_native_splash_update @fffffff007a81c6c` / `_native_gop_virtual @fffffff00c0e5788`
   （l707 板报 §2）⇒ **零新探针、零重编**。
5. **splash 画法（`NativeLegacyProbe.cpp:896-950`）**：取 `native_gop_virtual` 与
   `native_legacy_gop[3..5]`（w/h/row），首轮画 `back=0xff0f1216` 近黑蓝全屏、
   `mark=0xff2ad4ff` 居中 **240x90** 标记（`:922-930`）、
   `done=0xffffffff` / `todo=0xff2a2a2a` **8 格进度条**（`:933-946`），
   `:949 dsb sy` + `state[2]++`；`state[1]` 每 banner 一轮 +1 格（`:911-913`）。
   l708 已证 `transfer[5]=0`，所以本轮不再 stand down。
6. **§7ab / §7ac（屏上 Logo 的性质）**：l707 全量 os_log 119,428 行里 backboardd 主动呈现
   `BKDisplayRenderOverlayAppleLogo`、`No explicit screen owner` 共 4 条、
   `Dismissing boot logo` / `screen owner is now pid` / `systemShellDidFinishLaunching` 全 0；
   SpringBoard pid27 前台（`ForegroundRunning, BKSApplicationStateAppIsFrontmost=1`）但
   `finishedStartup=0` ⇒ 屏上 Logo 是 BackBoard boot overlay，不是纯 ABL 残留；
   它是否覆盖 splash 写入是运行期判据之一。
7. **日志与静态共同沉默的量**：恢复 banner 时钟后真机 `native_gop_virtual` 映射属性与
   panel scanout 是否一致、banner 时序、blank swap / BackBoard overlay 是否覆盖、
   `native_splash_state[0..3]` 是否变非 0、`screen.bgrx` 像素直方图 ⇒ 历轮从未同时具备
   「transfer 已删 + splash 在 + state-ring 时钟在」，必须上板。

## 3. 为什么离线不行（`AGENTS.md` 0.5 `--why_board`，已写进 `state/precheck.json`）

静态能证：`NativeUfs.cpp:3542` 是 splash 的唯一调用点、`:4092-4100` 的
`nabu-state-ring` 门与 >=10 s 节拍、`:4085-4086` 会打 `native_splash_state[0..3]`、
`NativeLegacyProbe.cpp:896-950` 的 back/mark/8 格写入 + `dsb sy`、l708 已让
`transfer[5]=0` 解除 stand down、板上六符号与字符串都在、本包只加一个已有 boot-arg、
五文件与 l708 逐字节同。但「恢复 `NABUSTATE1` banner 时钟后 `nabu-splash=1` 能否真正画出
背景+240x90 中心标记+8 格进度条」是**运行期像素结果**：依赖真机 GOP 映射属性与 scanout
基址是否一致、banner 调用时序与 blank swap / BackBoard BootUI overlay 是否把 splash 盖掉、
以及 `NABUSTATE1` 是否只进 RAM ring 而不进 4 KiB CDC 尾窗（历史 l520 教训）；
离线无寄存器 / 时序 / 像素直方图，无法计算。现有 os_log 只证明 overlay 未 dismiss，
不能证明 splash 通道坏。⇒ 属 0.5 允许的「验证静态结论在真机上是否成立的那**一次**判定」
与「日志与静态都无法产生的新状态」。

## 4. 失败也有信息（三种判读）

| 结果 | 判读 |
|---|---|
| **画出背景 + 240x90 标记 + 8 格进度条**（`screen.bgrx` 直方图出现 `0xff0f1216` 大面积 + `0xff2ad4ff` 居中块 + `0xffffffff`/`0xff2a2a2a` 格，sha 离开 `112b94b6…`、`ca18b728…`、`e1547326…`），且 banner 里 `native_splash_state[0..3]` 非 0 | **splash 通道完整，只是缺定时调用**；之后可回到 `offline-legacy-transfer=1` + 直接催 SpringBoard `finishedStartup` / dismiss BootUI overlay，不再追写路径 |
| **仍纯黑**（`112b94b6…` 同类） | **`native_gop_virtual` / `native_legacy_gop[3..5]` 为 0 或 splash 写入被覆盖**：先对照 `NABUSTATE1` 是否出现、`state[0..3]` 是否抬升、`nabu-display: frames`、`nabu: scanout hand-off online`、`nabu-gpu-addon:`，下一刀查这些全局值 |
| **出现 iBoot Logo**（`ca18b728…`）或**全白**（`e1547326…`） | 对照 l706 / l707：Logo 说明 BackBoard overlay 盖住或 `sheng-fb-fill` 残留（后者须对账 payload 抽回的 toml）；全白说明 forcedFill 残留。不得把「没采到」当「没发生」 |

**观察项（全部已有输出，零新增 printf/IOLog/计数器/探针）**：
`screen.bgrx` sha / 直方图（对比 `112b94b6…`、`ca18b728…`、`e1547326…`、l696 纯黑）、
CDC `nabu-state-ring=10` 与 `nabu-splash=1` 回显、`NABUSTATE1` / `NABUGPU1`
（**若进尾窗**；只进 RAM ring 不算失败）、banner 里 `native_splash_state[0..3]`、
`nabu-display: frames xfer nswap nsurf sbytes nok ncpy nret`、
`nabu: scanout hand-off online`、`nabu-gpu-addon:`、panic。一次性、用完回滚。

**fc76 未消费与双审计两侧板侧实测偏离**：编译侧 fc76 自 **11-06 14:57** 未消费；
双审计两侧均为**板侧实测**（`audit_dual.py --tag l709 --peer … --mine …`，7/7 equal，
真实 sha 见 §6），照 `state/board-report-l708.md` / `-l707.md:98-100` 注明，
偏离 `AGENTS.md:36-47` 0.6 的「编译侧 vs 板侧」原意；无编译侧成果可比对，本轮零重编。

## 5. 唯一变量与回滚

**唯一功能变量 = args 末尾追加 ` nabu-state-ring=10`**（含前导空格 19 B）；680/38，
其余 37 token 同序同字节。`added=['nabu-state-ring=10']` / `dropped=[]`。

| 文件 | l708 sha8 | **l709 sha8** | 说明 |
|---|---|---|---|
| kernel.macho | `fb3ce142` | `fb3ce142` | 逐字节相同（未重编） |
| kernel.macho.adt | `78a6f7e9` | `78a6f7e9` | 逐字节相同 |
| gpu-addon.bin | `950a3968` | `950a3968` | 逐字节相同 |
| owner-trust.bundle | `a4a6bae6` | `a4a6bae6` | 逐字节相同 |
| boot-merge-r3.img | `ead4b531` | `ead4b531` | 逐字节相同 |
| payload.img | `fe2368bb` | **`9e8895fb`** | 唯一变化；包内仅 `UNIXNU~1TOM` 变（1087→1106 B） |

手术：`E:\nabu\tmp\l709-build-20261111\build_l709.py` 以**只读 mmap ACCESS_READ** 打开 l708 模板
（**绝不 mmap 输入 payload 可写**，历史坑见 `state/board-report-l695p.md`），192 MiB 用
`shutil.copyfileobj` **流式**复制，2048 B 簇 **12688 整簇替换**（grow +19 B、slack 全零、FAT 链不变）
+ 目录项 size `1087 → 1106`（偏移 804028）。payload 内抽回复核：
`KERNEL~1MAC fb3ce142` / `KERNEL~1ADT 78a6f7e9` / `KERNEL~1GPU 950a3968` /
`KERNEL~1TRU a4a6bae6` / `SCRATC~1IMG 3cd3d0d3` 与 l708 全同，仅 `UNIXNU~1TOM` 变
（`selfcheck.txt` / `payload-inner-compare.txt`）。

- **回滚基线**：`E:\nabu\hold\l700\`（kernel `efcb4c34`，已验证 4 分钟窗口）；
  **上一包 / 只读模板** `E:\nabu\hold\l708\`（kernel `fb3ce142`、payload `fe2368bb`，
  本会话六文件 sha 前后未变）。
- **一次性、用完回滚**：`nabu-state-ring=10` 只属于 l709，**不进默认 args**；
  判读完成后刷回 `hold/l700/payload.img` 或 `hold/l708/payload.img`，由用户在
  `logs/rounds/l709.json` 写明「一次性临时诊断轮、用完回滚」。回滚命令（仅文字）：
  `fastboot flash cust hold/l700/payload.img`。

## 6. 恢复预案 / 门禁 / 附注

**`AGENTS.md` 0.10（绝对红线）：不碰 `boot_a`**（绝不 `fastboot flash boot` / `flash boot_a`；
这块平板没有独立 recovery 分区，recovery 就在 boot 里，boot_a 里是 TWRP）。
流程（**仅文字，本会话不执行**；顺序不可换）：

```
recovery arm misc
fastboot flash cust hold/l709/payload.img
fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin   # flash 后最后重 arm
fastboot boot hold/l709/boot-merge-r3.img                      # 临时加载，不写任何分区
```

`E:\nabu\dwc3\build\misc-bootrecovery.bin`（4096 B，实测存在）；设备 `TWRP recovery f91fa664`。
`arm-delay=0`（`sheng-smp=1` 硬性配对），咬点 **380-410 s**，观察完整 **4 分钟窗口**
（`AGENTS.md:57-64`），早 panic 也有重置路径。若 fastboot 全失败：人工长按电源 10 秒。
回滚见 §5；**真正需要还原时只由用户当场点名文件**，仍不经过自动化脚本。

**门禁（本会话实测，全部 PASS）**：

| 项 | 结果 | 证据 |
|---|---|---|
| ① 整簇替换 + 五文件 | l709 六文件：五文件与 l708 `cmp SAME`；payload 只 `unixnu.toml` 变；`hold/l708/` 六文件 sha 前后未变（payload `fe2368bb`） | `build_l709.py`、`l708-baseline-sha.txt`、`l708-after-sha.txt`、`l709-sha8.txt` |
| ② `fsck.fat -n` 双侧 + `audit_bootability.py hold/l709 hold/l708 --expected=fb3ce142…` | l709/l708 均 `10 files, 67550/97913 clusters` rc=0；→ **`OK (0 fail, 0 warn)`** RC=0 | `gate1-fsck.txt`、`gate1-bootability.txt` |
| ③ `audit_dual.py --tag l709` | 真实 sha `kernel=fb3ce142,payload=9e8895fb,adt=78a6f7e9,addon=950a3968,trust=a4a6bae6,boot=ead4b531,args=680/38` → `7 items compared, 7 match, 0 differ` | `gate2-audit_dual.txt`、`state/audit-l709.json` |
| ④ `audit_args.py hold/l708 hold/l709 --expect-added nabu-state-ring=10` | `661/37 → 680/38`（≤700）；`added=['nabu-state-ring=10']`、`dropped=[]` → **`RESULT: OK`** rc=0；逐 token 37/37 同序同字节、零 mismatch、关键开关全 PRESENT | `gate3-audit_args.txt`、`gate3-token-by-token.txt` |
| ⑤ Windows Python 3.12.10 `precheck.py` | `run_precheck_l709.py` 驱动 `--pkg E:/nabu/hold/l709 --baseline E:/nabu/hold/l708 --expect-added nabu-state-ring=10` → `stamp written: state/precheck.json` rc=0；**八字段全非空**、`token_diff=ok`；`precheck_check.py` → **`[gate] OK`** rc=0，`dual audit OK: audit-l709.json (7 items)` | `gate4-precheck.txt`、`gate4-precheck_check.txt`、`state/precheck.json` |
| ⑥ 六问板报 | 本文件 | `state/board-report-l709.md` |
| 自检 | 抽回 toml 逐 token 只差一个 added；抽回 kernel/addon/trust/adt 与 l708 `cmp SAME`；六文件 sha8；`hold/l708/` 未污染 | `selfcheck.txt`、`payload-inner-compare.txt` |

## 7. 证据路径（全部绝对路径）

- 构建与预算：`E:\nabu\tmp\l709-build-20261111\build_l709.py`、`payload.img`、`build.log`、
  `token_check.py`、`run_precheck_l709.py`
- 门禁输出：同目录 `gate1-fsck.txt`、`gate1-bootability.txt`、`gate2-audit_dual.txt`、
  `gate3-audit_args.txt`、`gate3-token-by-token.txt`、`gate4-precheck.txt`、
  `gate4-precheck_check.txt`、`payload-inner-compare.txt`、`selfcheck.txt`、
  `l708-baseline-sha.txt`、`l708-after-sha.txt`、`l709-sha8.txt`
- 戳与板报：`E:\nabu\state\audit-l709.json`、`E:\nabu\state\precheck.json`、
  `E:\nabu\state\board-report-l709.md`
- 包：`E:\nabu\hold\l709\`（六文件）；只读模板 `E:\nabu\hold\l708\`；回滚 `E:\nabu\hold\l700\`
- 板上证据：`E:\nabu\logs\rounds\l706.json`、`E:\nabu\logs\rounds\l707.json`、
  `E:\nabu\logs\rounds\l708.json`、`E:\nabu\state\HANDOVER-20261110-l696.md`
  §7aa / §7ab / §7ac / :392 / :1535-1539、
  `E:\nabu\logs\l708-diag-tree\screen.bgrx`、`E:\nabu\logs\l706-diag-tree\screen.bgrx`
- 静态链：`/root/merge/UniXNU-nabu/drivers/GopFramebuffer/NativeLegacyDisplay.cpp:61,329-334`、
  `/root/merge/UniXNU-nabu/package/NativeLegacyProbe.cpp:861-893,896-950`、
  `/root/merge/UniXNU-nabu/drivers/NativeUfs/NativeUfs.cpp:3542,4085-4086,4092-4100`；
  板上快照 `E:\nabu\hold\l598-touch-fullcrc\sources\NativeUfs.cpp`、
  `E:\nabu\hold\l624-probe-in-touch\sources\NativeLegacyProbe.cpp`、
  `E:\nabu\hold\l709\kernel.macho` 内字符串偏移与 `llvm-nm` 六符号；`E:\nabu\AGENTS.md`

**注**：本轮**零新探针**（用板上已有 `nabu-state-ring` / banner / splash 通道与已有 boot-arg 门）、
**零重编内核**、**零安全策略放宽**、**零源码改动**、**零 boot_a 触碰**，
**未运行 adb/fastboot、未上板、未写 `E:\nabu\logs\rounds\l709.json`**。
**可否上板：可**（全部门禁 PASS；上板由用户执行并判读）。

## 8. 下一步（仅建议，本会话不执行）

由用户上板后自行写 `logs/rounds/l709.json`（本会话**不写**），按 §4 三判读表读
`screen.bgrx` sha / 直方图、CDC `nabu-state-ring=10` 与 `nabu-splash=1` 回显、
`NABUSTATE1` / `NABUGPU1`（若进尾窗）、banner 里 `native_splash_state[0..3]`、
`nabu-display: frames`、`nabu: scanout hand-off online`、`nabu-gpu-addon:`、panic，
并写明 **「一次性临时诊断轮、用完回滚」**，然后回滚到 `hold/l700` 或 `hold/l708`。
