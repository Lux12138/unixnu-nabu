# l738 六问板报草稿（未生成候选包，禁止用于上板）

> 这是门禁模板，不是已审核板报。仅当同路径 namespace 构建成功、18 段 old_hex 全匹配、两遍扫描增量与 l730 语义补丁均已通过全套审计后，才允许填写实测哈希、运行 `board_report_check.py` 并改名为 `board-report-l738.md`。`E:/nabu/tmp/board_report_check.py` 只认六节标题且拒绝空/占位正文；正式板报 mtime 必须在 precheck 的 2400 s 窗口内，所有“待填”必须替换为实测证据。

## 1. 唯一问题

在保持 l730 全部启动/迁移/WDT/双槽修复的前提下，`importTask()` 两遍扫描、精确 span 容量能否消除 `IONewData(4000 × 32B)` 的 stage21 失败，让 Mesa host import 成功并进入 `WAIT_SWAP/COMPLETE_SWAP`，从而停止每帧 16 MB CPU blit？

## 2. 已闭合到哪

- 待填：完整流水线产物路径、kernel sha256、payload sha256、ADT/addon/trust/boot 五文件哈希。
- 待填：新 kernel 的 25 LC、无 `LC_CODE_SIGNATURE`、18 段 VA 与 out-v6 一致性、18 段 ledger old_hex 全匹配证明。
- 待填：两遍扫描源码 diff、对象编译结果、源码/二进制补丁 ledger 的具体来源与静态链 `--chain/--why_board`。
- 待填：args 700 字节 / 39 token，`sheng-smp=1 + nabu-wdt-arm-delay=0`，pet600 / diagAt0=220 / diagAt1=300 双槽。
- 已验证 WDT/双槽基线（`logs/rounds/l730.json:27-30`）：l730 `pet=600 arm=0`、内核到 336.8 s、seq0≈220 s / seq1≈300 s 两槽非 stale、PON `0x80041` 自动回 TWRP `f91fa664`、无 panic / APFS unencrypted / buf_bread；BITE / WDT.CFG / Mu / `NABU_WDT_BITE_MS` / `arm-delay=0` 全未动。l738 不重复时间常数轮，只保持同一安全窗口。

## 3. 为什么离线不行

静态链只能证明分配容量、源码语义与包结构；`c4641cbd/P3`、`import_stage=19`、`P4/PF/PX`、`ncpy<xfer`、锁屏流畅度与 WDT 咬合都必须由板上 CDC/诊断窗口判定。

## 4. 失败也有信息

- `import_stage=19`、无 `c4641cbd/P3`、`P4/PF>0`、`ncpy<xfer` ⇒ 两遍扫描方向有效，继续评估 GPU handoff。
- 仍 `stage21` ⇒ 还需查自有 descriptor backing 路径或 `IONewData` 精确容量之外的分配/权限限制，不再猜 WIMG。
- `stage18`/`fail_pa!=0` ⇒ 物理段/WIMG 是第一阻断，按官方诊断词定位，不回滚到容量猜测轮。
- 任何 APFS nop 缺失、panic、buf_bread、WDT 异常、锁屏丢失 ⇒ 立即回滚 l730。

## 5. 唯一变量与回滚点

- 唯一功能变量：`importTask()` 两遍扫描/精确 span 容量；其余 l701–l730 修复保持语义等价。
- 不新增 printf/IOLog/探针；不改默认 args；不碰 `boot_a`；不启用 DPU；不放宽安全策略。
- 回滚点：`hold/l730`，`cust` `3876e1a2…`，`misc` `b9f0f5a5…`。

## 6. 恢复预案

- 上板只走 `fastboot flash cust` → **最后** `fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin` → `fastboot boot <boot-merge-r3.img>`。
- `pet=600`、`arm=0`；PON `0x80041` 应自动回 TWRP；不使用 `fastboot flash boot_a` / `flash boot`。
- 每轮失败即逐字节恢复 `cust/misc` 到 l730 并停止自动轮，先读 CDC。

## 门禁状态

- `audit_bootability.py`：待运行。
- `audit_args.py`：待运行。
- `audit_dual.py`：待运行。
- `precheck.py --chain/--why_board`、`precheck_check.py`、`guard_no_boot_flash.py`、`board_report_check.py`：待运行。
- `E:/nabu/tmp/precheck.py` 八个必填字段为 `--question --logs --window --kernel --args --device --chain --why_board`；`--chain` 不得为 none/no/unknown/?/-；`sheng-smp=1` 必须配 `nabu-wdt-arm-delay=0`；`--pkg/--baseline` 必须成对给，stamp 有效 2400 s。
