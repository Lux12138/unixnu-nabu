# 上板报告 l723（一次性临时 Keychains uid 轮：`/private/var/Keychains` uid 0→64）

> 本包相对 `E:\nabu\hold\l722\` 只改 kernel `writable[]` 中 `/private/var/Keychains` 行的 uid 一个 32 位字段 `0→64`；保留 l722 的 `pet=600`、`diagAt0=220`、`diagAt1=300`、installd 33:33，args/toml 700/39 逐字节不变，零新增 token、零重编、零探针。用户 2026-11-11 授权继续自动推进；设备只做 `flash cust` + 最后重 arm misc + `fastboot boot`，`boot_a` 不碰。

## 1. 唯一问题

把 `/private/var/Keychains` 的 kernel 重设 owner 从 uid 0 改成 securityd 实际运行 uid 64 后，securityd 能否在 `/Library/Keychains/keychain-2.db` 成功创建/打开数据库，从而让 `Code=14` / `Couldn't bring up keychain` / MobileActivation `Failed to query keychain` 下降或归零，并让迁移/交还链继续推进？本轮不改 WDT/diag 时间闸、installd 属主、migration plist、keychain db 内容、staged apps 或 args，只改这一个 uid 字段。

## 2. 已闭合到哪

WDT 延期正常：`pet=600` 在 l719/l720/l721/l722 全命中；l722 约 358 s 自动回 TWRP、双槽非 stale、`data tree attr=49 attrfail=0`。l722 板上实证 33:33 修好 installd home/staging：home uid/gid Fault 2→0、`MIInstallerErrorDomain Code=105/4` 305/305→0/0、`Failed to create temporary staging dir` 305→0。l720/l722 的 keychain 失败形态几乎完全一致：`keychain-2.db Code=14` 首现约 127–129 s、两轮均 0 成功建库，差异只随日志尾长度；`Migration failed` 都 13 条，完整嵌套为 `perform_data_migration:1666 → Failed to query keychain`；`securityd` `Unable to create database / Couldn't bring up keychain` 路径恒为 `/Library/Keychains/keychain-2.db`。l722 `diag-procs` 正控 `securityd uid=64`、`installd uid=33`；CDC 里 owner/auth、MKB_INIT、`data tree attr=49 attrfail=0` 全正常，`systembag.kb` 缺失是 `-apfs_shared_datavolume` 官方空卷首启路径。

官方/版本证据：21H461 `/Library/Keychains` 是 symlink → `../private/var/Keychains`；`hold/privvar-dirs.txt:3` 与 §7an 真机 IPSW 记 `/private/var/Keychains` 为 `755 root:root` 空目录，官方 seed 不预置 `keychain-2.db`。但 `/root/src/UniXNU/out/ipsw/21H461/private/etc/passwd:13` 与 `master.passwd:13` 记 `_securityd=64:64`；`upstream/source-ci/nabu_data_skeleton.py:79` 的 21H16 参考布局写 `installd 0755 33:33`，`:80` 写 `Keychains 0755 64:0`，而 installd 33:33 已在 21H461 板上被 l721/l722 实证修好，说明两个 17.7 小版本在 daemon 目录属主上可能同构。该 21H16 清单标 `runtime_qualified: False`，是 restore volume 静态布局，不是 21H461 真机定论；因此本轮明确是**受控跨版本同构实验**，不是「恢复 21H461 工厂值」。历史/Qoder 对改属主/0777/补空 db 只见推测，未见实测阴性。

## 3. 为什么离线不行

静态能证 securityd uid=64、symlink、kernel 数据行、21H16 同构线索与 l722 的完整失败链；不能证明 `vnode_setattr` 把 Data 卷上的 `/private/var/Keychains` 改成 uid64 后，securityd 是否真能创建 `keychain-2.db`、`Code=14` 是否下降、`MobileActivation Migration failed` 是否消失、`checkInAndWaitForDataMigration` 是否 complete、后续 migrator 是否推进、屏是否离开 PUIProgressWindow、是否出现 `Migration complete / Dismissing overlay / screen owner is now`。l722 是唯一同时具备长窗口双槽与 33:33 修复的基线，没有 uid64 组合；这些运行期量只能上板得到，满足 `AGENTS.md` 0.5。

