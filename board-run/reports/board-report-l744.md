# board-report l744（链闭合确认轮：双下毒路径修复）

## 1. 唯一问题
按已闭合因果链修复两处（① QueryPageCacheMode 范围外页→Unknown 拒收；② importTask 失败路径强制清扫防预算泄漏）后，Metal 命令流（c/d>0、M 增长）与帧（xfer>0）是否恢复？

## 2. 已闭合到哪
- **包级**：l731/l742 六组件清点——trustcache/gpuaddon/data-seed/toml 全同、adt 仅 random-seed ⇒ 变量=内核。
- **代码级**：final(活) vs l742(死) 全符号反译 43185 相同；差异全集=QPCM/scanout-client/display 内部；桥=addon 内 NativeAdrenoMetal.bundle（zlib 容器）+libvulkan_freedreno，纯用户态。
- **机制**：ABI span.writeback=1 要求缓存一致内存（NativeAdrenoABI.h:160）。P1=范围外 carveout 页误分类（pmap→Cached/强制 Uncached）→ GPU 映射属性错位→桥死（ST0 M18、c0/0 d0）；final 拒收→桥回退自有分配→活。P2=两遍 importTask `failed:`→release()/destroyOwnedStorage() 早退泄漏（Busy/quarantine 持有 prepared 描述符与 spans——源码已实证）→预算枯竭→同死。存活矩阵（l731/final/l739/l742）四线自洽。
- 修复=final 语义复原（拒收分类）+ 失败路径强制清扫（两处 failed 标签）。

## 3. 为什么离线不行
修复是否让桥恢复出命令、CA 恢复合成，属运行期行为；但改动本身完全由闭合链推出，本轮只做**确认**（不带新假设）。

## 4. 失败也有信息
- c/d>0 且 xfer>0 ⇒ 链证实，进入撕裂/锁屏治理（临时诊断字段届时回滚）；
- c/d 仍 0 ⇒ P2 触发点判据不足（complete() 失败条件需运行期）——但 P1 修复此时为单一变量可再收敛；
- import 大量拒收但回退出帧（l731 模式）⇒ 部分证实（P2 清扫生效），DPU 直出需等全成功 import 轮。

## 5. 唯一变量与回滚点
变量 = `NativeAdrenoCpu.cpp` QPCM 范围检查复原 + `NativeAdrenoMemory.cpp` 两处 failed 强制清扫。与 hold/l742 其余同源（display/args/boot/trust/addon 全同）。
回滚 = `hold/l742` 或 `hold/l731`（有帧基线）或 `hold/l730`；失败即回滚。

## 6. 恢复预案
WDT `pet=600 arm-delay=0` 咬点 ≈380–410s 回 TWRP；`fastboot boot` 前查 `slot-unbootable`（yes 则 `set_active`，非破坏）；**任何 erase 禁止**；恢复只用用户点名的 `backup-boot_a-20261103.img`。无需人工断电。
