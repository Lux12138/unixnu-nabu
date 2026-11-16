# 上板报告 l718（一次性临时功能轮：补 banner 调用时钟 `nabu-state-ring=10`）

生成时间：2026-11-11。性质：**一次性临时功能轮 / 可证伪诊断轮**，用户授权自动推进、
临时插帧、数据补丁，要求不停；用完回滚，**不进默认 args**。本会话只构建与门禁，
**未运行 adb/fastboot、未上板、未写 `E:\nabu\logs\rounds\l718.json`**。

## 0. 用户授权与性质（六问板报总纲）

- 授权：用户授权自动推进 / 临时插帧 / 数据补丁，要求不停；依据
  `E:\nabu\state\HANDOVER-20261110-l696.md` §7ag–§7ao、`E:\nabu\logs\rounds\l714.json`、
  `E:\nabu\logs\rounds\l717.json` 与 `E:\nabu\AGENTS.md` 0.5/0.6/0.7/0.9/0.10。
- 边界：**只用板上内核已有机制**；一次性临时轮、用完回滚、不进默认 args；
  **零新增 printf / IOLog / 计数器 / 探针、不改源码、不重编内核、不放宽安全策略、不动 boot_a**。
- **不是重复 AKS 盲试**：l717 已含 `offline-key-events=1` + `sheng-aks=1` + `sheng-mig=1`
  + 隐藏键 kernel，但 `NABUAKS1=0`。阴性原因已自证为「缺调用时钟」——
  `E:\nabu\kits\sheng\KeyStoreProbe.cpp:204` 的 `native_keystore_probe()` 只在
  `E:\nabu\drivers\ShengUfs\NativeUfs.cpp:3478` 的 `native_ufs_state_block()` 里调用；
  `nabuLogThread` 的 `nabu_log_state_snapshot()` → `serial_general.c:741
  native_ufs_state_block` 由 `nabu-state-ring=<秒>` 门控（floor 10 s）。
  l717 args **没有 `nabu-state-ring`** ⇒ 探针函数没被调用。本轮只补已证的调用时钟。

## 1. 唯一问题

**补 banner 时钟后，`native_keystore_probe()` 是否真正跑起来并发布 `AppleKeyStore`
替身（`NABUAKS1 st=3`），从而消掉 `Can't find AppleKeyStore service` /
`failed to open connection to AppleKeyStore` / `aks_get_lock_state failed: e00002bc`，
以及 `keychain-2.db Code=14` / `Migration failed` / 屏是否变化？**

唯一变量：以 `E:\nabu\hold\l717\` 为模板（六文件 kernel `d9b5a174`、adt `78a6f7e9`、
addon `950a3968`、trust `a4a6bae6`、boot `ead4b531`、payload `c1533813`；args 681/38），
只在 `unixnu.toml` 的 boot-args 末尾**加 `nabu-state-ring=10`**（19 字符含前导空格），
实测 **700/39 ≤700**（硬条件满足，无需缩短任何 token）。`added=['nabu-state-ring=10']`、
`dropped=[]`，其余 38 token 同序同字节。kernel 与其余五文件逐字节同 l717。

判据全为**官方已有输出、零新探针**：`NABUSTATE1` / `NABUAKS1 st=3`（RAM ring/CDC 视尾窗而定）、
os_log 三类 AKS 错误计数、`lg:tick`、`screen.bgrx` sha/直方图、frames、panic。
**即使 `Code=14` 仍在，只要 `st=3` 出现且锁状态错误消失，就是明确机制进展。**
`native_aks_lock_state()`（`KeyStoreProbe.cpp:36-45`）只回报 0..7 锁状态、不创建
`/Library/Keychains/keychain-2.db`；§7ao 已证 AKS 与 db 是两条独立链，故本轮**不承诺桌面**。

## 2. 已闭合到哪（六问之二，chain）

静态链每环带证据，不伪造闭环：

1. **最佳基线实测**：`E:\nabu\hold\l714\` kernel `d9b5a174` / payload `da7df504` /
   args 685/38；`E:\nabu\logs\rounds\l714.json` 与
   `E:\nabu\logs\l714-diag-tree\screen.bgrx`：PreBoard 白页 `e39720f2…`
   （1,021,128 白 + 少量黑灰）、frames `ncpy=3`、`finishedStartup:YES` / `Dismissing overlay`。
2. **l717 阴性实测**：`E:\nabu\logs\rounds\l717.json`：l717 = l714 +
   `offline-key-events=1` − `native-ufs-diagnostics=1`（payload `c1533813`、681/38），
   屏退回纯黑 `112b94b6…`、frames `ncpy=3` 仍在；`NABUAKS1` 0、
   `Can't find AppleKeyStore service` 1、`failed to open connection to AppleKeyStore` 5、
   `aks_get_lock_state failed: e00002bc` 2、`keychain-2.db Code=14` 23、
   `Migration failed` 9 ⇒ 阴性。
