# 上板报告 l729（一次性临时迁移完成态轮：写回官方 `com.apple.DataMigration.plist` 域）

> ⛔ **本候选已暂缓上板（2026-11-12 00:45 静态复核后 NO-GO）**：以下原方案把 `setup_skip[4]` 完成态移到 `com.apple.DataMigration.plist`，但官方 `com.apple.datamigrator` 的 `CFPreferencesAppSynchronize` 与 `lastBuildVersionPref` 指向 `com.apple.migration` 域；`(null)` 更可能由 `sheng-mig=1` 用 264 B 强制态覆盖旧域的 365 B 完成态造成。继续上板此包方向可能反了。保留报告/包作审计记录，不刷写；预检戳已作废。

## 1. 唯一问题

在 l727 已修通 keychain/installd/WDT/双槽并把迁移链推进到 `SystemAppMigrator` 的前提下，把官方完成态 `nabu_mig[]` 写到 DataMigration.framework 真正消费的 `com.apple.DataMigration.plist` 正确域（而不是上游 seed 的 `com.apple.migration.plist`），能否消除 `System build version changed from (null) to 21H461. Migration necessary.`，让迁移完成通知与桌面交还链出现 `Migration complete / finishedStartup / Dismissing overlay / screen owner is now`，让屏离开 PUIProgressWindow？本轮不改迁移触发、看门狗、诊断时间闸、显示链或任何 boot-arg。

## 2. 已闭合到哪

l727 是迁移断点最前的实验包：`LaunchServicesMigrator` 227.9 s success 1、`MSUDataMigrator` 228.0 s success 1、`SystemAppMigrator` 228.1 s 开始；完整 CDC 到 489.4 s、自动回 TWRP、无 panic/APFS/`buf_bread`。但 seq1 os_log 到 296.1 s 仍只有 `All system apps finished installing` / `Validating App Install State`，没有 `System apps finished demoting` / `MobileInstallationSetSystemAppMigrationComplete` / `Notifying clients of completion` / `Data migration complete; waiting 3 seconds.`；交还四标志全 0。29 条 `MIInstallerErrorDomain Code=128` 是官方安装前清理 coordinator 的正常分支，不是失败。l728 证明单纯 `pet=900` + 450 s 反序槽没有制造晚窗，看门狗延期本身正常但 332.6 s 停笔、363 s 自复位，停止纯时间常数方向。

官方 DSC `.25` 的 `DMContext` 字符串簇中 `LastSystemVersion @0x4144be7`、`DMUserDataDisposition @0x4144c10`、`buildVersion @0x4144d4f`、`/var/mobile/Library/Preferences/com.apple.DataMigration.plist @0x4144ddf`、`Couldn't chown the preferences file to the mobile user` 相邻；官方 `com.apple.datamigrator.xpc/Info.plist` 的 `_RoleAccount=mobile` 与内核 `nabu_kern_write_file_owner(...,0600,501,501)` 相容。`/var` 是到 `/private/var` 的官方 symlink，`private/var/mobile/Library/Preferences` 原档是 `drwxr-xr-x` 且官方 APFS 不含该 plist（运行期生成），因此可写正确域。上游 `docs/devices/sirius-huajifruit.md:375` 也明记 done marker 是 `LastSystemVersion in .../com.apple.DataMigration.plist`。

同时核对到另一条链：`com.apple.datamigrator` 本体含 `com.apple.migration @0x1bb51`、`lastBuildVersionPref @0x19a12`、`setLastBuildVersionPref: @0x1a666`、`System build version changed from %@ to %@` @`0x1b69d`、`_CFPreferencesAppSynchronize`；这意味着「CFPreferences 域」与「DMContext 文件」相邻但不同。l729 只改 DMContext 完成态，不改 `com.apple.migration` CFPreferences 域；因此板上 `Migration necessary` 未必归零，但能判别 DMContext `LastSystemVersion` 是否足以让完成通知/交还链推进。这是机制判读轮，阴性也有明确信息。

