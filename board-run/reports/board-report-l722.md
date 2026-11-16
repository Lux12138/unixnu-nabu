# 上板报告 l722（一次性临时采集轮：`diagAt0 260→220`，提前第一诊断槽）

> 本包相对 `E:\nabu\hold\l721\` 只改 kernel 中 `diagAt0` 默认常量的一条同长 ARM64 立即数；保留 l721 的 `pet=600`、`diagAt1=300` 与 `/private/var/installd 33:33`，args/toml 700/39 逐字节不变，零新增 token、零重编、零探针。用户 2026-11-11 要求确认 WDT 延期正常后继续推进；设备只做 `flash cust` + 最后重 arm misc + `fastboot boot`，`boot_a` 不碰。

## 1. 唯一问题

在 l721 的 33:33 属主补丁基础上，把第一诊断槽默认 `diagAt0` 从 260 s 前移到 220 s，能否赶在 l721 约 253.8 s 停笔 / 294 s 自动回收之前导出 `NABUDIAG seq=0`，从而第一次拿到 33:33 组合下的用户态 os_log，判读 installd home uid/gid Fault、`NSPOSIXErrorDomain Code=13/2` 与 `MIInstallerErrorDomain Code=105/4` 是否下降？本轮不改 WDT、属主、migration plist、keychain、staged apps 或 args，只提前诊断时间闸。

## 2. 已闭合到哪

WDT 延期本身已核清：`pet=600` 在 l719/l720/l721 都命中；l719 409 s、l720 628 s 自动回 TWRP，无 panic/APFS/`buf_bread`，l720 双槽非 stale、内核 tick 到 603 s、frames 647。l721 官方背书的 `33:33` 补丁生效（`data tree attr=49 attrfail=0`），但 16:57:18→17:02:12 只约 294 s，CDC 在 253.8 s 停笔、frames 最大 20，`NABUDIAG1/2` 与 `NABUOSLOG1` 全 0；因没到 `diagAt0=260`/`diagAt1=300` 的时间闸，无法判断用户态错误是否改善。l720 的第二槽证明 `diagAt1=300` 可用；l721 缺的只是更早的第一份导出。板上反汇编定位 `@0xa336a4 mov w8,#0x104`（260，`88208052`），同长改成 `mov w8,#0xdc`（220，`881b8052`）；`@0xa33690` 仍是 `diagAt1=300`，`@0xa384f4` 仍是 `pet=600`，`@0x1f222c/0x1f2230` 仍是 33:33，APFS nop 不变。Qoder/CURRENT 历史无同类 `diagAt0` 前移阴性实验。

## 3. 为什么离线不行

静态能证指令落点、WDT/属主/第二槽全部保留、历史无否证；不能证明 220 s 阈值后是否有一次 I/O 完成真正 arm 第一槽、`nabu_diag_dump_thread` 是否写出非 stale SHEXR001/M2VCACH1、该槽的 os_log 是否覆盖 installd 224.8 s Fault 窗口，以及 33:33 是否让 home Fault / `Code=13/2` / `MIInstallerErrorDomain 105/4` 下降。l721 是唯一 33:33 组合但没有诊断制品；离线无法替代运行期导出，满足 `AGENTS.md` 0.5。

## 4. 失败也有信息

若 CDC 出现 `NABUDIAG1/2 seq=0`，且回 TWRP 后拉到 SHEXR001/M2VCACH1 头，就解码 os_log：home uid/gid Fault 归零、`Code=13/2` 下降、staging `Code=105/4` 下降 ⇒ 属主链成立；若 Fault 归零但 staging 仍失败，收窄到深层目录骨架；若属主 Fault 仍在，证 33:33 没有落到实际 Data 卷目录，回滚 l720 基线。若 220 s 仍无 `seq=0`、或导出 stale、或早复位/panic/APFS/`buf_bread`，则证 l721 的停笔不是时间闸太晚而是 I/O/USB 通道中断，停止时间轮并如实封存。三种结果都把当前「33:33 是否有效」收窄成可判读事实。

## 5. 唯一变量与回滚点

唯一变量是 `E:\nabu\hold\l721\kernel.macho @0xa336a4` 的一条同长指令：`mov w8,#260`（`88208052`）→ `mov w8,#220`（`881b8052`），实际仅 `@0xa336a5` 1 个文件字节改变，文件长度 125,628,416 B 不变。kernel `e4d771f5 → e5994f1e`，payload `9170e91a → 6c290ecc`；payload 内只 `KERNEL~1.MAC` 同步，`UNIXNU~1TOM` 1126 B 与 l721/l720 逐字节相同，其余 inner files 全 SAME。`diagAt1=300 @0xa33690=88258052`、`pet=600 @0xa384f4=084b8052`、installd uid/gid `21000000/21000000`、APFS nop `1f2003d5` 全保留。args 仍 700/39，`added=[]`、`dropped=[]`；`sheng-smp=1 + nabu-wdt-arm-delay=0`、BITE / `WDT.CFG` / Mu / `hang=0` 全不动。五文件 `boot=ead4b531`、`addon=950a3968`、`adt=78a6f7e9`、`trust=a4a6bae6` 与 l721 逐字节相同。一次性临时轮、用完回滚、不进默认 args；回滚 `hold/l720`（最佳诊断/动态基线）、`hold/l721`、`hold/l719`、`hold/l718`、`hold/l714`、`hold/l700`。

## 6. 恢复预案

`AGENTS.md` 0.10 绝对红线：绝不 `fastboot flash boot_a` / `flash boot`；recovery 在 boot_a 里，写 boot 会清 misc 并丢失 TWRP。执行顺序仅用既有 `E:\nabu\tmp\go_round_next.sh l722`：recovery 写 `misc-bootrecovery.bin`，进 fastboot，`fastboot flash cust hold/l722/payload.img`，**最后** `fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin` 重 arm，再 `fastboot boot hold/l722/boot-merge-r3.img`。`arm-delay=0` 保留早 panic 复位路径；`pet=600`、`hang=0` 预期仍自动回 TWRP，无需人工长按。采集沿用现有 CDC watcher 到回 TWRP，`pull_diag.py l722 <cdc>` 按 base 拉槽（若导出大于 32 MiB 用 8700×4 KiB 完整范围），`extract_diag_tree.py` + `build_l702_arch.py` + WSL `unifiedlog_iterator` 解码；关键判据为 `NABUDIAG seq=0`、home uid/gid Fault、`Code=13/2`、`MIInstallerErrorDomain 105/4`、`SystemAppMigrator`、screen/frames、panic/APFS/`buf_bread`。若 fastboot 全失败才需人工长按电源 10 秒。门禁按 `audit_bootability` / `audit_dual` / `audit_args` / Windows Python `precheck.py` + `precheck_check.py` / `guard_no_boot_flash` 逐项落盘；设备当前 TWRP `f91fa664`，`boot_a` 全程未碰。
