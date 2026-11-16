# 上板报告 l717（一次性临时功能轮：只补 `offline-key-events=1` 发布 AppleKeyStore 替身）

生成时间：2026-11-11。性质：**一次性临时功能轮 / 可证伪诊断轮**，用户授权自动推进、
临时插帧、数据补丁，要求不停；用完回滚，**不进默认 args**。本会话只构建与门禁，
**未运行 adb/fastboot、未上板、未写 `E:\nabu\logs\rounds\l717.json`**。

## 0. 用户授权与性质（六问板报总纲）

- 授权：用户授权自动推进 / 临时插帧 / 数据补丁，要求不停；依据
  `E:\nabu\state\HANDOVER-20261110-l696.md` §7ag–§7ao 及 `E:\nabu\AGENTS.md` 0.5/0.6/0.7/0.9/0.10。
- 边界：**只用板上内核已有机制**；一次性临时轮、用完回滚、不进默认 args；
  **零新增 printf / IOLog / 计数器 / 探针、不改源码、不重编内核、不放宽安全策略、不动 boot_a**。
- 撤回更正：§7ag.1(3) 的「l717 不补 `offline-key-events=1`」在机制复核后**撤回**。
  旧否决用的是「Qoder `l578ak2` 两 token 齐备但 `nswap` 仍是 5 / 不推进桌面」这一
  **桌面判据**（`E:\nabu\logs\rounds\l578ak2.json` 的 `state_line nswap=5`）；
  本轮换用「AppleKeyStore 服务是否发布、`failed to open connection` /
  `aks_get_lock_state failed` / `keychain-2.db Code=14` 是否减少」这一**不同的、板上已证**
  的服务判据。旧阴性对照不构成对本轮问题的否证。§7ao 明确
  `native_aks_lock_state()` 只回报锁状态、不创建 `keychain-2.db`，故本轮**不承诺桌面**。

## 1. 唯一问题

**发布 `AppleKeyStore` 替身后，l714 的
`KeyStoreNotifier - Can't find AppleKeyStore service` /
`securityd failed to open connection to AppleKeyStore` /
`aks_get_lock_state failed: e00002bc` 是否消失，以及 `keychain-2.db`
`Unable to create database ... Code=14` / MobileActivation `Migration failed` /
PreBoard 白页是否变化？**

唯一变量：以 `E:\nabu\hold\l714\` 为模板，只在 `unixnu.toml` 的 boot-args 里
**加 `offline-key-events=1`**，与已在的 `sheng-aks=1` 组成双 token，让板上内核
`native_keyevents_probe()` 发布 `AppleKeyStore` 替身。kernel 与其余五文件不动。

判据全为**官方已有输出、零新探针**：CDC/ring `NABUAKS1 st=3`、os_log
`AppleKeyStore` 连接成功与三类错误计数、`keychain-2.db` Code=14 计数、
`Migration failed` 次数、`screen.bgrx` sha/直方图、`nabu-display: frames`、panic。
**即使 Code=14 仍在，只要 AKS 替身发布、锁状态错误消失，也是明确进展。**

## 2. 已闭合到哪（六问之二，chain）

静态链每环带证据，不伪造闭环：

1. **最佳基线实测**：`E:\nabu\hold\l714\` 六文件 sha8 =
   `ead4b531`(boot) / `950a3968`(addon) / `d9b5a174`(kernel) / `78a6f7e9`(adt) /
   `a4a6bae6`(trust) / `da7df504`(payload)，args **685/38**。
   `E:\nabu\logs\rounds\l714.json`、`E:\nabu\logs\l714-diag-tree\screen.bgrx`：
   PreBoard 白页 `e39720f2…`（1,021,128 白 + 少量黑灰）、frames `ncpy=3`、
   `finishedStartup:YES` / `Dismissing overlay`。
2. **l714 精确计数**（本会话对 `E:\nabu\logs\l714-v1.jsonl` 逐行按 `eventMessage`
   复测，与用户给出的数字一致）：
   `Can't find AppleKeyStore service` **1**、
   `failed to open connection to AppleKeyStore` **4**、
   `aks_get_lock_state failed: e00002bc` **2**、
   `keychain-2.db` **48**、`Unable to create database ... Code=14` **25**、
   `NABUAKS1` **0**、`Migration failed` **10**。
   `E:\nabu\logs\l714-cdc.bin` boot-args 回显含 `sheng-aks=1`、无
   `offline-key-events=1` ⇒ 替身未发布，与 `NABUAKS1=0` 相符。