板上 kernel 静态链：`kern_exec.c:8355-8379` 的 `setup_skip[4]` 把 365 B 官方完成态 `nabu_mig[]` 写到旧 `com.apple.migration.plist`，随后 `:8602-8640` 的 `sheng-mig=1` 把 264 B 强制态覆盖同一旧路径；l726/l727 已把强制键/哨兵分别失效/改无害路径，但完成态文件仍不是官方 DMContext。l727 kernel 旧路径 C 串唯一 @file `0x11175b` / va `0xfffffff00711575b`（65+NUL），正确路径 69+NUL；`__TEXT` load commands 结束 `0x16b0` 到 `__const` 起点 `0x5180` 为全零 padding，且 Mach-O 无 `LC_CODE_SIGNATURE`，可把新路径放在 @file `0x175b` / va `0xfffffff00700575b`。

反汇编确认 `setup_skip[4]` 完成态写入被内联为 `0xfffffff007779038 adrp x23,0xfffffff007115000` + `0xfffffff00777903c add x23,x23,#0x75b`，readback 循环从数组基址 `0xfffffff0071f6490` 取 `setup_skip[i].path`，数组第 5 项 @file `0x1f24f0` 存旧指针；`sheng-mig` 强制态写入另在 `0xfffffff0077798e0/8e4`。l729 同时改「数组第 5 项 readback 指针」与「完成态写入 ADRP 页」为新字符串页，保留 `ADD #0x75b` 不变（同页同偏移），强制态 ADRP/ADD 完全不动。补丁后反汇编正控：`adrp x23,0xfffffff007005000` + `add x23,x23,#0x75b` 指向 @`0xfffffff00700575b` 的新 C 串；强制态仍指 @`0xfffffff00711575b` 旧串。

制品 kernel `f579e056`、payload `0e19b464`，相对 l727 共 72 个文件字节变化：新路径 69 字节写入原全零 padding、数组指针 3 字节变化（旧/新 VA 只有低 3 字节不同）、完成态 ADRP 1 字节变化；Mach-O 125,628,416 B 与 `ncmds=5776` 不变，APFS nop `1f2003d5`、`pet=600`、`diagAt0=220`、`diagAt1=300`、`migratx`、`norceMigrationOnNextReboot` 全保留。payload 内只 `KERNEL~1.MAC` 变，`UNIXNU~1TOM` 1126 B 与 l727/l723 逐字节相同；五文件其余 `boot=ead4b531`、`addon=950a3968`、`adt=78a6f7e9`、`trust=a4a6bae6` 全同。

## 3. 为什么离线不行

静态能证正确域路径、`_RoleAccount=mobile` 与 0600:501:501 属主相容、`/var→/private/var` symlink、Mach-O 全零 padding 与无代码签名、两处写入/readback 路径重指向、制品单变量和全套门禁。不能静态得到的是运行期：cfprefsd / DataMigration.framework 是否真正读取 kernel 预写的文件、DMContext 是否把 `LastSystemVersion=21H461` 当作「已迁移」并 post `com.apple.migration.didComplete`、CFPreferences `com.apple.migration` 域仍可能独立打印 `(null) → Migration necessary`、`SystemAppMigrator` 是否在 300 s 快照内完成、`Migration complete / finishedStartup / Dismissing overlay / screen owner is now` 是否出现、屏是否离开 PUIProgressWindow，以及 WDT/自动回 TWRP 是否保持。现有 l727/l728 日志对「正确 DMContext 文件已写完成态」的组合沉默。满足 `AGENTS.md` 0.5：这是验证静态链在真机上是否推进迁移完成/交还的一次运行期判定。

## 4. 失败也有信息

