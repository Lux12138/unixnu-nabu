# 上板报告 l703（唯一变量：args 换成内核已有官方通道 nabu-gpu-observe=1）

> 本报告是**上板前**门禁文件；本会话未运行 adb/fastboot、未上板、未改 `hold/l702/`（只读模板）、未写 `logs/rounds/l703.json`。
> 按 `E:\nabu\AGENTS.md` 0.5 / 0.6 / 0.7 / 0.9 / 0.10 编写；格式照 `state/board-report-l702.md`。

## 1. 唯一问题（一句话）

**`publish::requested()` 的 4 字节路径修复（l702）之后，只把 args 里 `nabu-gpu-abi=1` 换成内核已有的官方通道 `nabu-gpu-observe=1`，GPU ready +120 s 的 `NabuGPU: observation=4/4` 采样里 stage-1 自测特征（`ext_entries=266` / `ext_selector=0` / `metal_calls=10`）是否消失？**

- 消失 ⇒ published 分支已改道，断点在 `deviceForPort()` 之后；
- 仍是 266/10 ⇒ `publish::requested()` 仍为 false（或 4 字节修复未生效）。

其余一切不再问：**不新增/删除别的 token、不加 `nabu-autolog`、不加探针、不重编内核、不放宽安全策略**。`nabu-gpu-observe=1` 是 shipped kernel 里已编入的官方采样开关，不是新造探针。

## 2. 已闭合到哪（日志优先 + 静态链）

**为什么这一轮 / 传导链（同时写进 `state/precheck.json` 的 `chain` / `why_board` 与本包 §6）**：

1. **l702 实证**：`logs/l702-v1.jsonl` 121436 行、`logs/l702-screen.bgrx.bin` 与 l701 逐字节相同（`logs/l702-screen-stats.txt`：sha `ca18b728…`，与 `l701-diag-tree/screen.bgrx` cmp OK，10 255 非零像素仍只是 iBoot Apple Logo 残留）⇒ 4 字节 `publish::requested()` 路径修复后**仍无 UI**。
2. 但**新版插件确在 backboardd 内映射**：新版 uuidtext `4C4C441955553144A1D6E9E40FE81188` 在 `logs/l702-v1.jsonl:60238-60243` 有 **6 行** `native-metal: no device answer`（l701 为 **0** 行）。这就是"插件 exe 已运行"的直接证据。
3. `publish::requested()` 是否读到 `true`，**成功路径静默、现有 os_log 区分不了**；离线也没有任何能读出该布尔值的通道（`state/board-report-l702.md:23,27`）。
4. **内核已有官方读数通道**：`hold/l702/kernel.macho` 内三个静态字符串均在 —— `NabuGPU: observation=` @1611828、`nabu-gpu-observe` @1606509、`NabuGPU: Metal observations enabled` @1610504。`upstream/gpu/a640/NativeAdrenoService.cpp:269-308` 定义 GPU ready 后 **+0/+10/+30/+120 s** 共 4 次采样，输出 `metal_opens` / `ext_entries` / `metal_calls` / `metal_method` 累计计数（`logs/rounds/l699.json:15` 指出开关在 `:1310`、`A640State::poll():1348`）。
5. **判据基线**：l699（旧 exe）第 **4/4** 次采样是 `ext_entries=266 ext_selector=0 metal_calls=10`（`logs/rounds/l699.json:20,38-45`），对应 stage-1 的 256 selector probe + 10 tunneled ABI。l702 未开 `nabu-gpu-observe`，所以 l702 的 CDC 里**预期 0 条 observation**，不是"没采到=没发生"。

**已排除/冻结**：sandbox/MACF deny 作为最终墙已撤回（shipped kernel 的 `zzw/zzwb` grant 短路两道准入）；union / 8 条目 addon / trust+seal / 背光 / display 帧全退役等离线闭合（`state/board-report-l702.md:23`）；本轮不再改 exe、不再重签、不再动内核。

## 3. 为什么离线不行（AGENTS.md 0.5 `--why_board`）

静态能证：三字符串偏移与采样时间表（`NativeAdrenoService.cpp:269-308`）、`nabu-gpu-observe=1` 的开关语义、l699 的 4/4 基线、以及本包只改一个 token。但 `metal_opens` / `ext_entries` / `metal_calls` / `metal_method` 是 `A640State::poll()` 在 GPU ready 后**运行期累加**的计数，随本轮 publish 分支是否改道而变，**静态只能证通道存在，只有上板能读**。现有 os_log 成功路径静默、l702 无 observation 行 ⇒ 满足 0.5；只需**一次**板上判定。

## 4. 失败也有信息（两种结果各自的判读）

