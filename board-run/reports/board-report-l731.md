# 上板报告 l731（一次性临时性能轮：静音 spawn/exec 无条件 printf）

> 本包相对 `E:\nabu\hold\l730\` 只改 kernel 中四条 `nabu: spawn>/spawn err=/exec>/exec done` printf 格式串的首字节为 NUL，共 4 个文件字节。四个 printf 调用、参数、os_log 空格式串早退、`nabuLogThread` 的 2 秒 `nabu_wdt_pet()`、4 KiB 日志环 pet 钩子、WDT 截止、args 700/39、五文件其余产物、APFS nop、pet600、diagAt0/1=220/300、migratx、migratiox.plist、norceMigrationOnNextReboot 全部保留。零重编、零新增 token、零新增打印/探针、零安全放宽。用户已授权 7×24 自动推进；设备只做 `flash cust` + 最后重 arm misc + `fastboot boot`，`boot_a` 不碰。

## 1. 唯一问题

l730 已现场确认锁屏可见、触摸有效但交互很卡；在低风险前提下静音复现 CDC 中占 26.1% 的 2,661 条无条件 spawn/exec printf，能否减少串口/日志环/锁开销，使同一用户滑动动作对应的 `xfer/nswap` 帧增长更快、主观更流畅，同时仍到锁屏、触摸有效、自动回 TWRP？本轮不改显示/GPU/触摸/迁移/WDT 时间参数或任何 boot-arg。

## 2. 已闭合到哪

用户现场反馈「确实进入了锁屏页面，也有触摸，但是太卡了」已写入 `logs/rounds/l730-reproduction.json`、HANDOVER §7bc 和记忆。复现 `logs/l730r-cdc-merged.bin`（1,014,001 B，sha8 `4514102e`）：`pet=600 arm=0`、PON `0x80041` 自动回 TWRP、无 panic/APFS/`buf_bread`；`fingers` 4→436、`NABU TOUCH status ... ccreator=pid 56, backboardd`，触摸链通；滑动段 `nabu-display: frames xfer=1@251.8s →3@272.7s →5@290.7s`，约 18–21 秒才增加 2 帧；`NativeAdrenoProfile lat≈0.42–0.47s`、`NativeAdrenoMetal ctc≈0.43–0.56s`。用户态 `NativeAdrenoMetal` 还报告 `vm_map calls=48 tables_us=68118 index_us=46633 max_us=93204 / pte_map publish_us=32483`，说明页表/显存映射本身也重；本轮只检验「减少高频调试输出」能否改善，不声称能根治 GPU 开销。

CDC 字节量化：`nabu: spawn/exec/exec done/spawn err` 共 2,661 行、264,581 B（26.1%）；`NativeAdrenoProfile/Metal` 223,835 B（22.1%）；`NABU TOUCH` 9,643 B（1.0%）。四条格式串在 l730 kernel 中各唯一：@file `0x10ff15` 77 B、`0x10ffd9` 92 B、`0x1102de` 34 B、`0x110473` 55 B。源码 `/root/merge/out-v6/.../bsd/kern/kern_exec.c:1201,2168,3642,4948` 是无条件 `printf`；`osfmk/kern/printf.c:897-907` 的 `printf → vprintf_internal` 中 `os_log_with_args(OS_LOG_DEFAULT,...fmt...)` 与 `__doprnt(fmt,...)` 均接收该格式串。`libkern/os/log.c:769-784` 对 `fmt[0]=='\0'` 明确 `counter_inc(error_count); return`，不会读取后续 varargs；`osfmk/kern/printf.c:241-270` 的 `__doprnt` 以 `while ((c=*fmt)!='\0')` 开头，空串同样不输出字符、不读参数。改为 NUL 后四个 printf 仍被调用，但不再向 `console_printbuf_putc → nabu_log_putc` 或 CDC 输出任何字符。

WDT 安全性闭合三条链：
1. `owner-trust.bundle` 从 l718 到 l730 始终 `a4a6bae6`/61,456 B 不变，而 kernel 每轮不同（l730 `d3598dd6`）；25 条 load command 无 `LC_CODE_SIGNATURE`，`__TEXT` 覆盖 file `0..0x1b4000`。既有 l719–l730 多轮同长数据补丁均通过启动，证明 owner-trust 不校验整份 kernel `__TEXT`。
2. 空格式串在 os_log 内部早退，不会解析后续 `%d/%llu/%s` 参数。
3. `NativeUfs.cpp:4090-4127` 的 `nabuLogThread` 每 2 秒 `IOSleep(2000) → nabu_log_puts("lg:tick\n") → nabu_wdt_pet() → nabu_log_periodic()`，板上 `llvm-nm` 仍有 `_nabu_log_service_4k`/`__ZL13nabuLogThreadPvi`/`__ZL12nabu_wdt_petv`。即便静音后 `nabu_log_total` 字符产量下降、每 4 KiB 的 `nabu_log_service_4k()` 少触发，独立 2 秒 pet 仍在，`pet=600` 截止与 `arm-delay=0` 不变；l730 复现也是 PON `0x80041` 内核自复位回 TWRP，不依赖这些 spawn 行存活。

历史负向排除：`nabu-log=0` 会停 nabuLogThread/日志窗口/WDT 心跳；`sheng-log=0` 历史有 BSD root 早停和丢盘尾通道；`nabu-touch=0` 会关已证触摸链；`sheng-cpu0-park`/`cpu0_park` 字符串在 l730 kernel 中 0 命中，旧版 40→55 fps 注释不适用；l724/l725/l728 证明移动诊断时间闸会早复位。故本轮不碰这些。

全套门禁 PASS：双包 `fsck` rc=0；`audit_bootability` OK 0fail/0warn；`audit_dual` 7/7 `state/audit-l731.json`；`audit_args` 700/39 → 700/39、`added=[] dropped=[]`；`guard_no_boot_flash` OK。制品 kernel `e6e59357`、payload `1a9aabc5`；相对 l730 仅 4 文件字节变化，Mach-O 125,628,416 B 与 `ncmds=5776` 不变；payload 内只 `KERNEL~1.MAC` 变，`UNIXNU~1TOM` 1126 B 与 l730/l727/l723 逐字节相同；五文件其余 `boot=ead4b531`、`addon=950a3968`、`adt=78a6f7e9`、`trust=a4a6bae6` 全同。

## 3. 为什么离线不行

静态能证四条格式串唯一、空格式串安全、owner-trust 不校验、独立 2 秒 pet 存在、制品单变量和全套门禁。不能静态得到的是运行期：`nabu: spawn/exec` 是否真的降为 0、`nabu_log_total` 减少后是否仍保持自动回收、CDC/USB 负载下降是否足以改变用户滑动时的 `xfer/nswap` 斜率、`NativeAdrenoProfile lat/ctc` 是否下降、是否仍到同一锁屏并触摸有效、用户主观是否更流畅，以及是否引入早复位/双槽缺失。现有 l730/l730r 日志对「静音 spawn/exec 输出」组合沉默。满足 `AGENTS.md` 0.5；这是一次可上行可下行的性能 A/B，不是重复参数轮。

## 4. 失败也有信息

- **阳性**：CDC `nabu: spawn/exec/exec done/spawn err` 0 命中，但仍 `pet=600 arm=0`、自动回 TWRP、无 panic/APFS/`buf_bread`；若 `xfer/nswap` 斜率明显变快、`lat/ctc` 下降或用户反馈更顺，则证高频调试输出是卡顿重要来源，可继续在保持 WDT 心跳前提下收敛其它 profile 输出。
- **静音正控成立但帧率不变**：证明 spawn/exec printf 不是主因，卡顿主要来自 GPU/显存映射/系统 UI 合成；停止「只关日志」方向，转向编译侧/路线级 GPU 优化，不再出低价值轮。
- **仍到锁屏/触摸但自动回收异常或双槽缺失**：证 `nabu_log_total` 下降仍影响 4 KiB pet/诊断 I/O 形状；按阴性回滚 l730，不再静音 printf。
- **黑屏/锁屏丢失/触摸失效/早复位/panic/APFS/`buf_bread`**：按阴性立即回滚 l730；`arm=0` + `panic-restart=2` 保留早死自动复位路径，不需人工长按。

## 5. 唯一变量与回滚点

唯一变量是「四条 spawn/exec printf 格式串首字节 `n/e/e/e → \0`」；printf 调用本身、`nabu-log=1`、`sheng-log=1`、`nabu-state-ring=10`、其它所有调试输出、`sheng-smp=1 + nabu-wdt-arm-delay=0`、BITE / `WDT.CFG` / Mu / `NABU_WDT_BITE_MS` / `hang=0` / `autolog=0`、Keychains uid64 / installd 33:33 / APFS nop / pet600 / 220/300 双槽 / migratx / migratiox.plist / norceMigrationOnNextReboot 全不动。kernel `d3598dd6→e6e59357`、payload `3876e1a2→1a9aabc5`；精确 diff 为 @`0x10ff15`、@`0x10ffd9`、@`0x1102de`、@`0x110473` 各 1 字节，共 4 字节。一次性临时轮、用完回滚、不进默认 args；回滚链 `hold/l730`（最佳已验证锁屏/触摸包）、`hold/l727`、`hold/l723`、`hold/l722`、`hold/l721`、`hold/l720`、`hold/l719`、`hold/l718`、`hold/l714`、`hold/l700`。

## 6. 恢复预案

`AGENTS.md` 0.10 绝对红线：绝不 `fastboot flash boot_a` / `flash boot`；recovery 在 boot_a 里，写 boot 会清 misc 并丢失 TWRP。执行顺序仅用既有 `E:\nabu\tmp\go_round_next.sh l731`：recovery 写 `misc-bootrecovery.bin`，进 fastboot，`fastboot flash cust hold/l731/payload.img`，**最后** `fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin` 重 arm，再 `fastboot boot hold/l731/boot-merge-r3.img`。`arm-delay=0` 保留早 panic 复位路径；`pet=600`、`hang=0` 预期仍自动回 TWRP、无需人工长按。采集沿用单个现有 CDC watcher 到回 TWRP；watcher 每 300 s 分段，全部 `cdc-*.bin` 必须按 mtime/内核 `t=` 顺序拼接后判读。按完整 CDC 的 `NABUDIAG base=` 拉两槽（`bs=4096 count=9400` 以覆盖 38 MB 级 seq1），`extract_diag_tree.py` + 包含所有 `Special/*.tracev3` 的 archive + WSL `unifiedlog_iterator` 解码；重点核 `nabu: spawn/exec` 0、`pet=600 arm=0`、`attr=49 attrfail=0`、`lg:tick`、`Migration not necessary`、`finishedStartup/Dismissing overlay`、`nabu-display: frames` 斜率、`fingers`、`NativeAdrenoProfile lat`、`NativeAdrenoMetal ctc`、屏 sha/bbox、panic/APFS/`buf_bread`、自动回 TWRP 时刻，并由用户现场反馈滑顺程度。若 fastboot 全失败才需人工长按电源 10 秒。门禁按 `audit_bootability` / `audit_dual` / `audit_args` / Windows Python `precheck.py` + `precheck_check.py` / `guard_no_boot_flash` / 六问板报逐项落盘；设备当前 TWRP `f91fa664`，`boot_a` 全程未碰。
