# 当前状态（2026-10-23 07:27）

## 今晚的两条主线

### 一、QEMU 秒级验证环境（已建成）

本地 QEMU 能打印完整内核日志，一轮 40 秒。打通的四道关：

| # | 症状 | 根因 |
|---|---|---|
| 1 | 完全无日志 | `kprintf` 门是 `if (!disable_serial_output && !disable_kprintf_output)`，两个默认都是 true |
| 2 | 清了仍无日志 | `PE_init_kprintf` 在 `STARTUP(KPRINTF)`，早于 fb 映射，按就绪判断导致 hook 永不安装 |
| 3 | UART 也不输出 | `0x9000000` 是物理地址，未映射即 fault |
| 4 | Undefined Instruction | `cortex-a72` 不认 `MSR S3_0_C4_C1_4`，要 `-cpu max` |

QEMU 的边界：`ulock_initialize` 的 `zalloc_permanent` 走 `_zalloc_permanent_large`，
依赖 prelink/内存布局。

### 二、真机推进（关键突破）

**checkpoint 从 50 跳到 123**，真机第一次完整跑出内核日志：

```
corecrypto_kext_start completed successfully
ulock_initialize>thread_max=5120, ull_hash_buckets=2048
[0]ml_processor_register>pe:find_cpu(clusid=0)
SKYWALK: sk_copy64_8 ... PASSED     （32 项全过）
bsd_autoconf: calling kminit
- IOsurfaceRoot::installMemoryRegions()
- IOSurface disallowing global lookups
```

**决定性修复**：UART tee 在真机上是毒药。`0x9000000` 在 QEMU 是 PL011，在 SM8150 不是，
但 `ml_io_map_unmappable` 仍返回成功，第一个字符就写进真实硬件，内核崩在 KPRINTF 子系统。
现在按面板尺寸判断平台（QEMU 800x600，nabu 1600x2560），真机不映射。

## 以 sheng 为参照对齐（重要）

`sheng` 是官方维护、Display/Touch/UFS/GPU 全通的端口。对比结论：

| 项 | sheng | 我们（改前）| 我们（改后）|
|---|---|---|---|
| `[kernel] archives` | 19 | 7 | **9** |
| `build-libpthread-kernel.py` | ✅ | 未接 | ✅ **已接** |
| `build-mockfs-fixture.py` | ✅ | 未接 | ✅ **已接** |
| `com.apple.kec.pthread` 在 roots | ❌ 不列 | 列了 | ❌ **已移除** |
| roots 数量 | 31 | 20 | 19 |
| `packed_data_ramdisk` | true | 手动用了非 packed | 待对齐 |

**sheng 不 prelink `kec.pthread`，因为它用 `build-libpthread-kernel.py` 从源码提供符号。**
我们之前两者都做，反而重复注册。改后真机日志出现
`pthread kext extension loaded`（之前是 `not loaded`）。

**仍缺（在 sheng 仓库，本地没有）**：

| 脚本 | 提供 |
|---|---|
| `build-sheng-platform-power.py` | **PMGR 电源管理** |
| `build-boot-progress.py` | **官方诊断页** |

## 观测工具的教训

我们自己那套探针有三个坑，都修了：

1. `nabu_mark_val` 和 `nabu_mark_show` 画在同一坐标，后者永远覆盖前者
2. `draw_number` 把 >99 钳成 99，123 显示成 99
3. `draw_number` 清除 11*s 像素（s=20 时 220 px），从 x=32 清到 x=252，
   **正好切掉 x=220 的 within 计数** —— 屏幕上那个「57」是半清除的残影，不是读数

现在每个读数独占一格：`g_h-370` 最高 checkpoint、`g_h-320` 左 subsystem / 右条目数、
`g_h-430` 条目 ID。

## 下一步（按优先级）

| # | 动作 | 依据 |
|---|---|---|
| 1 | 从 sheng 移植 `build-sheng-platform-power.py` | 官方全通配置里它提供 PMGR |
| 1b | 从 sheng 移植 `build-boot-progress.py` | 诊断页能直接给 last PC / TCR / TTBR |
| 2 | 对齐 `packed_data_ramdisk = true` + `root_match_volume_uuid` | sheng 的 payload.adt |
| 3 | 确认 IOSurface 之后卡在哪（有干净读数后）| 待照片 |
| 4 | 决策：自研 surface vs 切 8796 | 长期 |
