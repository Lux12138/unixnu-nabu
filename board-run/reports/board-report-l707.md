# 上板报告 l707（一次性临时局部插帧轮：板上内核已有的 `nabu-splash=1`）

> 本报告是**上板前**门禁文件；本会话未运行 adb/fastboot、未上板、未写 `E:\nabu\logs\rounds\l707.json`、
> 未改 `E:\nabu\hold\l706\`（只读模板）、未污染 `E:\nabu\hold\l700\`。按 `E:\nabu\AGENTS.md`
> 0.5 / 0.6 / 0.7 / 0.9 / 0.10 编写；格式照 `E:\nabu\state\board-report-l706.md`。

## 0. 用户授权与 args 预算（≤700 硬上限，逐项实测不猜）

**用户授权（2026-11-11）**：「哪怕是插帧，你可以使用临时插帧，进行必要地区的插帧，义无反顾地向前推进」。
⇒ 本轮允许使用**板上内核已有的** `nabu-splash=1`（`native_splash_update()`，只画背景 + 8 格进度条，
真实帧落地即 stand down）。约束：**只在 l707 这一临时轮用**；**不进默认 args**；
**不污染 `hold/l700` 基线**（也不改 `hold/l706` 模板）；**上板后由用户判读并写明「一次性 / 用完回滚」**。
仍然**零新增 printf / IOLog / 计数器 / 采样 / once-only 探针**，
**不改任何源码、不重编内核、不放宽安全策略、不动 boot_a**。

预算（实测，非推算）：

| 步骤 | 操作 | 字符 | token | ≤700 |
|---|---|---|---|---|
| 0 | l706 实测（`tmp/audit_args.py`） | **689** | **38** | PASS |
| A | 原位替换 `sheng-fb-fill=4`(15 B) → `nabu-splash=1`(13 B)，Δ=−2 | **687** | **38** | **PASS** |
| B | l707 payload 抽回 `UNIXNU~1TOM` 复测 | **687** | **38** | **PASS** |

`added=['nabu-splash=1']`、`dropped=['sheng-fb-fill=4']`；其余 **37 token 同序同字节**、零 mismatch、
toml 头尾除 args 外逐字节同（`gate3-token-by-token.txt`）。toml 1115 → 1113 B。
关键开关逐项实测全在：`sheng-fb-gpu=1`、`t733-managed-rt-normal=1`、`sheng-gpu-addon=1`、
`sheng-smp=1`+`nabu-wdt-arm-delay=0`、`-apfs_shared_datavolume`、`offline-legacy-*` 7 项、
`native-ufs-*` 2 项、`nabu-fbcon=0`（Qoder 不可删名单，关系 screen.bgrx 像素读数）、
`-v serial=3 debug=0x14e`、`nabu-splash=1`；`sheng-fb-fill` 已消失（l706 一次性项）。

## 1. 唯一问题（一句话）

**用板上已有的局部插帧通道，能否在 `screen.bgrx` 看到非 Logo、非纯白/纯黑的「必要地区」渲染
（近黑蓝背景 `0xff0f1216` + 居中橙标 `0xff2ad4ff` + 8 格进度条 `0xffffffff`/`0xff2a2a2a`），
且它是否按设计 stand down（`native_splash_state[3]=1`）？**
这是运行期像素结果，静态只能证代码在，必须上板读 `screen.bgrx`（AGENTS.md 0.5）。

## 2. 已闭合到哪（日志优先 + 静态链）

1. **l706 写路径已证**：`E:\nabu\logs\rounds\l706.json` verdict `DECISIVE-POSITIVE`——
   `sheng-fb-fill=4` 把 `screen.bgrx` 从 iBoot Logo sha
   `ca18b728a7def68ea23fea77bb6b91f2a224a18b481f9ec5cff862a87ba5588b` 变成全屏纯白
   sha `e15473266f3c1b5b`（4096016 B、nonblack=1024000、white=1024000、
   1024000 个 `(255,255,255,255)`、bbox `[0,0,799,1279]`）；
   CDC `frames xfer=5 nswap=0 nsurf=0 sbytes=0 nok=5 ncpy=0 nret=5 fingers=0` ⇒
   **GOP `bootMapping` 写像素 / 扫描 / retire 全活**；iBoot Logo 不变的根因是
   **CA 没交出可渲染内容**。`handoff 'nabu: scanout hand-off online gop=9c000000/fa0000
   1600x2560 stride=1900'`、`nabu-gpu-addon: mounted=1 target=1 control=1 status=0`、panic=0。
2. **前史**：`logs/l70{1,2,3,4,5}-diag-tree/screen.bgrx` 逐字节全同 `ca18b728…`；
   `l696` 纯黑是另一状态。所有现有日志对「nabu-splash 局部插帧能否出背景+进度格、
   是否 stand down」**沉默**（历轮从未传过 `nabu-splash`，无任何一轮出现自定义图案）。
