# board-report l753（功能轮：连续上滑解锁 → 进桌面）

## 1. 唯一问题
把内核合成手势从「3 步：按下→中间一点→抬手」改成「11 点 600ms 从底边连续拖到顶」后，SpringBoard 能否把它认作 home pan，把画面从锁屏交还到桌面？判据：`inject i=10` 出现（补丁独有，l750/l751/l752 均最多 i=2）、`finishedStartup / Dismissing overlay` 出现、470s 快照出现 app 图标而非时钟壁纸。

## 2. 已闭合到哪
- **前轮事实与纠错**：l750/l752 都画出真锁屏；l752 的 `inject i=0/1/2`（`rc=0`）是 13 字节数据补丁接通内核既有 home 手势分支的独有证据；但 `fingers=103 / reports=112 / xfer=61` 主要来自用户手动触摸（`fingers` 计数 `nabu_touch_stats[2]` 只在 `decode()` 解出真实触点时 +1，NabuTouch.cpp:1236；合成报告每次只给 `stats[4]` 加 1）。470s 快照仍是锁屏（全彩渐变壁纸+时钟+状态栏+Home 条，100% 非零点），`finishedStartup/Dismissing overlay` 全 0 ⇒ **没进桌面**。
- **撤回的错误成绩**：`ncpy=0` 曾被报成"去掉每帧 16MB CPU blit"。源码核对：`native_legacy_copy_timing_observed` 唯一计数点只在 `NativeLegacyDisplay.cpp:764` 的 CPU-copy 路径；9011f295 独有的 `nabuScanoutClient` 走 WAIT_SWAP→memcpy→COMPLETE_SWAP，那次整帧拷贝不经此计数点；`native_ufs_dpu_map` 是 `return pages[0]` 桩（NativeLegacyProbe.cpp:1410）。⇒ `ncpy=0` 只说明"拷贝没被计数"，不能证明零拷贝或流畅。静态研究另证 9011f295 已有 18/18 l730 ledger new_hex，禁止裸偏移重放。
- **手势不生效的机制**：源码 `NabuTouch.cpp:1112` `int y=(step==0)?(gYMax-40):(gYMax/2)`——只有两个位置，SpringBoard 的 home pan 需要连续位移序列。步间隔常量 `0x3938700` = 60ms，循环上限 `cmp x28,#2`。
- **补丁**（源 l752 内核只读，脚本 `E:\nabu\tmp\l753-patch.py`，5 条 4B 指令 / 13 字节 / 3 空间簇，每条 capstone 验过原字节与新解码）：
  | 文件偏移 | 原指令 | 新指令 | 作用 |
  |---|---|---|---|
  | 0xA8D380 | `cmp x28,#2` | `cmp x28,#0xa` | 循环 3 步 → 11 步 |
  | 0xA8D46C | `cmp x28,#2` | `cmp x28,#0xa` | 抬手从 step2 → step10 |
  | 0xA8D3E8 | `sub w8,w9,#0x28` | `lsl w8,w28,#8` | step×256 |
  | 0xA8D3EC | `lsr w10,w9,#1` | `sub w10,w9,w8` | y = gYMax − step×256 |
  | 0xA8D3F4 | `csel w10,w8,w10,eq` | `sub w10,w10,#0x28` | y −= 40（底边起） |
  - 结果：y 从 2520 线性递减到 -40（scale() 钳到 0），x 恒 gXMax/2，11 点 × 60ms = 600ms 连续上滑；`cmp x28,#0` 变死码但后续所有 csel 都自带前置 cmp，无害。
  - 链闭合核对：`w8` 在 0xA8D3FC 被 `ldr w8,[x8,#0x7bc]` 重载，不影响 0xA8D40C 的 `cmp w11,w8`；`x28` 由 0xA91308 `ldr x28,[sp,#0x488]` 每轮恢复且 l752 已实证 0/1/2 递增。
- 包组装（`l753-pack.sh`）：以 l752 payload 为基 `mcopy` 单文件换 kernel.macho；内部实测 kernel `670aa1ccc0d5f54c`，adt `ddb096bf1b20222f`，gpuaddon `950a3968baa13ccd`，trustcache `a4a6bae61f3febcf`，scratch `3cd3d0d3a755920a` —— 除 kernel 外全同；args 700/39 同。

## 3. 为什么离线不行
SpringBoard 是否把 HID 位移序列判定为解锁 pan、是否交还桌面、屏幕是否出 app 图标，都只能运行期得到；静态只能证明指令语义与补丁落点正确。

## 4. 失败也有信息
- `inject i=10` 出现但仍是锁屏 ⇒ 手势送达但方向/速度/起点不被认，改坐标或时长（功能参数）；
- `inject i=10` + `Dismissing overlay` + 桌面图标 ⇒ 目标达成，进入可交互验收；
- 无 `inject i=10`（只到 i=2）⇒ 循环上限补丁未生效或设备早死，回 l752 基线；
- 任何回归 ⇒ 回滚 `hold/l752`（已验证彩色锁屏 + 手势）→ `hold/l750`。

## 5. 唯一变量与回滚点
变量 = kernel.macho 的 13 字节（5 条指令）；payload 其余四文件、boot/trust/addon/ADT/args 与 l752 全同。回滚 `hold/l752` / `hold/l750` / `hold/l730`。仅 `fastboot boot`，不写任何分区。

## 6. 恢复预案
`flash cust` + `fastboot boot`（boot_a 保持 TWRP）；**任何 erase 禁止**；boot 前查 `slot-unbootable:a/b`；`sheng-smp=1` 配 `nabu-wdt-arm-delay=0`，pet=600。CDC 采 720s（含 280s 手势 + 470s 快照窗口），NABUDIAG seq0(260s)/seq1(470s) 两快照判读屏幕。若 USB 卡死不强拉，等人工按键回 TWRP；恢复只用用户点名 `backup-boot_a-20261103.img`。