3. **阴性原因自证（本轮依据）**：`E:\nabu\logs\l717-cdc.bin` 实测
   `nabu-state-ring` / `NABUSTATE1` / `NABUAKS1` / `lg:tick` **全 0 命中**；
   `E:\nabu\logs\l714-cdc.bin` 同样 0 命中。等价源码
   `E:\nabu\hold\l586-touch\sources\NativeUfs.cpp:4092-4124`：`nabuLogThread` 解析
   `nabu-state-ring`（`cadence=0` 关闭，`<10` 抬到 10），每 2 s `IOSleep` + `lg:tick`，
   `cadence!=0 && elapsed>=cadence` 才 `nabu_log_state_snapshot()` →
   `serial_general.c:741 native_ufs_state_block` → `NativeUfs.cpp:3478
   native_keystore_probe()`。⇒ `NABUAKS1=0` 是**无调用时钟**，不是替身发布失败。
4. **已证的调用时钟**：`E:\nabu\logs\rounds\l578ak2.json` / `aks-l529.json` /
   `aks-l533.json` 都有 `NABUSTATE1` state_line；
   `E:\nabu\logs\l578ak2-ring.txt:805-806` 官方 CDC 行
   `NABUAKS1 st=3 rc=0`、`AKS opens=79 calls=354` 证同一机制带状态环时替身发布成功。
5. **机制源码**：`E:\nabu\tmp\l331-src\UniXNU-nabu\package\KeyStoreProbe.cpp:76-92`
   `native_keyevents_probe()` 首门要 `offline-key-events=1` 且
   `(ephemeral-storage==1 || native_aks_state_enabled() 读 sheng-aks=1)`；`:93-108`
   缺席判定 → `native_keyevents_allocate()` → `IOService` 发布 `AppleKeyStore` 替身；
   `:126-127` `native_keystore_probe()` 是唯一入口。
6. **板上已有可观测量**：`E:\nabu\hold\l718\kernel.macho` 实测字面量
   `nabu-state-ring` 1、`NABUSTATE1` 1、`NABUAKS1` 1、`native_keystore_probe` 2、
   `native_ufs_state_block` 2、`lg:tick` 1、`offline-key-events` 1、`sheng-aks` 2；
   官方格式串已编入，非新探针。
7. **待证传导（运行期）**：加 `nabu-state-ring=10` → 状态环每 10 s 发快照 →
   `native_ufs_state_block` → `native_keystore_probe` → 双 token 过门 → 替身注册
   （`NABUAKS1 st=3`）→ UserEventAgent/securityd 连上服务 → 三类错误归零或下降 →
   观察 `Code=14` / `Migration failed` / 屏 / frames / panic。

## 3. 为什么离线不行（六问之三，why_board / AGENTS.md 0.5）

静态能证：§2 全链、l714/l717 精确错误计数与白页/黑屏/frames、`l578ak2` 的 `st=3` 先例、
`nabu-state-ring` 门与 floor 10、`native_keystore_probe` 唯一调用点、内核字面量与官方格式串、
抽回 toml 逐 token 比、toml 外 inner 文件全 SAME、五文件逐字节同 l717、
关键开关全在、APFS nop 未动、`hold/l717` 六文件 sha 未变。

**不能静态得到的运行期量**：状态环是否真被调度（`lg:tick` / `NABUSTATE1`）、
IOKit 是否真走到 `stage=3` / `NABUAKS1 st=3`、UserEventAgent 是否还报
`Can't find AppleKeyStore service`、securityd 是否仍 `failed to open connection` /
`aks_get_lock_state failed`、`keychain-2.db Code=14` 与 `Migration failed` 计数、
`screen.bgrx` sha/直方图、`nabu-display: frames`、panic。
这些是线程调度时序、用户态连接时序与 IOKit 服务注册状态。l714 与 l717 都没跑过
「`d9b5a174` + 隐藏 BootUI + `sheng-mig=1` + 双 AKS token + 状态环」这一组合。
**日志与静态都无法产生该新状态 ⇒ 满足 AGENTS.md 0.5**（闭环已写，只为「验证静态结论
在真机上是否成立」的那一次判定）。

## 4. 失败也有信息（六问之四，三判读 + 观察项）

