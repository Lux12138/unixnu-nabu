# board-report l742（cache-mode 归类修正轮：统一 pmap 分类）

## 1. 唯一问题
把 `QueryPageCacheMode` 改为**全物理页统一走 pmap_cache_attributes 分类**（去掉范围外页强制 NormalUncached）后，CA 渲染（Metal 命令流 c/d>0）与帧（xfer>0）是否与 l731 重现轮同样恢复？

## 2. 已闭合到哪
- **l741 二分决定性**：l731 内核在当前系统态下**帧重现**（xfer 0→1→2→3、render 管线 c4/d4 x37 D142 F9）⇒ 渲染死是**今日内核改动**所致，非系统态。
- l740（今日改动集）的渲染死表现：Metal **零命令**（c0/0 d0）+ Compile(InvalidDescriptor)+no-device-answer；同错误在 l731 也有但**非致命**（回退链活着）。
- 今日改动集中唯一影响 **GPU 资源属性**的 = l740 的范围外页→NormalUncached 强制归类 ⇒ IOSurface texture 以非一致属性进 GPU ⇒ CA 渲染资源/管线绑定死。l742 改为：**去掉范围拒绝、统一 pmap WIMG 分类**（RT/NOT_CACHEABLE/WRITE_THROUGH 位掩码不变；pmap 无属性时才 Cached 兜底），保留 Unknown 出口 fail_pa 诊断。
- 静态辩护：import 只走 IOMemoryDescriptor 页（真实内存非 MMIO），pmap 持有其真实 WIMG；l731 时代范围外页走 pmap 分类（无范围拒绝）且渲染正常——l742 恢复该语义。

## 3. 为什么离线不行
固件每次把 buffer 放不同物理区（在/不在 gPhysSize 内），页的 pmap WIMG 也运行期才可读；只有真机能验证归类是否恢复渲染。

## 4. 失败也有信息
- 仍零命令 ⇒ cache-mode 假说错——回退到 l741 的清单第二项（display flip/64位/early-retire 逐一回退）；
- 命令流恢复但 xfer=0 ⇒ 渲染通了、提交层另有问题（nval 诊断字段现成）；
- **正向**：c/d>0 且 xfer>0 ⇒ 与 l731 等价，渲染链修复确认。

## 5. 唯一变量与回滚点
变量 = `NativeAdrenoCpu.cpp` QueryPageCacheMode 一个函数（范围拒绝删除、统一 pmap 分类）。与 hold/l740 其余逐字节同源（display/args/boot/trust/addon 全同）。
回滚 = `hold/l740`（import 19 版）或 `hold/l731`（有帧基线）或 `hold/l730`；失败即回滚。

## 6. 恢复预案
WDT `pet=600 arm-delay=0` 咬点 ≈380–410s 自动回 TWRP；fastboot 可用；NABUDIAG UFS 导出兜底。无需人工断电。
