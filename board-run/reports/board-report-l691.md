# 上板报告 l691

## 1. 唯一问题
`cs_enforcement_disable=1` 能否让 Metal 插件通过 `mac_vnode_check_signature`，从而让 WindowServer 建立 CADisplayLink 并开始渲染？

## 2. 已闭合到哪
因果链（每一环都有实证）：
- Metal 发现 → 加载 `/System/Library/Extensions/NativeAdrenoMetal.bundle`；
- **l690 CDC：`mac_vnode_check_signature ... NativeAdrenoMetal ... code signature validation failed fatally`（4 次）**；
- trust cache 生效（`cdhash 1dceadc2... is trusted`，owner-trust 内 4 个 CDHash 逐个核过）；
- 但 MACF vnode 检查不走 trust cache，adhoc 签名直接被判死 ⇒ 插件未加载；
- 无 Metal 渲染器 ⇒ `BKDisplayLink` 在 122,997 行 os_log 中 **0 条**（display link 从未建立）；
- `screen.bgrx` = 黑底 + 227×280 苹果 logo 残留；`nodef=16384000`（surface 建了但没画）；`frames=5`（bics 初始化）。

已排除：AMFI/CT policy（trust cache 有命中）、`nabu-mapper`、`sheng-vsync`（l684 有 vsync=60 仍失败，因为同一签名根因）、PreBoard（未启用）、Metal 二进制缺失（bundle 与 addon 均在）。

## 3. 为什么离线不行
`mac_vnode_check_signature` 是运行期内核 MACF 检查，取决于 `cs_config`（`CS_CONFIG_ALLOW_ANY_SIGNATURE`）与 AMFI hook 的实际行为；静态只能证明“会走到这条检查”，不能证明“`cs_enforcement_disable` 是否被 AMFI 采纳”。

## 4. 失败也有信息
- CDC 仍报 `mac_vnode_check_signature`：该 boot-arg 不覆盖这一层 ⇒ 转 `security.mac.vnode_enforce=0`（需内核补丁或 root 启动脚本）。
- 签名过了但 `BKDisplayLink` 仍 0 条：断点在 display link/vsync 层，下一轮查 `IOMobileFramebuffer` 的 vsync 注册。
- `BKDisplayLink` 出现但 `frames` 不涨：断点在我们驱动的 transfer 路径（`native_legacy_transfer`）。
- `screen.bgrx` 仍为 logo 残留但帧数涨：渲染内容层问题（图层/资源），非签名。

## 5. 唯一变量与回滚点
- 唯一变量：args 增加 `cs_enforcement_disable=1`（37→38 token，673→696 字符，上限 700）。
- 逐字节相同：kernel `efcb4c34`、boot `ead4b531`、gpu-addon `bbdb4bc2`、owner-trust `7533eec8`。
- 回滚包：`hold/l686/`（kernel efcb4c34 + trustcache 7533eec8，实测 575 s）。
- 回滚命令：`fastboot flash cust hold/l686/payload.img && fastboot boot hold/l686/boot-merge-r3.img`。

## 6. 恢复预案
- 软件侧：`fastboot flash boot_a /e/nabu/tmp/boot_a.twrp.img && fastboot reboot recovery` —— 今天 06:23 实测成功。
- 若 `fastboot boot/reboot/continue` 全部失败（l689 那次）：需要人工长按电源 10 秒，软件侧无手段；提前写明。