- **+120 s 的 4/4 采样里 266/10 这组 stage-1 特征消失**（`ext_entries` 显著 <266 或 `metal_calls` 不落 10、`ext_selector` 不再 0）：`publish::requested()` 已读到 true，`initWithAcceleratorPort:` 改走 `publish::deviceForPort()`，stage-1 detached 自测没再跑 ⇒ **断点前移到 `deviceForPort()` 之后**（MTLDevice 注册 / CAContext / 首帧提交），下一轮只需盯这几环。
- **仍是 `ext_entries=266` / `ext_selector=0` / `metal_calls=10`**：`publish::requested()` 仍为 false（或 l702 的 4 字节修复在真机上未生效）；与"插件已在 backboardd 内映射但无 UI"完全自洽 ⇒ **断点仍在 requested() 或其 `Info.plist` 读取**，不再重复试 published 侧。
- 两种结果都给出可判读的新事实，不是"再看看"。附带：CDC 若 **4/4 全无**（`NabuGPU: Metal observations enabled` 也未出现）⇒ 开关未按预期进内核，属包/装载问题，须先对账 `unixnu.toml`，不得据此判 GPU 结论。

## 5. 唯一变量与回滚点

**唯一变量 = args 两 token 等义替换**：`nabu-gpu-abi=1`（14 B）→ `nabu-gpu-observe=1`（18 B），**非等长**，args 长度 **694 → 698** 字符（实测，`≤700`），仍 **38 token**；`tag=l700` 保持、`sheng-smp=1` 仍配 `nabu-wdt-arm-delay=0`，无 `nabu-autolog`、无其它增删（`added=['nabu-gpu-observe=1']`、`dropped=['nabu-gpu-abi=1']`，**不是零增删**）。
手术方式：`tmp/l703-build-20261110/grow_l703.py` 以 **只读 mmap ACCESS_READ** 打开 l702 模板（绝不写输入，避免 l695p 污染，`state/HANDOVER-20261110-l696.md:1166-1168`），在 2048 B FAT 簇内做**整簇替换**（toml 单簇 12688，`1120 → 1124` B、slack 全零、FAT 链不变）+ **目录项 size 更新**（cluster 4 @160+28：`1120 → 1124`），写新文件到新目录；l702 六文件 sha 前后未变。

| 文件 | l702 sha8 | **l703 sha8** | 说明 |
|---|---|---|---|
| boot-merge-r3.img | `ead4b531` | `ead4b531` | 逐字节相同 |
| kernel.macho | `fb3ce142` | `fb3ce142` | 逐字节相同（未重编） |
| kernel.macho.adt | `78a6f7e9` | `78a6f7e9` | 逐字节相同 |
| gpu-addon.bin | `950a3968` | `950a3968` | 逐字节相同（l702 新版 exe/重签不动） |
| owner-trust.bundle | `a4a6bae6` | `a4a6bae6` | 逐字节相同 |
| payload.img | `b91cfe98` | **`96da4263`** | 唯一变化；包内 7 条中仅 `UNIXNU~1TOM` 变（`24ebae84` → `f1d61f79`、1120→1124 B） |

payload 内逐字节复核：`KERNEL~1MAC fb3ce142` / `KERNEL~1ADT 78a6f7e9` / `KERNEL~1GPU 950a3968` / `KERNEL~1TRU a4a6bae6` / `SCRATC~1IMG 3cd3d0d3` 与 **l702 全同**，仅 toml 变（抽回件在 `tmp/l703-build-20261110/extract-l703/`）。

- **回滚基线**：`hold/l700/`（kernel `efcb4c34`，已验证 4 分钟窗口）；**上一包** `hold/l702/`（kernel `fb3ce142`、payload `b91cfe98`，同窗可用，本会话只读未污染）。
- **回滚命令（仅文字，不执行）**：`fastboot flash cust hold/l700/payload.img`。

## 6. 恢复预案 / 观察项 / 门禁

**AGENTS.md 0.10 绝对红线：不碰 `boot_a`（绝不 `fastboot flash boot` / `flash boot_a`）**。流程（仅文字）：

```
recovery arm misc
fastboot flash cust hold/l703/payload.img
fastboot flash misc misc-bootrecovery.bin   # flash 后重 arm
fastboot boot hold/l703/boot-merge-r3.img  # 临时加载，不写任何分区
```

`misc-bootrecovery.bin` 在 `E:\nabu\dwc3\build\misc-bootrecovery.bin`（4096 B）。设备 `TWRP recovery f91fa664`。`arm-delay=0`，咬点 **380–410 s**，观察完整 **4 分钟窗口**（`AGENTS.md:57-64`），覆盖 +120 s 的 4/4 采样；早 panic 也有重置路径。若 fastboot 全失败：人工长按电源 10 秒。回滚命令见 §5。

**观察项（全部是官方已有输出，零新增 printf/IOLog/计数器/采样/探针）**：
- CDC `NabuGPU: observation=4/4`、`NabuGPU: metal_opens=`、`NabuGPU: metal_calls=… ext_entries=… ext_selector=…`、`NabuGPU: metal_method=`，**重点第 4/4 次（+120 s）的 266/0/10 是否消失**；
- CDC `nabu-gpu-addon:` 官方 mounted/target/control/status 行、`nabu-addon: files/wr/fail/err`、`nabu-display: frames xfer/nswap/nsurf/nok`，以及 panic；
- os_log 新 uuid `4C4C4419…` 的 `native-metal: no device answer`（l702 为 6 行；与 l703 对照），以及 `PUB`/`ST`/`CAContext`/`CALayer`；
- 屏 `screen.bgrx`（比照 `logs/l702-screen.bgrx.bin` 命名保存，本会话不采）。