- **正确域写入正控成立**：CDC `nabu: skip-setup / skip-read i=4` 的 path/keys 与 365 B 完成态回读应指向 `com.apple.DataMigration.plist`；`sheng-mig: sentinel=0 prefs=0 err=0` 仍成立。若 `Migration necessary` 归零、迁移完成通知与交还四标志出现、屏离开 PUIProgressWindow，则证 DMContext done marker 是最后开机墙，继续沿交还链推进。
- **正确域写入成立但 `Migration necessary` 仍 29 条且无交还**：证明 CFPreferences `com.apple.migration` 域的 `lastBuildVersionPref` 与 DMContext 是两条独立判据，DMContext 完成态不能绕过 datamigrator 的必要性判定；停止本方向，回滚 l727/l723，转向官方偏好写入或编译侧路线级改动。
- **正确域写入成立、迁移必要但 SystemAppMigrator 完成/交还出现**：证明桌面交还不要求 `(null)` 归零，只需 DMContext 与 plugin 完成链；继续静态核对 `lastBuildVersionPref` 的独立来源。
- **readback 路径不一致、`attrfail>0`、panic/APFS/`buf_bread`、早复位或双槽缺失**：按阴性回滚 l727/l723，不再加时；`arm=0` + `panic-restart=2` 保持早死自动复位路径，不需人工长按。
- **系统未自动回 TWRP**：按既有 `go_round_next.sh` 的 misc re-arm / fastboot 恢复；只有 fastboot/adb 全失败才需人工长按电源 10 秒。

## 5. 唯一变量与回滚点

唯一变量是「官方完成态 plist 的落盘路径」：同长 Mach-O 内新增一条 69 字节 C 串并用两处指针/ADRP 把 `setup_skip[4]` 写入与 readback 指向它；`sheng-mig` 强制态路径、迁移触发、属主、keychain、WDT、双槽、args 全不变。kernel `052e1ec5→f579e056`、payload `7960c99c→0e19b464`；精确 diff 为 `@0x175b` 新路径、`@0x1f24f0` 数组指针、`@0x775038` ADRP，共 72 字节。args 仍 700/39，`added=[]`、`dropped=[]`，39 token 同序同字节；`sheng-mig=1`、`sheng-smp=1 + nabu-wdt-arm-delay=0`、`nabu-state-ring=10` 全在。BITE / `WDT.CFG` / Mu / `NABU_WDT_BITE_MS` / `hang=0` / `autolog=0` / Keychains uid64 / installd 33:33 / APFS nop 全不动。一次性临时轮、用完回滚、不进默认 args；回滚链 `hold/l727`（迁移断点最前实验包）、`hold/l723`（最佳稳定双槽/启动基线）、`hold/l726`、`hold/l722`、`hold/l721`、`hold/l720`、`hold/l719`、`hold/l718`、`hold/l714`、`hold/l700`。

## 6. 恢复预案

`AGENTS.md` 0.10 绝对红线：绝不 `fastboot flash boot_a` / `flash boot`；recovery 在 boot_a 里，写 boot 会清 misc 并丢失 TWRP。执行顺序仅用既有 `E:\nabu\tmp\go_round_next.sh l729`：recovery 写 `misc-bootrecovery.bin`，进 fastboot，`fastboot flash cust hold/l729/payload.img`，**最后** `fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin` 重 arm，再 `fastboot boot hold/l729/boot-merge-r3.img`。`arm-delay=0` 保留早 panic 复位路径；`pet=600`、`hang=0` 预期仍自动回 TWRP、无需人工长按。采集沿用单个现有 CDC watcher 到回 TWRP；watcher 每 300 s 分段，全部 `cdc-*.bin` 必须按 mtime/内核 `t=` 顺序拼接后判读。按完整 CDC 的 `NABUDIAG base=` 拉两槽（`bs=4096 count=8800` 以覆盖 36,044,800 B 级 seq1），`extract_diag_tree.py` + 包含所有 `Special/*.tracev3` 的 archive + WSL `unifiedlog_iterator` 解码；重点核 `pet=600 arm=0`、`attr=49 attrfail=0`、`nabu: skip-setup / skip-read i=4` 的 path/keys、`sheng-mig: sentinel/readback`、`System build version changed from (null)`、`Migration necessary`、迁移完成链、`Migration complete / finishedStartup / Dismissing overlay / screen owner is now`、屏 sha/bbox、frames、panic/APFS/`buf_bread`、自动回 TWRP 时刻。若 fastboot 全失败才需人工长按电源 10 秒。门禁 `audit_bootability` / `audit_dual` / `audit_args` / Windows Python `precheck.py` + `precheck_check.py` / `guard_no_boot_flash` / 六问板报逐项落盘；设备当前 TWRP `f91fa664`，`boot_a` 全程未碰。
