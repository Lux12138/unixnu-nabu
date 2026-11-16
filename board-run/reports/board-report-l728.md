# 上板报告 l728（一次性临时晚窗轮：保留 300 s 续命槽，反序取 450 s 用户态日志）

> 本包相对 `E:\nabu\hold\l727\` 只改 kernel 中两条同长 ARM64 `movz w8,#imm` 立即数：`petSeconds 600→900`、`diagAt0 220→450`；`diagAt1=300`、args/toml 700/39、五文件其余产物、APFS nop、WDT `arm-delay=0` 逐字节不变。零重编、零新增打印/探针、零 args 改动、零安全放宽。用户已授权 7×24 自动推进到开机、必要时延后看门狗并使用临时诊断轮；设备只做 `flash cust` + 最后重 arm misc + `fastboot boot`，`boot_a` 不碰。

## 1. 唯一问题

在 l727 已把迁移链推进到 `SystemAppMigrator` 开始、但 300 s 诊断导出只截到真实 296.1 s 的前提下，保留 300 s 第二槽约 36 MB 导出 I/O 续命、把独立的第一槽移到 450 s 并把喂狗截止从 600 s 延到 900 s，能否取得覆盖 296–450 s 的非 stale 用户态 os_log / 屏快照，从而判定 `SystemAppMigrator` 是否完成、是否出现 `Migration complete / finishedStartup / Dismissing overlay / screen owner is now`、屏是否离开 PUIProgressWindow？本轮不改迁移 plist、哨兵、Keychains、installd、显示链或任何 boot-arg。

## 2. 已闭合到哪

l727 完整 CDC 是 `logs/l727-cdc-merged.bin`（4,752,943 B，sha8 `c4f24baa`；第一段 `cdc-224600.bin` 0–337.9 s，第二段 `cdc-225101.bin` 343.3–489.4 s）。CDC 明确 `pet=600 arm=0 hang=0 autolog=0`、`attr=49 attrfail=0`，`NABUDIAG1/2 seq=0` 在 216.6 s、`seq=1` 在 297.4 s 都非 stale；22:46:00 boot、22:53:54 自动回 TWRP `f91fa664`，无 `panic:` / `panic(` / APFS unencrypted / `buf_bread`，无需人工长按。看门狗延期按历史 `feedback-dont-touch-wdt-timing.md` 与源码落在 `petSeconds` 截止，不改 BITE / `WDT.CFG` / Mu / `NABU_WDT_BITE_MS` / `arm-delay=0`。

`logs/l727-seq0-v1.jsonl` 92,326 行、真实 38.2–211.3 s；`logs/l727-seq1-v1.jsonl` 172,201 行、真实 38.2–296.1 s；把两份 `Special/*.tracev3` 全放进 archive 后的 `logs/l727-seq1-v2.jsonl` 183,974 行，真实仍 38.2–296.1 s，`SystemAppMigrator` 3,306 条，但完成行仍只有 ContainerMigrator 139.3 s、LaunchServicesMigrator 227.9 s、MSUDataMigrator 228.0 s 三条；`Migration complete / Dismissing overlay / screen owner / finishedStartup` 全 0。273.9–274.0 s 的 29 条 `MIInstallerErrorDomain Code=128 "Intentional Cancel from SystemAppMigrator"` 是官方主动取消，不是旧 Code=105/4 staging 失败。屏 `f56d5c96…` 仍 PUIProgressWindow，frames 到 495 说明显示链继续提交。现有日志只能证「296.1 s 时 SystemAppMigrator 仍在跑」，不能证 296–489 s 是否完成/交还。

l724/l725 已实证：把 `diagAt1` 从 300 单独后移到 500/400 会丢掉 300 s 的第二份导出 I/O，导致 337.2/280.5 s 早复位且 seq1 为 0。l723 在同 `pet=600`、220/300 双槽下活到 698 s、内核 tick 666.6 s；l727 在 l726/l727 迁移触发补丁下活到 489.4 s。历史 l406 的 470/505 双槽能落地、l407 的 545/590 全不落地，也把安全晚窗上界定在约 500 s。故 l728 取 450 s，落在 l727 已观测活动窗口内，且保留 300 s 续命 I/O。

