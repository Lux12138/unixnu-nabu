# 上板报告 l719（一次性临时 WDT 窗口轮：同长数据补丁把 petSeconds 默认 300 改成 600）

> 本包相对 `E:\nabu\hold\l718\` 只改 `kernel.macho` 一条 ARM64 立即数；args/toml 700/39 逐字节不变，零新增 token、零重编、零探针。用户 2026-11-11 明确授权「注意看门狗咬合时间，必要时把咬合时间延后，去翻历史记忆和相关文档找方法」。设备只做 `flash cust` + 最后重 arm misc + `fastboot boot`，`boot_a` 不碰。

## 1. 唯一问题

把板上已有的 `nabu-wdt-pet-until` 默认截止从 300 s 延到 600 s 后，`com.apple.datamigrator` 在 l718 真实 boot+136.0 s 开始的 `00LaunchServicesMigrator` 会不会在 boot+253～560 s 这段此前被 WDT 截断的窗口里出现 `completed migration` / `Migration complete` / `Dismissing overlay` / `screen owner is now`，或者由默认 `nabu-diag-at1=470` 的第二诊断槽给出可判读的失败证据？本轮不修迁移、不改 keychain、不碰 LaunchServices 参数，只延长取证窗口并验证第二槽是否产出新制品。

## 2. 已闭合到哪

l718 已证明动态迁移进度窗：屏 `3d0be3a57ec23c08`、frames `xfer=56 nswap=56 ncpy=56 nret=56` 持续上涨；但它不是桌面。`l718-v1.jsonl` 的 `time×52.0833` 才是真实秒，覆盖 boot+117～253 s，同一 boot_uuid、5 条 Loss；`ContainerMigrator` 135.7 s 开始、136.0 s 完成，`00LaunchServicesMigrator` 136.0 s 开始后到 253 s 无完成行。`progress host semaphore open failed (ENOENT)` 随后被 `progress host is ready / wait did finish` 追上，不是最终卡点。`lsd(99) deny(1) syscall-mig 139` 在 l714/l715/l717/l718 分别 43/41/40/37 条，是跨轮背景噪声。DataClassMigrators / csstore / keychain db 方向经静态核查为 NO-CONCRETE-TARGET，diag-tree 只有 os_log 制品，没有实体 bundle/db/semaphore，源码也没有可跳过该 migrator 的现成开关。MKB / usermanagerd / persona 官方验收已通过；keychain-2.db `Code=14` 和 `Migration failed` 仍是并行的后续失败链。l718 CDC 回显 `pet=300 arm=0 hang=0 diagnostics=0`，15:14:58 boot、15:21:02 自动回 TWRP，约 364 s；`NABUDIAG2 seq=0` 已导出 29,905,640 B / 302 records，`seq=1` 因早咬从未运行。两套采集时基、`NABUOSLOG1` 只是 8 KiB 内核 ring、不是用户态 os_log 来源，也都已分清。

## 3. 为什么离线不行

静态只能证明「延长窗口的机制在、第二诊断槽在、采集脚本在」；不能证明运行期到底有没有 `00LaunchServicesMigrator` 完成、是否在 60 s 估算后被 datamigrator watchdog 杀掉、是否出现 `Migration complete` / overlay dismiss / screen owner 交还，也不能证明 `nabu-diag-at1=470` 在延寿后真被 I/O 完成路径触发。l718 的所有 os_log/tracev3/screen 制品都止于约 253 s；l714/l717 同通道也止于约 250/239 s，且没有 l719 的长窗口组合。要回答「后面到底完成了没有」和「第二槽是不是 stale」，只能让板上 WDT 活到 470 s 之后并采集运行期产物，满足 `AGENTS.md` 0.5 的上板条件。

## 4. 失败也有信息

若 CDC 回显从 `pet=300 arm=0` 变成 `pet=600 arm=0`，且板子明显晚于 364 s 自动回 TWRP，则 WDT 延后机制成立。若 `NABUDIAG1/2 seq=1` 或 `l719-diag-seq1.bin` 出现且 SHEXR001/M2VCACH1 头与 l718 `seq=0` 不同，就能把 os_log 尾段、tracev3 和 screen 拉到 470 s 之后，直接判读迁移是否完成。若寿命延长但 CDC 只有 `seq=0`、`seq=1` 缺失或与 l718 逐块相同，且 os_log 尾仍 ≤253 s、无 `Migration complete`/`completed migration`/`Dismissing overlay`，则得到明确否定结论：不是迁移在等 60 s，而是诊断导出通道本身冻结，继续延寿不再产生新证据，应停止并判 NO-NEW-EVIDENCE。若 `pet=600` 未回显、仍约 364 s 咬合，则 4 字节数据补丁未命中运行路径，按阴性封存并回滚 l718，不追加猜测参数。三种结果都把当前「不知道 253 s 之后发生什么」收窄成可判读事实。

## 5. 唯一变量与回滚点

唯一变量是 `E:\nabu\hold\l718\kernel.macho` @`0xa384f4` 的一条同长指令：`mov w8,#0x12c`（`88258052`，源码 `petSeconds=300`）→ `mov w8,#0x258`（`084b8052`，`petSeconds=600`），仅 2 个文件字节改变，文件长度仍 125,628,416 B。该指令紧接着初始化 `armDelay/petSeconds`，随后两个 `PE_parse_boot_argn` 分别消费 `nabu-wdt-arm-delay` 与 `nabu-wdt-pet-until`；反汇编与 `/root/merge/out-v6/.../drivers/ShengUfs/NativeUfs.cpp:4411-4416`、`:3271-3289` 一致。l719 kernel sha8 `21bc44b8`，payload sha8 `948241a8`；payload 内只 `KERNEL~1.MAC` 同步，`UNIXNU~1TOM` 1126 B 与 l718 逐字节相同，其余 inner files 全 SAME。args 仍 700/39，`added=[]`、`dropped=[]`；`sheng-smp=1 + nabu-wdt-arm-delay=0` 硬配对不动，BITE / `WDT.CFG` / Mu / `NABU_WDT_BITE_MS` / `nabu-hang-reset=0` 全不动。APFS nop @`0x6cd2f68` 仍 `1f2003d5`，Mach-O `ncmds=5776`、无新增签名/loadcmd。五文件 `boot=ead4b531`、`addon=950a3968`、`adt=78a6f7e9`、`trust=a4a6bae6` 与 l718 逐字节相同。一次性临时轮、用完回滚、不进默认 args；回滚 `E:\nabu\hold\l718\`（动态最佳）、`E:\nabu\hold\l714\`（静态画面最佳）、`E:\nabu\hold\l700\`（安全基线）。

