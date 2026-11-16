# 上板报告 l695（回滚轮）

## 1. 唯一问题

l694（0018 路径限定版）**是否照样早死**？——已在 CDC 回答：是。停在 `disk5 no metazone` 之后、
`spaceman_scan_free_blocks` 之前，18,665 B / 244 行，`exec done`/`launchd`/签名失败全 0 条。
与 l692/l693 同一卡点 ⇒ `vn_getpath` 在签名检查里重入路径解析，0018 线撤回。

本轮的**新问题**：l690 基线（无 0018、SHA-1 addon）在同一台设备上能否**复现 577 s 正常寿命**。
它是后续 SHA-256 addon 的对照底座；若这轮不成立，SHA-256 的判据就没有参照系。

## 2. 已闭合到哪

- 0018 两次编译形态（lazy 全局 / 路径限定）都在 Data 卷 spaceman 阶段挂死 ⇒ 不是匹配宽度问题；
  静态：`vn_getpath` 原只存在于 fatal 日志分支，被前移到每条检查。
- 签名 fatal 的真因已从 l690 CDC 原文读出：`in-kernel: Hash type is not SHA256 (2) but 1`。
- 本地解包 addon（NGPUADD2 + zlib）确认三个二进制 CD 全是 `hashType=1 / hashSize=20`（ad-hoc、无 CMS）。
- 已发给 fc76：撤回 0018，用 `rcodesign sign --digest sha256` 重签并回执新 cdhash / trust 生成命令。

## 3. 为什么离线不行

本轮目标是"基线与历史轮一致"的复现性判据——寿命（秒）、`nabu: owner` 五段、Data skeleton 数、
frames/nodef 计数都只能由真机跑出来；离线只能确认哈希，不能确认运行。

## 4. 失败也有信息

- 若寿命仍是 ~577 s：l690 是稳定底座，SHA-256 重签后的差异可唯一归因于 addon 签名。
- 若寿命明显偏离（<400 s 或 >800 s）：说明 l690 的 577 s 不是可复现基线，后续判据要改用同轮配对。

## 5. 唯一变量与回滚点

唯一变量 = 内核/载荷回到 `hold/l690/`（kernel `6fea6249…`、payload `de3c9a15…`、addon 同 l690）。
回滚点 = `hold/l694/`（本轮早死现场，保留 CDC 证据即可，不再启动）。

## 6. 恢复预案

设备当前在 **fastboot**（l694 早死后 WDT 复位）。先 `fastboot reboot recovery`；
若 misc 已被清除导致回到 fastboot，则走 11-04 验证过的恢复法：
`fastboot flash boot_a /e/nabu/tmp/boot_a.twrp.img` + `fastboot reboot recovery`（不动 down 里的 HyperOS boot）。
进入 recovery 后：arm misc → fastboot 刷 `hold/l690/payload.img` 到 cust → `fastboot boot hold/l690/boot-merge-r3.img`。
