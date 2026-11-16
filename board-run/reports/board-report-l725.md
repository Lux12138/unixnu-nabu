# 上板报告 l725（一次性临时扩窗轮：`diagAt1 300→400`）

> 本包相对 `E:\nabu\hold\l723\` 只改 kernel 中 `diagAt1` 默认常量的一条同长 ARM64 立即数；保留 l723 的 `pet=600`、`diagAt0=220`、installd 33:33、Keychains uid64、args/toml 700/39，零新增 token、零重编、零探针。用户 2026-11-11 授权 7×24 自动推进到能开机；设备只做 `flash cust` + 最后重 arm misc + `fastboot boot`，`boot_a` 不碰。

## 1. 唯一问题

在 l723 全部修复基础上，把第二诊断槽 `diagAt1` 从 300 s 后移到 400 s（而非 l724 的 500 s），能否取得覆盖 287–400 s 的非 stale os_log，判断 `LaunchServicesMigrator` 是否在更晚完成、后续 migrator 是否推进、`Migration complete / Dismissing overlay / screen owner is now` 是否出现？本轮不改 WDT、属主、keychain db、migration plist、staged apps 或 args，只改诊断时间闸。

## 2. 已闭合到哪

l723 17:47:35 boot、17:59:13 自动回 TWRP，约 698 s；三段 CDC 合并 7,566,528 B，`pet=600`、`data tree attr=49 attrfail=0`、内核 tick 到 666.6 s、frames 24，无 panic/APFS/`buf_bread`。官方 21H461 APFS `/private/var/Keychains uid64` 与 installd 33:33 已板上实证修通：l723 `keychain-2.db` 相关命中 75→1、`Unable to create database / Code=14 / Failed to query keychain / Migration failed` 全 0，`MIInstallerErrorDomain 105/4` 仍 0。`diagAt0=220` 在约 216.6 s 后触发 seq0，`diagAt1=300` 在约 307.5 s 触发非 stale seq1（261→319 files）。但 `l723-seq0-v1.jsonl` 只到 212.0 s、`l723-seq1-v1.jsonl` 只到 287.3 s；两槽均无 `Migration complete / Dismissing overlay / screen owner`，`LaunchServicesMigrator` 138.9 s 开始后无完成行，`seq1` 最后相关行是 259.0 s BADEDBAD stackshot。CDC/内核继续活到 666.6 s，因此 287 s 之后是否完成/交还仍是日志快照截断造成的未知。

l724 把 `diagAt1` 推到 500 s 后，第一槽 223.3 s 正常导出 29.7 MB，但第二槽 500 s 阈值前板子在 337.2 s 停笔、22:00:54 回 TWRP，`seq1` 全程 0；证明第二份约 31 MB 导出 I/O 本身是续命关键，时间闸不能放到寿命之外。静态子任务最终裁决：官方无现成跳过 LaunchServicesMigrator 的低风险机制，`.csstore` 运行期生成、不需 seed，`pkd Candidate plugin count` 10→0→0 更像 LS 重建时序；建议 `diagAt1=400`，比 287.3 s 多约 110 s 且落在历史最坏 409 s 回收线之前。源码 `NativeUfs.cpp:2912-2931` 确认只有 `diagAt0/diagAt1` 两阈值，`NativeLegacyProbe.cpp:1061/1101/1232` 确认只有 seq0/1 两槽；板上反汇编 `@0xa33690 mov w8,#300` (`88258052`)，同长改 `mov w8,#400` (`88328052`) 只差 1 字节。相邻 `@0xa336a4` 仍是 diagAt0=220，`@0xa384f4` 仍是 pet=600。

## 3. 为什么离线不行

静态能证两槽上限、指令落点、WDT/属主/第一槽全部保留、l724 阴性、官方无跳迁移开关、400 s 在历史寿命边界内；不能证明 400 s 阈值后是否有一次 I/O 完成真正 arm 第二槽、导出是否非 stale、tracev3 是否包含 287–400 s 的用户态日志，以及 `LaunchServicesMigrator` 完成/后续任务/交还标志/晚段屏是否出现。l723 是唯一同时具备全部修复和 698 s 长窗口的基线，但第二槽在 300 s 触发得过早；l724 证明 500 s 太晚；离线无法产生 400 s 组合的运行期制品，满足 `AGENTS.md` 0.5。

