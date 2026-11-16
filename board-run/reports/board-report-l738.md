# l738 六问板报（2026-11-12，两遍扫描 importTask 精确 span 容量）

## 1. 唯一问题

在保持 l730 全部启动/迁移/WDT/双槽修复的前提下，`importTask()` 两遍扫描、精确 span 容量能否消除 `IONewData(4000 × 32B)` 的 stage21 分配失败，让 Mesa host import 成功并进入 `WAIT_SWAP/COMPLETE_SWAP`，从而停止每帧 16 MB CPU blit？

## 2. 已闭合到哪

- l734 官方诊断精确钉定 `import_stage=21` 为 `IONewData(NativeAdrenoPhysicalSpan, 4000)` 返回 null；l735/l736 证明 32000 B 仍失败、WIMG 补丁未执行；容量缩至 <4096 B 风险内核堆越界，已静态否决。
- 两遍扫描源码增量：第一遍逐页验证物理/WIMG 并数合并后真实 span 数，第二遍按精确容量分配/填充；firmware 单 span 快路径保留；stage17/18/19 语义、`fail_pa/fail_offset`、`goto failed` 释放链不变。源码 `d266d1e9 → d071fe10`，`archive-19` 编译 rc=0，42 对象 / `libAdreno740.a` 3,170,688 B。
- 同字面路径重建：`unshare -m` + bind 独立 tree 到 `/root/merge/out-v6/.../tree`，`--out` 保持独立根；compile-command 19 个 out-v6 字面量、零 l738-full；18 段 ledger `old_hex` 全匹配（verify `GO_FOR_LEDGER`）。
- 重放 204 字节 / 18 段 l701–l730 ledger → 候选 kernel `dcef3f10`；18/18 `new_hex` 全命中；importTask 反汇编确认两次 `IOMalloc_internal`（firmware/非 firmware 分配路径）+ 三次 `QueryPageCacheMode`（两遍循环 + firmware 检查）。
- 五文件：kernel `dcef3f10` / ADT `dcac3cfe`（211,568 B）/ payload `d06bb9be` / trust `a4a6bae6` / gpu-addon `950a3968` / boot `ead4b531`；后三与 l730 逐字节一致；args 700 B / 39 token，`sheng-smp=1 + nabu-wdt-arm-delay=0`。
- WDT/双槽基线（l730 实测）：`pet=600 arm=0`、内核到 336.8 s、seq0≈220 s / seq1≈300 s、PON `0x80041` 自动回 TWRP `f91fa664`、无 panic/APFS/buf_bread。

## 3. 为什么离线不行

静态链只能证明分配容量、源码语义与包结构；`c4641cbd/P3`、`import_stage=19`、`P4/PF/PX`、`ncpy<xfer`、锁屏流畅度与 WDT 咬合都必须由板上 CDC/诊断窗口判定。

## 4. 失败也有信息

- `import_stage=19`、无 `c4641cbd/P3`、`P4/PF>0`、`ncpy<xfer` ⇒ 两遍扫描方向有效，继续评估 GPU handoff。
- 仍 `stage21` ⇒ 查自有 descriptor backing 路径或 `IONewData` 精确容量之外的分配/权限限制，不再猜 WIMG。
- `stage18`/`fail_pa!=0` ⇒ 物理段/WIMG 是第一阻断，按官方诊断词定位。
- 任何 APFS nop 缺失、panic、buf_bread、WDT 异常、锁屏丢失 ⇒ 立即回滚 l730。

## 5. 唯一变量与回滚点

- 唯一功能变量：`importTask()` 两遍扫描/精确 span 容量；其余 l701–l730 修复保持语义等价（18 段 `new_hex` 全命中）。
- 不新增 printf/IOLog/探针；不改默认 args；不碰 `boot_a`；不启用 DPU；不放宽安全策略。
- 回滚点：`hold/l730`，`cust` `3876e1a2…`，`misc` `b9f0f5a5…`。

## 6. 恢复预案

- 上板只走 `fastboot flash cust` → **最后** `fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin` → `fastboot boot <boot-merge-r3.img>`。
- `pet=600`、`arm=0`；PON `0x80041` 自动回 TWRP；不使用 `fastboot flash boot_a` / `flash boot`。
- 每轮失败即逐字节恢复 `cust/misc` 到 l730 并停止自动轮，先读 CDC。

## 门禁状态

- `audit_bootability.py`：OK (0 fail, 0 warn)。
- `audit_args.py`：700/39 → 700/39, added=[] dropped=[]。
- `audit_dual.py`：7/7 match, `state/audit-l738.json` 已写。
- `precheck.py --chain/--why_board`：待运行。
- `precheck_check.py`、`guard_no_boot_flash.py`、`board_report_check.py`：待运行。