**门禁（本会话实测，全部 PASS；证据见 §7）**：

| 项 | 结果 |
|---|---|
| ① `fsck.fat -n` 双侧 | l702/l703 均 `10 files, 67550/97913 clusters` rc=0；`python3 -I tmp/audit_bootability.py hold/l703 hold/l702 --expected=fb3ce142…` → **`OK (0 fail, 0 warn)`**（四关键文件全 same、Mach-O `flags=0x0 ncmds=5776`、PRELINK 47/47、payload 201326592 同）；包外五文件 cmp 逐字节同，payload 内 kernel/adt/initrd/addon/trust 逐字节同，**仅 toml 变** |
| ② `audit_dual.py --tag l703` | **逗号**分隔、真实 sha：`kernel=fb3ce142,payload=96da4263,adt=78a6f7e9,addon=950a3968,trust=a4a6bae6,boot=ead4b531,args=698/38` → `7 items compared, 7 match, 0 differ`；`state/audit-l703.json` `equal: true` / 7 items |
| ③ `audit_args.py E:/nabu/hold/l702 E:/nabu/hold/l703` | `694/38` → `698/38`（≤700）；`added=['nabu-gpu-observe=1']`、`dropped=['nabu-gpu-abi=1']`；带 `--expect-added/--expect-dropped` 时 **`RESULT: OK`**（不带声明时默认期望空集合，故 FAIL，不是包的问题、不得写成"零增删"） |
| ④ Windows Python 3.12.10 `precheck.py` | `tmp/l703-build-20261110/run_precheck_l703.py` 驱动 `python -I tmp/precheck.py --pkg E:/nabu/hold/l703 --baseline E:/nabu/hold/l702 --expect-added … --expect-dropped …`（硬编码 `E:/nabu/tmp` 只在 Windows Python 下可用）→ `stamp written: E:/nabu/state/precheck.json`；`question`/`chain`/`why_board`/`window`/`kernel`/`args`/`device` 全非空，`window` = `arm-delay=0，咬点 380-410 s，观察 4 分钟窗口`，`device` = `TWRP recovery f91fa664`；`python -I tmp/precheck_check.py` → `[gate] dual audit OK: audit-l703.json (7 items)` / **`[gate] OK`** rc=0 |
| ⑤ 六问板报 | 本文件 `state/board-report-l703.md` |
| 自检 | payload 抽回 toml 逐 token 只差两 token；抽回 kernel/addon/trust/adt 与 l702 `cmp` SAME；`hold/l702/` 六文件 sha 前后未变 |

## 7. 证据路径

- 构建脚本：`tmp/l703-build-20261110/grow_l703.py`（只读输入）、`tmp/l703-build-20261110/run_precheck_l703.py`
- 中间产物：`tmp/l703-build-20261110/payload.img`、`tmp/l703-build-20261110/extract-l703/UNIXNU~1TOM.bin`（1124 B `f1d61f79`）、`KERNEL~1MAC.bin` / `KERNEL~1GPU.bin` / `KERNEL~1TRU.bin` / `KERNEL~1ADT.bin`
- 门禁戳：`state/audit-l703.json`、`state/precheck.json`、`state/board-report-l703.md`
- l702 证据：`logs/l702-v1.jsonl`（121436 行，`:60238-60243` 6 行 `native-metal: no device answer`）、`logs/l702-screen.bgrx.bin` / `logs/l702-screen-stats.txt`
- 官方通道：`upstream/gpu/a640/NativeAdrenoService.cpp:269-308`、`hold/l702/kernel.macho` 三字符串；基线 `logs/rounds/l699.json:20,38-45`
- 参考：`state/board-report-l702.md`、`state/HANDOVER-20261110-l696.md:1164-1177`（l695p 教训、Windows Python、逗号分隔）

**注**：编译侧 fc76 自 **11-06 14:57** 未消费，本轮板侧自行出包；双审计两侧均为板侧实测（偏离 `AGENTS.md:36-47` 0.6，照 `state/board-report-l702.md:86`、`state/board-report-l701.md:109-110` 注明）。本轮**零新探针**（`nabu-gpu-observe` 是内核已有官方通道）、**零重编内核**、**零安全策略放宽**。

## 8. 下一步（仅建议，本会话不执行）

由用户上板后自行写 `logs/rounds/l703.json`；先按 §6 读 +120 s 的 4/4 采样判读，再决定是否继续。若判定仍为否定，按 §4 收窄断点，**不连续两轮问同一问题**（`AGENTS.md:130-134`）。
