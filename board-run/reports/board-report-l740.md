# board-report l740（import stage17 非确定性修复轮）

## 1. 唯一问题
把 `QueryPageCacheMode` 的范围外页从 `Unknown` 改为 `NormalUncached`（+ Unknown 出口补 fail_pa 诊断）后，import 是否稳定到 `stage=19`（每轮），进而让 CA 有合成帧、swap 提交（nval>0）、DPU 直出上屏（d0>0）？

## 2. 已闭合到哪
- l739/l739b CDC：`import_stage=17 waits=23 total_us=42ms`（part1 同参数到 19、l739b 停 17）⇒ **非确定性**；stage17 后唯一升 19 前的失败出口 = `QueryPageCacheMode` 返回 Unknown（NativeAdrenoMemory.cpp:735）+ firmware 分支 IONewData 失败。
- Unknown 三来源（NativeAdrenoCpu.cpp:31-55）：① 物理页在 `gPhysBase/gPhysSize`（内核 DRAM 窗）之外——carveout/固件保留区（GOP/GPU 区）；② `VM_MEM_RT` 且无 t733 接管（args 有 t733-managed-rt-normal=1 ⇒ 已接管）；③ 其他 WIMG 组合（位掩码修复后已覆盖 IO/POSTED/WRITE_THROUGH）。
- 判据链：import 停 17 ⇒ 无 GPU 合成帧 ⇒ CA 无物可提交（nval=0 与 l731 对照：l731 import 快速失败时代 CA 回退 CPU 合成仍提交）；屏幕快照（NABUDIAG screen.bgrx seq0/seq1）双时刻均纯黑，与 xfer=0 一致。
- 修复静态理据：import 只走 IOMemoryDescriptor 页（非 MMIO）；范围外页=固件 carveout 内存，GPU 对 NormalUncached 类按非一致处理（安全侧）；l730/l731 无此判据时代同样能显示（CPU 路径）。

## 3. 为什么离线不行
哪个物理页落在 gPhysSize 外由**固件当次分配**决定（每轮不同），必须运行期看 fail_pa 与 import 结果分布。

## 4. 失败也有信息
- 仍停 17 且 `fail_pa≠0` ⇒ 出口不是范围检查（是 RT/WIMG 残余）——fail_pa 直接给出问题页；
- fail_pa=0 ⇒ firmware 分支 IONewData 失败（KHEAP）；
- import=19 但 nval 仍 0 ⇒ 提交链独立问题（转 IOMFB 提交门调查）。

## 5. 唯一变量与回滚点
变量 = `NativeAdrenoCpu.cpp` QueryPageCacheMode 范围外分支（Unknown→NormalUncached）+ `NativeAdrenoMemory.cpp` Unknown 出口写 fail_pa/fail_offset（官方字段复用，无新机制）。与 hold/l739 其余逐字节同源（display/args/boot/gpu-addon/trust 全同）。
回滚 = `hold/l739`（同步 flip 版）或 `hold/l730`（最佳锁屏基线）；失败即回滚。

## 6. 恢复预案
WDT `pet=600 arm-delay=0` 咬点 ≈380–410s 自动回 TWRP `f91fa664`；fastboot 可用；AMFI 场景 panic-restart=2 重启回 TWRP（18:05 实测）；USB CDC 断线可从 NABUDIAG UFS 导出区（全盘 sda bs=4096 skip=first）拉完整日志+屏幕快照。无需人工断电。