3. **已排除/已闭合**：§7al/§7am 证 MKB/keybag/usermanagerd/persona 官方 7 条验收全过；
   §7an 证 `keychain-2.db` 应由 securityd 自建、官方 seed 不预置；§7ak 证
   `/private/var/Keychains 0755 root:root` 与 IPSW 工厂清单一致；
   l715 写 `buddyComplete` plist、l716 `runSetupAssistant=false` 均退回纯黑，不沿用；
   §7ao 证 `native_aks_lock_state()` 不创建 db ⇒ 本轮不承诺桌面。
4. **§7ag 撤回理由**：旧否决采用 nswap 桌面判据；本轮采用 AppleKeyStore 服务判据，
   二者是不同的、板上已证的问题，`E:\nabu\logs\l578ak2-ring.txt:805-806` 的
   `NABUAKS1 st=3 rc=0` / `AKS opens=79 calls=354` 正是「服务发布成功」的官方证据。
5. **机制源码**（`E:\nabu\tmp\l331-src\UniXNU-nabu\package\KeyStoreProbe.cpp`）：
   `:27-34` `native_aks_state_enabled()` 由 `PE_parse_boot_argn("sheng-aks")` 置位；
   `:76-92` `native_keyevents_probe()` 首门要 `offline-key-events=1` 且
   `(ephemeral-storage==1 || native_aks_state_enabled())`；`:93-108`
   `OSSymbol::withCString("AppleKeyStore")` 缺席判定 → `preModLoad/postModLoad` →
   `native_keyevents_allocate()` → `IOService attach/start` 发布替身；
   `:36-45` `native_aks_lock_state()` 只把 `sheng-aks-sls` 报成 0..7，不写 db。
6. **板上已有可观测量**：`E:\nabu\hold\l717\kernel.macho` 实测
   `offline-key-events` 1、`sheng-aks` 2、`NABUAKS1` 1、`native_aks` 30、
   `AppleKeyStore` 107 字面量；官方 CDC 格式串 `NABUAKS1 st=` 已编入，非新探针。
7. **待证传导（运行期）**：补 token → `native_keyevents_probe` 过门 →
   `AppleKeyStore` 替身注册 → UserEventAgent / securityd 能连上服务 →
   三类错误归零或下降 → 观察 Code=14 / `Migration failed` / 白页 / frames / panic。

## 3. 为什么离线不行（六问之三，why_board / AGENTS.md 0.5）

静态能证：§2 全链、l714 精确错误计数、l578ak2 的 `st=3` 先例、两个
`PE_parse_boot_argn` 门与发布路径、内核字面量与官方格式串、抽回 toml 逐 token 比、
toml 外 inner 文件全 SAME、五文件逐字节同 l714、关键开关全在、APFS nop 未动、
`hold/l714` 六文件 sha 未变。

**不能静态得到的运行期量**：IOKit 是否真走到 `stage=3` / `NABUAKS1 st`；
UserEventAgent 是否还报 `Can't find AppleKeyStore service`；securityd 是否仍
`failed to open connection` / `aks_get_lock_state failed`；`keychain-2.db Code=14`
与 `Migration failed` 计数；`screen.bgrx` sha/直方图、`nabu-display: frames`、panic。
这些是用户态连接时序与 IOKit 服务注册状态。l714 与 l578ak2 都没跑过
「`d9b5a174` + 隐藏 BootUI + `sheng-mig=1` + 双 AKS token」这一组合。
**日志与静态都无法产生该新状态 ⇒ 满足 AGENTS.md 0.5**（静态先于上板、闭环已写、
只为「验证静态结论在真机上是否成立」的那一次判定）。

