# 上板报告 l695s（SHA-256 bundle 轮）

## 1. 唯一问题

把 Metal 插件的 CodeDirectory 从 SHA-1 改成 SHA-256（并同步三条 cdhash 到 owner trust 模块）之后，
AMFI 的 `mac_vnode_check_signature` fatal 是否消失、Metal 是否真的被加载 ——
判据：CDC 里 4 次 fatal 归零、`BKDisplayLink` 出现、`frames=` 越过 5。

## 2. 已闭合到哪

- l690 CDC 原文：`in-kernel: Hash type is not SHA256 (2) but 1.validation of code signature failed through MACF policy: 1`
  ⇒ 拒绝发生在 CD 的 hash 类型，不是 ad-hoc、不是 trust cache 查询。
- 解包 l690 addon：三个二进制 CD 全是 `hashType=1 / hashSize=20`，ad-hoc（无 CMS）。
- 构建侧源码：`scripts/build-adreno-metal-plugin.py:47` **显式** `'--digest','sha1'`；`:67` 拿 `sha1:` 行当 cdhash；
  `:129` 用 `hashlib.sha1` 校验页。rcodesign 本身在 iOS17 目标下默认取 SHA-256。
- 本机用同版本 rcodesign `sign --digest sha256` 重签，验得 `digest_size=32 / digest_type=Sha256 / page_size=4096`；
  产物三条 cdhash：`901585dc…f04e37`（插件）、`61512d6b…8c9326`（turnip）、`e1bb0330…b05215`（守护）。
- trust 模块条目类型字节本就写 2（SHA-256），改值后类型/值自洽；新旧 trust bundle 均 61,456 B、结构通过 loader 同款校验。
- **打包往返证明**：用自写打包器把 l690 addon 按其自身内容（zlib level 9、8 字节对齐）重打，得到
  `bbdb4bc2e3762271…` 与 l690 逐字节相同 ⇒ 新 addon 的容器格式不是猜的。
- 新 payload `ae5ad6e1…`：`fsck.fat -n` 干净、大小仍 192 MiB；抽回比对 kernel.macho `efcb4c34…` 与 l690 **逐字节相同**。

## 3. 为什么离线不行

AMFI 是否接受 SHA-256 CD、trust cache 命中后是否放行、Metal 插件能否真正实例化，只能由真机的 MACF 决策回答。
离线能证明的只是"签名形态正确、信任条目自洽"，不能证明"内核会接受"。

## 4. 失败也有信息

- 若 fatal 仍在但描述改变（例如变成 trust cache 未命中）：直接给出下一条关卡，无需再猜 hash 类型。
- 若 fatal 消失但 `BKDisplayLink` 仍 0：签名不是唯一闸门，下一步查 WindowServer 选择合成路径的条件。
- 若出现 `BKDisplayLink` 但 frames 不涨：断点移到帧提交/传输，和签名无关。

## 5. 唯一变量与回滚点

唯一变量 = addon 内三个二进制的 CD 摘要类型（SHA-1→SHA-256）与 trust 模块里对应的三条 cdhash；
内核、boot、ADT、ramdisk 与 l690 逐字节相同，args 只把 `sheng-build-tag=l690` 改成 `l695s`。
回滚点 = `hold/l690/`（今日 11:36 已用它启动过一轮，包完整）。

## 6. 恢复预案

按 l695 流程：recovery 里 arm misc → fastboot 刷 `hold/l695s/payload.img` 到 cust →
`fastboot boot hold/l695s/boot-merge-r3.img`（或已装 boot_a TWRP 时 `fastboot reboot recovery`）。
若进不去 recovery：`fastboot flash boot_a /e/nabu/tmp/boot_a.twrp.img` + `fastboot reboot recovery`（11-04/今日已验证）。
