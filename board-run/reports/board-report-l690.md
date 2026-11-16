# 上板报告 l690

## 1. 唯一问题
去掉 `nabu-mapper=2`（不创建任何 IOMapper）之后，`nabu-display: open` 的 **pid 数量**是否从 1 个（只有 85=bics_daemon）变成多个，回到 l637 的 4 个（54/94/56/84）？

## 2. 已闭合到哪
- `nabu-display: state ... mapper=` 打印的是 `identityMapper?1:0`（`NativeLegacyDisplay.cpp:907`），只反映 identityMapper 指针，不是"是否有 mapper 服务"。
- `:456`：`enabled` 不是 1/2 时 `startIdentityMapper()` 直接 return —— 不创建 mapper、也不设 `iommu-parent`（`:495` 之后才设）。
- `:524-528`：`start()` 失败时**仍保留服务注册**，只是不给 identityMapper 赋值 —— 所以 `nabu-mapper=1` 的 `mapper=0` 是"已注册但 start 失败"，**不是 NULL**。
- l637（`.pre-l641`）没有 `start()` 调用，`identityMapper` 无条件赋值 ⇒ 它的 `mapper=0` 只能是 attach 失败、未注册；该轮 IOFB 被 4 个 pid 打开、xfer=7。
- l685（`nabu-mapper=2`）：identity mapper attach+start 成功，但 `iovmMapMemory` 去掉了连续性检查（`remaining<length`），只返回第一个物理 span 却声称映射整个 length；运行时签名 `map=4 refuse=0`；IOFB 只被 1 个 pid 打开、xfer=5。
- 已排除：Metal 插件（l684 无 bundle 也 xfer=5）；AMFI/信任（l686 已过）；xfer 的定义（= 用户态 IOFB transfer 提交次数）。

## 3. 为什么离线不行
`copyMapperForDeviceWithIndex()` 在无 `iommu-parent` 时返回 NULL，之后 IOFB 走哪条路径、`create_default_fb_surface` 是否因此成功、哪些进程能 `open` —— 全是运行期 IOKit 状态。静态只能读出"代码会返回 NULL"，不能读出"NULL 之后 IOFB 能否 open"。

## 4. 失败也有信息
- pid 仍只有 85：NULL 路径也不够，下一步查 IOFB 的 `getMemoryRegion` / `layout_buffers` 失败日志（surface 创建路径），而不是继续改 mapper。
- pid 变多但 xfer 仍 5：IOFB 已通、卡在 UI 进程（SpringBoard/backboardd 未提交帧），转向 UI 拉起链。
- 出现 `smmu-mapper` 行：说明 args 没删干净（参数仍生效），本轮作废、先核 args。
- 日志里 `nabu-smmu: sid800` 探针消失是 args 生效的附带证据，不是判据。

## 5. 唯一变量与回滚点
- 唯一变量：args 删除 `nabu-mapper=2`（38→37 token）；tag 改 l690。
- 逐字节相同：kernel `efcb4c34`、boot `ead4b531`、gpu-addon `bbdb4bc2`、owner-trust `7533eec8`。
- 回滚包：`hold/l686/`（kernel efcb4c34 + trustcache 7533eec8，实测 575 s）。
- 回滚命令：`fastboot flash cust hold/l686/payload.img && fastboot boot hold/l686/boot-merge-r3.img`。

## 6. 恢复预案
- 软件侧：`fastboot flash boot_a /e/nabu/tmp/boot_a.twrp.img && fastboot reboot recovery` —— 今天 06:23 实测成功（设备从异常 fastboot 恢复）。
- 若 `fastboot boot/reboot/continue` 全部失败（l689 那次）：需要人工长按电源 10 秒，软件侧无手段；提前写明。
