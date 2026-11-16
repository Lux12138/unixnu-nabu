# board-report l754（功能轮：短促快滑解锁 → 进桌面）

## 1. 唯一问题
把 l753 的 11 点手势从「600ms 慢拖到屏幕顶端」改成「200ms 快滑到屏幕中部」后，SpringBoard 能否把它认作 home flick，把画面从锁屏交还到桌面？判据：`inject i=0..10` 仍出现（补丁链健康）、`finishedStartup / Dismissing overlay` 出现、470s 快照出现 app 图标而非时钟壁纸。

## 2. 已闭合到哪
- **前轮事实与纠错**：l752 接通内核既有 home 手势分支（`inject i=0/1/2`，补丁独有）；l753 用 5 条指令改成 11 点连续上滑，实测 `inject i=0..10` 全 `rc=0`、y 32255→0 线性、`fingers=0` 全程（用户未摸屏）、`xfer` 0→14 与触摸无关 ⇒ **补丁确实生效且送达 SpringBoard/锁屏 UI**：470s 快照相对 l752 多出 "No Older Notifications"。但仍是锁屏、`finishedStartup/Dismissing overlay` 全 0 ⇒ **未进桌面**。
- **撤回的错误成绩**：`ncpy=0` 不等于零拷贝（计数点只覆盖 CPU-copy 路径，kclient 的 memcpy 不计数）；l752 的 `fingers/reports/xfer` 主要来自用户手动触摸。已写入记忆，不再重复记功。
- **本轮机制判断（离线证据）**：固件日志 `firmware ok=1 ... swap=0 invx=0 invy=0` ⇒ **没有轴反转**，y 32255→0 就是底→顶，方向本来就对。l753 出现 "No Older Notifications" 是 **600ms 慢拖 + 一路拖到顶端**被锁屏当成上翻通知列表，而非快速 home flick。线程轮询 `IOSleep(8)`（NabuTouch.cpp:1336）支持 20ms 步间隔。
- **补丁**（源 l753 内核只读，脚本 `E:\nabu\tmp\l754-patch.py`，3 条 4B 指令 / 6 字节 / 2 簇，capstone 逐条验原字节与新解码）：
  | 文件偏移 | 原指令 | 新指令 | 作用 |
  |---|---|---|---|
  | 0xA8D39C | `mov w8,#0x8700` | `mov w8,#0x2d00` | 步间隔常量低 16 位 |
  | 0xA8D3A0 | `movk w8,#0x393,lsl#16` | `movk w8,#0x131,lsl#16` | 合成 0x1312d00 = 20,000,000ns |
  | 0xA8D3E8 | `lsl w8,w28,#8` | `lsl w8,w28,#7` | 步距 256→128 |
  - 结果：y = 2560 − step×128 − 40，step 0→10 得 2520→1240（底边→屏幕中部），11 点 × 20ms = 200ms 快滑；常量重建断言 `(0x131<<16)|0x2d00 == 20_000_000` 通过。
- 包组装（`l754-pack.sh`）：以 l753 payload 为基 `mcopy` 单文件换 kernel.macho；内部实测 kernel `d51155a435388ade`，adt `ddb096bf1b20222f`，gpuaddon `950a3968baa13ccd`，trustcache `a4a6bae61f3febcf`，scratch `3cd3d0d3a755920a` —— 除 kernel 外全同；args 700/39 同。

## 3. 为什么离线不行
SpringBoard 是否把 200ms 的 HID 位移序列判定为解锁 flick、是否交还桌面、屏幕是否出 app 图标，都只能运行期得到；静态只能证明指令语义、常量与补丁落点正确。

## 4. 失败也有信息
- `inject i=10` + `Dismissing overlay` + 桌面图标 ⇒ 目标达成，进入可交互验收；
- `inject i=10` 但仍锁屏 ⇒ 手势送达但不被认作解锁，下一步改起点/时长/多指或查 SpringBoard 的解锁策略（功能参数）；
- 快照 "No Older Notifications" 消失但仍锁屏 ⇒ 快滑已不被当滚动，方向/节奏对，差最后交还条件；
- 无 `inject i=10` ⇒ 回 l753 基线；任何回归 ⇒ 回滚 `hold/l753` → `hold/l750`。

## 5. 唯一变量与回滚点
变量 = kernel.macho 的 6 字节（3 条指令）；payload 其余四文件、boot/trust/addon/ADT/args 与 l753 全同。回滚 `hold/l753` / `hold/l750` / `hold/l730`。仅 `fastboot boot`，不写任何分区。

## 6. 恢复预案
`flash cust` + `fastboot boot`（boot_a 保持 TWRP）；**任何 erase 禁止**；boot 前查 `slot-unbootable:a/b`；`sheng-smp=1` 配 `nabu-wdt-arm-delay=0`，pet=600。CDC 采 720s（含 280s 手势 + 470s 快照窗口），NABUDIAG seq0(260s)/seq1(470s) 两快照判读屏幕。若 USB 卡死不强拉，等人工按键回 TWRP；恢复只用用户点名 `backup-boot_a-20261103.img`。
