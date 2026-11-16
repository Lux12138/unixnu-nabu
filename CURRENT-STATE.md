# 当前状态 —— nabu 线（2026-11-16）

> 本文取代此前所有「当前状态 / 下一步」段落。与旧文档冲突时**以本文为准**。
> 本文记录 2026-10-28 → 2026-11-16 这 19 天的成果。

## 0. 一句话

**目标已完成：iPadOS 17.7.11 在小米平板 5（nabu / SM8150）上开机，到达原生桌面（l769）。**
剩余工作全部是**流畅度**，不是功能性开机。

| 目标层 | 状态 | 关键轮次 |
|---|---|---|
| 内核启动 → 用户态 | 完成 100% | l730 之前 |
| 系统 UI（锁屏） | 完成 100% | l730 / l750 |
| **桌面（Home Screen）** | **完成 100%** | **l769**（ToHome + UI Locked:NO） |
| 触摸 / 手势 | 完成 100% | l752–l754 |
| GPU scanout 接管 | 功能闭合 | l770（P3 破除）→ l771（WAIT_SWAP 生效） |
| **流畅度（帧率/延迟）** | **0%** | l771 之后：present 221–844 ms 长尾 |

## 1. 显示链的三次质变

### 1.1 l730：交还系统 UI（锁屏）

最后一道**开机墙**不是 GPU、不是 keychain、不是 WDT，而是：

> 内核自己把 `com.apple.migration` CFPreferences 域里的 365 B 官方完成态，
> 用 `sheng-mig=1` 的 264 B 强制态覆盖了，导致 `lastBuildVersionPref=(null)`
> 而每轮强制重跑数据迁移，SpringBoard 永不 `finishedStartup`。

l730 只改 **67 文件字节**：保留 365 B 完成态，把强制态写去无害路径
`com.apple.migratiox.plist`。结果：

```
(null) / Migration necessary   : 0
Migration not necessary        : 11
finishedStartup:YES            : 2
Dismissing overlay             : 2
screen.bgrx                    : ff868216 全屏锁屏 53856 色
```

包：`kernel d3598dd6` / `payload 3876e1a2` / args `700/39`。

### 1.2 l738 / l750：DPU 直出

`NativeLegacyDisplay::startDirect()` 写 VIG0（`0xae05000`）已被证实可行。
l737 的 `data abort (esr 0x96000010)` 是探针地址写错（`0xae004000`，
比真 VIG0 低 0x1000），不是 DPU 不可用。

纯数据修复即可打开：

```
native_ufs_dpu_state = 0x80000000   (bit31)
mode = 2
```

l750 用 `9011f295` 内核画出完整原生 iPadOS 锁屏。
**注意：`ncpy=0` 不等于零拷贝，该判读已撤回。**

### 1.3 l769：桌面达成

决定性证据链：

| 事实 | 证据 |
|---|---|
| SpringBoard 开机固定走 `SBStartupTransitionToLockOut` | l756 逆向 |
| 硬编码 `Boot{ForceLockKey:1}` 是真凶 | l756 |
| `SBDontLockEver` 与各类旁路全否证 | l756 |
| `SBDontLockAfterCrash` 改为 `startup destination=ToHome` | **l769 上板** |
| 结果 `ToHome` / `UI Locked:NO` / 无 `ForceLockKey` | l769 CDC |
| 主屏 PNG：彩色壁纸 + App Store + Settings + Dock | l769 seq1 screen |
| 防陈旧校验 PASS，设备回 TWRP | l769 |

**桌面目标 100% 达成。流畅度仍 0%。**

## 2. GPU scanout 接管（性能前置）

### 2.1 第一道墙：client 竞争（l770 破除）

`P3 ee00002c2` 的根因**不是** VM/cache/WIMG，而是：

> l750/l769 独有的内核 `nabuScanoutClient` 抢先用掉 `WAIT_SWAP`，
> 把 handoff 的独占 `client_` 占住了。

最小修复：`0xa7c6d0` 处 **4 字节 NOP**。结果：

```
P3 ee00002c2  ->  P3 e0        第一道墙破
桌面 PNG / ToHome 不回归        无副作用
```

### 2.2 第二道墙：no-copy import（l771 破除）

`HostMemory / NotRepresentable` 拒绝 import。把 GOP carveout 的 WIMG
`Unknown -> Cached`（4 字节）后该墙消失：

```
P4 / PF 首次出现
GPU 服务 6 帧
桌面不回归
```

### 2.3 剩下的墙：延迟

| 指标 | 实测 |
|---|---|
| commit -> complete | **1.30 s** |
| present 均值 | **221 ms** |
| present 尾帧 | **844 ms** |
| 触摸稳态数据 | 尚无（禁止把 P4/PF 当成流畅达成） |

启动期 pipeline compile 与长尾共现，因果未分离。**继续 NO-GO。**

## 3. 内核补丁序列（patches/xnu）

现行 `series` 20 行，关键补丁：

