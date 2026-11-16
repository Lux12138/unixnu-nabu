# 上板报告 l695u（marker 移进 Info.plist）

## 1. 唯一问题

把 publish 标记从 bundle 同级的 `NativeAdrenoMetal.publish` 文件改成 bundle 内 `Info.plist` 的
`NativeAdrenoPublish=true`（插件侧 `publish::requested()` 改为读它），插件是否终于进入 published 模式：
CDC 出现 `PUB …` / `STn …`，frames 是否越过 5。

## 2. 已闭合到哪

- l695p（同级文件 marker）：签名放行（0 fatal）、Data 挂载正常，但 `PUB`/`ST`/`P` 全 0、frames=5。
- 推论：addon 写入是通用路径（`vnode_open`+`vn_rdwr`，无文件名白名单），写入应成功；因此最可能是
  backboardd 的 sandbox 只授权到 `/System/Library/Extensions/NativeAdrenoMetal.bundle` 这个 subpath，
  同级文件的 `access(..., R_OK)` 被拒且不留日志。
- 本轮 addon（`39dc17fe…`，8 条目）的 Info.plist 是 663 B，实测含 `<key>NativeAdrenoPublish</key><true/>`；
  不再有独立的 publish 文件。
- **内核换回 `efcb4c34`（l695p 的内核，Data 挂载正常）**：l695r 的 `1f82e4a3` 在 Data 挂载时 panic
  （`unencrypted data volume is not allowed` @apfs_vfsops.c:2276），且 `llvm-nm` 证明它与 `efcb4c34`
  不止差 0017（无关文件的行号符号整体漂移）。诊断行对"PUB 是否出现"这个问题不是必须的，先把 APFS 回归挡在外面。
- payload `599bae84…`：从 payload 抽回的 kernel 与 `hold/l695p/kernel.macho` 逐字节相同（`efcb4c34…`），`fsck.fat -n` 干净。

## 3. 为什么离线不行

`publish::requested()` 的结果取决于 backboardd 的沙箱裁决与插件的加载时机，离线只能确认 Info.plist 内容正确。

## 4. 失败也有信息

- 有 `PUB …`：marker 路径判断正确，device 注册成功 → 接着看 frames 与 `STn` 文本。
- 有 `PUB no device: …`：marker 读到了，device 创建失败，错误文本给出下一步。
- 仍然全 0：marker 不是障碍，剩下"插件没被加载"这一支，需要回到发现链（MetalPluginName → bundle 加载）。
- 若 backboardd 崩（alioth 的 `0x106da915c` SIGBUS）：publish 是唯一变量，退回 l695p 即对照。

## 5. 唯一变量与回滚点

唯一变量 = addon 的 marker 载体（同级文件 → Info.plist key）。内核/boot/ADT/ramdisk 与 l695p 完全相同；
args 只把 tag 改成 `l695u`。回滚点 = `hold/l695p/`、`hold/l690/`。

## 6. 恢复预案

TWRP 里 arm misc → `reboot bootloader` → 只 flash cust → 最后 arm misc → `fastboot boot`；
**boot_a 不碰**（AGENTS.md 0.10，guard 强制）。misc 最后写，复位后自动落回 TWRP。
