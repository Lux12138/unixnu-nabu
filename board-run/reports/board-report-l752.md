# board-report l752（功能轮：开机自动上滑解锁 → 进桌面）

## 1. 唯一问题
在 l750 已达成 `ncpy=0` 无拷贝上屏 + 470s 完整原生锁屏（args 无 nabu-diag-at1，seq1 默认 470s）的前提下，接通内核里**已编进但从未触发**的 home 上滑手势，让无人值守启动在锁屏出现后自己解锁、把画面交还到桌面（`fingers>0`、`finishedStartup`/`Dismissing overlay`、桌面快照）？本轮不查死因、不加诊断，只看功能结果。

## 2. 已闭合到哪
- l750 功能事实：CDC `xfer=2→5→6`、`ncpy=0` 全程、`nswap/nok/nret` 与 xfer 同步；NABUDIAG seq1（350s）快照是完整 iPadOS 锁屏（Wednesday, 31 December / 4:04 / 状态栏 / Home 条）。触摸驱动活着：`reads=16824`、`opened=1`、`IOHIDInterface` 已挂，但 `reports=0 / fingers=0`（没人摸屏）。
- 源码链（`UniXNU-nabu/package/NabuTouch.cpp`）：`touchThread` 的 `nabu-touch-inject` 分支在 t>=200s 时执行三步 home 手势（`deliverSt`，从 `gYMax-40` 上滑到 `gYMax/2` 再抬手），`nabu-touch-synth` 分支另有单击。二者都需各自 boot-arg，而 args 已满 700 字符/39 token，加新 token 会破红线。
- 内核二进制实证（capstone 反汇编，`l750/kernel.macho` = 9011f295）：
  - `nabu-touch-inject\0` 唯一 @file `0x18B80C`，参数解析 `PE_parse_boot_argn` @VA `0xfffffff007a9116C`（ADRP+ADD 指向该串，`mov w2,#4`）。
  - 200s 时间门由 3 条指令构造 64 位常量 `0x2E90EDCFFF` = 200e9−1 ns，@file `0xA8D364`：`mov x9,#0xcfff` / `movk x9,#0x90ed,lsl#16` / `movk x9,#0x2e,lsl#32`；紧邻 `ldr w8,[sp,#0x55c]; cmp w8,#1`（即 doInject==1 判定）。
- 补丁（**共 13 字节、两簇**，脚本 `E:\nabu\tmp\l752-patch.py`，源 l750 内核只读）：
  1. 字符串 `nabu-touch-inject\0` → `nabu-touch\0` + 7 NUL（等长 18B），复用 args 里**已有**的 `nabu-touch=1`（token 27）作为开关，零新增 token、零字符变化（700/39 不变）。
  2. 时间常量改 280s：`0x5D21DB9FFF` = 400e9−1 ns，3 条指令重编码 `e9ff9dd2 8929a6f2 2908c0f2`；capstone 复验 `mov x9,#0xefff / movk x9,#0x314c,lsl#16 / movk x9,#0x41,lsl#32` 正确。选 280s 因锁屏在 350s 已画出、WDT bite ~658s，窗口内且有余量。
  - 断言：原串/原指令逐字节匹配才写；diff = 13 字节、两簇（0x18B816-1C、0xA8D366-6D），无布局漂移。
- 包组装（`l752-pack.sh`）：以 l750/payload.img 为基 `mcopy` 单文件替换 kernel.macho；实测内部五文件：kernel `f37947b986b368ff`，adt `ddb096bf1b20222f`，gpuaddon `950a3968baa13ccd`，trustcache `a4a6bae61f3febcf`，scratch `3cd3d0d3a755920a` —— 除 kernel 外与 l750 全同。args 700/39 全同（含 `nabu-touch=1`、`sheng-smp=1`、`nabu-wdt-arm-delay=0`、`sheng-mig=1`）。

## 3. 为什么离线不行
手势是否被 HID 栈接受、SpringBoard 是否真的把锁屏交还成桌面、触摸事件能否驱动 UI，都只能运行期得到；静态只能证明补丁落在正确的解析点与时间门上。

## 4. 失败也有信息
- `inject i=0/1/2` + `fingers>0` + 桌面快照 ⇒ 手势链路通，进入「可交互桌面」验收；
- `inject i=` 有但 `fingers=0`/仍锁屏 ⇒ HID 层收下但 UI 没响应，下一步改手势坐标/时长（功能参数）；
- 无 `inject i=` ⇒ 280s 还没到就死或时间门未命中，回 l750 锁屏基线并把触发点前移到 360s；
- 任何回归 ⇒ 回滚 `hold/l750`（已验证无拷贝锁屏）。

## 5. 唯一变量与回滚点
变量 = kernel.macho 的 13 字节（字符串复用 + 时间常量 200s→280s）；payload 其余四文件、boot/trust/addon/ADT/args/设备状态与 l750 全同。回滚 = `hold/l750` → `hold/l730`（用户目验锁屏）。仅 `fastboot boot`，不写任何分区。

## 6. 恢复预案
`flash cust` + `fastboot boot`（boot_a 保持 TWRP）；**任何 erase 禁止**；boot 前查 `slot-unbootable:a/b`；`sheng-smp=1` 配 `nabu-wdt-arm-delay=0`，pet=600。若 USB 卡死，软件侧不强拉，等人工按键回 TWRP；恢复只用用户点名 `backup-boot_a-20261103.img`。采集 CDC 700s + NABUDIAG seq0(260s)/seq1(350s) 两快照 + 可选更晚 seq2 覆盖 280s 手势后的桌面。