3. **板上内核现量（本会话复核 `E:\nabu\hold\l706\kernel.macho` sha `fb3ce142`，与 l707 逐字节同）**：
   `nabu-splash` 字面量 **1 命中 @1617707**；`llvm-nm` 实测
   `_native_splash_update @fffffff007a81c6c`、`_native_splash_state @fffffff00c19ee10`、
   `_native_gop_virtual @fffffff00c0e5788`、`_native_legacy_transfer @fffffff00c19ede8`、
   `_native_legacy_transfer_observed @fffffff007a80dc4`、
   `__ZZ20native_splash_updateE11gSplashRead @fffffff00c803250` ⇒ **零新探针、零重编**。
4. **源码链**：`/root/merge/UniXNU-nabu/package/NativeLegacyProbe.cpp:860-900`——
   `PE_parse_boot_argn("nabu-splash",&s,sizeof s)` 置 `native_splash_state[0]=1`；
   **`native_legacy_transfer[5]!=0` 时 `native_splash_state[3]=1` 自动 stand down**
   （「client has begun presenting: hand the panel over」，用 `[5]` 而非 `[0]`：只有真实 CPU 拷贝 + dsb 才抬升）；
   否则取 `native_gop_virtual` / `native_legacy_gop[3..5]`（w/h/row）直接画：
   `:906-911` `back=0xff0f1216` 近黑蓝全屏、`mark=0xff2ad4ff` 居中 240x90 橙标、
   `done=0xffffffff`/`todo=0xff2a2a2a` 8 格进度条（40 行、3/4 宽），
   `:949 __asm__ volatile("dsb sy")` + `state[2]++`；
   `drivers/NativeUfs/NativeUfs.cpp:3542` **每轮 banner 调一次** `native_splash_update()`
   （同一 ~20 s 时钟，无新 timer / 线程 / 拷贝路径工作）。
5. **日志与静态共同沉默的量**：真机 `native_gop_virtual` 映射属性与 panel scanout 是否一致、
   banner 调用时序、`native_legacy_transfer[5]` 何时被抬升、像素直方图 ⇒ 必须上板。

## 3. 为什么离线不行（AGENTS.md 0.5 `--why_board`）

静态能证 splash 代码 / 符号 / 地址、`PE_parse_boot_argn` 门、`[5]` 的 stand down、
`back/mark/done/todo` 四色与 `dsb sy`、banner 每轮调一次，以及本包只改一个功能 token、五文件与 l706
逐字节同。但「局部插帧通道能否在 `screen.bgrx` 看到非 Logo、非纯白/纯黑的必要地区渲染、且是否按设计
stand down」是**运行期结果**：依赖真机 GOP 映射属性、blank swap 是否覆盖 banner 写入、
`native_legacy_transfer[5]` 在真机何时抬升（CA 是否交出真实帧）、以及 CDC/snapshot 是否读到
`native_splash_state[3]`；离线无寄存器 / 时序 / 像素直方图，无法计算。
⇒ 属 0.5 允许的「验证静态结论在真机上是否成立的那**一次**判定」与「日志与静态都无法产生的新状态」。

## 4. 失败也有信息（三种判读）

| 结果 | 判读 |
|---|---|
| **屏出现背景 + 进度格**（`screen.bgrx` 直方图出现 `0xff0f1216` 大面积 + `0xff2ad4ff` 居中块 + 8 格 `0xffffffff`/`0xff2a2a2a`，sha 离开 `ca18b728…` 与 `e1547326…`） | **局部插帧通**：板上已有 `native_splash_update()` 经 `native_gop_virtual` 直接画能成为 panel 像素；`nabu-splash` 可作为 CA 交出内容前的合法过渡画面。继续看 `native_splash_state[3]` 是否随真实帧抬升而 stand down |
| **屏仍 iBoot Logo**（`screen.bgrx` sha `ca18b728…` 逐字节同） | **`native_gop_virtual` / banner 调用时序问题**：要么 `nabu-splash` 未被解析（`gSplashRead`/`state[0]`）、要么 `native_gop_virtual` 为 0 或 `row<w*4` 提前 return、要么 banner 未走到 `NativeUfs.cpp:3542`、要么写入被 blank swap 覆盖；对照 CDC `nabu-display: frames`、`nabu: scanout hand-off online`、`nabu-gpu-addon:` 与 unixnu.toml args 回显 |
| **全白 / 全黑** | **与 l706 / l696 对比**：全白 `e1547326…` 说明仍有 `sheng-fb-fill` 残留（应已消失，须对账 payload 抽回的 toml）；全黑说明 `0xff000000U` blank 分支覆盖或 splash 根本没画；不得把「没采到」当「没发生」 |

