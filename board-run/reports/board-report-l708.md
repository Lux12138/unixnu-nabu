# 上板报告 l708（一次性临时诊断轮：删 `offline-legacy-transfer=1`，保留 `nabu-splash=1`）

> 本报告是**上板前**门禁文件；本会话未运行 adb/fastboot、未上板、未写 `E:\nabu\logs\rounds\l708.json`、
> 未改 `E:\nabu\hold\l707\`（只读模板）、未污染 `E:\nabu\hold\l700\`。按 `E:\nabu\AGENTS.md`
> 0.5 / 0.6 / 0.7 / 0.9 / 0.10 编写；格式照 `E:\nabu\state\board-report-l707.md`。

## 0. 用户授权与 args 预算（≤700 硬上限，逐项实测不猜）

**用户自动授权（2026-11-11）**：「哪怕是插帧，你可以使用临时插帧，进行必要地区的插帧，义无反顾地向前推进」，
并明确「临时插帧/插针、义无反顾向前、自动推进、不要等指示」。⇒ 本轮是
**用户自动授权 + 一次性临时轮 + 用完回滚**：
**只用板上内核已有机制**（`nabu-splash=1` 的 `native_splash_update()` 独立写路径 +
`offline-legacy-transfer` 这个已有的 `PE_parse_boot_argn` 门）；**用完回滚、不进默认 args**；
仍然**零新增 printf / IOLog / 计数器 / 采样 / once-only 探针**，**不改任何源码、不重编内核、
不放宽安全策略、不动 `boot_a`**。

预算（从 payload 抽回 `UNIXNU~1TOM` 实测，非推算）：

| 步骤 | 操作 | 字符 | token | ≤700 |
|---|---|---|---|---|
| 0 | l707 实测（`tmp/audit_args.py`） | **687** | **38** | PASS |
| A | **只删** `offline-legacy-transfer=1` + 相邻空格（26 B） | **661** | **37** | **PASS** |
| B | l708 payload 抽回 `UNIXNU~1TOM` 复测 | **661** | **37** | **PASS** |

`added=[]`、`dropped=['offline-legacy-transfer=1']`；其余 **37 token 同序同字节**、零 mismatch、
toml 头尾除 args 外逐字节同（`gate3-token-by-token.txt`）。toml 1113 → 1087 B。
门禁脚本显式 `--expect-dropped offline-legacy-transfer=1`（`audit_args.py` 与 `precheck.py` 驱动都带），
**不靠裸调用默认空集合**，避免误判 FAIL。关键开关逐项实测全在：`nabu-splash=1`（回显必须在）、
`sheng-fb-gpu=1`、`t733-managed-rt-normal=1`、`sheng-gpu-addon=1`、
`sheng-smp=1` + `nabu-wdt-arm-delay=0`、`-apfs_shared_datavolume`、
`offline-legacy-validate=1`、`offline-legacy-retire=1`、`offline-legacy-gop=1`、
`offline-legacy-pm=1`、`offline-legacy-phase=4`、`offline-legacy-register=1`、`nabu-fbcon=0`
（Qoder 不可删名单，关系 screen.bgrx 像素读数）、`native-ufs-{root,diagnostics}`、
`-v serial=3 debug=0x14e`；`sheng-fb-fill` 已消失（l706 一次性项）。

## 1. 唯一问题（一句话）

**解除 `native_legacy_transfer[5]` 的 stand-down 后，`nabu-splash=1` 能否在 `screen.bgrx`
画出非 Logo、非纯白/纯黑的「必要地区」背景 + 8 格进度条？**
若画出 ⇒ 证明 **splash 通道本身能写、之前只是按设计让位**，且后续可沿
「让 BackBoard overlay dismiss / SpringBoard finishedStartup」推进；若屏黑 / 仍 Logo ⇒
说明 `native_gop_virtual` / banner 时序或 overlay 覆盖另有断点。
`frames` 预计 `xfer` 不再涨到 5（copy 路径未触发）、`ncpy` 应停 0；`nabu-splash=1` 回显必须在。
运行期像素结果静态不能判 ⇒ 满足 `AGENTS.md` 0.5。

## 2. 已闭合到哪（日志优先 + 静态链，`chain` 已写进 `state/precheck.json`）

1. **l706 写路径已证**（`E:\nabu\state\HANDOVER-20261110-l696.md` §7aa.1、
   `E:\nabu\logs\rounds\l706.json`）：`sheng-fb-fill=4` 把 `screen.bgrx` 从 iBoot Logo
   `ca18b728…` 变成全屏纯白 `e1547326…`（1,024,000 个 `(255,255,255,255)`、bbox
   0..799/0..1279）；`frames xfer=5 nswap=0 nsurf=0 sbytes=0 nok=5 ncpy=0 nret=5`
   ⇒ **GOP `bootMapping` 写像素 / 扫描基址 / retire 全部是活的**；屏不变不是写路径/映射/APFS/sandbox 问题。
2. **l707 stand-down 链已证**（§7aa.2 / §7aa.3、`E:\nabu\logs\rounds\l707.json`）：
   `nabu-splash=1` 回显在、屏仍 Logo，`frames xfer=5 nswap=5 nsurf=2 sbytes=16384000
   nok=5 ncpy=5 nret=5`。`/root/merge/UniXNU-nabu/drivers/GopFramebuffer/NativeLegacyDisplay.cpp:329-334`
   的 `nabu-display: frames … ncpy=%u` 第 6 字段就是 `(unsigned)native_legacy_transfer[5]`；
   `/root/merge/UniXNU-nabu/package/NativeLegacyProbe.cpp:861-893` 明写 `native_splash_update()`
   一见 `transfer[5]!=0` 就 `native_splash_state[3]=1` stand down
   （「client has begun presenting: hand the panel over」）。⇒ `ncpy=5` 实证 CA/WindowServer
   已提交 5 个 surface 并完成 5 次真实 CPU 拷贝 + dsb + retire；**splash 没画是按设计让位，不是通道坏**。
   `NativeLegacyDisplay.cpp:615-617` 的 `kickOnce()` 开头
   `PE_parse_boot_argn("offline-legacy-transfer",&enabled,sizeof(enabled)); if(enabled!=1 …) return;`
   ⇒ 只要**去掉** `offline-legacy-transfer=1`，这条 copy/kick 路径不再运行，`transfer[5]` 应停 0。
3. **§7ab / §7ac（屏上 Logo 的性质）**：l707 全量 os_log 119,428 行里 backboardd 主动呈现
   `BKDisplayRenderOverlayAppleLogo`、`No explicit screen owner` 共 4 条、
   `Dismissing boot logo` / `screen owner is now pid` / `systemShellDidFinishLaunching` 全 0；
   SpringBoard pid27 前台（`ForegroundRunning, BKSApplicationStateAppIsFrontmost=1`）但
   `finishedStartup=0` ⇒ **屏上 Logo 是 BackBoard boot overlay，不是纯 ABL 残留**；
   本轮若仍 Logo/黑，另有 `native_gop_virtual` / banner 时序或 overlay 覆盖断点；
   若画出，则可沿「让 BackBoard overlay dismiss / SpringBoard finishedStartup」推进。
4. **板上内核现量（l707 板报 §2 已复核，五文件逐字节同 l707 ⇒ 同一份 kernel）**：
   `nabu-splash` @1617707；`_native_splash_update @fffffff007a81c6c`、
   `_native_splash_state @fffffff00c19ee10`、`_native_gop_virtual @fffffff00c0e5788`、
   `_native_legacy_transfer @fffffff00c19ede8`、`__ZZ20native_splash_updateE11gSplashRead
   @fffffff00c803250` ⇒ **零新探针、零重编**。
   `NativeLegacyProbe.cpp:906-911` 画 `back=0xff0f1216` 近黑蓝全屏、`mark=0xff2ad4ff` 居中
   240x90 橙标、`done=0xffffffff` / `todo=0xff2a2a2a` 8 格进度条（40 行、3/4 宽），
   `:949 dsb sy` + `state[2]++`；`NativeUfs.cpp:3542` 每轮 banner 调一次（同一 ~20 s 时钟，
   无新 timer / 线程 / 拷贝路径工作）。
5. **日志与静态共同沉默的量**：去掉 `offline-legacy-transfer=1` 后真机 `transfer[5]` 是否真停 0、
   `native_gop_virtual` 映射属性与 panel scanout 是否一致、banner 时序、blank swap / BackBoard
   overlay 是否覆盖、`screen.bgrx` 像素直方图 ⇒ 历轮从未在 transfer 关闭的同时保留 `nabu-splash=1`，
   必须上板。

**历史注释特别注明**：`NativeLegacyDisplay.cpp:61`「removing offline-legacy-transfer left the
screen completely black」是 **l510 的旧状态**（那时内核**没有** `nabu-splash` 这条独立写路径，
删 transfer 只剩 blank-swap 全黑分支）。本轮正是**保留 `nabu-splash=1`、用 splash 独立写路径
重新验证**，条件已不同，**不算重复死路**。

## 3. 为什么离线不行（`AGENTS.md` 0.5 `--why_board`，已写进 `state/precheck.json`）

静态能证：`kickOnce()` 的 `offline-legacy-transfer` 门、`frames` 串 `ncpy=transfer[5]`、
`native_splash_update()` 的 `transfer[5]!=0` stand down 与 `native_gop_virtual` 直接画
`back/mark/done/todo` + `dsb sy`、`NativeUfs.cpp:3542` 每轮 banner 调一次、板上符号与地址，
以及本包只删一个功能 token、五文件与 l707 逐字节同。但「解除 stand-down 后 `nabu-splash=1`
能否在 `screen.bgrx` 画出非 Logo、非纯白/纯黑的必要地区背景 + 8 格进度条」是**运行期像素结果**：
依赖真机 GOP 映射属性与 scanout 基址是否一致、banner 调用时序与 blank swap / BackBoard boot overlay
是否覆盖、`transfer[5]` 是否真的停 0、以及 CDC/snapshot 能否读到 `state[3]`；
离线无寄存器 / 时序 / 像素直方图，无法计算。现有 os_log 只证明 overlay 未 dismiss，
不能证明 splash 通道坏。⇒ 属 0.5 允许的「验证静态结论在真机上是否成立的那**一次**判定」
与「日志与静态都无法产生的新状态」。

## 4. 失败也有信息（三种判读）

| 结果 | 判读 |
|---|---|
| **画出背景 + 8 格进度格**（`screen.bgrx` 直方图出现 `0xff0f1216` 大面积 + `0xff2ad4ff` 居中块 + `0xffffffff`/`0xff2a2a2a` 格，sha 离开 `ca18b728…` 与 `e1547326…`） | **splash 通道本身能写、之前只是按设计让位**；后续沿「让 BackBoard overlay dismiss / SpringBoard finishedStartup」推进，不再追写路径 |
| **屏仍 iBoot Logo**（`screen.bgrx` sha `ca18b728…`）或**全黑**（l696 `0xff000000`） | **`native_gop_virtual` / banner 时序或 overlay 覆盖另有断点**：对照 CDC `nabu-display: frames`（`xfer`/`ncpy` 是否真停 0）、`nabu: scanout hand-off online`、`nabu-gpu-addon:` 与 `nabu-splash=1` 回显；区分「没画」与「画了被 BackBoard overlay / blank swap 盖掉」 |
| **全白**（`e1547326…`） | 与 l706 对照：说明仍有 `sheng-fb-fill` 残留（应已消失，须对账 payload 抽回的 toml）；不得把「没采到」当「没发生」 |

**观察项（全部已有输出，零新增 printf/IOLog/计数器/探针）**：
`screen.bgrx` sha / 直方图（对比 `ca18b728…`、`e1547326…`、l696 纯黑）、
CDC `nabu-splash=1` 回显、`nabu-display: frames xfer nswap nsurf sbytes nok ncpy nret`、
`nabu: scanout hand-off online`、`nabu-gpu-addon:`、panic。
`frames` 预计 `xfer` 不再涨到 5（copy 路径未触发）、`ncpy` 停 0。一次性、用完回滚。

**fc76 未消费与双审计两侧板侧实测偏离**：编译侧 fc76 自 **11-06 14:57** 未消费；
双审计两侧均为**板侧实测**（`audit_dual.py --tag l708 --peer … --mine …`，7/7 equal，
真实 sha 见 §6），照 `state/board-report-l707.md` / `board-report-l706.md:98-100` 注明，
偏离 `AGENTS.md:36-47` 0.6 的「编译侧 vs 板侧」原意；无编译侧成果可比对，本轮零重编。

## 5. 唯一变量与回滚

**唯一功能变量 = args 删除 `offline-legacy-transfer=1`**（含相邻空格 26 B）；661/37，
其余 37 token 同序同字节。`added=[]` / `dropped=['offline-legacy-transfer=1']`。

| 文件 | l707 sha8 | **l708 sha8** | 说明 |
|---|---|---|---|
| kernel.macho | `fb3ce142` | `fb3ce142` | 逐字节相同（未重编） |
| kernel.macho.adt | `78a6f7e9` | `78a6f7e9` | 逐字节相同 |
| gpu-addon.bin | `950a3968` | `950a3968` | 逐字节相同 |
| owner-trust.bundle | `a4a6bae6` | `a4a6bae6` | 逐字节相同 |
| boot-merge-r3.img | `ead4b531` | `ead4b531` | 逐字节相同 |
| payload.img | `84a2ec1f` | **`fe2368bb`** | 唯一变化；包内仅 `UNIXNU~1TOM` 变（1113→1087 B） |

手术：`E:\nabu\tmp\l708-build-20261111\build_l708.py` 以**只读 mmap ACCESS_READ** 打开 l707 模板
（**绝不 mmap 输入 payload 可写**，历史坑见 `state/board-report-l695p.md`），192 MiB 用
`shutil.copyfileobj` **流式**复制，2048 B 簇 **12688 整簇替换**（shrink −26 B、slack 全零、FAT 链不变）
+ 目录项 size `1113 → 1087`（偏移 804028）。payload 内抽回复核：
`KERNEL~1MAC fb3ce142` / `KERNEL~1ADT 78a6f7e9` / `KERNEL~1GPU 950a3968` /
`KERNEL~1TRU a4a6bae6` / `SCRATC~1IMG 3cd3d0d3` 与 l707 全同，仅 `UNIXNU~1TOM` 变
（`selfcheck.txt` / `payload-inner-compare.txt`）。

- **回滚基线**：`E:\nabu\hold\l700\`（kernel `efcb4c34`，已验证 4 分钟窗口）；
  **上一包 / 只读模板** `E:\nabu\hold\l707\`（kernel `fb3ce142`、payload `84a2ec1f`，本会话六文件 sha 前后未变）。
- **一次性、用完回滚**：删 `offline-legacy-transfer=1` 只属于 l708，**不进默认 args**；
  判读完成后刷回 `hold/l700/payload.img` 或 `hold/l707/payload.img`，由用户在
  `logs/rounds/l708.json` 写明「一次性临时诊断轮、用完回滚」。回滚命令（仅文字）：
  `fastboot flash cust hold/l700/payload.img`。

## 6. 恢复预案 / 门禁 / 附注

**`AGENTS.md` 0.10（绝对红线）：不碰 `boot_a`**（绝不 `fastboot flash boot` / `flash boot_a`；
这块平板没有独立 recovery 分区，recovery 就在 boot 里，boot_a 里是 TWRP）。
流程（**仅文字，本会话不执行**；顺序不可换）：

```
recovery arm misc
fastboot flash cust hold/l708/payload.img
fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin   # flash 后最后重 arm
fastboot boot hold/l708/boot-merge-r3.img                      # 临时加载，不写任何分区
```

`E:\nabu\dwc3\build\misc-bootrecovery.bin`（4096 B，l707 板报确认存在）；设备 `TWRP recovery f91fa664`。
`arm-delay=0`（`sheng-smp=1` 硬性配对），咬点 **380-410 s**，观察完整 **4 分钟窗口**
（`AGENTS.md:57-64`），早 panic 也有重置路径。若 fastboot 全失败：人工长按电源 10 秒。
回滚见 §5；**真正需要还原时只由用户当场点名文件**，仍不经过自动化脚本。

**门禁（本会话实测，全部 PASS）**：

| 项 | 结果 | 证据 |
|---|---|---|
| ① 整簇替换 + 五文件 | l708 六文件：五文件与 l707 `cmp SAME`；payload 只 `unixnu.toml` 变；`hold/l707/` 六文件 sha 前后未变（payload `84a2ec1f`） | `build_l708.py`、`l707-baseline-sha.txt`、`l707-after-sha.txt`、`l708-sha8.txt` |
| ② `fsck.fat -n` 双侧 + `audit_bootability.py hold/l708 hold/l707 --expected=fb3ce142…` | l708/l707 均 `10 files, 67550/97913 clusters` rc=0；→ **`OK (0 fail, 0 warn)`** RC=0 | `gate1-fsck.txt`、`gate1-bootability.txt` |
| ③ `audit_dual.py --tag l708` | 真实 sha `kernel=fb3ce142,payload=fe2368bb,adt=78a6f7e9,addon=950a3968,trust=a4a6bae6,boot=ead4b531,args=661/37` → `7 items compared, 7 match, 0 differ` | `gate2-audit_dual.txt`、`state/audit-l708.json` |
| ④ `audit_args.py hold/l707 hold/l708 --expect-dropped offline-legacy-transfer=1` | `687/38 → 661/37`（≤700）；`added=[]`、`dropped=['offline-legacy-transfer=1']` → **`RESULT: OK`** rc=0；逐 token 37/37 同序同字节、零 mismatch、关键开关全 PRESENT | `gate3-audit_args.txt`、`gate3-token-by-token.txt` |
| ⑤ Windows Python 3.12.10 `precheck.py` | `run_precheck_l708.py` 驱动 `--pkg E:/nabu/hold/l708 --baseline E:/nabu/hold/l707 --expect-dropped offline-legacy-transfer=1` → `stamp written: state/precheck.json` rc=0；**八字段全非空**、`token_diff=ok`；`precheck_check.py` → **`[gate] OK`** rc=0，`dual audit OK: audit-l708.json (7 items)` | `gate4-precheck.txt`、`gate4-precheck_check.txt`、`state/precheck.json` |
| ⑥ 六问板报 | 本文件 | `state/board-report-l708.md` |
| 自检 | 抽回 toml 逐 token 只差一个 dropped；抽回 kernel/addon/trust/adt 与 l707 `cmp SAME`；六文件 sha8；`hold/l707/` 未污染 | `selfcheck.txt`、`payload-inner-compare.txt` |

## 7. 证据路径（全部绝对路径）

- 构建与预算：`E:\nabu\tmp\l708-build-20261111\build_l708.py`、`payload.img`、`build.log`、
  `token_check.py`、`run_precheck_l708.py`
- 门禁输出：同目录 `gate1-fsck.txt`、`gate1-bootability.txt`、`gate2-audit_dual.txt`、
  `gate3-audit_args.txt`、`gate3-token-by-token.txt`、`gate4-precheck.txt`、
  `gate4-precheck_check.txt`、`payload-inner-compare.txt`、`selfcheck.txt`、
  `l707-baseline-sha.txt`、`l707-after-sha.txt`、`l708-sha8.txt`
- 戳与板报：`E:\nabu\state\audit-l708.json`、`E:\nabu\state\precheck.json`、
  `E:\nabu\state\board-report-l708.md`
- 包：`E:\nabu\hold\l708\`（六文件）；只读模板 `E:\nabu\hold\l707\`；回滚 `E:\nabu\hold\l700\`
- 板上证据：`E:\nabu\logs\rounds\l706.json`、`E:\nabu\logs\rounds\l707.json`、
  `E:\nabu\state\HANDOVER-20261110-l696.md` §7aa / §7ab / §7ac、
  `E:\nabu\logs\l707-diag-tree\screen.bgrx`、`E:\nabu\logs\l706-diag-tree\screen.bgrx`
- 静态链：`/root/merge/UniXNU-nabu/drivers/GopFramebuffer/NativeLegacyDisplay.cpp:61,329-334,615-617`、
  `/root/merge/UniXNU-nabu/package/NativeLegacyProbe.cpp:861-893,906-949`、
  `/root/merge/UniXNU-nabu/drivers/NativeUfs/NativeUfs.cpp:3542`、
  `E:\nabu\hold\l707\kernel.macho` @1617707 / `llvm-nm` 六符号；`E:\nabu\AGENTS.md`

**注**：本轮**零新探针**（用板上已有 splash 通道与已有 boot-arg 门）、**零重编内核**、
**零安全策略放宽**、**零源码改动**、**零 boot_a 触碰**，**未运行 adb/fastboot、未上板、
未写 `E:\nabu\logs\rounds\l708.json`**。**可否上板：可**（全部门禁 PASS；上板由用户执行并判读）。

## 8. 下一步（仅建议，本会话不执行）

由用户上板后自行写 `logs/rounds/l708.json`（本会话**不写**），按 §4 三判读表读
`screen.bgrx` sha / 直方图、CDC `nabu-splash=1` 回显与 `nabu-display: frames`、
`nabu: scanout hand-off online`、`nabu-gpu-addon:`、panic，并写明
**「一次性临时诊断轮、用完回滚」**，然后回滚到 `hold/l700` 或 `hold/l707`。
