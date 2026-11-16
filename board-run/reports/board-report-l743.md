# board-report l743（二分第二刀：内核 vs ADT/data-seed）

## 1. 唯一问题
把 l742 内核放进 **l731 原包外壳**（adt/data-seed/toml 逐字节 l731 原件，仅 FAT 内 kernel.macho 替换）后，渲染（Metal c/d>0）与帧（xfer>0）是否恢复？

## 2. 已闭合到哪
- l742 否证 cache-mode 假说：统一 pmap 分类后 import 19×11 但 Metal 仍 c0/0 d0 零命令。
- l741 修正结论：l731 **整包**（内核+ADT+data seed）渲染正常 ⇒ 变量是"包"整体，未分内核/ADT/seed。
- 本轮刀法：mtools 将 hold/l742/kernel.macho 覆写入 hold/l731/payload.img 的 FAT（已验证 FAT 内哈希=a71ef0e9），其余文件（adt 78a6f7e9、scratch-data 106672、toml、trustcache、gpuaddon）逐字节不变。
- 分叉判读：恢复渲染 ⇒ 元凶在 ADT/data-seed/toml（再切三者）；仍死 ⇒ 元凶在内核代码（再切 display vs Adreno）。

## 3. 为什么离线不行
ADT/seed 内容与内核行为的组合效应（CA 路径选择、管线缓存状态）只能真机观察。

## 4. 失败也有信息
- 仍 c0/0 d0 ⇒ 内核代码致死（缩到今日改动集：display flip/64位/early-retire vs Adreno importTask 两遍/WIMG）；
- c/d>0 但 xfer=0 ⇒ 渲染恢复但提交层另有问题（nval 诊断现成）；
- **正向**：c/d>0 且 xfer>0 ⇒ 与 l731 等价，元凶在 ADT/seed，逐项替换收尾。

## 5. 唯一变量与回滚点
变量 = 仅 FAT 内 kernel.macho（l731 壳 + l742 核）。boot/gpu-addon/trust/l731-seed 全同。
回滚 = `hold/l742`（纯 l742 包）或 `hold/l731`（有帧基线）或 `hold/l730`；本包为观察包。

## 6. 恢复预案
WDT `pet=600 arm-delay=0` 咬点 ≈380–410s 回 TWRP；`fastboot boot` 前查 `slot-unbootable`（为 yes 先 `set_active`，非破坏）；**禁止任何 erase**；恢复只用用户点名的 `backup-boot_a-20261103.img`。无需人工断电。
