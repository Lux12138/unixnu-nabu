# board-report l748（观察仪器轮：seq1 快照提前到 350s）

## 1. 唯一问题
把 NABUDIAG seq1 时刻从默认 470s 提前到 350s（`nabu-diag-at1=350`）后，260s/350s 两个屏幕快照与环/tracev3 是否覆盖 CA 合成窗口（250-400s）——l747 的 Cached 类修复下 CA 到底画不画？

## 2. 已闭合到哪（观察盲区的机制链）
- NativeUfs.cpp:2915-2930 实证：seq0 快照=`nabu-diag-at0`（默认 **260s**）、seq1=`nabu-diag-at1`（默认 **470s**），19.2MHz 计时；dump 尾部 `nabu_log_flush_tail_nosync` 同步刷新 NABULOG1 环窗口。
- **三轮 seq1 导出哈希全同（f8c26696）**= 陈旧数据（设备 ~410s 咬合，从未活到 470s）——此前"seq1 快照全黑"不构成本轮证据。
- 已有真证据：seq0（260s）快照黑（l746）；环窗口止 ~225s（疑日志写手被死串口堵）；l741 首帧 ~150-200s。**260-410s 为观察盲区**——本轮的唯一目的即照亮它。
- 工具链：NABULOG1 环拉取（skip=121718715）、SHEXR001 提取、screen.bgrx 解析全部就绪；tracev3 LZ4 导出损坏（例外：不阻塞，环+快照足够）。
- l747 事实保留：Cached 类导入通（idg=1/0/0）；225s 时 tex x0/nval=0（首帧前）。

## 3. 为什么离线不行
CA 合成时刻与屏幕内容是运行期事实；观测点配置（args）生效与否也需真机确认。

## 4. 失败也有信息
- 350s 快照有内容/环内 frames 递增 ⇒ l747 修复生效，进入临时字段回滚与撕裂/锁屏治理；
- 350s 仍黑且环 frames 全零 ⇒ CA 纹理/管线墙未除（tex x0 持续），回到纹理拒绝点（l747 修复不充分）；
- seq1 未触发（设备早死）⇒ 读 seq0（260s）+环；如需更早快照再调 at0。

## 5. 唯一变量与回滚点
变量 = args 换 token：dropped `nabu-state-ring=10`（状态环打印，纯装饰）/ added `nabu-diag-at1=350`（seq1 快照提前）。内核/kernel.macho 与 hold/l747 逐字节同（d6e2f33b）；boot/trust/addon 同。
回滚 = `hold/l747`（同内核默认快照）/`hold/l746`/`hold/l731`/`hold/l730`；失败即回滚。诊断字段（nval/don/tr/idg）仍挂账，渲染恢复后回滚。

## 6. 恢复预案
WDT `pet=600 arm-delay=0` 咬点 ≈380–410s 回 TWRP；boot 前查 `slot-unbootable`（yes 先 `set_active`）；**任何 erase 禁止**；恢复只用 `backup-boot_a-20261103.img`（用户点名）。NABULOG1 环与 NABUDIAG 导出兜底。无需人工断电。