静态链已闭合：`/root/merge/out-v6/.../drivers/ShengUfs/NativeUfs.cpp:4411-4416` 解析 `nabu-wdt-pet-until` 并赋给 `gWdtPetSeconds`；`:3271-3289` 用 `clock_interval_to_deadline(gWdtPetSeconds,...)` 设 `gWdtPetUntil`，到点 `nabu_wdt_pet()` 停止写 EN，硬件咬合时长本身不变；`:4092-4117` 的已有 log thread 每 2 s 无条件 `nabu_wdt_pet()`，保证空闲系统仍被喂到截止。`:2912-2931` 两个独立阈值 `diagAt0/diagAt1` 分别 `nabu_diag_dump_arm(0/1)`，没有 `at0<at1` 约束；`/root/merge/out-v6/.../kits/sheng/NativeLegacyProbe.cpp:1061` 只有 `g_nabu_diag_done[2]`，`:1101/:1230-1237` 按 seq 独立一次性执行，`:1110` 让 seq0 用 region base、seq1 用 `base+32768` 个 4 KiB 块（+128 MiB），两槽物理区不重叠。450 s 时 `arm(0)` 先写 `g_nabu_diag_pending=0`，随后 `arm(1)` 因 `g_nabu_diag_done[1]` 已置 1 而立即返回，不会覆盖 seq0。`BootDiagExport.c:215,271` 每次 `boot_diag_scan()` 都会按当前 GOP 映射导出 `screen.bgrx`，所以后触发的 seq0 仍可带 450 s 屏快照。

板上 kernel 反汇编/字节定位：`@0xa384f4` 是 `mov w8,#600`（`084b8052`），同长改 `mov w8,#900`（`88708052`）；`@0xa336a4` 是 `mov w8,#220`（`881b8052`），同长改 `mov w8,#450`（`48388052`）；`@0xa33690` 仍 `mov w8,#300`（`88258052`）；APFS nop `@0x6cd2f68=1f2003d5` 不变。相对 l727 实际只有 4 个文件字节变化（两条指令各 2 字节），Mach-O 125,628,416 B、`ncmds=5776`、无 LC_CODE_SIGNATURE 改动。制品 kernel `9b141ea0`、payload `b5f09f4c`，payload 内只 `KERNEL~1.MAC` 变、`UNIXNU~1TOM` 1126 B 与 l727/l723 逐字节相同；五文件其余 `boot=ead4b531`、`addon=950a3968`、`adt=78a6f7e9`、`trust=a4a6bae6` 全同。

## 3. 为什么离线不行

静态能证 WDT 截止/喂狗链、两个独立一次性槽、反序 arm 不被 seq1 覆盖、两槽 UFS 区域不重叠、450 s 落在 l727 的 489.4 s 实测活动窗口内，以及全套制品字节差异。静态不能产生的是运行期量：300 s 第二槽导出后系统是否仍会持续产生 I/O 到 450 s、450 s 的 `nabu_diag_dump_arm(0)` 是否真被 I/O 完成路径触发、seq0 的 epoch/头是否非 stale、tracev3 是否把 296–450 s 用户态日志归档进去、`SystemAppMigrator` 是否完成、交还四标志/屏是否出现，以及 `pet=900` 是否改变本轮自动回 TWRP 的时刻。现有 l723/l727 日志对「296–450 s 用户态内容」沉默；l727 seq1 v2 含全部 tracev3 仍止于 296.1 s。满足 `AGENTS.md` 0.5：这是验证静态补丁在真机上是否产生新的晚窗状态的一次运行期判定。

## 4. 失败也有信息