## 6. 恢复预案

`AGENTS.md` 0.10 绝对红线：绝不 `fastboot flash boot_a` / `flash boot`；这块板 recovery 在 boot_a 里，写 boot 会清 misc 并丢失 TWRP。执行顺序仅用既有 `E:\nabu\tmp\go_round_next.sh l719`：先在 recovery 写 `misc-bootrecovery.bin`，进 fastboot，`fastboot flash cust hold/l719/payload.img`，**最后** `fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin` 重 arm，再 `fastboot boot hold/l719/boot-merge-r3.img`。`arm-delay=0` 保留早 panic 的复位路径；`hang=0`，无第二静默复位源；预期喂狗截止 600 s、自动咬合约 550～660 s，仍自动回 TWRP，不需要人工长按。采集用现有 CDC watcher 持续到回 TWRP，随后 `python E:/nabu/tmp/pull_diag.py l719 <cdc>` / `round_watch.py` 拉双槽，`extract_diag_tree.py` + `build_arch.py` + WSL `unifiedlog_iterator` 解 `l719-v1.jsonl`。若 fastboot 全失败才需人工长按电源 10 秒恢复。门禁已落盘：`audit_bootability` OK 0fail/0warn、`audit_dual` 7/7、`audit_args` OK、`fsck.fat -n` 双包 rc=0、`guard_no_boot_flash` rc=0；`precheck.json` 与本报告由本轮刷新，设备当前 TWRP `f91fa664`，`boot_a` 全程未碰。