**判读 A（明确机制进展）**：`NABUSTATE1` / `lg:tick` 出现且 `NABUAKS1 st=3`，
`Can't find ...` / `failed to open connection` / `aks_get_lock_state failed` 归零或下降
（`Code=14` 可仍在）⇒ 证明 l717 缺的正是调用时钟，墙收窄到 sqlite db 创建链。

**判读 B（部分进展）**：`NABUSTATE1` 在、`st=3` 但三类错误不减 ⇒ 环已驱动探针、
替身已发布，但 UserEventAgent/securityd 未连上或走别的失败路径；
把墙收窄到服务匹配/权限，而非时钟或 token 门。

**判读 C（阴性）**：`NABUSTATE1` / `lg:tick` 在而 `NABUAKS1` 仍 0 ⇒
双 token 在 `d9b5a174` 上未触发 `native_keyevents_probe()`，机制假设被证伪；
不再重复空跑 AKS / 时钟组合。

三种结果都产生可判读的新事实，不是「再看看」。

**观察项（全官方已有输出、零新探针）**：CDC/ring `NABUSTATE1`、`NABUAKS1 st`、
`lg:tick`、`AKS opens/calls`；os_log `Can't find AppleKeyStore service` /
`failed to open connection to AppleKeyStore` / `aks_get_lock_state failed: e00002bc` /
`keychain-2.db` / `Unable to create database ... Code=14` / `Migration failed`；
本轮 `screen.bgrx` sha 与直方图；`nabu-display: frames`（xfer/nswap/ncpy/nret）；
panic / unencrypted panic / `buf_bread_err16`。

## 5. 唯一变量与回滚点（六问之五）

### 变量表（四行）

| 项 | 值 |
|---|---|
| 内核线 | `d9b5a174`（与 `hold/l717/kernel.macho` 逐字节相同，125,628,416 B；payload 内 `KERNEL~1MAC` 抽回亦同 sha） |
| args | 681/38 → **700/39 ≤700**；`added=['nabu-state-ring=10']`、`dropped=[]`；余量 0 字符 |
| 设备状态 | TWRP `f91fa664`，`boot_a` 未碰；本会话不上板 |
| 窗口长度 | `arm-delay=0`，咬点 380-410 s，完整 **4 分钟**窗口 |

### 包清单（`E:\nabu\hold\l718\` 六文件 sha256 前 8）

| 文件 | 大小 (B) | sha8 | vs `hold/l717` |
|---|---:|---|---|
| `boot-merge-r3.img` | 3,041,280 | `ead4b531` | SAME |
| `gpu-addon.bin` | 12,317,544 | `950a3968` | SAME |
| `kernel.macho` | 125,628,416 | `d9b5a174` | SAME |
| `kernel.macho.adt` | 211,568 | `78a6f7e9` | SAME |
| `owner-trust.bundle` | 61,456 | `a4a6bae6` | SAME |
| `payload.img` | 201,326,592 | `62749f18` | **DIFF**（只 `UNIXNU~1TOM`） |

payload 内 7 个 inner file：`KERNEL~1ADT` / `KERNEL~1GPU` / `KERNEL~1MAC` /
`KERNEL~1TRU` / `NABUESP` / `SCRATC~1IMG` 全 **SAME**，仅 `UNIXNU~1TOM`
CHANGED（1107→1126 B，目录项 size 同步 1107→1126）。

### args 预算（精确实测，别猜）

`E:\nabu\hold\l717\payload.img` 抽回实测 args 681 字符 / 38 token；
`nabu-state-ring=10` 18 B + 1 前导空格 = 19 B ⇒ **700 字符 / 39 token**，
正好等于硬上限 700，**未超 701，无需从已否证/纯诊断 token 里缩短任何 1 字符**。
38/38 存活 token 同序同字节；`added=['nabu-state-ring=10']`、`dropped=[]`。
若实测曾 >700，方案是从已否证/纯诊断 token 合法缩短并在此说明——本轮实测未触发。

**关键开关全在**：`offline-key-events=1`、`sheng-aks=1`、`sheng-mig=1`、
`sheng-fb-gpu=1`、`t733-managed-rt-normal=1`、`sheng-smp=1` +
`nabu-wdt-arm-delay=0`、`-apfs_shared_datavolume`、`offline-legacy-*`、
`nabu-fbcon=0`、`nabu-log=1`、`qcom-usb-*`。

### 构建方式与模板只读