## 4. 失败也有信息

若 `Code=14` 显著下降或出现成功建库、`Migration failed` 下降、`completed migration for ...` / `MSUDataMigrator` / `SystemAppMigrator` 等后续任务开始，则跨版本 uid64 同构成立，keychain 链被修通；进一步看 `Migration complete / Dismissing overlay / screen owner is now` 与屏。若 `attrfail=0`、uid 补丁生效但 `Code=14` 形态不变，则明确 21H461 真机 `root:root` 不是阻塞点，停止 Keychains 属主方向，回滚 l722。若 `attrfail>0`、早复位、panic/APFS/`buf_bread` 或双诊断槽缺失，则按阴性回滚，不再加时或追加猜测参数。三种结果都把「securityd 为何建不了库」收窄成可判读事实。

## 5. 唯一变量与回滚点

唯一变量是 `E:\nabu\hold\l722\kernel.macho` 的 `writable[]` 数据行 @`0x1f2178`（`/private/var/Keychains` 唯一指针行）：uid @`0x1f2184` `00000000→40000000`（0→64），实际 1 个文件字节改变；mode 0755、gid 0、path_ptr 不变，文件长度 125,628,416 B 不变。kernel `e5994f1e → fc917183`，payload `6c290ecc → fa9a1d42`；payload 内只 `KERNEL~1.MAC` 同步，`UNIXNU~1TOM` 1126 B 与 l722 逐字节相同，其余 inner files 全 SAME。`diagAt0=220 @0xa336a4=881b8052`、`diagAt1=300 @0xa33690=88258052`、`pet=600 @0xa384f4=084b8052`、installd uid/gid `21000000/21000000`、APFS nop `1f2003d5` 全保留。args 仍 700/39，`added=[]`、`dropped=[]`；`sheng-smp=1 + nabu-wdt-arm-delay=0`、BITE / `WDT.CFG` / Mu / `hang=0` 全不动。五文件 `boot=ead4b531`、`addon=950a3968`、`adt=78a6f7e9`、`trust=a4a6bae6` 与 l722 逐字节相同。一次性临时轮、用完回滚、不进默认 args；回滚 `hold/l722`（最佳 33:33 双槽基线）、`hold/l721`、`hold/l720`、`hold/l719`、`hold/l718`、`hold/l714`、`hold/l700`。

## 6. 恢复预案

`AGENTS.md` 0.10 绝对红线：绝不 `fastboot flash boot_a` / `flash boot`；recovery 在 boot_a 里，写 boot 会清 misc 并丢失 TWRP。执行顺序仅用既有 `E:\nabu\tmp\go_round_next.sh l723`：recovery 写 `misc-bootrecovery.bin`，进 fastboot，`fastboot flash cust hold/l723/payload.img`，**最后** `fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin` 重 arm，再 `fastboot boot hold/l723/boot-merge-r3.img`。`arm-delay=0` 保留早 panic 复位路径；`pet=600`、`hang=0`、`diagAt0=220`、`diagAt1=300` 预期仍自动回 TWRP，无需人工长按。采集沿用现有 CDC watcher 到回 TWRP，`pull_diag.py l723 <cdc>` 按 base 拉槽（若导出大于 32 MiB 用 8700×4 KiB 完整范围），`extract_diag_tree.py` + `build_l702_arch.py` + WSL `unifiedlog_iterator` 解码；关键判据为 `NABUDIAG seq=0/1`、`data tree attr/attrfail`、`keychain-2.db Code=14`、`Unable to create database`、`Migration failed`、`Failed to query keychain`、`completed migration`、`Migration complete / Dismissing overlay / screen owner is now`、screen/frames、panic/APFS/`buf_bread`。若 fastboot 全失败才需人工长按电源 10 秒。门禁按 `audit_bootability` / `audit_dual` / `audit_args` / Windows Python `precheck.py` + `precheck_check.py` / `guard_no_boot_flash` 逐项落盘；设备当前 TWRP `f91fa664`，`boot_a` 全程未碰。
