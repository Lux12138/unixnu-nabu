# 上板报告 l730（一次性临时迁移完成态轮：让 `sheng-mig` 强制态不再覆盖官方 CFPreferences 完成态）

> 本包相对 `E:\nabu\hold\l727\` 只改 `sheng-mig=1` 的 264 B 强制态 plist 写入路径：把内联 ADRP 指向 `__TEXT` 全零 padding 中一条等长无害路径 `com.apple.migratiox.plist`。`setup_skip[4]` 的 365 B 官方完成态 `LastSystemVersion=21H461 / DMUserDataDisposition` 与 readback 仍写旧 `com.apple.migration.plist`，不再被强制态覆盖。`migratx` 哨兵、`norceMigrationOnNextReboot`、args/toml 700/39、五文件其余产物、WDT、双槽、APFS nop 全不变。零重编、零新增 token、零新增打印/探针、零安全放宽。用户已授权 7×24 自动推进到开机；设备只做 `flash cust` + 最后重 arm misc + `fastboot boot`，`boot_a` 不碰。

## 1. 唯一问题

在 l727 已修通 keychain/installd/WDT/双槽并把迁移链推进到 `SystemAppMigrator` 的前提下，让 `com.apple.migration` CFPreferences 域保留 365 B 官方完成态 `LastSystemVersion=21H461`、不再被 `sheng-mig=1` 的 264 B 强制态覆盖，能否让 `com.apple.datamigrator` 的 `lastBuildVersionPref` 不再读到 `(null)`，出现 `System build version unchanged / Migration not necessary`，进而完成通知与桌面交还链出现 `Migration complete / finishedStartup / Dismissing overlay / screen owner is now`，让屏离开 PUIProgressWindow？本轮不改完成态内容、迁移 token、看门狗、诊断时间闸、显示链或任何 boot-arg。

## 2. 已闭合到哪

l727 是迁移断点最前的实验包：完整 CDC 到 489.4 s、自动回 TWRP、无 panic/APFS/`buf_bread`；`LaunchServicesMigrator` 227.9 s success 1、`MSUDataMigrator` 228.0 s success 1、`SystemAppMigrator` 228.1 s 开始，但 seq1 完整 v2 os_log 183,974 行真实只到 296.1 s，只完成 `All system apps finished installing` / `Validating App Install State`，`System apps finished demoting` / `MobileInstallationSetSystemAppMigrationComplete` / `Notifying clients of completion` / `Data migration complete` 全 0，交还四标志全 0。29 条 `MIInstallerErrorDomain Code=128` 是官方正常清理 coordinator。l728 的 `pet=900` + 450 s 反序槽阴性，证纯时间常数方向已穷尽；WDT 延期本身正常但 332.6 s 停笔、363 s 自复位。

复核官方 `com.apple.datamigrator` 二进制：`0x100008ec4` 调 `lastBuildVersionPref`（stub `0x100014880` → selector @`0x19a12`），其结果在 `0x100008ef8` 传给 `isMigrationNeededWithBuildVersion:lastBuildVersion:clientPID:`；`0x100008d54 cbz x19` 与 `0x100008d90` 的 literal pool → `System build version changed from %@ to %@` @`0x1b69d` 正是 `(null)` 日志。`0x10000a510 CFPreferencesAppSynchronize` 的 literal pool 指向 `com.apple.migration @0x1bb51`（键 `DMSuppressMigrationFailureAlert @0x1bb65`）；`0x1b764` 是 `System build version unchanged / Migration not necessary`。因此 `com.apple.migration.plist` 是判定链 A；DSC `.25` 的 DMContext `/var/mobile/.../com.apple.DataMigration.plist @0x4144ddf` 是相邻但不同的链 B。旧 l729 候选把完成态移去 B 会让 A 只剩强制态，方向较差，已 NO-GO、预检戳作废，未上板。

板上 `kern_exec.c:8355-8379` 先写 365 B 完成态，随后 `:8602-8640` 的 `sheng-mig=1` 用 264 B `norceMigrationOnNextReboot / runSetupAssistant` 覆盖同一旧路径；这正是 `lastBuildVersionPref` 读到 `(null)` 的最直接解释。l727 kernel 中旧路径 C 串唯一 @file `0x11175b` / va `0xfffffff00711575b`（65+NUL），被 `setup_skip[4]` 完成态写入/readback 与 `sheng-mig` 强制态写入共用。反汇编：`setup_skip[4]` 写入在 `0xfffffff007779038 adrp x23,0xfffffff007115000` + `0xfffffff00777903c add x23,x23,#0x75b`，readback 从数组基址 `0xfffffff0071f6490` 取同一路径；`sheng-mig` 强制态写入另在 `0xfffffff0077798e0 adrp x0,0xfffffff007115000` + `0xfffffff0077798e4 add x0,x0,#0x75b`。若直接把 C 串改一个字符会同时改两边，故 l730 在 `__TEXT` load commands 结束 `0x16b0` 到 `__const` 起点 `0x5180` 的全零 padding 中 @file `0x175b` / va `0xfffffff00700575b` 放一条等长 66 B 无害路径 `com.apple.migratiox.plist\0`，只把强制态 ADRP 从 `e0ccff90` 改为 `60c4ff90`（页 `0xfffffff007115000`→`0xfffffff007005000`），保留 `ADD #0x75b` 指向新串；完成态写入/readback ADRP、数组指针、旧 C 串全部不动。反汇编正控：完成态仍 `adrp x23,0xfffffff007115000`，强制态已 `adrp x0,0xfffffff007005000`。

