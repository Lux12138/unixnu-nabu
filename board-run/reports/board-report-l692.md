# 上板报告 l692

## 1. 唯一问题
`mac-vnode-enforce=0`（配合内核补丁 0017 在 `mac_base.c` 的 startup 里置 `mac_vnode_enforce=0`）能否让 `mac_vnode_check_signature` 跳过 AMFI，从而加载 Metal 插件、建立 CADisplayLink、让 `frames xfer` 越过 5？

## 2. 已闭合到哪
- l691 证明 `cs_enforcement_disable=1` 被 AMFI **主动 panic**：`"can't has cs_enforcement_disable" @AppleMobileFileIntegrity.cpp:5261`（319 s，PON 0x40001）。该路线关闭。
- `security/mac_vfs.c:1344`：`if (!mac_proc_enforce || !mac_vnode_enforce) return 0;` —— 这是绕过点。
- 补丁 0017 落点已核：`mac-vnode-enforce` 字符串 **1 处**、`mac_vnode_enforce_init` 符号 **2 处**（定义 + startup entry）。
- 启动项安全：`llvm-nm -n` 对比 l690/l692，EARLY_BOOT 14 项**顺序相同**（地址整体偏移 0x18，因为新增了 startup 函数）；`pthread519_pthread_start` 在两个内核中都是 **0**（l688 的 namespace 回归没有回来）。
- 已排除：AMFI/trust cache（命中但 MACF 不走它）、`nabu-mapper`、`sheng-vsync`（l684 有 vsync 也失败）、PreBoard。

## 3. 为什么离线不行
`mac_vnode_enforce` 是运行期内核变量；补丁是否在正确的 startup 阶段生效、AMFI hook 是否因此不再被调用，只有板上能看到。静态只能证明代码路径存在。

## 4. 失败也有信息
- CDC 仍报 `mac_vnode_check_signature`：补丁未生效（startup 阶段/顺序问题），下一臂查 `mac_vnode_enforce_init` 是否执行。
- 签名消失但 `BKDisplayLink` 仍 0：断点在 display link/vsync 层。
- `BKDisplayLink` 出现但 `frames` 不涨：断点在驱动的 transfer 路径。
- `frames` 涨但 `screen.bgrx` 仍是 logo 残留：图层/资源层问题。

## 5. 唯一变量与回滚点
- 唯一变量：kernel `efcb4c34` → `113731f6`（补丁 0017）；args 去掉 `cs_enforcement_disable=1`、加 `mac-vnode-enforce=0`（38 token / 693 字符）。
- 逐字节相同：boot `ead4b531`、gpu-addon `bbdb4bc2`、owner-trust `7533eec8`。
- 回滚包：`hold/l686/`（kernel efcb4c34 + trustcache 7533eec8，实测 575 s）。
- 回滚命令：`fastboot flash cust hold/l686/payload.img && fastboot boot hold/l686/boot-merge-r3.img`。

## 6. 恢复预案
- 软件侧：`fastboot flash boot_a /e/nabu/tmp/boot_a.twrp.img && fastboot reboot recovery` —— 06:23 实测成功。
- 若 fastboot 命令全部失败：人工长按电源 10 秒（l689 那次）。
- 额外注意：补丁 0017 的 `disable` 初值为 0，**不传该 boot-arg 时也会关闭** `mac_vnode_enforce`；若本轮之后要回默认行为，必须改补丁而不能只删参数。
