# board-report l741（l731 内核重现轮：kernel vs 系统态二分）

## 1. 唯一问题
把已知有帧的 `hold/l731` 内核原样放回当前系统态（Data 卷/迁移状态已历多轮）后，compute 管线（c4/d4）与帧（xfer>0）是否重现？——即渲染死亡是**内核差异**还是**持久系统态**。

## 2. 已闭合到哪
- l731 与 l740 的 Metal report **错误清单相同**（Compile(InvalidDescriptor(ColorAttachment 2))、BinaryArchive(InvalidFile)、DeviceLost/no-device-answer），但 l731 的 compute 管线**回退成功**（c1/d1→c4/d4 x743 命令流）且 frames 递增；l740 **零命令**（c0/0 d0）。⇒ 错误本身非致命，死的是回退链。
- 内核源码差异已列尽（importTask/WIMG/QueryPageCacheMode、display flip/64位/early-retire/诊断字段）；vtable 槽位对比一致；validateSwap/kickSwapQueue 函数同构；GPU 服务方法通道（calls/status）两轮同。
- 剩两个未分辨：kernel 差异 vs 系统态（Data 卷管线缓存/迁移阶段）。本轮用原 l731 内核一刀切开。

## 3. 为什么离线不行
同一内核在不同时期的系统态上的行为（管线缓存/持久文件/迁移状态影响 CA 路径选择）只能运行期观察。

## 4. 失败也有信息
- l731 内核**仍有帧** ⇒ 渲染死是**今日内核改动**所致（回到 import cache-mode / display 差异清单逐一回退）；
- l731 内核**也零命令** ⇒ 渲染死是**系统态**（Data 卷的管线缓存/归档损坏跨轮持久）——清 Data 缓存/换 seed 即可绕过，内核无罪。

## 5. 唯一变量与回滚点
变量 = 仅 kernel.macho/adt/payload.img 换成 hold/l731 原件（args 39 token 与两基线全同、boot/gpu-addon/trust 全同）。
回滚 = `hold/l740`（import 修复版）或 `hold/l730`；本包是纯观察包，看完即弃。

## 6. 恢复预案
WDT `pet=600 arm-delay=0` 咬点 ≈380–410s 自动回 TWRP；fastboot 可用；NABUDIAG UFS 导出兜底采集。无需人工断电。