官方 `com.apple.datamigrator.xpc/Info.plist` 的 `_RoleAccount=mobile` 与内核 `0600/501/501` 写属主相容；`/var→/private/var` 官方 symlink、`private/var/mobile/Library/Preferences` 目录 `drwxr-xr-x`。Mach-O 25 条 load command 无 `LC_CODE_SIGNATURE`，`__TEXT` file `0..0x1b4000` 覆盖全零 padding，首 section 起 `0x5180`，补丁不改 LC/段/节/长度。制品 kernel `d3598dd6`、payload `3876e1a2`，相对 l727 共 67 文件字节变化：新路径 66 字节写入原全零 padding、强制态 ADRP 1 字节；Mach-O 125,628,416 B 与 `ncmds=5776` 不变，APFS nop `1f2003d5`、`pet=600`、`diagAt0=220`、`diagAt1=300`、`migratx`、`norceMigrationOnNextReboot` 全保留。payload 内只 `KERNEL~1.MAC` 变，`UNIXNU~1TOM` 1126 B 与 l727/l723 逐字节相同；五文件其余 `boot=ead4b531`、`addon=950a3968`、`adt=78a6f7e9`、`trust=a4a6bae6` 全同。

## 3. 为什么离线不行

静态能证 `lastBuildVersionPref` 与 `com.apple.migration` 域、`(null)` 日志分支、`sheng-mig` 覆盖顺序、两处写入分离、新路径等长 padding、无代码签名、制品单变量和全套门禁。不能静态得到的是运行期：cfprefsd 是否把 365 B kernel 预写完成态同步进 `com.apple.migration` 域、`lastBuildVersionPref` 是否返回 `21H461`、`Migration not necessary` 后官方是否仍 post `com.apple.migration.didComplete` / `Data migration complete; waiting 3 seconds.`、backboardd 是否交还桌面、SpringBoard 是否 `finishedStartup`、屏是否离开 PUIProgressWindow、是否因跳过 plugin 而黑屏，以及 WDT/自动回 TWRP 是否保持。官方已有 `Migration not necessary` 分支，但静态未闭合「不跑 plugin 也发完成通知」的 xref，不能把交还当作预期；不论阳性/阴性/黑屏都是明确机制判据。现有 l727/l728 日志对「旧域保留完成态、强制态移走」组合沉默。满足 `AGENTS.md` 0.5。

## 4. 失败也有信息

