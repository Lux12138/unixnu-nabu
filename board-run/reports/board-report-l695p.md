# 上板报告 l695p（publish 标记轮）

## 1. 唯一问题

在 addon 里补上 `addons/NativeAdrenoMetal.publish`（内容 `1\n`、mode 0644、与 `make-gpu-addon.py --publish` 同语义）之后，
Metal 插件是否从 stage 1（返回 nil）切到 published 模式：CDC 是否出现 `PUB …` / `STn …` 行、`frames xfer` 是否开始增长。

## 2. 已闭合到哪

- 签名闸门：l695s（SHA-256 CD + 三条 cdhash 同步到 trust）三轮独立启动，`mac_vnode_check_signature` 与
  `Hash type is not SHA256` 均为 0；同内核的 SHA-1 包每轮稳定 4 次 fatal。⇒ AMFI 那关已过。
- **frames 不涨的原因（静态）**：`NativeAdrenoMetalPlugin.mm:413`
  `if (publish::requested()) return publish::deviceForPort(port);` —— 否则走 stage 1：跑一遍 userspace test 后 `return nil`，
  不注册 device。`requested()` 检查 bundle 目录**同级**的 `NativeAdrenoMetal.publish`（`/System/Library/Extensions/` 下）。
- 解包 l685→l695s 共 11 份 addon：每份 8 个条目，**没有一份带 publish / clients / primelist**。
  所以从 l685 起插件一直在 stage 1，软件合成继续；这与 frames 恒在 5/6 一致。
- published 模式自带可观测点：`deviceForPort` 成功后每 10 s 往内核日志写 `PUB …`/`STn …`，并起
  `bridgephase::serveScanout` 线程；失败会打印 `PUB no device: <nmb_last_error>` 与 mark 0x68f2。
- 新 addon `dcd25289…`（12,317,024 B）只是在 bundle 条目之后插入一个约 80 B 的文件，其余字节与 l695s 相同；
  写进 payload 后 `fsck.fat -n` 干净、大小仍 192 MiB；payload `f84046de…`。

## 3. 为什么离线不行

`publish::deviceForPort` 要走 IOKit 拿 accelerator port、加载 Turnip、创建 `_MTLDevice` 子类并起 scanout 线程；
这些全部发生在目标机的 backboardd 里，离线只能证明"分支存在、标记文件路径对"，不能证明 device 能建起来。

## 4. 失败也有信息

- CDC 有 `PUB …`：device 已注册，看 `STn` 文与 frames；frames 仍不动 ⇒ 断点在 `serveScanout`/scanout 写入。
- CDC 有 `PUB no device: …` 或 mark `68f2`：device 创建失败，错误文本直接给出下一步（Turnip 加载、IOKit 连接、格式）。
- CDC 完全没有 PUB/ST 且 frames 不变：插件没走到 `initWithAcceleratorPort:`，回到"发现链/加载"层查（已知签名已过）。
- **backboardd 崩溃循环**：alioth 同栈实测过（`docs/devices/alioth.md:378`）——带插件时 backboardd 在 `0x106da915c` SIGBUS
  `ESR 0x82000007`（用户态读未映射的 L3），SpringBoard/PosterBoard/WidgetRenderer 一起被终止，ReportCrash 也死，写不出报告；
  去掉 publish 的对照镜像则只开 type 1 且 170 s 不崩。若我们这轮出现同样签名，"publish 文件"就是唯一变量，回退即对照。
- 即便稳定，alioth `:369` 记录过 "frames stop short of the panel … sheng-fb-gpu=1 did not move the swap counter"，
  所以 device 起来不等于帧到屏；那一步按闸门 3 单独查（`P0/P1/P2` 行）。

## 5. 唯一变量与回滚点

唯一变量 = addon 增加 `addons/NativeAdrenoMetal.publish`（其余字节、内核 `efcb4c34`、boot `ead4b531`、ADT、args 全同 l695s，
args 只把 tag 改成 `l695p`）。回滚点 = `hold/l695s/` 与 `hold/l690/`。

## 6. 恢复预案

**这次不再在 fastboot 会话里 flash boot_a**（l695s 循环的教训：flash 会让 ABL 丢掉 misc 的 boot-recovery 标记）。
流程：recovery 里 arm misc → `fastboot flash cust hold/l695p/payload.img` → `fastboot flash misc misc-bootrecovery.bin`（flash 后重新 arm）
→ `fastboot boot hold/l695p/boot-merge-r3.img`。脚本 `tmp/go_round_next.sh l695p` 已按此改写。
若已落入循环：需要 fastboot 或 TWRP 入口（长按电源断电后 电源+音量下）。