- **双槽齐全且 seq0 日志尾 >296.1 s**：得到 296–450 s 真实判据。若出现 `completed migration for ... SystemAppMigrator`、`Migration complete / finishedStartup / Dismissing overlay / screen owner is now` 或屏离开 PUIProgressWindow，则证上一轮只是日志快照截断，继续沿交还链推进；若仍只有 SystemAppMigrator placeholder/stall 且交还全 0，则证任务确实在 450 s 仍未完成，墙在 SystemAppMigrator 或其下游，停止纯时间窗方向。
- **450 s seq0 未触发或 stale，但 300 s seq1 正常**：证明即使 `pet=900`，本配置在 300 s 后缺足够 I/O 或另有约 489 s 复位源；不再堆时间窗，回滚 l723，并转向复位源/日志归档机制。
- **300 s seq1 也缺失、早复位、panic/APFS/`buf_bread`、`attrfail>0`**：按阴性回滚 l723，不再加时；`arm=0` + `panic-restart=2` 保持早死自动复位路径，不需人工长按。
- **系统未自动回 TWRP**：先按既有 `go_round_next.sh` 的 misc re-arm 与 fastboot 恢复流程判定；只有 fastboot/adb 全失败才需人工长按电源 10 秒。

## 5. 唯一变量与回滚点

唯一变量组是「为取得一个晚窗而必须同时保留续命与延截止」的两条已有时间常数：`petSeconds 600→900` 与 `diagAt0 220→450`；`diagAt1=300` 必须保持，属已验证约束而非第二个功能变量。kernel `052e1ec5→9b141ea0`、payload `7960c99c→b5f09f4c`；diff 精确为 `@0xa336a4/@0xa336a5` 与 `@0xa384f4/@0xa384f5` 共 4 字节。args 仍 700/39，`added=[]`、`dropped=[]`，39 token 同序同字节；`sheng-mig=1`、`sheng-smp=1 + nabu-wdt-arm-delay=0`、`nabu-state-ring=10`、`offline-key-events=1` 全在。BITE / `WDT.CFG` / Mu / `NABU_WDT_BITE_MS` / `hang=0` / `autolog=0` / Keychains uid64 / installd 33:33 / 完成态 plist / `migratx` / APFS nop 全不动。一次性临时轮、用完回滚、不进默认 args；回滚链 `hold/l723`（最佳启动/诊断基线）、`hold/l727`（迁移断点最前实验包）、`hold/l726`、`hold/l722`、`hold/l721`、`hold/l720`、`hold/l719`、`hold/l718`、`hold/l714`、`hold/l700`。

## 6. 恢复预案

`AGENTS.md` 0.10 绝对红线：绝不 `fastboot flash boot_a` / `flash boot`；recovery 在 boot_a 里，写 boot 会清 misc 并丢失 TWRP。执行顺序仅用既有 `E:\nabu\tmp\go_round_next.sh l728`：recovery 写 `misc-bootrecovery.bin`，进 fastboot，`fastboot flash cust hold/l728/payload.img`，**最后** `fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin` 重 arm，再 `fastboot boot hold/l728/boot-merge-r3.img`。`arm-delay=0` 保留早 panic 复位路径；`pet=900` 只延后喂狗截止，硬件咬合时长不变，预期仍自动回 TWRP、无需人工长按。采集沿用单个现有 CDC watcher 到回 TWRP；watcher 每 300 s 分段，全部 `cdc-*.bin` 必须按 mtime/内核 `t=` 顺序拼接后判读，不能只看 `logs/l728-cdc.bin` 头段。按完整 CDC 的 `NABUDIAG base=` 拉两槽（`bs=4096 count=8800` 以覆盖 36,044,800 B 级 seq1），`extract_diag_tree.py` + 包含所有 `Special/*.tracev3` 的 archive + WSL `unifiedlog_iterator` 解码；重点核 `NABUDIAG seq=0/1` 的 epoch/头、`pet=900 arm=0`、`attr=49 attrfail=0`、seq0 os_log 尾、`SystemAppMigrator` 完成行与交还四标志、屏 sha/bbox、frames、panic/APFS/`buf_bread`、自动回 TWRP 时刻。若 fastboot 全失败才需人工长按电源 10 秒。门禁 `audit_bootability` / `audit_dual` / `audit_args` / Windows Python `precheck.py` + `precheck_check.py` / `guard_no_boot_flash` / 六问板报逐项落盘；设备当前 TWRP `f91fa664`，`boot_a` 全程未碰。