- **完成态 readback 正控成立**：CDC `nabu: skip-setup / skip-read i=4` 应回读 365 B `LastSystemVersion / DMUserDataDisposition / SyntheticDidUpgrade / buildVersion`；`sheng-mig: sentinel=0 prefs=0 err=0` 仍成立。
- **`Migration not necessary` 且交还四标志/屏离进度窗**：证 CFPreferences 完成态是最后开机墙，继续沿交还链收口。
- **`Migration not necessary` 但黑屏/交还全 0**：证官方跳过 plugin 不会 post didComplete；当前「必须跑完迁移」和「完成态正确」之间存在冲突，停止本方向，回滚 l727/l723，转向 backboardd/SpringBoard 路线级改动。
- **仍 `(null) → Migration necessary`**：证 `lastBuildVersionPref` 不是从这份 plist 读、或 cfprefsd 不接受 kernel 预写文件；停止 plist 路径方向，转向编译侧/官方偏好写入链。
- **readback 不一致、`attrfail>0`、panic/APFS/`buf_bread`、早复位或双槽缺失**：按阴性回滚 l727/l723；`arm=0` + `panic-restart=2` 保持早死自动复位路径，不需人工长按。
- **系统未自动回 TWRP**：按既有 `go_round_next.sh` 的 misc re-arm / fastboot 恢复；只有 fastboot/adb 全失败才需人工长按电源 10 秒。

## 5. 唯一变量与回滚点

唯一变量是「`sheng-mig` 264 B 强制态不再覆盖 `com.apple.migration.plist` 的 365 B 官方完成态」：新无害路径 C 串 + 一处强制态 ADRP 页重指向。`setup_skip[4]` 完成态内容/路径/readback、`sheng-mig=1` token、`runSetupAssistant=true`、`migratx` 空哨兵、迁移触发、属主、keychain、WDT、双槽、args 全不变。kernel `052e1ec5→d3598dd6`、payload `7960c99c→3876e1a2`；精确 diff 为 `@0x175b` 新路径 66 字节与 `@0x7758e0` ADRP 1 字节，共 67 字节。args 仍 700/39，`added=[]`、`dropped=[]`，39 token 同序同字节；`sheng-smp=1 + nabu-wdt-arm-delay=0`、`nabu-state-ring=10` 全在。BITE / `WDT.CFG` / Mu / `NABU_WDT_BITE_MS` / `hang=0` / `autolog=0` / Keychains uid64 / installd 33:33 / APFS nop 全不动。一次性临时轮、用完回滚、不进默认 args；回滚链 `hold/l727`（迁移断点最前实验包）、`hold/l723`（最佳稳定双槽/启动基线）、`hold/l726`、`hold/l722`、`hold/l721`、`hold/l720`、`hold/l719`、`hold/l718`、`hold/l714`、`hold/l700`。

## 6. 恢复预案

`AGENTS.md` 0.10 绝对红线：绝不 `fastboot flash boot_a` / `flash boot`；recovery 在 boot_a 里，写 boot 会清 misc 并丢失 TWRP。执行顺序仅用既有 `E:\nabu\tmp\go_round_next.sh l730`：recovery 写 `misc-bootrecovery.bin`，进 fastboot，`fastboot flash cust hold/l730/payload.img`，**最后** `fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin` 重 arm，再 `fastboot boot hold/l730/boot-merge-r3.img`。`arm-delay=0` 保留早 panic 复位路径；`pet=600`、`hang=0` 预期仍自动回 TWRP、无需人工长按。采集沿用单个现有 CDC watcher 到回 TWRP；watcher 每 300 s 分段，全部 `cdc-*.bin` 必须按 mtime/内核 `t=` 顺序拼接后判读。按完整 CDC 的 `NABUDIAG base=` 拉两槽（`bs=4096 count=8800` 以覆盖 36,044,800 B 级 seq1），`extract_diag_tree.py` + 包含所有 `Special/*.tracev3` 的 archive + WSL `unifiedlog_iterator` 解码；重点核 `pet=600 arm=0`、`attr=49 attrfail=0`、`nabu: skip-setup / skip-read i=4` 的 path/keys、`sheng-mig: sentinel/readback`、`System build version changed from (null)` / `unchanged` / `Migration not necessary`、迁移完成链、`Migration complete / finishedStartup / Dismissing overlay / screen owner is now`、屏 sha/bbox、frames、panic/APFS/`buf_bread`、自动回 TWRP 时刻。若 fastboot 全失败才需人工长按电源 10 秒。门禁 `audit_bootability` / `audit_dual` / `audit_args` / Windows Python `precheck.py` + `precheck_check.py` / `guard_no_boot_flash` / 六问板报逐项落盘；设备当前 TWRP `f91fa664`，`boot_a` 全程未碰。