**观察项（全部已有输出，零新增 printf/IOLog/计数器/探针）**：
`screen.bgrx` 直方图 / sha（对比 `ca18b728…`、`e1547326…`、l696 纯黑）、
CDC `nabu-display: frames xfer nswap nsurf nok ncpy nret`、
`nabu: scanout hand-off online`、`nabu-gpu-addon:`、panic、
`native_splash_state[3]` / `NABUGPU1` / `nabu-oslog`（若有）。
一次性、用完回滚；`nabu-splash=1` 不进默认 args。

**fc76 未消费与双审计偏离**：编译侧 fc76 自 **11-06 14:57** 未消费；双审计两侧均为**板侧实测**
（`audit_dual.py --tag l707 --peer … --mine …`，7/7 equal），照 `state/board-report-l706.md:98-100` /
`board-report-l705.md` 注明，偏离 `AGENTS.md:36-47` 0.6 的「编译侧 vs 板侧」原意。

## 5. 唯一变量与回滚

**唯一功能变量 = args 原位替换 `sheng-fb-fill=4` → `nabu-splash=1`**；687/38，其余 37 token 同序同字节。

| 文件 | l706 sha8 | **l707 sha8** | 说明 |
|---|---|---|---|
| kernel.macho | `fb3ce142` | `fb3ce142` | 逐字节相同（未重编） |
| kernel.macho.adt | `78a6f7e9` | `78a6f7e9` | 逐字节相同 |
| gpu-addon.bin | `950a3968` | `950a3968` | 逐字节相同 |
| owner-trust.bundle | `a4a6bae6` | `a4a6bae6` | 逐字节相同 |
| boot-merge-r3.img | `ead4b531` | `ead4b531` | 逐字节相同 |
| payload.img | `1d394898` | **`84a2ec1f`** | 唯一变化；包内仅 `UNIXNU~1TOM` 变（1115→1113 B） |

手术：`E:\nabu\tmp\l707-build-20261111\build_l707.py` 以**只读 mmap ACCESS_READ** 打开 l706 模板
（**绝不 mmap 输入 payload 可写**，历史坑见 `state/board-report-l695p.md`），192 MiB 用
`shutil.copyfileobj` **流式**复制，2048 B 簇 **12688 整簇替换**（shrink −2 B、slack 全零、FAT 链不变）
+ 目录项 size `1115 → 1113`（偏移 804028）。payload 内抽回复核：
`KERNEL~1MAC fb3ce142` / `KERNEL~1ADT 78a6f7e9` / `KERNEL~1GPU 950a3968` /
`KERNEL~1TRU a4a6bae6` / `SCRATC~1IMG 3cd3d0d3` 与 l706 全同，仅 `UNIXNU~1TOM` 变
（`selfcheck.txt` / `payload-inner-compare.txt`）。

