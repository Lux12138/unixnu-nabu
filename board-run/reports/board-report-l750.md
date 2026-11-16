# board-report l750（功能轮：DPU 直出取代每帧 16MB CPU blit）

## 1. 唯一问题
把已验证的 DPU 直出内核 `9011f295`（历史实测 `ncpy=0 xfer=122 fingers=430`：GPU 画 + DPU 读 GPU 内存、零 CPU 拷贝、触摸活跃）装进当前启动链，能否在真机上重新拿到**无拷贝上屏**的帧与触摸，把用户目验"很卡"的锁屏推进到流畅可用？本轮不查死因、不补诊断，只看功能指标 `xfer/ncpy/fingers` 与锁屏。

## 2. 已闭合到哪
- 功能基线事实：l730 用户目验真 iPadOS 锁屏 + 可触摸，但每帧 16MB CPU blit 到 GOP、帧交换极慢（l731 定性：显示是原生/GPU 合成 + CPU blit）。
- 无拷贝路径已实证：l738 时代 `9011f295` 内核达成 `ncpy=0 xfer=122 fingers=430`，`startDirect()` 写正确 VIG0 `0xae05000`，`native_ufs_dpu_state=0x80000000` + `mode=2` 两处数据门已开（本包内核实测同值）。
- 现役内核（l747/l748 = d6e2f33b）的 `native_ufs_dpu_state` 也是 `0x80000000`、`mode=2` 默认，但属 src0only 线、直出未生效；`9011f295` 是独立构建产物 `/root/merge/l738-full/build/l738-kernel-dpuscanout2.macho`。
- 包组装：以 `hold/l738/payload.img` 为基，`mcopy` 单文件替换内部 `kernel.macho`；ADT/boot/trust/addon/scratch-data 与 l738 全同。内部哈希实测：kernel `9011f295242fea5b`，adt `ddb096bf1b20222f`，gpuaddon `950a3968baa13ccd`，trustcache `a4a6bae61f3febcf`，scratch `3cd3d0d3a755920a`。trustcache 是用户态签名豁免、不绑定 kernel.macho，换内核不影响 AMFI。
- 与 hold/l738 内核 diff 为 620 万字节/22781 簇 = **两条构建线的整体布局漂移**，不是单变量补丁；已如实记录，本轮作为"整机功能组合"验证，不冒充单变量实验。

## 3. 为什么离线不行
`xfer/ncpy/fingers`、帧交换延迟、锁屏是否流畅都是运行期事实；静态只能确认内核里两处数据门已开、payload 五文件齐备。要确认"真的不卡"必须真机看帧。

## 4. 失败也有信息
- `xfer>0 且 ncpy=0` ⇒ 无拷贝直出恢复，功能达成，进入帧率/撕裂治理；
- `xfer>0 但 ncpy=xfer` ⇒ 仍走 CPU blit，回 l730 基线改推 WAIT_SWAP 客户端；
- 无帧/死 ⇒ 记录最后 frames 行，回 hold/l730（用户目验基线），不改诊断路线。

## 5. 唯一变量与回滚点
变量 = payload 内 `kernel.macho`（c9f395b4 → 9011f295）；ADT/boot/trust/addon/scratch-data/args 与 hold/l738 全同（39 token，700 chars）。回滚 = `hold/l730`（用户目验锁屏）/`hold/l738`。仅 `fastboot boot`，不写任何分区。

## 6. 恢复预案
`flash cust` + `fastboot boot`（boot_a 保持 TWRP），**任何 erase 禁止**；boot 前查 `slot-unbootable:a/b`；`sheng-smp=1` 配 `nabu-wdt-arm-delay=0`；pet=600。若设备卡在 USB 无响应（l749 状态），软件侧不强拉，等人工按键回 TWRP；恢复只用用户点名 `backup-boot_a-20261103.img`。CDC 采集 600s 足以覆盖出帧窗口（历史 150–330s）。
