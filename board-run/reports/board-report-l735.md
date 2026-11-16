# l735 六问板报（2026-11-12，IONewData span 容量真实修复候选）

## 1. 唯一问题

把 `NativeAdrenoMemory::importTask()` 的最坏情况 `span_capacity` 从 `bytes/4096` 降到 `bytes/16384`，让 16 MB GOP host import 的 span 数组从 `4000 × 32 B = 128000 B` 降到 `1000 × 32 B = 32000 B`，能否消除 l734 板上已实证的 `IONewData` 失败，进而让 GPU handoff 越过 `P3` 并出现 `P4/PF>0 / ncpy<xfer`？

这是针对 l734 exact 根因的一次真实修复判定；不改 args、不新增探针、不重编整核。

## 2. 已闭合到哪

- l734 诊断轮用既有 `import_stage` 字段把 Mesa 折叠后的 `c4641cbd` 拆开：18 次 `import_stage=21 fail_pa=0`，无 `stage=20`。按 l734 补丁语义，exact 内核原因是 `IONewData(NativeAdrenoPhysicalSpan, storage_.span_capacity)` 返回 null，不是 WIMG `Unknown`。
- 源码 `NativeAdrenoMemory.cpp:708-713`：`firmware_physical=0` 时 `span_capacity=bytes/4096`；GOP `bytes=0xfa0000`，故 4000 个 span。`NativeAdrenoMemory.h:10-13` 的 `NativeAdrenoPhysicalSpan` 为 32 B，总请求 128000 B。
- XNU `IOLib.h:283,343-344`：`IONewData → IOMallocData(KHEAP_DATA_BUFFERS)`；arm64 `KHEAP_MAX_SIZE=32 KiB`，大尺寸走 `kalloc_large`/VM，在当前板上返回 null。精确 `kmem_alloc_guard` 子状态未知，但板上失败点已被 l734 钉死。
- `NativeAdrenoMemory.cpp:719-741`：逐页循环只在物理/WIMG 属性变化时新写一个 span；相邻同属性页会合并到上一个 span 的 `bytes`。GOP 是 `0x9c000000 + 0xfa0000` 的连续保留 framebuffer，`firmwareImportPhysical()` 的连续性检查也要求 `physical == video.v_baseAddr + offset`。因此 1000 槽位远大于实际所需段数。
- `NativeAdrenoMemory.cpp:252-253` 的 `IODeleteData(..., span_capacity)` 与分配容量同源；l735 改的是 `span_capacity` 本身，释放大小同步变为 32000 B，不会错配。
- 板上反汇编 `@0xfffffff007a5a3e8` 原 `lsr x8,x22,#12` 写 `span_capacity`，`@0xfffffff007a5a3f8` 用 `lsl x1,x8,#5` 计算分配字节。l735 只把前者改成 `lsr x8,x22,#14`，后者不变，得到 1000 × 32 = 32000 B。

## 3. 为什么离线不行

静态能证明 128000 B 分配失败、缩小容量同步且连续 GOP 合并后不会用满 1000 段，但不能证明真机 `kalloc` 在 32000 B 一定成功、后续 `BoMap`/SMMU/WIMG 是否还有阻断，以及 GPU handoff 是否真的接管搬帧。这些必须一次上板判定。

## 4. 失败也有信息

- `import_stage=19` 且 `P3 e0` 消失、`P4/PF>0`、`ncpy<xfer` ⇒ span 容量确为主阻断，GPU handoff 接管成功。
- `import_stage=19` 但仍 `P3 e0` / `PF=0` ⇒ 分配已过，后续 `vkGetMemoryHostPointerPropertiesEXT` / external features / NGP descriptor 另有墙，转内核 NGP 或 Mesa 路线。
- `import_stage=21` ⇒ 32000 B 仍失败，容量方向否证。
- `import_stage=20` ⇒ WIMG Unknown 暴露为下一阻断，按 l733/l734 结论补分类。
- panic / APFS / `buf_bread` / 花屏 / 锁屏退化 / WDT 异常 ⇒ 阴性回滚。

## 5. 唯一变量与回滚点

- 唯一变量：相对 `hold/l730`，kernel `@0xa563ea` **1 个字节** `0x4c→0x4e`，对应 `lsr x8,x22,#12 → #14`；kernel sha8 `d3598dd6→f07eed18`。
- 五文件其余产物：`gpu-addon.bin 950a3968`、`adt 78a6f7e9`、`trust a4a6bae6`、`boot ead4b531` 与 l730 相同；payload FAT 仅替换 `KERNEL~1.MAC`，args 700/39 逐 token 相同（added=[] dropped=[]）。
- `pet600`、220/300 双槽、APFS nop、migratx/migratiox/norce、WDT `arm=0`、锁屏链全部保留。
- 回滚点：`hold/l730`。l735 一次性，不进默认 args；后续启动前显式从选定包 `flash cust` 并最后重 arm misc。

## 6. 恢复预案

- 上板只走 `fastboot flash cust` → **最后** `fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin` → `fastboot boot <boot-merge-r3.img>`。
- `nabu-wdt-arm-delay=0`、`pet=600`；l732/l733/l734 均证实 `PON=0x80041` 自动回 TWRP。
- 若早 panic，WDT 已 arm；若 WDT 未触发，`fastboot devices` 应仍可用；否则才需人工长按。
- 不使用 `fastboot flash boot_a` / `flash boot`。

## 门禁状态

- `audit_bootability.py`：WSL `OK (0 fail, 0 warn)`。
- `audit_args.py`：700/39 → 700/39，added=[] dropped=[]，`OK`。
- `audit_dual.py`：11/11 match，`state/audit-l735.json` 已写。
- 本板报、`precheck.py`、`precheck_check.py`、`guard_no_boot_flash.py`、`board_report_check.py` 全 PASS 后才允许上板。
