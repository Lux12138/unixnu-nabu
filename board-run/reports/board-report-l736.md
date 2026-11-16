# l736 六问板报（2026-11-12，容量 + 低位 WIMG + 官方诊断组合轮）

## 1. 唯一问题

把 l734 板上已证的两层阻断同时处理——`IONewData` 128000 B span 分配失败、低位 WIMG 比较不认 `VM_WIMG_IO=0x7`——并用 l734 的官方 `stage20/21` 字段保留可判读性后，能否让 16 MB GOP host import 到 `stage=19`，进而让 GPU handoff 出现 `P4/PF>0 / ncpy<xfer`？

## 2. 已闭合到哪

- l734 用既有 `import_stage/fail_pa/fail_offset` 把 Mesa 折叠后的 `c4641cbd` 拆开：18 次 `stage=21`，无 stage20，exact 根因是 `IONewData(NativeAdrenoPhysicalSpan,4000)` 分配 128000 B 返回 null。
- l735 将 `span_capacity` 4000→1000、分配降到 32000 B 后，`IONewData` 这一关已过；它没有 l734 诊断补丁，所以下一层 WIMG `Unknown` 仍按原始 `stage=17` 返回。事后反汇编确认 `stage=17` 不再代表分配失败。
- l733 的低位 WIMG `cmp #0x6→#0x7` 单独无效，是因为当时先卡在 `IONewData`；两个变量必须组合。
- l736 三段补丁互不重叠、反汇编逐条复核：
  1. `@0xa563e8 lsr x8,x22,#12→#14`：`span_capacity=1000`、分配 32000 B，`IODeleteData` 同源；
  2. `@0xa50f34 cmp w8,#0x6→#0x7`：可达低分支把 `VM_WIMG_IO` 归为 `NormalUncached=2`；
  3. `@0xa56418 / @0xa564e8` 复用 l734 官方字段：`IONewData` 失败标 stage21，WIMG Unknown 标 stage20 并写 `fail_pa/fail_offset`。
- 成功仍走原始 `stage=19`。不改 ABI、格式串、字段或自造探针；不新增 boot-arg。
- l733 seq1 os_log 已解码，锁屏/迁移链与 l730 同相；exact WIMG/external features/Mesa raw `kr` 仍未知，但 l734/l735 已把两个实际阻断定位到板上分支。

## 3. 为什么离线不行

静态能证明三段补丁的机器码、分支目标和容量一致，但不能证明真机 32000 B 分配成功、WIMG 实际值确为 `0x7`、后续 `BoMap`/SMMU/WIMG 分类无第三阻断，以及 GPU handoff 是否真正接管搬帧。这些只能一次上板判定。

## 4. 失败也有信息

- `import_stage=19` 且 `P3 e0` 消失、`P4/PF>0`、`ncpy<xfer` ⇒ 组合成功，GPU 接管搬帧。
- `import_stage=20 fail_pa=<页> fail_offset=<偏移>` ⇒ 分配已过但 WIMG 仍未覆盖，需核 exact attribute 与白名单。
- `import_stage=21` ⇒ 32000 B 仍分配失败，容量方案否证。
- `P4/PF>0` 但花屏/锁屏退化/WDT 异常 ⇒ handoff 打开但 cache/fence/SMMU 语义失败。
- panic / APFS / `buf_bread` / 屏退化 ⇒ 阴性回滚。

## 5. 唯一变量与回滚点

- 唯一变量：相对 `hold/l730`，kernel 35 个差异字节、3 组同长指令替换；kernel sha8 `d3598dd6→e0cedf98`。
- 五文件其余产物：`gpu-addon.bin 950a3968`、`adt 78a6f7e9`、`trust a4a6bae6`、`boot ead4b531` 与 l730 相同；payload FAT 仅替换 `KERNEL~1.MAC`，args 700/39 逐 token 相同（added=[] dropped=[]）。
- `pet600`、220/300 双槽、APFS nop、migratx/migratiox/norce、WDT `arm=0`、锁屏链全部保留。
- 回滚点：`hold/l730`。l736 一次性，不进默认 args；后续启动前显式从选定包 `flash cust` 并最后重 arm misc。

## 6. 恢复预案

- 上板只走 `fastboot flash cust` → **最后** `fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin` → `fastboot boot <boot-merge-r3.img>`。
- `nabu-wdt-arm-delay=0`、`pet=600`；l732/l733/l734/l735 均证实 `PON=0x80041` 自动回 TWRP。
- 若早 panic，WDT 已 arm；若 WDT 未触发，`fastboot devices` 应仍可用；否则才需人工长按。
- 不使用 `fastboot flash boot_a` / `flash boot`。

## 门禁状态

- `audit_bootability.py`：WSL `OK (0 fail, 0 warn)`。
- `audit_args.py`：700/39 → 700/39，added=[] dropped=[]，`OK`。
- `audit_dual.py`：12/12 match，`state/audit-l736.json` 已写。
- 本板报、`precheck.py`、`precheck_check.py`、`guard_no_boot_flash.py`、`board_report_check.py` 全 PASS 后才允许上板。
