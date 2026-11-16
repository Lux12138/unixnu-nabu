# 上板报告 l727（一次性临时迁移触发轮：失效 `/private/var/datamigrator/migrate` 空哨兵路径）

> 本包相对 `E:\nabu\hold\l726\` 只改 kernel 字符串表一个字节：`/private/var/datamigrator/migrate` → 等长 `/private/var/datamigrator/migratx`。l726 的完成态 plist 修复保留；`sheng-mig=1`、args/toml 700/39、WDT/双槽/属主全不变，零重编、零探针。用户授权 7×24 自动推进到能开机；设备只做 `flash cust` + 最后重 arm misc + `fastboot boot`，`boot_a` 不碰。

## 1. 唯一问题

在 l726 已证实「失效 forceMigration 键只改机制、不交还」的基础上，再让 `sheng-mig=1` 不再重建官方 `/private/var/datamigrator/migrate` 空哨兵，能否让 datamigrator 不再判定 `Migration sentinel present. Migration necessary.`，从而推进到 `LaunchServicesMigrator` 完成、`Migration complete / finishedStartup / Dismissing overlay / screen owner is now`，让屏离开 PUIProgressWindow？本轮不改 WDT、属主、keychain、诊断时间闸、args，只改这一个哨兵路径字节。

## 2. 已闭合到哪

l723 是最佳基线：约 698 s 自动回 TWRP、双槽非 stale、内核 tick 666.6 s；官方 Keychains uid64 / installd 33:33 / WDT pet600 / diagAt0=220 / diagAt1=300 全修通，`Code=14 / Failed to query keychain / Migration failed / MIInstallerErrorDomain 105/4` 全 0。但 `seq1` os_log 只到 287.3 s，屏仍 PUIProgressWindow；287 s 后 CDC 仍持续 spawn 到 666.6 s，说明系统没停，停的是用户态日志归档与迁移完成链。l724/l725 证明后移第二槽 500/400 s 会丢掉 300 s 续命 I/O，停止时间轮。

l726 把 `nabu_mig_force[]` 的 `forceMigrationOnNextReboot` 改等长不识别键后，板上 CDC 正控读到 365 B 官方完成态 plist（`LastSystemVersion / DMUserDataDisposition / SyntheticDidUpgrade / buildVersion`），`forceMigrationOnNextReboot` 0 命中；双槽仍 220/300 s 稳定触发，keychain/MIInstaller/Migration failed 仍全 0。但 `LaunchServicesMigrator` 仍无完成行、`Migration complete / Dismissing overlay / screen owner / finishedStartup` 全 0，屏 `862ba602…` 仍同一 PUIProgressWindow。静态裁决指出官方 `com.apple.datamigrator` 真正消费的是 Darwin notification `com.apple.datamigrator.forceMigrationOnNextReboot`、空哨兵路径 `/private/var/datamigrator/migrate` 与 `Migration sentinel present. Migration necessary.`，裸 plist key 单独失效不足以取消强制迁移；推荐「不重建空哨兵」为唯一有机制的单变量。

源码 `kern_exec.c:8602-8630` 的 `sheng-mig=1` 分支同时写空哨兵与 plist；官方 21H461 `/System/Library/PrivateFrameworks/DataMigration.framework/.../com.apple.datamigrator` 二进制里 `/private/var/datamigrator/migrate` 唯一，`Migration sentinel present. Migration necessary.` 与 `Failed to remove migration sentinel with error` 同段，证明哨兵是官方消费点。板上 `hold/l726/kernel.macho` 中该路径也唯一 @`0x111b73`，后接 NUL；改为 `/private/var/datamigrator/migratx` 全文件 0 命中，等长、只差末字符。历史 l714 证 `sheng-mig=1` 是 PreBoard 触发，l717 明记不删 token；本轮保留 token 与 plist 写入，只让哨兵落到无害路径。FTABHarvest 报错是跨轮背景噪声，AXSpringBoardServer 缺失是 SpringBoard 自身注册状态，不是本轮变量。

## 3. 为什么离线不行

静态能证官方哨兵消费者、路径唯一性、等长补丁与 l726 机制正控；不能证明运行期 `sheng-mig=1` 写 `migratx` 后 datamigrator 是否完全不再看到 sentinel、是否仍因 Darwin notification / 其它状态开进度窗、`LaunchServicesMigrator` 是否完成、后续 migrator 是否推进、`Migration complete / finishedStartup / Dismissing overlay / screen owner is now` 是否出现，屏是否离开 PUIProgressWindow，是否仍自动回 TWRP。l726 是唯一同时具备完成态 plist 与双槽/长窗口的基线，但没有哨兵失效组合；运行期结果必须上板，满足 `AGENTS.md` 0.5。

## 4. 失败也有信息

若 CDC 中 `sheng-mig: sentinel/readback` 仍成功但指的是 `migratx`，且 os_log 出现 `LaunchServicesMigrator` 完成、后续 migrator、`Migration complete / Dismissing overlay / screen owner is now / finishedStartup` 或屏离开 PUIProgressWindow，则空哨兵是最后强制源；继续沿交还链推进。若仍 PUIProgressWindow、LaunchServicesMigrator 无完成行，但 keychain/MIInstaller 仍 0，则明确 Darwin notification 或 SpringBoard 初始化才是真正剩余墙，停止迁移触发方向并如实收敛。若 `attrfail>0`、panic/APFS/`buf_bread`、早复位或双槽缺失，则按阴性回滚 l723，不再加时。三种结果都把「迁移为何不完成」收窄成可判读事实。

## 5. 唯一变量与回滚点

唯一变量是 `E:\nabu\hold\l726\kernel.macho @0x111b93` 一个 ASCII 字节 `0x65`('e') → `0x78`('x')，把 `/private/var/datamigrator/migrate` 改成等长 `/private/var/datamigrator/migratx`；Mach-O 长度 125,628,416 B 不变，l726 的 `norceMigrationOnNextReboot`、APFS nop `1f2003d5`、pet600、diagAt0/1、installd33、Keychains uid64 全不动。kernel `9f410f46 → 052e1ec5`，payload `f9b528ac → 7960c99c`；payload 内只 `KERNEL~1.MAC` 同步，`UNIXNU~1TOM` 1126 B 与 l726/l723 逐字节相同，其余 inner files 全 SAME。args 仍 700/39，`added=[]`、`dropped=[]`；`sheng-mig=1` 与 `sheng-smp=1 + nabu-wdt-arm-delay=0` 硬配对全在，BITE / `WDT.CFG` / Mu / `hang=0` 全不动。五文件 `boot=ead4b531`、`addon=950a3968`、`adt=78a6f7e9`、`trust=a4a6bae6` 与 l726 逐字节相同。一次性临时轮、用完回滚、不进默认 args；回滚 `hold/l723`（最佳启动/诊断基线）、`hold/l722`、`hold/l721`、`hold/l720`、`hold/l719`、`hold/l718`、`hold/l714`、`hold/l700`。

## 6. 恢复预案

`AGENTS.md` 0.10 绝对红线：绝不 `fastboot flash boot_a` / `flash boot`；recovery 在 boot_a 里，写 boot 会清 misc 并丢失 TWRP。执行顺序仅用既有 `E:\nabu\tmp\go_round_next.sh l727`：recovery 写 `misc-bootrecovery.bin`，进 fastboot，`fastboot flash cust hold/l727/payload.img`，**最后** `fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin` 重 arm，再 `fastboot boot hold/l727/boot-merge-r3.img`。`arm-delay=0` 保留早 panic 复位路径；`pet=600`、`hang=0` 预期仍自动回 TWRP，无需人工长按。采集沿用现有 CDC watcher 到回 TWRP，`pull_diag.py l727 <cdc>` 按 base 拉槽（导出大于 32 MiB 用 8700×4 KiB 完整范围），`extract_diag_tree.py` + `build_l702_arch.py` + WSL `unifiedlog_iterator` 解码；关键判据为 `NABUDIAG seq=0/1`、`sheng-mig: sentinel/readback`、`attr=49 attrfail=0`、keychain/Migration failed 是否仍 0、LaunchServicesMigrator/后续 migrator 完成行、`Migration complete / finishedStartup / Dismissing overlay / screen owner is now`、screen/frames、panic/APFS/`buf_bread`。若 fastboot 全失败才需人工长按电源 10 秒。门禁按 `audit_bootability` / `audit_dual` / `audit_args` / Windows Python `precheck.py` + `precheck_check.py` / `guard_no_boot_flash` 逐项落盘；设备当前 TWRP `f91fa664`，`boot_a` 全程未碰。