## 4. 失败也有信息（六问之四，三判读 + 观察项）

**判读 A（明确进展）**：`NABUAKS1 st=3` 且
`Can't find AppleKeyStore service` / `failed to open connection` /
`aks_get_lock_state failed` 归零或下降（Code=14 可仍在）⇒ 证明
「l714 的 AppleKeyStore 连接链缺的正是服务发布」，墙收窄到 sqlite db 创建链。

**判读 B（部分进展）**：`st=3` 但三类错误不减 ⇒ 替身已发布但 UserEventAgent /
securityd 未连上，或连上后仍走别的失败路径；把墙收窄到服务匹配/权限，而非 token 门。

**判读 C（阴性）**：`st=0` 或 `NABUAKS1` 仍 0 命中 ⇒ 双 token 在
`d9b5a174` 上未触发 `native_keyevents_probe()`，机制假设被证伪；不再重复空跑 AKS。

三种结果都产生可判读的新事实，不是「再看看」。

**观察项（全官方已有输出、零新探针）**：CDC/ring `NABUAKS1`、`AKS opens/calls`；
os_log `AppleKeyStore` 连接成功 / `Can't find ...` / `failed to open connection` /
`aks_get_lock_state failed: e00002bc` / `keychain-2.db` / `Unable to create database ... Code=14` /
`Migration failed`；`E:\nabu\logs\` 下本轮 `screen.bgrx` sha 与直方图；
`nabu-display: frames`（xfer/nswap/ncpy/nret）；panic / unencrypted panic / `buf_bread_err16`。

## 5. 唯一变量与回滚点（六问之五）

### 变量表（四行）

| 项 | 值 |
|---|---|
| 内核线 | `d9b5a174`（与 `hold/l714/kernel.macho` 逐字节相同，125,628,416 B；payload 内 `KERNEL~1MAC` 抽回亦同 sha） |
| args | 685/38 → **681/38 ≤700**；`added=['offline-key-events=1']`、`dropped=['native-ufs-diagnostics=1']` |
| 设备状态 | TWRP `f91fa664`，`boot_a` 未碰；本会话不上板 |
| 窗口长度 | `arm-delay=0`，咬点 380-410 s，完整 **4 分钟**窗口 |

### 包清单（`E:\nabu\hold\l717\` 六文件 sha256 前 8）

| 文件 | 大小 (B) | sha8 | vs `hold/l714` |
|---|---:|---|---|
| `boot-merge-r3.img` | 3,041,280 | `ead4b531` | SAME |
| `gpu-addon.bin` | 12,317,544 | `950a3968` | SAME |
| `kernel.macho` | 125,628,416 | `d9b5a174` | SAME |
| `kernel.macho.adt` | 211,568 | `78a6f7e9` | SAME |
| `owner-trust.bundle` | 61,456 | `a4a6bae6` | SAME |
| `payload.img` | 201,326,592 | `c1533813` | **DIFF**（只 `UNIXNU~1TOM`） |

payload 内 7 个 inner file：`KERNEL~1ADT` / `KERNEL~1GPU` / `KERNEL~1MAC` /
`KERNEL~1TRU` / `NABUESP` / `SCRATC~1IMG` 全 **SAME**，仅
`UNIXNU~1TOM` CHANGED（1111→1107 B，目录项 size 同步 1111→1107）。

### args 预算与腾位方案（精确实测）

直接加 `offline-key-events=1` 会到 706/39 > 700（硬条件）。
`E:\nabu\hold\l714\payload.img` 抽回实测：args 685 字符 / 38 token；
`offline-key-events=1` 20 B、`native-ufs-diagnostics=1` 24 B，
原位替换净 **−4 B ⇒ 681/38 ≤700**（余 19 字符），37/37 存活 token 同序同字节。

**选 `native-ufs-diagnostics=1` 的理由与历史证据**：只由
`E:\nabu\hold\l588-touch\sources\NativeUfs.cpp:4242/:4306` 解析到
`gRootDiagnosticEnabled`，`:1660-1674/:1712/:1833/:1870/:2286` 只控首传输故障快照 /
root-fault 诊断文本；`E:\nabu\logs\l714-cdc.bin` 实测 `diagnostics=1` 但
`UFS TRANSPORT FAULT` **0**、root 故障 **0** ⇒ 本轮摘掉只损失未触发的诊断快照，
不改 CDC/USB、状态环、UFS 实际读写、显示或 AKS 策略。
`E:\nabu\bridge\status\d46a.json:90` 与
`E:\nabu\bridge\inbox-33dd\20261103-1900-args-budget-table-for-four-display-gates.md:26`
曾列「不可摘（改动即改行为）」，此代价在此明示为**可证伪成本**；
`E:\nabu\tmp\l717-build-20261111\rejected-v1\reevaluation.txt` 逐行复核后保留该方案。
（旧版 11:40 的 `state/precheck.json` 用的是 nswap 桌面判据，已由本轮 14:48 重写替换。）

**逐项排除其它候选**：`sheng-mig=1` 是 l714 白页触发条件，绝不删；
`nabu-fbcon=0` 关系 `screen.bgrx` 像素读数
（`E:\nabu\state\board-report-l704.md:14`），保留；
`debug=0x14e`→`0x0` 省 2、`serial=3` 删省 9、`-v` 删省 3 组合省字不够 21，
且 `E:\nabu\HANDOVER-20261105-compile-side-fc76.md:104` 与
`board-report-l704.md:15-17` 列为不可删（panic/CDC/串口可读性）；
`qcom-usb-*` 全关系 CDC 采集；`sheng-log=1` 经 l714 内核反汇编会打开
`NABUDIAG1/2` 与 `screen.bgrx` 导出区；`nabu-touch=1` 丢 126 行官方 touch 输出；
把 `sheng-mig=1` 原位换成 `offline-key-events=1`（695/38）虽合法但会丢白页，
仅作最后手段，本轮不用。

**关键开关全在**：`sheng-aks=1` + `offline-key-events=1` + `sheng-mig=1`、
`sheng-fb-gpu=1`、`t733-managed-rt-normal=1`、`sheng-gpu-addon=1`、
`sheng-smp=1` + `nabu-wdt-arm-delay=0`、`-apfs_shared_datavolume`、
`offline-legacy-*`、`nabu-fbcon=0`、`nabu-log=1`、`qcom-usb-*`。

### 构建方式与模板只读

中间新目录 `E:\nabu\tmp\l717-final-20261111\`（本轮门禁与自检）与既有
`E:\nabu\tmp\l717-build-20261111\`（包构建）；最终包 `E:\nabu\hold\l717\`。
`hold/l714/` **只读**：输入 payload 以 `mmap ACCESS_READ` 打开，
192 MiB 用 `shutil.copyfileobj` **流式**写到全新输出文件，绝不 mmap 输入 payload 可写；
`EFI/UNIXNU/UNIXNU~1TOM` 单簇 12688 **整簇替换**（slack 零填充），
目录项 size @804028 同步更新 1111→1107。
构建脚本：`E:\nabu\tmp\l717-build-20261111\build_l717.py`、
`assemble_l717.py`；日志 `build.log`、`assemble.log`。

**`hold/l714/` 未污染**：完成后复测六文件 sha8 仍为
`ead4b531 / 950a3968 / d9b5a174 / 78a6f7e9 / a4a6bae6 / da7df504`（payload 未变）。

### 回滚

一次性临时轮、用完回滚、不进默认 args。回滚包：
`E:\nabu\hold\l700\`（kernel `efcb4c34`）/ **`E:\nabu\hold\l714\`**（kernel `d9b5a174`、
payload `da7df504`，PreBoard 白页最佳基线）。回滚只需把六文件换回 l714，
不改任何源码、不重编、不动 boot_a。

## 6. 恢复预案（六问之六，AGENTS.md 0.10 / 0.7 / 0.9 / 附注）

**`AGENTS.md` 0.10 绝对红线：不碰 `boot_a`**（绝不
`fastboot flash boot` / `flash boot_a`；这块板没有独立 recovery 分区，
boot_a 里是 TWRP）。流程（**仅文字，本会话不执行**；顺序不可换）：

```
recovery arm misc                                             # 不碰 boot_a
fastboot flash cust hold/l717/payload.img
fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin   # 最后才重 arm misc
fastboot boot hold/l717/boot-merge-r3.img                     # 临时加载，不写任何分区
```

`E:\nabu\dwc3\build\misc-bootrecovery.bin`（4096 B）；设备 TWRP recovery `f91fa664`。
**`arm-delay=0`**（`sheng-smp=1` 硬性配对，`AGENTS.md:57-64`），咬点
**380-410 s**，观察完整 **4 分钟窗口**，早 panic 也有重置路径；
若 fastboot 全失败：**人工长按电源 10 秒**。回滚见 §5。

### 门禁（本会话实测，全部 PASS）

| 项 | 结果 | 证据（绝对路径） |
|---|---|---|
| ① 构建 + 模板只读 | 新 tmp 目录 / 新 `hold/l717/`；五文件逐字节同 l714；payload 只 `UNIXNU~1TOM` 变；整簇替换 + 目录项 size；`ACCESS_READ` / 192 MiB 流式；l714 六文件 sha 前后未变（payload `da7df504`） | `E:\nabu\tmp\l717-build-20261111\build_l717.py`、`assemble_l717.py`、`build.log`、`assemble.log`；`E:\nabu\tmp\l717-final-20261111\selfcheck.txt` |
| ② `fsck.fat -n` 10 files + `audit_bootability.py hold/l717 hold/l714 --expected=d9b5a17405b2aaac767eeddd82f3914b7e753bb2753262032ccb045d740246e3` | l714/l717 均 `10 files, 67550/97913 clusters` rc=0；→ **`OK (0 fail, 0 warn)`** RC=0（PRELINK 47/47、ncmds=5776、payload 体积不变、kernel sha 匹配） | `E:\nabu\tmp\l717-final-20261111\gate1-fsck.txt`、`gate1-bootability.txt` |
| ③ `audit_dual.py --tag l717` | 逗号分隔真实 sha `kernel=d9b5a174,payload=c1533813,adt=78a6f7e9,addon=950a3968,trust=a4a6bae6,boot=ead4b531,args=681/38` → **`7 items compared, 7 match, 0 differ`** | `E:\nabu\tmp\l717-final-20261111\gate2-audit_dual.txt`、`measured-values.txt`、`E:\nabu\state\audit-l717.json` |
| ④ `tmp/audit_args.py l714 l717` | `685/38 → 681/38`（≤700）；**`added=['offline-key-events=1']`、`dropped=['native-ufs-diagnostics=1']`** → **`RESULT: OK`** rc=0；其余 37 token 同序同字节、关键开关全在 | `E:\nabu\tmp\l717-final-20261111\gate3-audit_args.txt`、`selfcheck.txt` |
| ⑤ Windows Python 3.12.10 `precheck.py` | `E:\nabu\tmp\l717-final-20261111\run_precheck_l717.py` 驱动 `--pkg E:/nabu/hold/l717 --baseline E:/nabu/hold/l714`，声明 added/dropped，chain/why_board 用**更正后的 AppleKeyStore 服务判据** → `stamp written: E:/nabu/state/precheck.json` rc=0；八字段齐、`token_diff=ok`；`precheck_check.py` → **`[gate] OK`** rc=0，`dual audit OK: audit-l717.json (7 items)` | `E:\nabu\tmp\l717-final-20261111\gate4-precheck.txt`、`gate4-precheck_check.txt`、`E:\nabu\state\precheck.json` |
| ⑥ 六问板报 | 本文件；`E:\nabu\tmp\board_report_check.py l717` → `[report] OK` 6/6 节 | `E:\nabu\state\board-report-l717.md`、`E:\nabu\tmp\l717-final-20261111\gate6-board-report.txt` |
| 自检 | 抽回 toml 逐 token 比（37/37 同序同字节、681/38 ≤700、added/dropped 与声明一致）；抽回 kernel == l714 kernel `d9b5a174`；toml 外其余 inner 文件与 l714 全 SAME；六文件 sha8 见 §5；`hold/l714/` 未污染；payload 字节差 848 B 全落在 toml 簇 + 目录项 size | `E:\nabu\tmp\l717-final-20261111\selfcheck.txt`、`l717-sha8.txt`（见 §5 表） |

**零新增 printf/IOLog/计数器/探针、零重编、零源码改动、零安全策略放宽、
零 boot_a 触碰**；kernel 仍是 l714 同长数据补丁（含
`BKHideAppleLogoOnLaunch=true`，文件长 125628416 不变），
**APFS nop `@0x6cd2f68 = 1f2003d5` 未动**；本轮未运行 adb/fastboot、未上板、
未写 `E:\nabu\logs\rounds\l717.json`。

**fc76 未消费与双审计偏离**：编译侧 fc76 自 **11-06 14:57** 未消费；
`audit_dual.py --tag l717` 的 `--peer` / `--mine` **两侧均为板侧实测**
（7/7 equal，真实 sha 见 §5），照 `E:\nabu\state\board-report-l714.md:191-195`
注明：偏离 `AGENTS.md:36-47` 0.6 的「编译侧 vs 板侧」原意；本轮零重编、
无编译侧成果可比对。

## 7. 证据路径（全部绝对路径）

- 构建：`E:\nabu\tmp\l717-build-20261111\`（`build_l717.py`、`assemble_l717.py`、
  `build.log`、`assemble.log`、`rejected-v1\reevaluation.txt`）
- 本轮门禁/自检：`E:\nabu\tmp\l717-final-20261111\`（`selfcheck.txt`、
  `measured-values.txt`、`gate1-fsck.txt`、`gate1-bootability.txt`、
  `gate2-audit_dual.txt`、`gate3-audit_args.txt`、`gate4-precheck.txt`、
  `gate4-precheck_check.txt`、`gate6-board-report.txt`、`run_precheck_l717.py`）
- 包：`E:\nabu\hold\l717\`；只读模板：`E:\nabu\hold\l714\`；回滚：`E:\nabu\hold\l700\`
- 状态戳：`E:\nabu\state\audit-l717.json`、`E:\nabu\state\precheck.json`、
  `E:\nabu\state\board-report-l717.md`
- 判据来源：`E:\nabu\logs\rounds\l714.json`、`E:\nabu\logs\l714-v1.jsonl`、
  `E:\nabu\logs\l714-cdc.bin`、`E:\nabu\logs\l714-diag-tree\screen.bgrx`、
  `E:\nabu\logs\l578ak2-ring.txt`、`E:\nabu\logs\rounds\l578ak2.json`、
  `E:\nabu\state\HANDOVER-20261110-l696.md` §7ag–§7ao、
  `E:\nabu\tmp\l331-src\UniXNU-nabu\package\KeyStoreProbe.cpp`、
  `E:\nabu\AGENTS.md`
