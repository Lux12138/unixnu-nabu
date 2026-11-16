# 上板报告 l693

## 1. 唯一问题
`mac-vnode-enforce=0`（补丁 0018，lazy 检查已移到 `#if SECURITY_MAC_CHECK_ENFORCE` 之外）能否让 `mac_vnode_check_signature` 跳过 AMFI、加载 Metal 插件、建立 `BKDisplayLink` 并让 `frames xfer` 越过 5？

## 2. 已闭合到哪
- l692 的早期卡死归因于 0017 的 `STARTUP(CODESIGNING)`；本补丁改为 **lazy（无新 startup）**。
- 0018 此前不生效的根因已定性：`security/mac.h:105 #if DEBUG` ⇒ `SECURITY_MAC_CHECK_ENFORCE=0`，检查被写在 `#if` 内整块不编译。l693 已把它移到 `#if` 外，并保留 `if (!mac_proc_enforce || !mac_vnode_enforce) return 0;` 于同一位置。
- 产物验证：`mac_vfs.o` 的 `mac-vnode-enforce` 字符串 1 处；kernel 字符串 1 处；新增符号 `mac_vnode_enforce_checked`（lazy 静态变量）。
- 启动项门禁：EARLY_BOOT 14 项**地址与 l690 完全相同**（`nabu_gpu_start @ fffffff00dc2f838`、`source_pthread_bootstrap @ fffffff00dc2f850`），只是标签 `_27→_23`（行号）；**无新 startup 项**，启动顺序未变。
- 已排除：AMFI/trust cache、mapper、vsync、PreBoard、`cs_enforcement_disable`（l691 AMFI panic）。

## 3. 为什么离线不行
`mac_vnode_check_signature` 是否在运行期跳过 AMFI、Metal 是否因此加载成功、display link 是否建立，全部是运行期行为；静态只能证明检查已被编译且位置在 `MAC_CHECK` 之前。

## 4. 失败也有信息
- CDC 仍报 `mac_vnode_check_signature`：说明 `mac_vnode_enforce=0` 未生效（检查没执行或值被覆盖），下一臂查 `mac_vnode_enforce_checked` 的执行时机。
- 签名消失但 `BKDisplayLink` 仍 0：断点在 display link/vsync 层。
- `BKDisplayLink` 出现但 `frames` 不涨：断点在驱动的 transfer 路径。
- `frames` 涨但 `screen.bgrx` 仍是 logo 残留：图层/资源层问题。

## 5. 唯一变量与回滚点
- 唯一变量：kernel `efcb4c34` → `607cce60`（补丁 0018）；args 与 l692 相同（38 token / 693 字符，含 `mac-vnode-enforce=0`）。
- 逐字节相同：boot `ead4b531`、gpu-addon `bbdb4bc2`、owner-trust `7533eec8`。
- 回滚包：`hold/l686/`（kernel efcb4c34 + trustcache 7533eec8，实测 575 s）。
- 回滚命令：`fastboot flash cust hold/l686/payload.img && fastboot boot hold/l686/boot-merge-r3.img`。

## 6. 恢复预案
- 软件侧：`fastboot flash boot_a /e/nabu/tmp/boot_a.twrp.img && fastboot reboot recovery` —— 06:23 实测成功。
- 若 fastboot 命令全部失败：人工长按电源 10 秒（l689 那次）。
- 本轮内核与 l690 的启动地址逐项一致（无新 startup），l692 那种早期卡死风险已消除。
