# 上板报告 l694

## 1. 唯一问题
路径限定的签名旁路（仅对 `/System/Library/Extensions/NativeAdrenoMetal.bundle` 返回 0，全局 enforcement 保持 1）能否让 Metal 插件通过签名检查、加载、建立 `BKDisplayLink`，并让 `frames xfer` 越过 5？

## 2. 已闭合到哪
- l693 证明**全局** `mac_vnode_enforce=0` 会在早期卡死（20,240 B，停在 `Serial requested`）：首次 vnode 签名检查发生在早期 boot task/kext 加载。
- l694 改为路径判定：`vn_getpath(vp)` 后 `memcmp` 比较后缀（内核无 `strstr`），**仅命中插件路径时 return 0**，不动全局变量。
- 产物验证：kernel 新增字符串 `/System/Library/Extensions/NativeAdrenoMetal.bundle` 1 处、`mac_vnode_enforce_checked` 静态变量 1 个。
- 启动项门禁：EARLY_BOOT 14 项**地址与 l690 完全一致**（`nabu_gpu_start @ fffffff00dc2f838`、`source_pthread_bootstrap @ fffffff00dc2f850`），无新 startup。
- 已排除：AMFI/trust cache、mapper、vsync、PreBoard、`cs_enforcement_disable`（panic）、全局 vnode 关闭（早期卡死）。

## 3. 为什么离线不行
路径判定只在运行期发生（`vn_getpath` 返回什么、插件路径是否带符号链接/相对形式），静态无法证明命中；只有板上能看到 `mac_vnode_check_signature` 是否对插件放行。

## 4. 失败也有信息
- 仍早期卡死（20 KB 级）：说明路径判定误伤了其它 vnode（`memcmp` 后缀匹配过宽或 `vn_getpath` 失败），下一臂改为精确全路径比较。
- 签名消失但 `BKDisplayLink` 仍 0：断点在 display link/vsync 层。
- `BKDisplayLink` 出现但 `frames` 不涨：断点在驱动 transfer。
- `frames` 涨但 `screen.bgrx` 仍是 logo：图层/资源层。

## 5. 唯一变量与回滚点
- 唯一变量：kernel `efcb4c34` → `6fea6249`（0018 路径限定版）；args 与 l693 相同（38 token / 693 字符）。
- 逐字节相同：boot `ead4b531`、gpu-addon `bbdb4bc2`、owner-trust `7533eec8`。
- 回滚包：`hold/l686/`（kernel efcb4c34 + trustcache 7533eec8，实测 575 s）。
- 回滚命令：`fastboot flash cust hold/l686/payload.img && fastboot boot hold/l686/boot-merge-r3.img`。

## 6. 恢复预案
- 软件侧：`fastboot flash boot_a /e/nabu/tmp/boot_a.twrp.img && fastboot reboot recovery` —— 06:23 实测成功。
- 若 fastboot 命令全部失败：人工长按电源 10 秒（l689 那次）。
- 启动项与 l690 地址逐项一致，l692/l693 的早期卡死风险已从结构上排除。
