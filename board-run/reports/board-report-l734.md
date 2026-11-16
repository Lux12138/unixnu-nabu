# l734 六问板报（2026-11-12，既有诊断字段细分 stage 轮）

## 1. 唯一问题

用板上已有 `NativeAdrenoMetal: import_stage=%u fail_pa=%llx fail_offset=%llx` 字段，把 `NativeAdrenoMemory::importTask()` stage 17 后两类不写 `fail_pa` 的失败分开，能否一次判定 `P3` 的 exact 内核原因是 WIMG `Unknown` 还是 `IONewData` 失败？

- WIMG `Unknown` → 新增 stage `20`，并写 `fail_pa=physical / fail_offset=offset`。
- `IONewData` 失败 → 新增 stage `21`。
- 成功仍 stage `19`。

这是诊断拆分轮，不声称修性能；不新增格式串、字段、ABI 或自造探针。

## 2. 已闭合到哪

- l730/l733：`P3 e0`、`HostMemory/NotRepresentable`、`import_stage=17 fail_pa=0`、`host import c4641cbd`。
- `c4641cbd=VK_ERROR_INVALID_EXTERNAL_HANDLE`；`reason=10=NativeAdrenoBoCacheImport`；`flags=5` 是 miss 计数。
- `NativeAdrenoMemory.cpp:707-745`：stage 17 后只有 `IONewData` 失败（`kIOReturnNoMemory`）、WIMG `Unknown`（`kIOReturnUnsupported`）、全页成功（stage 19）三类出口；物理段非法会写 `fail_pa/fail_offset` 且 stage 改 18，已被 `stage=17 fail_pa=0` 排除。
- Mesa `tu_knl_darwin.cc:1722-1723,1752-1765` 把 `BoImport` 任意非零 `kr` 折成同一 VkResult，现有 BO profile 只保留 VkResult，无法区分上述内核原因。
- `writer_import` / `nabu-gpu-userprobe=1` 在当前 shipped kernel 依赖空 `nabu_capture_append` 弱实现，CDC 不可用；`nabu-gpu-observe=1` 直接加入会令 args 700→719 超限。故不靠加参数，只复用已板上可见的 `import_stage/fail_pa/fail_offset`。
- 板上反汇编确认两个补丁点：`IONewData` 失败 `@0xfffffff007a5a418` 原 `cbz x8, failed`；WIMG `Unknown` `@0xfffffff007a5a4e8` 原 `cbz w0, failed`。替换为互异 stage 写入 + 精确 `b 0xfffffff007a5a4cc`，不改返回码、不绕开释放路径。
- l733 seq1 os_log 已解码：222,694 行，真实到 277.0 s；`System build version unchanged=10`、`Migration not necessary=10`、`finishedStartup:YES=2`、`Dismissing overlay=2`、`screen owner is now=0`，与 l730 同相；`AXSpringBoardServer=27`，`nabu_scanout_completed=0`。exact WIMG/features/kr 仍未知。

## 3. 为什么离线不行

静态只能把失败缩小到两类，无法知道真机上 16 MB GOP descriptor 的 `IONewData` 是否分配失败、`pmap_cache_attributes()` 是否返回 `Unknown`。两类都导致同一 `c4641cbd`，只有运行期 stage 能判定。本轮用既有官方字段读取该运行期值，属 `AGENTS.md` 允许的一次判定。

## 4. 失败也有信息

- `import_stage=20 fail_pa=<GOP页> fail_offset=<页偏移>` ⇒ WIMG `Unknown`，下一候选改 `NativeAdrenoSmmu`/WIMG 分类或 NGP descriptor 页属性。
- `import_stage=21 fail_pa=0` ⇒ `IONewData` 分配失败，下一候选改 span 容量/分配策略，不再碰 WIMG。
- 仍 `stage=17` ⇒ 补丁没命中预期分支或有更早失败，停止该二进制补丁方向。
- 若 `stage=19` 且出现 `P4/PF>0 / ncpy<xfer` ⇒ 意外获得 GPU handoff，需按阳性重新审计屏/WDT/fence。
- 若 panic / APFS / `buf_bread` / 屏退化 ⇒ 立即阴性、WDT 自动回 TWRP、回滚 l730。

## 5. 唯一变量与回滚点

- 唯一变量：相对 `hold/l730`，kernel 文件偏移 `@0xa56418..@0xa5642c` 与 `@0xa564e8..@0xa564fc` 共 9 条指令替换、33 个差异字节；kernel sha8 `d3598dd6→6686588b`。
- 五文件其余产物：`gpu-addon.bin 950a3968`、`adt 78a6f7e9`、`trust a4a6bae6`、`boot ead4b531` 与 l730 相同；payload FAT 仅替换 `KERNEL~1.MAC`，args 700/39 逐 token 相同（added=[] dropped=[]）。
- `pet600`、220/300 双槽、APFS nop、migratx/migratiox/norce、WDT `arm=0`、锁屏链全部保留。
- 回滚点：`hold/l730`（最佳已验证锁屏/触摸包）。l734 一次性，不进默认 args；后续启动前显式从选定包 `flash cust` 并最后重 arm misc。

## 6. 恢复预案

- 上板只走 `fastboot flash cust` → **最后** `fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin` → `fastboot boot <boot-merge-r3.img>`。
- `nabu-wdt-arm-delay=0`、`pet=600`；l732/l733 已实证 `PON=0x80041` 自动回 TWRP。
- 若早 panic，WDT 已 arm；若 WDT 未触发，`fastboot devices` 应仍可用；否则才需人工长按。
- 不使用 `fastboot flash boot_a` / `flash boot`。

## 门禁状态

- `audit_bootability.py`：WSL `OK (0 fail, 0 warn)`。
- `audit_args.py`：700/39 → 700/39，added=[] dropped=[]，`OK`。
- `audit_dual.py`：10/10 match，`state/audit-l734.json` 已写。
- 本板报、`precheck.py`、`precheck_check.py`、`guard_no_boot_flash.py`、`board_report_check.py` 全 PASS 后才允许上板。
