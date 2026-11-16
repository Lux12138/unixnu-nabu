# board-report l747（统一 Cached 分类确认轮）

## 1. 唯一问题
把范围外（GOP）页**统一分类 Cached**（对齐 final 的可用导入类）、firmware 路由撤回 opt-in、preflight 放行 Cached 后，CA 纹理是否建立（tex>0）、Metal 命令是否流动（c/d>0）、帧是否上屏（xfer>0）？

## 2. 已闭合到哪（本修复的完整链）
- **三线 A/B**：final（用户亲验锁屏）= 窗口内页经 QPCM 得 **Cached** 类导入 → 纹理/渲染全通；l746 = **FirmwareUncached** 类 → 导入成功（idg=1/0/0、mapped 144MB）但 **tex x0** 零纹理零命令（firmware 类专为拷贝/扫描设计，不可作渲染目标）；l742 = pmap 逐页分类产生 **Mixed** 类（页间不一致）→ map preflight 拒 → "HostMemory NotRepresentable"。三者与 l741（Cached 类导入支撑 x743 纹理、c4/d4 命令）全自洽。
- 机制文本：ABI `writeback=1 要求缓存一致内存`；preflight 的 Cached/Mixed 分支受 `cached_coherent_imports_` 门（bring-up 硬编码开启 0x06040001，但存在时序窗口）；firmware 分支强制 wb=0/单 span/Device-UC CPU 映射——均与"渲染目标需 Cached/一致类"对账一致。
- 修复三处：① QPCM 范围外页→Cached（统一类，杜绝 Mixed）；② firmwareImportPhysical 撤回默认开（不再劫持 GOP 为 firmware 类）；③ preflight 放行 Cached/Mixed（去 flag 门，硬件本就支持）。
- 证据链性质：A/B 对照 + 源码机制 + 用户目击（final 锁屏）三方合拢。

## 3. 为什么离线不行
纹理/命令的最终建立是运行期行为（纹理规则、计划、驱动落地），静态已穷尽判定条件。

## 4. 失败也有信息
- tex>0 且 c/d>0 且 xfer>0 ⇒ 链证实，进入临时字段回滚与撕裂/锁屏治理；
- 仍 tex=0 ⇒ 纹理拒绝点另在（表格式/线性图能力），idg/报告行指路，单变量收敛；
- 导入回归 17 ⇒ Cached 类触发了别的拒点（fail_pa/阶段号读数定位）。

## 5. 唯一变量与回滚点
变量 = `NativeAdrenoCpu.cpp` QPCM 范围外→Cached + `NativeAdrenoMemory.cpp` firmware 门撤回 + `NativeAdrenoVm.cpp` preflight 去 flag 门。与 hold/l746 其余同源（display/args/boot/trust/addon/entry-leak 修复/诊断字段全同）。
回滚 = `hold/l746`（firmware 版）/`hold/l745`/`hold/l731`/`hold/l730`；失败即回滚。

## 6. 恢复预案
WDT `pet=600 arm-delay=0` 咬点 ≈380–410s 回 TWRP；boot 前查 `slot-unbootable`（yes 先 `set_active`）；**任何 erase 禁止**；恢复只用 `backup-boot_a-20261103.img`（用户点名）。NABULOG1 环（skip=121718715）与 NABUDIAG 导出兜底观察。无需人工断电。