中间新目录 `E:\nabu\tmp\l718-build-20261111T070529Z\`；最终包 `E:\nabu\hold\l718\`。
`hold/l717/` **只读**：输入 payload 以 `mmap ACCESS_READ` 打开，192 MiB 用
`shutil.copyfileobj` **流式**写到全新输出文件，绝不 mmap 输入 payload 可写；
`EFI/UNIXNU/UNIXNU~1TOM` 单簇 12688 **整簇替换**（slack 零填充），
目录项 size @804028 同步 1107→1126。
构建脚本：`E:\nabu\tmp\l718-build-20261111T070529Z\build_l718.py`、
`assemble_l718.py`；日志 `build.log`、`assemble.log`。

**`hold/l717/` 未污染**：完成后复测六文件 sha8 仍为
`ead4b531 / 950a3968 / d9b5a174 / 78a6f7e9 / a4a6bae6 / c1533813`
（payload 未变；前后 sha 文件逐行 diff 为空）。

### 回滚

一次性临时轮、用完回滚、不进默认 args。回滚包：
`E:\nabu\hold\l700\`（kernel `efcb4c34`）+ `E:\nabu\hold\l717\`（payload `c1533813`）；
**最佳画面回滚 `E:\nabu\hold\l714\`**（kernel `d9b5a174`、payload `da7df504`，
PreBoard 白页 `e39720f2…`）。回滚只需把六文件换回，不改源码、不重编、不动 boot_a。

## 6. 恢复预案（六问之六，AGENTS.md 0.10 / 0.7 / 0.9 / 附注）

**`AGENTS.md` 0.10 绝对红线：不碰 `boot_a`**（绝不 `fastboot flash boot` /
`flash boot_a`；这块板没有独立 recovery 分区，boot_a 里是 TWRP）。
流程（**仅文字，本会话不执行**；顺序不可换）：

```
recovery arm misc                                             # 不碰 boot_a
flash cust hold/l718/payload.img                              # 先刷 cust
fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin   # 最后才重 arm misc
fastboot boot hold/l718/boot-merge-r3.img                     # 临时加载，不写任何分区
```

`E:\nabu\dwc3\build\misc-bootrecovery.bin`（4096 B）；设备 TWRP recovery `f91fa664`。
**`arm-delay=0`**（`sheng-smp=1` 硬性配对，`AGENTS.md:57-64`），咬点
**380-410 s**，观察完整 **4 分钟窗口**，早 panic 也有重置路径；
若 fastboot 全失败：**人工长按电源 10 秒**。回滚见 §5。

### 门禁（本会话实测，全部 PASS）

| 项 | 结果 | 证据（绝对路径） |
|---|---|---|
| ① 构建 + 模板只读 | 新 tmp 目录 / 新 `hold/l718/`；五文件逐字节同 l717；payload 只 `UNIXNU~1TOM` 变；整簇替换 + 目录项 size；`ACCESS_READ` / 192 MiB 流式；l717 六文件 sha 前后逐行 diff 为空（payload `c1533813`） | `E:\nabu\tmp\l718-build-20261111T070529Z\build_l718.py`、`assemble_l718.py`、`build.log`、`assemble.log`、`l717-before-sha256.txt`、`l717-after-sha256.txt` |
| ② `fsck.fat -n` 10 files + `audit_bootability.py hold/l718 hold/l717 --expected=d9b5a17405b2aaac767eeddd82f3914b7e753bb2753262032ccb045d740246e3` | l717/l718 均 `10 files, 67550/97913 clusters` rc=0；→ **`OK (0 fail, 0 warn)`** RC=0（PRELINK 47/47、ncmds=5776、payload 体积不变、kernel sha 匹配） | `E:\nabu\tmp\l718-build-20261111T070529Z\gate1-fsck.txt`、`gate1-bootability.txt`、`gate1-run.log` |
| ③ `audit_dual.py --tag l718` | 逗号分隔真实 sha `kernel=d9b5a174,payload=62749f18,adt=78a6f7e9,addon=950a3968,trust=a4a6bae6,boot=ead4b531,args=700/39` → **`7 items compared, 7 match, 0 differ`** | `E:\nabu\tmp\l718-build-20261111T070529Z\gate2-audit_dual.txt`、`claim.txt`、`measured-values.txt`、`E:\nabu\state\audit-l718.json` |
| ④ `tmp/audit_args.py l717 l718` | `681/38 → 700/39`（≤700）；**`added=['nabu-state-ring=10']`、`dropped=[]`** → **`RESULT: OK`** rc=0；其余 38 token 同序同字节、关键开关全在 | `E:\nabu\tmp\l718-build-20261111T070529Z\gate3-audit_args.txt`、`gate4-precheck.txt` |
| ⑤ Windows Python 3.12.10 `precheck.py` | `E:\nabu\tmp\l718-build-20261111T070529Z\run_precheck_l718.py` 驱动 `--pkg E:/nabu/hold/l718 --baseline E:/nabu/hold/l717`，声明 added，chain/why_board 用「补调用时钟」判据 → `stamp written: E:/nabu/state/precheck.json` rc=0；八字段齐、`token_diff=ok`；`precheck_check.py` → **`[gate] OK`** rc=0，`dual audit OK: audit-l718.json (7 items)` | `E:\nabu\tmp\l718-build-20261111T070529Z\gate4-precheck.txt`、`gate4-precheck_check.txt`、`gate4-run.log`、`E:\nabu\state\precheck.json` |
| ⑥ 六问板报 | 本文件；`E:\nabu\tmp\board_report_check.py l718` → `[report] OK` 6/6 节 | `E:\nabu\state\board-report-l718.md`、`E:\nabu\tmp\l718-build-20261111T070529Z\gate6-board-report.txt` |
| 自检 | 抽回 toml 逐 token 比（38/38 同序同字节、700/39 ≤700、added/dropped 与声明一致）；抽回 kernel == l717 kernel `d9b5a174`；toml 外其余 inner 文件与 l717 全 SAME；六文件 sha8 见 §5；`hold/l717/` 未污染 | `E:\nabu\tmp\l718-build-20261111T070529Z\selfcheck.txt`、`l718-sha8.txt`、`assemble.log` |

**零新增 printf/IOLog/计数器/探针、零重编、零源码改动、零安全策略放宽、
零 boot_a 触碰**；kernel 仍是 l714 同长数据补丁（含 `BKHideAppleLogoOnLaunch=true`，
文件长 125628416 不变），**APFS nop `@0x6cd2f68 = 1f2003d5` 未动**；
本轮未运行 adb/fastboot、未上板、未写 `E:\nabu\logs\rounds\l718.json`。

**fc76 未消费与双审计偏离（照 l717 注明）**：编译侧 fc76 自 **11-06 14:57** 未消费；
`audit_dual.py --tag l718` 的 `--peer` / `--mine` **两侧均为板侧实测**
（7/7 equal，真实 sha 见 §5），照 `E:\nabu\state\board-report-l717.md` §6 与
`E:\nabu\state\board-report-l714.md:191-195` 注明：偏离 `AGENTS.md:36-47` 0.6 的
「编译侧 vs 板侧」原意；本轮零重编、无编译侧成果可比对。

## 7. 证据路径（全部绝对路径）

- 构建：`E:\nabu\tmp\l718-build-20261111T070529Z\`（`build_l718.py`、`assemble_l718.py`、
  `build.log`、`assemble.log`、`l717-before-sha256.txt`、`l717-after-sha256.txt`、
  `l718-sha8.txt`、`selfcheck.txt`）
- 本轮门禁：`E:\nabu\tmp\l718-build-20261111T070529Z\`（`run_gate1.py`、`gate1-*.txt`、
  `gate1-run.log`、`gate2-audit_dual.txt`、`claim.txt`、`measured-values.txt`、
  `gate3-audit_args.txt`、`run_precheck_l718.py`、`gate4-*.txt`、`gate6-board-report.txt`）
- 包：`E:\nabu\hold\l718\`；只读模板：`E:\nabu\hold\l717\`；
  回滚：`E:\nabu\hold\l700\` / `E:\nabu\hold\l717\`，最佳画面 `E:\nabu\hold\l714\`
- 状态戳：`E:\nabu\state\audit-l718.json`、`E:\nabu\state\precheck.json`、
  `E:\nabu\state\board-report-l718.md`
- 判据来源：`E:\nabu\logs\rounds\l714.json`、`E:\nabu\logs\rounds\l717.json`、
  `E:\nabu\logs\l717-cdc.bin`、`E:\nabu\logs\l717-v1.jsonl`、
  `E:\nabu\logs\l714-cdc.bin`、`E:\nabu\logs\l714-diag-tree\screen.bgrx`、
  `E:\nabu\logs\rounds\l578ak2.json` / `aks-l529.json` / `aks-l533.json`、
  `E:\nabu\logs\l578ak2-ring.txt`、
  `E:\nabu\state\HANDOVER-20261110-l696.md` §7ag–§7ao、
  `E:\nabu\kits\sheng\KeyStoreProbe.cpp`、`E:\nabu\drivers\ShengUfs\NativeUfs.cpp`、
  `E:\nabu\hold\l586-touch\sources\NativeUfs.cpp:4092-4124`、
  `E:\nabu\tmp\l331-src\UniXNU-nabu\package\KeyStoreProbe.cpp`、
  `E:\nabu\AGENTS.md`