## 4. 失败也有信息

若 CDC 出现 `NABUDIAG seq=0/1`，且回 TWRP 后 `seq1` os_log 尾显著超过 287.3 s，就直接判读 `completed migration for ...LaunchServicesMigrator` / 后续 migrator / `Migration complete / Dismissing overlay / screen owner is now / finishedStartup` / 晚段屏。若第二槽仍只到约 287 s 或与 l723 seq1 逐块相同，则证明诊断导出只抓固定 tracev3 窗口，而不是时间闸太晚；停止诊断时间轮并如实封存。若 400 s 前早复位、panic/APFS/`buf_bread` 或双槽缺失，则按阴性回滚 l723，不再加时。三种结果都把「287 s 之后到底有没有推进」收窄成可判读事实。

## 5. 唯一变量与回滚点

唯一变量是 `E:\nabu\hold\l723\kernel.macho @0xa33690` 的一条同长指令：`mov w8,#300`（`88258052`）→ `mov w8,#400`（`88328052`），实际 `@0xa33691` 1 个文件字节改变，文件长度 125,628,416 B 不变。kernel `fc917183 → b49bd30b`，payload `fa9a1d42 → 7a6faeb6`；payload 内只 `KERNEL~1.MAC` 同步，`UNIXNU~1TOM` 1126 B 与 l723 逐字节相同，其余 inner files 全 SAME。`diagAt0=220 @0xa336a4=881b8052`、`pet=600 @0xa384f4=084b8052`、Keychains uid `40000000`、installd uid/gid `21000000/21000000`、APFS nop `1f2003d5` 全保留。args 仍 700/39，`added=[]`、`dropped=[]`；`sheng-smp=1 + nabu-wdt-arm-delay=0`、BITE / `WDT.CFG` / Mu / `hang=0` 全不动。五文件 `boot=ead4b531`、`addon=950a3968`、`adt=78a6f7e9`、`trust=a4a6bae6` 与 l723 逐字节相同。一次性临时轮、用完回滚、不进默认 args；回滚 `hold/l723`（最佳启动/诊断基线）、`hold/l722`、`hold/l721`、`hold/l720`、`hold/l719`、`hold/l718`、`hold/l714`、`hold/l700`。

## 6. 恢复预案

`AGENTS.md` 0.10 绝对红线：绝不 `fastboot flash boot_a` / `flash boot`；recovery 在 boot_a 里，写 boot 会清 misc 并丢失 TWRP。执行顺序仅用既有 `E:\nabu\tmp\go_round_next.sh l725`：recovery 写 `misc-bootrecovery.bin`，进 fastboot，`fastboot flash cust hold/l725/payload.img`，**最后** `fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin` 重 arm，再 `fastboot boot hold/l725/boot-merge-r3.img`。`arm-delay=0` 保留早 panic 复位路径；`pet=600`、`hang=0` 预期仍自动回 TWRP，无需人工长按。采集沿用现有 CDC watcher 到回 TWRP，`pull_diag.py l725 <cdc>` 按 base 拉槽（导出大于 32 MiB 用 8700×4 KiB 完整范围），`extract_diag_tree.py` + `build_l702_arch.py` + WSL `unifiedlog_iterator` 解码；关键判据为 `NABUDIAG seq=0/1`、seq1 os_log 尾、`attr=49 attrfail=0`、keychain/Migration failed 是否仍 0、LaunchServicesMigrator 完成与后续任务、交还标志、screen/frames、panic/APFS/`buf_bread`。若 fastboot 全失败才需人工长按电源 10 秒。门禁按 `audit_bootability` / `audit_dual` / `audit_args` / Windows Python `precheck.py` + `precheck_check.py` / `guard_no_boot_flash` 逐项落盘；设备当前 TWRP `f91fa664`，`boot_a` 全程未碰。
