# 上板报告 l720（一次性临时采集轮：第二诊断槽 diagAt1 470→300）

> 本包相对 `E:\nabu\hold\l719\` 只改 `kernel.macho` 一条 ARM64 立即数；保留 l719 已验证的 `pet=600`、args/toml 700/39 逐字节不变，零新增 token、零重编、零探针。用户 2026-11-11 授权必要时延后 WDT 并自动推进；设备只做 `flash cust` + 最后重 arm misc + `fastboot boot`，`boot_a` 不碰。

## 1. 唯一问题

保留 l719 的 WDT 延寿，只把板上已有的第二诊断槽默认 `diagAt1=470` 前移到 `300`，能否在真实 300 s 左右、系统仍有 I/O 完成路径且第一槽已导出时触发 `NABUDIAG seq=1`，从而第一次拿到比 l718/l719 `seq=0` 更晚的 os_log / tracev3 / screen，判读 `00LaunchServicesMigrator` 是否在 253～300 s 之后完成或出现明确失败？本轮不改迁移、keychain、LaunchServices、WDT、args，只改诊断时间闸。

## 2. 已闭合到哪

l718 的动态 `PUIProgressWindow`、frames 56、真实 117～253 s os_log 与 `00LaunchServicesMigrator` 136 s 开始后无完成行已核清。l719 把 kernel 默认 `petSeconds` 从 300 改成 600，CDC `pet=600 arm=0 hang=0`，自动回收从 364 s 延后到 409 s，无 panic / APFS / `buf_bread`。但 l719 只有 `NABUDIAG seq=0`（301 files、真实到 255.1 s），`seq=1` 全程 0；原因是板上第二槽默认 `diagAt1=470`，而该槽只在 I/O 完成路径 arm，板子在真实约 335 s 停笔、409 s 咬合，470 s 从未到达。l719 屏 `fa2bf317…` 与 l718 `3d0be3a5…` 精确只差 52 像素，仍是同一进度窗；`Migration complete` / `Dismissing overlay` / `screen owner is now` / `finishedStartup` 全 0。编译树 `NativeUfs.cpp:2917-2931` 与板上反汇编 `kernel @0xa33690` 证明 `mov w8,#0x1d6` 是 `diagAt1=470`；相邻 `@0xa336a4 mov w8,#0x104` 是 `diagAt0=260`，`@0xa336c0 mov w0,#0` 与 `@0xa336d8 mov w0,#1` 分别 arm 槽 0/1。WDT/diag 两条时间闸都已静态定位，且均为已有官方机制。

## 3. 为什么离线不行

静态能证第二槽时间常量、arm 条件和 l719 的 409 s 回收窗口，但不能证明把常量改成 300 后有没有一次 I/O 完成恰好越过阈值、`nabu_diag_dump_thread` 是否真写出非 stale 的 `seq=1`、该槽的 tracev3/screen/os_log 是否会带上 300 s 后的迁移完成/失败行。l718/l719 都只有 `seq=0`，l719 是唯一带 `pet=600` 的长窗口轮但没有 300 s 的第二导出；没有运行期制品能替代这一判断，满足 `AGENTS.md` 0.5。

## 4. 失败也有信息

若 CDC 同时出现 `NABUDIAG1/2 seq=0` 与 `seq=1`，且回 TWRP 后 `pull_diag.py` 拉到两个不同 SHEXR001/M2VCACH1 头、两块 base/内容不逐块相同，就得到第二份可解码诊断树：可直接判 `completed migration for ...00LaunchServicesMigrator` / `Migration complete` / `Dismissing overlay` / `screen owner is now` / 晚段屏 sha 与 frames。若 `seq=1` 仍 0、或拉回的 `l720-diag-seq1.bin` 与 l719 `seq=0` 逐块相同，则证明第二槽不是单纯时间太晚，而是导出通道/线程在长窗口内不工作；应停止 WDT/diag 时间轮，不再继续改时间常量或 `pet-until`。若 `pet=600` 回显消失或出现早复位，则补丁/门禁隔离失败，按阴性回滚 l719，不追加猜测参数。三种结果都把「>253 s 到底能不能取证」收窄成可判读事实。

## 5. 唯一变量与回滚点

唯一变量是 `E:\nabu\hold\l719\kernel.macho` @`0xa33690` 的一条同长 ARM64 指令：`mov w8,#0x1d6`（`c83a8052`，`diagAt1=470`）→ `mov w8,#0x12c`（`88258052`，`diagAt1=300`），仅 2 个文件字节改变，文件长度仍 125,628,416 B。l719 的 `@0xa384f4 mov w8,#0x258`（`petSeconds=600`）与 APFS nop @`0x6cd2f68=1f2003d5` 逐字节保留。l720 kernel sha8 `13d14bdb`，payload sha8 `0e4f8cee`；payload 内只 `KERNEL~1.MAC` 同步，`UNIXNU~1TOM` 1126 B 与 l719/l718 逐字节相同，其余 inner files 全 SAME。args 仍 700/39，`added=[]`、`dropped=[]`；`sheng-smp=1 + nabu-wdt-arm-delay=0` 硬配对不动，BITE / `WDT.CFG` / Mu / `nabu-hang-reset=0` / `diagAt0=260` 全不动。五文件 `boot=ead4b531`、`addon=950a3968`、`adt=78a6f7e9`、`trust=a4a6bae6` 与 l719 逐字节相同。一次性临时轮、用完回滚、不进默认 args；回滚 `hold/l719`（延寿基线）、`hold/l718`（动态画面）、`hold/l714`（静态画面）、`hold/l700`（安全基线）。

## 6. 恢复预案

`AGENTS.md` 0.10 绝对红线：绝不 `fastboot flash boot_a` / `flash boot`；这块板 recovery 在 boot_a 里，写 boot 会清 misc 并丢失 TWRP。执行顺序仅用既有 `E:\nabu\tmp\go_round_next.sh l720`：先在 recovery 写 `misc-bootrecovery.bin`，进 fastboot，`fastboot flash cust hold/l720/payload.img`，**最后** `fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin` 重 arm，再 `fastboot boot hold/l720/boot-merge-r3.img`。`arm-delay=0` 保留早 panic 的复位路径；`hang=0`，无第二静默复位源；`pet=600` 预期仍自动回 TWRP，不需要人工长按。采集用现有 CDC watcher 持续到回 TWRP，随后 `python E:/nabu/tmp/pull_diag.py l720 <cdc>` 拉两个或多个 base，`python E:/nabu/tmp/extract_diag_tree.py` + `build_arch.py` + WSL `unifiedlog_iterator` 分别解 `seq=0/1`。若 fastboot 全失败才需人工长按电源 10 秒恢复。门禁按 `audit_bootability` / `audit_dual` / `audit_args` / Windows Python `precheck.py` + `precheck_check.py` / `guard_no_boot_flash` 逐项落盘；设备当前 TWRP `f91fa664`，`boot_a` 全程未碰。
