# board-report l745（条目泄漏修复 + 拒收定位诊断轮）

## 1. 唯一问题
修复已实证缺陷（importBo 失败路径 `unreleasable=true` 永久泄漏条目槽）后，CA 回退分配是否恢复（Metal c/d>0、xfer>0）？若仍有拒收，新增的 `idg` 诊断（import_diag：总导入/失败/首失阶段）定位剩余拒收层。

## 2. 已闭合到哪
- **缺陷实证（源码级闭合）**：`NativeAdrenoUserClient::importBo` 失败路径 `if(release()!=Success) entry->unreleasable=true`——失败导入的条目**永不归还**；`takeEntry()` 耗尽后对**所有**后续导入与分配回 `kIOReturnNoResources`（含 CA 回退的 newBufferWithLength→BoAlloc）→ 桥"buffer refused"→ CA 全面放弃 → 零命令零提交。与 l738+ 轮"回退也不出现"完全吻合；l731 单遍失败路径 release() 干净→不泄漏→回退可用（3 帧）。
- 修复=失败路径强制 `release()` + 无条件 `returnEntry()`（条目从未发布 handle=0，无引用悬垂风险）。
- **诊断闭合**：`native_adreno_import_diag[0/1/3]` 是现成计数器（importBo 每次调用记账），仅缺可见性；frames 行追加 `idg=总/败/首失阶段`（临时字段，随本轮诊断回滚）。读数分叉：idg[1] 增长⇒条目耗尽实锤（修复直接验证）；idg[1] 稳定而 fail_pa=9c000000⇒拒收仍在 QPCM 层（转向范围外页分类修复）。
- 前轮已闭事实：被拒页=GOP 帧缓冲（fail_pa=0x9c000000）；HOST_CACHED/预算/GMU/has_host_import 四论已排除。

## 3. 为什么离线不行
条目耗尽与拒收层分布是运行期计数；只有真机能读出 idg 分布。

## 4. 失败也有信息
- c/d>0 且 xfer>0 ⇒ 泄漏即根因（或主因），链证实；
- 仍零命令但 idg[1] 大涨 ⇒ 泄漏修复生效但另有拒收层（idg[3] 阶段号指路）；
- idg 干净且 fail_pa 仍 9c000000 ⇒ 转 QPCM 范围外页分类（FirmwareUncached/复用映射）单变量收敛。

## 5. 唯一变量与回滚点
变量 = ①`NativeAdrenoUserClient.cpp` importBo 失败路径强制释放；②`NativeLegacyDisplay.cpp` frames 行 `idg` 临时诊断字段。与 hold/l744 其余同源（QPCM=final 语义、failed 强制清扫、display/args/boot/trust/addon 全同）。
回滚 = `hold/l744` 或 `hold/l731` 或 `hold/l730`；失败即回滚。诊断字段用完回滚（与 nval 等一并）。

## 6. 恢复预案
WDT `pet=600 arm-delay=0` 咬点 ≈380–410s 回 TWRP；`fastboot boot` 前查 `slot-unbootable`（yes 先 `set_active`）；**任何 erase 禁止**；恢复只用用户点名的 `backup-boot_a-20261103.img`。无需人工断电。