- **回滚基线**：`E:\nabu\hold\l700\`（kernel `efcb4c34`，已验证 4 分钟窗口）；
  **上一包 / 只读模板** `E:\nabu\hold\l706\`（kernel `fb3ce142`、payload `1d394898`，本会话六文件 sha 前后未变）。
- **一次性、用完回滚**：`nabu-splash=1` 只属于 l707，**不进默认 args**；判读完成后刷回
  `hold/l700/payload.img` 或 `hold/l706/payload.img`，由用户在 `logs/rounds/l707.json` 写明
  「一次性临时局部插帧轮、用完回滚」。回滚命令（仅文字）：`fastboot flash cust hold/l700/payload.img`。

## 6. 恢复预案 / 门禁 / 附注

**AGENTS.md 0.10 绝对红线：不碰 `boot_a`**（绝不 `fastboot flash boot` / `flash boot_a`；recovery 就在 boot 里）。
流程（**仅文字，本会话不执行**；顺序不可换）：

```
recovery arm misc
fastboot flash cust hold/l707/payload.img
fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin   # flash 后最后重 arm
fastboot boot hold/l707/boot-merge-r3.img                      # 临时加载，不写任何分区
```

`E:\nabu\dwc3\build\misc-bootrecovery.bin`（4096 B，本会话确认存在）；设备 `TWRP recovery f91fa664`。
`arm-delay=0`，咬点 **380-410 s**，观察完整 **4 分钟窗口**（`AGENTS.md:57-64`），早 panic 也有重置路径。
若 fastboot 全失败：人工长按电源 10 秒。回滚见 §5。

**门禁（本会话实测，全部 PASS）**：

| 项 | 结果 | 证据 |
|---|---|---|
| ① 整簇替换 + 五文件 | l707 六文件：五文件与 l706 `cmp SAME`；payload 只 `unixnu.toml` 变；`hold/l706/` 六文件 sha 前后未变（payload `1d394898`） | `build_l707.py`、`l706-baseline-sha.txt`、`l706-after-sha.txt`、`l707-sha8.txt` |
| ② `fsck.fat -n` 双侧 + `audit_bootability.py hold/l707 hold/l706 --expected=fb3ce142…` | l707/l706 均 `10 files, 67550/97913 clusters` rc=0；→ **`OK (0 fail, 0 warn)`** RC=0 | `gate1-fsck.txt`、`gate1-bootability.txt` |
| ③ `audit_dual.py --tag l707` | 真实 sha `kernel=fb3ce142,payload=84a2ec1f,adt=78a6f7e9,addon=950a3968,trust=a4a6bae6,boot=ead4b531,args=687/38` → `7 items compared, 7 match, 0 differ` | `gate2-audit_dual.txt`、`state/audit-l707.json` |
| ④ `audit_args.py hold/l706 hold/l707` | `689/38 → 687/38`（≤700）；`added=['nabu-splash=1']`、`dropped=['sheng-fb-fill=4']` → **`RESULT: OK`** rc=0；逐 token 37/37 同序同字节、零 mismatch、关键开关全 PRESENT | `gate3-audit_args.txt`、`gate3-token-by-token.txt` |
| ⑤ Windows Python 3.12.10 `precheck.py` | `run_precheck_l707.py` 驱动 `--pkg E:/nabu/hold/l707 --baseline E:/nabu/hold/l706 --expect-added/--expect-dropped` → `stamp written: state/precheck.json` rc=0；**八字段全非空**、`token_diff=ok`；`precheck_check.py` → **`[gate] OK`** rc=0，`dual audit OK: audit-l707.json (7 items)` | `gate4-precheck.txt`、`gate4-precheck_check.txt`、`state/precheck.json` |
| ⑥ 六问板报 | 本文件 | `state/board-report-l707.md` |
| 自检 | 抽回 toml 逐 token 只差两 token；抽回 kernel/addon/trust/adt/scratch 与 l706 `SAME`；六文件 sha8；`hold/l706/` 未污染 | `selfcheck.txt`、`payload-inner-compare.txt` |

## 7. 证据路径（全部绝对路径）

- 构建与预算：`E:\nabu\tmp\l707-build-20261111\build_l707.py`、`payload.img`、
  `token_check.py`、`run_precheck_l707.py`
- 门禁输出：同目录 `gate1-fsck.txt`、`gate1-bootability.txt`、`gate2-audit_dual.txt`、
  `gate3-audit_args.txt`、`gate3-token-by-token.txt`、`gate4-precheck.txt`、
  `gate4-precheck_check.txt`、`payload-inner-compare.txt`、`selfcheck.txt`、
  `l706-baseline-sha.txt`、`l706-after-sha.txt`、`l707-sha8.txt`
- 戳与板报：`E:\nabu\state\audit-l707.json`、`E:\nabu\state\precheck.json`、
  `E:\nabu\state\board-report-l707.md`
- 包：`E:\nabu\hold\l707\`（六文件）；只读模板 `E:\nabu\hold\l706\`；回滚 `E:\nabu\hold\l700\`
- 板上证据：`E:\nabu\logs\rounds\l706.json`、`E:\nabu\logs\l706-diag-tree\screen.bgrx`、
  `E:\nabu\logs\l70{1,2,3,4,5}-diag-tree\screen.bgrx`、`E:\nabu\logs\rounds\l70{1..5}.json`
- 静态链：`E:\nabu\hold\l706\kernel.macho` @1617707 / `llvm-nm` 六符号；
  `/root/merge/UniXNU-nabu/package/NativeLegacyProbe.cpp:860-900,906-949`；
  `/root/merge/UniXNU-nabu/drivers/NativeUfs/NativeUfs.cpp:3542`；
  `E:\nabu\AGENTS.md:14-24,36-47,57-64,100-109`

**注**：本轮**零新探针**（用板上已有 splash 通道）、**零重编内核**、**零安全策略放宽**、
**零源码改动**、**零 boot_a 触碰**。**可否上板：可**（全部门禁 PASS；上板由用户执行并判读）。

## 8. 下一步（仅建议，本会话不执行）

由用户上板后自行写 `logs/rounds/l707.json`（本会话**不写**），按 §4 三判读表读
`screen.bgrx` 直方图与 sha、CDC `nabu-display: frames`、`nabu: scanout hand-off online`、
`nabu-gpu-addon:`、panic，并写明**「一次性临时局部插帧轮、用完回滚」**，然后回滚到
`hold/l700` 或 `hold/l706`。