| 补丁 | 作用 |
|---|---|
| `0001-nabu-fbcon` | 帧缓冲控制台 |
| `0002-nabu-unverified-root` | 未验证根卷 |
| `0005–0007-nabu-usbdebug-*` | DWC3 CDC 调试通道 |
| `0009-nabu-data-mkdir` | `/private/var` 目录骨架 |
| `0010-nabu-oslog-export` | os_log 导出 |
| `0011-nabu-corpse-limit` | corpse 限流 |
| `0012-nabu-data-dirmode` | installd 33:33 / Keychains 64 属主 |
| `0013-nabu-io-throttle-off` | 关 I/O 节流 |
| `0014-nabu-spawn-timing` | spawn 时序 |
| `0015-nabu-sysevent-payload` | sysevent 载荷 |
| `0016-nabu-smp-8core` | **8 核 SMP 启动** |
| `0017-nabu-addon-diag` | GPU addon 诊断 |
| `sheng-platform-power` | 平台电源 |
| `zzz-boot-progress` / `zzzb-nabu-keep-kprintf-putc` | 引导进度 / 保留 kprintf |

## 4. 驱动清单（drivers/）

| 驱动 | 文件数 | 作用 |
|---|---:|---|
| `NabuAdreno` | 101 | Adreno 640 GPU、SMMU、PM4、BO 管理、scanout |
| `NativeUfs` | 33 | UFS 存储、诊断导出、WDT pet、DPU 探针 |
| `GopFramebuffer` | 12 | GOP 呈现、SMMU mapper、legacy display 合成 |
| `ArmCpu` | 3 | 8 核 SMP 启动、PSCI、GIC |
| `ShengNovatekTouch` | 4 | NT36523 触摸协议 |
| `QcomGeniSpi` | 2 | GENI SPI 控制器 |
| `AppleKeyStoreState` | 1 | KeyStore 事件替身 |

历史快照（`.bak` / `pre-l*` / `.v*` / `.damaged`）已排除，只保留现行源码。

## 5. 板侧实验记录（board-run/）

从板侧同步的完整实验台账，可直接追溯每一轮：

| 目录 | 内容 | 数量 |
|---|---|---:|
| `board-run/reports/` | 六问板报（唯一问题 / 已闭合到哪 / 为什么离线不行 / 失败也有信息 / 唯一变量与回滚点 / 恢复预案） | **73** |
| `board-run/rounds/` | 结构化轮次判读 JSON | **135** |
| `board-run/state/HANDOVER-20261110-l696.md` | 主交接文档（3641 行） | 1 |
| `board-run/state/AGENTS-board-side.md` | 板侧作业铁律 | 1 |

板报覆盖到 **l771**；轮次 JSON 板侧本身只写到 l738（之后改用板报形式）。

## 6. 红线与教训（必须遵守）

1. **绝不 `fastboot flash boot_a` / `flash boot`** —— recovery 在 boot_a 里，写了会丢 TWRP。
2. **禁止任何 `erase`** —— 2026-11-13 `erase misc`/`vbmeta` 差点变砖。
   恢复只许用户点名备份；boot 前必须查 slot-unbootable。
3. **不新增自造探针** —— 禁 printf/IOLog/计数器/采样/once-only，只用内核已有官方输出。
4. **args ≤ 700 字符**；`sheng-smp=1` 必配 `nabu-wdt-arm-delay=0`。
5. **别 mmap 输入 payload 可写** —— 曾污染 l695p。
6. **同路径构建** —— 绝对路径字面量会移动 `__cstring`，204 字节固定偏移 ledger 会失效。
   必须在 bind mount namespace 内让构建看到与基准相同的路径。
7. `git push` 存在「exit=0 但远端未变」的坑，见 `PUSH-GOTCHA.md`。

## 7. 下一步（按判别力排序）

1. **分离 present 长尾的因果**：pipeline compile 与 844 ms 尾帧共现，
   需拿到无触摸稳态数据，才能判断是 GPU 频率、GOP UC 映射还是 CA 提交节奏。
2. **8 核 SMP 的实际调度均衡**：`SMPTBL8` 已证 8 个槽 `st=7 running`，
   但用户态 `hw.ncpu` 与逐核负载未测。
3. **桌面持久化**：当前是临时轮（一次性包），未固化到默认 args / 编译侧。

## 8. 历史文档说明

`docs/history/` 下的 `PLAN.md` / `FLOW.md` / `PREDICTIONS.md` /
`HANDOVER-2026-10-24.md` / `HANDOVER-2026-10-25.md` / `HANDOVER-legacy.md` /
`STATUS.md` 均为**重写之前**（2026-10-2x）的记录，只作追溯。
其中「AppleCLCD 是显示真身」「靠 UFS 窗口读日志」「panic 路径 PSCI 复位」
等说法均已被否证，以本文为准。

根目录的 [`STATUS.md`](STATUS.md) 是 2026-11-16 重写的**短版仪表盘**，
与本文同源、不冲突；两者若有出入，仍以本文为准。
