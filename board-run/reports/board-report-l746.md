# board-report l746（firmware 导入路径默认开启确认轮）

## 1. 唯一问题
把 GOP/显示表面的导入切到**已设计好的 firmware 路径**（单 span FirmwareUncached/wb=0，`firmwareImportPhysical` 门默认开启）后，CA 的这一次 no-copy 导入是否走通（import 成功→Metal c/d>0→xfer>0）？

## 2. 已闭合到哪
- **l745 决定性读数**：`idg=1/1/17`——348s 内 CA/桥仅 1 次导入尝试，死于 QPCM 范围检查（`fail_pa=9c000000`=GOP 页），此后永不重试（Apple 语义无回退）。条目泄漏论否证（仅 1 败，池未耗尽）。
- **firmware 路径全套静态对账**（本修复的链）：
  ① `firmwareImportPhysical`（Memory.cpp:665）判定=**完全匹配当前 GOP 整体**（extent 连续一致）+ `nabu_gpu_firmware_range`（NabuGpuCopyPlan.h:19）要求范围**完全在内核 DRAM 窗外**——正是 fail_pa=0x9c000000 的情形；
  ② 该判定在 importTask 第 17 阶段**先于** QPCM 循环执行 → 范围拒收不会触及 GOP；
  ③ 产物=单 span `{0,base,bytes,0}` + `FirmwareUncached` → mapBo preflight 的 firmware 分支（imported/spanCount==1/!readOnly/writeback==0/spans[0].writeback==0）逐项满足；
  ④ Turnip `BoMap` 恒 `writeback=0`（tu_knl_darwin.cc:1459）→ firmware 分支的 writeback==0 要求满足；
  ⑤ 别名层：wb=0 单类（class_objects 快路径/同 writeback 无冲突）。
- 门现状：路径被 `nabu-gpu-scanout=1` 关死（args 从未携带；700 字符预算无余量加 18 字符）——修复=**源码侧默认开启**（`scanout=1` 默认，`=0` 保留关闭口），不动冻结 args。
- 另含 l745 的 importBo 条目强制释放（实证缺陷，独立有益）。

## 3. 为什么离线不行
导入在真机的最终成败（firmware 判定的 PE_current_console 状态、映射落地）需运行期一次验证。

## 4. 失败也有信息
- import 成功（idg[1] 不涨）且 c/d>0、xfer>0 ⇒ 链证实，进入临时字段回滚与撕裂/锁屏治理；
- import 成功但仍零命令 ⇒ 拒点后移（mapBo/别名层，idg/新读数指路）；
- firmware 分判不命中（extent/连续性不匹配）⇒ 导入仍走 QPCM 拒——idg[3]=17 照旧，说明 CA 的 surface 不是整块 GOP，转向"范围外页单 span 分类"变体。

## 5. 唯一变量与回滚点
变量 = `NativeAdrenoMemory.cpp` firmwareImportPhysical 门默认开启 + importBo 条目强制释放（UserClient.cpp）+ frames 行 idg 临时字段。与 hold/l745 其余同源。
回滚 = `hold/l745` / `hold/l744` / `hold/l731` / `hold/l730`；失败即回滚。

## 6. 恢复预案
WDT `pet=600 arm-delay=0` 咬点 ≈380–410s 回 TWRP；boot 前查 `slot-unbootable`（yes 先 `set_active`）；**任何 erase 禁止**；恢复只用 `backup-boot_a-20261103.img`（用户点名）。无需人工断电。
