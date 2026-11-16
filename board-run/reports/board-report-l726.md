# 上板报告 l726（一次性临时迁移触发轮：失效 `forceMigrationOnNextReboot` 键）

> 本包相对 `E:\nabu\hold\l723\` 只改 kernel 内 `nabu_mig_force[]` 的一个字节：`forceMigrationOnNextReboot` → 等长不识别键 `norceMigrationOnNextReboot`（f→n）。264 B plist C 串、两个 `<true/>`、`runSetupAssistant`、空哨兵写入、`sheng-mig=1`、args/toml 700/39 全部不变，零重编、零探针。用户授权 7×24 自动推进到能开机；设备只做 `flash cust` + 最后重 arm misc + `fastboot boot`，`boot_a` 不碰。

## 1. 唯一问题

在 l723 全部修复基础上，只让官方 `datamigrator` 不再从 `com.apple.migration.plist` 读到 `forceMigrationOnNextReboot` 强制键（仍保留 `sheng-mig=1`、空哨兵与 runSetupAssistant），能否让 DataMigration 状态从「每次重开」推进到完成/交还，出现 `Migration complete / finishedStartup / Dismissing overlay / screen owner is now`，或至少让 `LaunchServicesMigrator` 完成并进入后续 migrator？本轮不改 WDT、属主、keychain、诊断时间闸、keychain db、staged apps 或 args，只改这一个键名的一个字节。

## 2. 已闭合到哪

l723 是最佳基线：17:47:35 boot、17:59:13 自动回 TWRP 约 698 s；三段 CDC 7,566,528 B，`pet=600`、`data tree attr=49 attrfail=0`、内核 tick 到 666.6 s、frames 24，无 panic/APFS/`buf_bread`。官方 21H461 `/private/var/Keychains uid64` 与 installd 33:33 已修通 `Code=14 / Failed to query keychain / Migration failed / MIInstallerErrorDomain 105/4` 全 0。`diagAt0=220` 约 216.6 s、`diagAt1=300` 约 307.5 s 双槽非 stale；`seq0` os_log 到 212.0 s、`seq1` 到 287.3 s。两槽均无 `Migration complete / Dismissing overlay / screen owner`，LaunchServicesMigrator 138.9 s 开始后无完成行。287 s 后 CDC 仍持续 spawn 到 666.6 s，说明系统没停，停的是用户态 os_log 归档快照。

l724 把 `diagAt1` 推到 500 s：seq0 正常但 seq1 0、337.2 s 停笔；l725 推到 400 s：seq0 正常但 seq1 0、280.5 s 停笔。⇒ 300 s 第二槽 I/O 是续命临界点，不能再改时间闸。静态裁决确认只有 seq0/1 两槽、官方无跳过 LaunchServicesMigrator 的低风险机制；FTABHarvest 报错是 l714/l717/l720/l722/l723 跨轮背景噪声，不是新根因。

**本轮机制链**：板上 `kern_exec.c:8355-8375` 每轮先写完成态 `com.apple.migration.plist`（`LastSystemVersion=21H461`、`DMUserDataDisposition{SyntheticDidUpgrade=false,buildVersion=21H461}`），随后 `kern_exec.c:8602-8630` 的 `sheng-mig=1` 分支用 `nabu_mig_force[]` 覆盖同一路径为 `forceMigrationOnNextReboot=true / runSetupAssistant=true`，并重建 `/private/var/datamigrator/migrate` 空哨兵；注释明说 migrator 完成后会删哨兵、页面只出现一次。官方 21H461 `/System/Library/PrivateFrameworks/DataMigration.framework/.../com.apple.datamigrator` 二进制里 `com.apple.datamigrator.forceMigrationOnNextReboot`、`forceMigrationOnNextRebootWithUserDataDisposition:`、`/private/var/datamigrator/migrate`、`Failed to remove migration sentinel with error`、`com.apple.migration.didComplete` 全部存在，证实键与哨兵都是官方消费者。历史 l714 证 `sheng-mig=1` 是进入 PreBoard 白页触发条件，l716 已把 `runSetupAssistant=false` 做阴性，l717 明记「sheng-mig=1 绝不删」；故不能直接删 token 或翻 runSetupAssistant。等长改键名可让 plist 不再携带官方识别的强制键，同时保留全部已证行为链。

## 3. 为什么离线不行

静态能证官方键/哨兵消费者、板上两次写同一 plist 的顺序、历史阴性和补丁唯一性；不能证明运行期 cfprefsd/datamigrator 是否仍因哨兵或其它状态重开迁移、键名改错后是否触发异常偏好解析、是否出现 `Migration complete / finishedStartup / Dismissing overlay / screen owner is now`、LaunchServicesMigrator 是否完成、屏是否离开 PUIProgressWindow、是否仍自动回 TWRP。l723 是唯一同时具备全部修复和 698 s 长窗口的基线，没有「强制键失效」组合；运行期结果必须上板，满足 `AGENTS.md` 0.5。

## 4. 失败也有信息

若 `Migration complete / LaunchServicesMigrator completed / MSUDataMigrator / SystemAppMigrator` 任一组出现，或 `finishedStartup / Dismissing overlay / screen owner is now` 出现、屏离开 PUIProgressWindow，则强制键是迁移不完成的根因之一；继续按该链推进。若 os_log 仍只有 PUIProgressWindow、LaunchServicesMigrator 无完成行，但 keychain/MIInstaller 仍 0，则明确哨兵或其它状态足以强制迁移，停止改 plist 键名并回滚。若 `attrfail>0`、panic/APFS/`buf_bread`、早复位或双槽缺失，则按阴性回滚 l723，不再加时。三种结果都把「迁移为何不完成」收窄成可判读事实。

## 5. 唯一变量与回滚点

唯一变量是 `E:\nabu\hold\l723\kernel.macho @0x2289e` 一个 ASCII 字节 `0x66`('f') → `0x6e`('n')，把内嵌 264 B `nabu_mig_force[]` 的键从 `forceMigrationOnNextReboot` 改成等长 `norceMigrationOnNextReboot`；Mach-O 长度 125,628,416 B 不变，APFS nop `1f2003d5`、pet600、diagAt0/1、installd33、Keychains uid64 全不动。kernel `fc917183 → 9f410f46`，payload `fa9a1d42 → f9b528ac`；payload 内只 `KERNEL~1.MAC` 同步，`UNIXNU~1TOM` 1126 B 与 l723 逐字节相同，其余 inner files 全 SAME。args 仍 700/39，`added=[]`、`dropped=[]`；`sheng-mig=1` 与 `sheng-smp=1 + nabu-wdt-arm-delay=0` 硬配对全在，BITE / `WDT.CFG` / Mu / `hang=0` 全不动。五文件 `boot=ead4b531`、`addon=950a3968`、`adt=78a6f7e9`、`trust=a4a6bae6` 与 l723 逐字节相同。一次性临时轮、用完回滚、不进默认 args；回滚 `hold/l723`（最佳启动/诊断基线）、`hold/l722`、`hold/l721`、`hold/l720`、`hold/l719`、`hold/l718`、`hold/l714`、`hold/l700`。

## 6. 恢复预案

`AGENTS.md` 0.10 绝对红线：绝不 `fastboot flash boot_a` / `flash boot`；recovery 在 boot_a 里，写 boot 会清 misc 并丢失 TWRP。执行顺序仅用既有 `E:\nabu\tmp\go_round_next.sh l726`：recovery 写 `misc-bootrecovery.bin`，进 fastboot，`fastboot flash cust hold/l726/payload.img`，**最后** `fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin` 重 arm，再 `fastboot boot hold/l726/boot-merge-r3.img`。`arm-delay=0` 保留早 panic 复位路径；`pet=600`、`hang=0` 预期仍自动回 TWRP，无需人工长按。采集沿用现有 CDC watcher 到回 TWRP，`pull_diag.py l726 <cdc>` 按 base 拉槽（导出大于 32 MiB 用 8700×4 KiB 完整范围），`extract_diag_tree.py` + `build_l702_arch.py` + WSL `unifiedlog_iterator` 解码；关键判据为 `NABUDIAG seq=0/1`、`attr=49 attrfail=0`、keychain/Migration failed 是否仍 0、`forceMigrationOnNextReboot` 是否不再回显、LaunchServicesMigrator/后续 migrator 完成行、`Migration complete / finishedStartup / Dismissing overlay / screen owner is now`、screen/frames、panic/APFS/`buf_bread`。若 fastboot 全失败才需人工长按电源 10 秒。门禁按 `audit_bootability` / `audit_dual` / `audit_args` / Windows Python `precheck.py` + `precheck_check.py` / `guard_no_boot_flash` 逐项落盘；设备当前 TWRP `f91fa664`，`boot_a` 全程未碰。
