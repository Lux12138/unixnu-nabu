# 上板报告 l721（一次性临时目录属主轮：`/private/var/installd` 0:0 → 33:33）

> 本包相对 `E:\nabu\hold\l720\` 只改 kernel 中 `writable[]` 的一行 uid/gid 两个 32 位字段；保留 `pet=600` 与 `diagAt1=300`，args/toml 700/39 逐字节不变，零新增 token、零重编、零探针。用户 2026-11-11 授权自动推进/临时数据补丁；设备只做 `flash cust` + 最后重 arm misc + `fastboot boot`，`boot_a` 不碰。

## 1. 唯一问题

把 `/private/var/installd` 的 kernel 重设属主从 root:root 改成 installd 实际运行且自检要求的 uid/gid 33 后，`installd` 的 home uid/gid Fault、`NSPOSIXErrorDomain Code=13/2` 与 `SystemAppMigrator` 的 `MIInstallerErrorDomain Code=105/4` staging 临时目录失败能否归零或下降，并让迁移任务继续推进到下一个 migrator / 交还标志？本轮不改 WDT、diag 时间闸、migration plist、keychain、staged apps 或 args，只修这一行属主。

## 2. 已闭合到哪

l720 第二诊断槽 `logs/l720-seq1-v1.jsonl` 证明 `00LaunchServicesMigrator` 226.5 s `success 1` 完成，`MSUDataMigrator` 226.7 s 完成；当前具体断点前移到 `SystemAppMigrator` 的 demoted app placeholder。224.8 s `installd` Fault 明确 `Daemon's home directory ... should be owned by uid 33, but found 0` / `should have gid 33, but found 0`；`logs/l720-seq1-diag-tree/diag-procs.txt` 正控 `pid=250 uid=33 comm=installd`。随后 224.8～225.0 s 连续 `NSPOSIXErrorDomain Code=13 Permission denied` / `Code=2 No such file or directory`，274.4～287.7 s 共 305 条 `MIInstallerErrorDomain Code=105/4` 无法在 `/var/installd/Library/Caches/com.apple.mobile.installd.staging/temp.XXXXXX` 建临时目录。编译树 `0012-nabu-data-dirmode.patch` 的 `writable[]` 把 `/private/var/installd` 写成 `{0755, 0, 0}`；`0009-nabu-data-mkdir.patch` 骨架没有 staging 父目录。Qoder feedback / `CURRENT.json` 历史窄查 0 条同类属主或 `MIInstallerErrorDomain` 阴性实验。上游 `zzy-staged-system-apps.patch` 的 mobile 501 是 installcoordinationd / migration plugin 的 staged apps 目录，属于另一层；板上 kernel 虽有 `sheng-staged-apps`，l720 args 未启用、CDC 无 staged 拷贝标记，本轮不夹带。l720 的 `pet=600`、`diagAt1=300`、双槽诊断与 628 s 自动回 TWRP 都已验证。

## 3. 为什么离线不行

静态能证 uid/gid 33 的运行期正控、kernel 数据行位置、历史无否证与补丁同长；不能证明改完属主后 APFS Data 卷上的既有目录 `vnode_setattr` 是否成功、installd 是否还会因更深层 `Library/Caches/...staging` 目录不存在而继续失败、305 条 placeholder 安装错误是否下降，以及 `SystemAppMigrator` 是否推进到完成/下一个插件或出现交还标志。l720 是唯一带 300 s 第二槽的基线，没有 `33:33` 组合；这些运行期量只能上板得到，满足 `AGENTS.md` 0.5。

## 4. 失败也有信息

若 `nabu: data tree attr=49 attrfail=0` 保持且 `l721-seq1` os_log 中两条 home uid/gid Fault 归零、`Code=13/2` 与 `MIInstallerErrorDomain 105/4` 显著下降，则属主链成立；进一步看 `SystemAppMigrator` 是否出现 `completed migration ... with success 1`、是否进入后续 migrator、`Migration complete / Dismissing overlay / screen owner is now` 是否出现。若属主 Fault 归零但 staging 仍失败，则明确下一层是深层目录骨架缺失，不再重复改属主。若 `attrfail>0`、属主没改成或出现 APFS/panic/早复位，则按阴性回滚 l720，不放宽安全策略、不补空 db、不改 staged apps。三种结果都把当前断点收窄成可判读事实。

## 5. 唯一变量与回滚点

唯一变量是 `E:\nabu\hold\l720\kernel.macho` 的 `writable[]` 数据行 @`0x1f2220`（`/private/var/installd` 唯一指针行）：uid @`0x1f222c` `00000000→21000000`、gid @`0x1f2230` `00000000→21000000`，即两个 4 字节字段 `0→33`；mode 仍 0755、path_ptr 不变，实际仅 2 个文件字节改变，文件长度 125,628,416 B 不变。kernel `13d14bdb → e4d771f5`，payload `0e4f8cee → 9170e91a`；payload 内只 `KERNEL~1.MAC` 同步，`UNIXNU~1TOM` 1126 B 与 l720/l719/l718 逐字节相同，其余 inner files 全 SAME。`pet=600` @`0xa384f4=084b8052`、`diagAt1=300` @`0xa33690=88258052`、APFS nop @`0x6cd2f68=1f2003d5` 全保留。args 仍 700/39，`added=[]`、`dropped=[]`；`sheng-smp=1 + nabu-wdt-arm-delay=0` 硬配对、BITE / `WDT.CFG` / Mu / `hang=0` / `diagAt0=260` 全不动。五文件 `boot=ead4b531`、`addon=950a3968`、`adt=78a6f7e9`、`trust=a4a6bae6` 与 l720 逐字节相同。一次性临时轮、用完回滚、不进默认 args；回滚 `hold/l720`（最佳诊断/动态基线）、`hold/l719`、`hold/l718`、`hold/l714`、`hold/l700`。

## 6. 恢复预案

`AGENTS.md` 0.10 绝对红线：绝不 `fastboot flash boot_a` / `flash boot`；recovery 在 boot_a 里，写 boot 会清 misc 并丢失 TWRP。执行顺序仅用既有 `E:\nabu\tmp\go_round_next.sh l721`：recovery 写 `misc-bootrecovery.bin`，进 fastboot，`fastboot flash cust hold/l721/payload.img`，**最后** `fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin` 重 arm，再 `fastboot boot hold/l721/boot-merge-r3.img`。`arm-delay=0` 保留早 panic 复位路径；`pet=600`、`hang=0`、`diagAt1=300` 预期仍自动回 TWRP，无需人工长按。采集沿用现有 CDC watcher 到回 TWRP，`pull_diag.py l721 <cdc>` 拉双槽，`extract_diag_tree.py` + `build_l702_arch.py` + WSL `unifiedlog_iterator` 解 `seq=1`；关键判据为 home uid/gid Fault、`Code=13/2`、`MIInstallerErrorDomain 105/4`、`SystemAppMigrator` 完成/后续任务、`screen.bgrx`、frames、panic/APFS/`buf_bread`。若 fastboot 全失败才需人工长按电源 10 秒。门禁按 `audit_bootability` / `audit_dual` / `audit_args` / Windows Python `precheck.py` + `precheck_check.py` / `guard_no_boot_flash` 逐项落盘；设备当前 TWRP `f91fa664`，`boot_a` 全程未碰。
