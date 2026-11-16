# board-report l739（DPU 直出同步 flip 恢复轮）

## 1. 唯一问题
把 `flip()` 从 latchTimer 异步回调（latchTick）移回 `kickOnce` 直出分支**同步执行**（恢复 13:29 `final` = kernel `9011f295` 语义）后，DPU 直出是否恢复（`xfer>0`、`ncpy=0`、帧上屏、锁屏可交互）？

## 2. 已闭合到哪
- 反汇编（capstone，函数体按符号地址提取）：`kickSwapQueue`（内联 kickOnce）在 `l738-kernel-final`（xfer=122 工作版）含 MDP 存储 `#0x4014/#0x4024/#0x6014/#0x6024` + `dsb sy`（同步 flip）；`latchFired`（内联 latchTick）72 条指令、**零** MDP 存储（只 retire）。
- 同一对函数在 `l738-kernel-src0only`（blank 版）：kickSwapQueue **零** MDP 存储；latchFired 89 条、含完整 flip —— flip 被移进 latchTick（tearfix 会话产物）。
- CDC（l738-tail-src0only，3.5 MB）：`import_stage=19×13`（GPU import 正常）、IB 提交 3321 次（backboardd 渲染活跃）、但 `xfer=0 nswap=0 nsurf=0 nret=0`；`state primary=1 mapper=0 pm=1 start=1`；probe diff（pre-l738-kclient→当前）确认 validateSwap/提交层零改动。
- 排除项：SSPP_SRC1 寄存器写法（回退 SRC0-only 后依旧 blank ⇒ 回归点不在 DPU 寄存器）；64 位 IOVA 类型本身（uint64 截断写回 SRC0 无害）。

## 3. 为什么离线不行
`latchTimer`（IOTimerEventSource 500 µs）是否在真机触发、flip 后 DPU 是否实际出帧、retire 是否发生，是运行期定时器/中断/CTL flush 状态，静态只能给出代码路径无法给出触发性。

## 4. 失败也有信息
本轮 frames 行新增临时诊断字段（用完回滚）：`nval=%u nrej=%u rsn=%u don=%d tr=%d d0=%u d7=%u`（validateSwap 调用/拒绝/最后原因、directOn、transferred、direct flip 计数、fallback 计数）。
- `nval=0` ⇒ 客户端从未提交 swap（提交层断）；
- `nrej>0` ⇒ validateSwap 拒绝（几何/格式原因码 rsn）；
- `tr=1 & d0=0` ⇒ 首帧进直出但未 flip / retire 卡死；
- `d0>0` ⇒ flip 已执行，问题在扫描出图或之后。

## 5. 唯一变量与回滚点
变量 = `NativeLegacyDisplay.cpp` 三处：①直出分支加同步 flip；②latchTick 删 flip；③frames 行临时诊断字段（仅日志格式，无行为）。其余与上包逐字节同源：args 700B/39 token 零变化（added=[] dropped=[]）、Probe/Adreno 源零改动、boot/gpu-addon/trust 沿用 l730 固定件。
回滚 = `hold/l738`（src0only）或 `hold/l730`（最佳锁屏基线）；失败即回滚。

## 6. 恢复预案
WDT `pet=600 arm-delay=0` 咬点 ≈380–410 s 自动回 TWRP `f91fa664`；`fastboot` 可用（fastboot boot 临时加载，不写 boot_a）；无早 panic 预期；无需人工断电。
