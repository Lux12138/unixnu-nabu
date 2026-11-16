# l732 六问板报（2026-11-12，WIMG 单字节候选）

## 1. 唯一问题

把 `NativeAdrenoQueryPageCacheMode()` 的 `VM_WIMG_RT=0x17` 比较常量临时改为 `VM_WIMG_IO=0x7`，让 `kIOMapInhibitCache` 映射的 firmware GOP 页经现有 `t733-managed-rt-normal=1` 分支返回 `NormalUncached=2`，能否让已编入的 GPU handoff 客户端通过 `P3` 的 `newBufferWithBytesNoCopy` target import，进入 `serveOne()` 并让 GPU 接管搬帧？

这是只读静态链闭合后的**一次**运行期判定：不新增打印/探针、不改 args、不改五文件其余内容、不重编、不碰 `boot_a`。

## 2. 已闭合到哪

- l730 已现场确认锁屏与触摸，但 `xfer/ncpy/nret` 同步、`ncpy==xfer`，主瓶颈是每帧 16,384,000 B CPU blit 到 GOP。
- `gpu-addon.bin` 解压后的 `NativeAdrenoMetal` 已含 `GpuScanoutClient::serveOne`、`NBWaitInFlight`、`PF %llu`；`WAIT_SWAP/COMPLETE_SWAP` 是编译常量，旧「客户端 0 命中」为假阴性。
- l730 CDC：`P3 e0`、`HostMemory/NotRepresentable`、`import_stage=17 fail_pa=0`、`host import ... reason=10 status=c4641cbd`。
- `c4641cbd == uint32(-1000072003) == VK_ERROR_INVALID_EXTERNAL_HANDLE`；`reason=10` 是 `NativeAdrenoBoCacheImport` 分类，`flags=5` 是 `miss_flags` 计数，均非额外错误。
- `NativeAdrenoMemory.cpp:686-728` 的 stage 17 在 `firmwareImportPhysical()==0` 后逐页查询 WIMG；`Unknown` 直接返回 `kIOReturnUnsupported` 且不写 `fail_pa`，与板上 `import_stage=17 fail_pa=0` 吻合。
- `nabu-gpu-scanout=1` 数学必败、l730 未启用；因此不是 firmware import 旁路问题。
- `NativeGOPPresenter.cpp:90` / `NativeAdrenoPublish.cpp:1129` 用 `kIOMapInhibitCache`；XNU `iokit_rpc.c:485-487` 把它映射为 `VM_WIMG_IO=0x7`。
- 板上 `llvm-objdump` / `aarch64-linux-gnu-objdump` 实反汇编 `NativeAdrenoQueryPageCacheMode@0xfffffff007a54e84`：比较 `0x2 / 0x6 / 0xb / 0x12 / 0x17`，无 `0x7`，故 `VM_WIMG_IO → Unknown`。
- 已有 `t733-managed-rt-normal=1` 分支在 `0x17` 命中时返回 `NormalUncached=2`；把该比较常量改为 `0x7` 即复用既有官方语义。
- `nabu-gpu-observe=1` 官方 writer 行没有打开，且直接加入会令 args 700→719 超限；本轮不靠新增参数，直接验证静态闭合后的唯一运行期结论。

## 3. 为什么离线不行

静态能证明「`VM_WIMG_IO` 不被白名单接受」以及「改成 `0x7` 后分支会返回 `NormalUncached`」，但不能证明真机 `BoImport` 是否真的成功、`Presenter::ready()` 是否转真、`WAIT_SWAP` 是否注册、`PF>0` 是否出现、`ncpy<xfer` 是否成立，以及屏/触摸/WDT 是否保持。这些是运行期的 GPU/RenderServer 行为，只能一次上板判定。

## 4. 失败也有信息

- 若仍 `P3 e0` / `PF=0` / `ncpy==xfer`：证明 WIMG `0x7` 不是唯一阻断，后续需转内核 NGP descriptor 表示或 Mesa external-host-memory 能力；停止单字节常量方向。
- 若出现 `P4`、`PF>0` 但画面异常或 WDT/双槽异常：证明 GPU handoff 可打开但 fence/cache 语义不安全，回滚并保留 `hold/l730`。
- 若出现 panic / APFS / `buf_bread` / 屏退化：立即判阴性，WDT 自动回 TWRP，回滚。

## 5. 唯一变量与回滚点

- 唯一变量：kernel `@0xa50ef8` 一条指令 `cmp w8,#0x17 → cmp w8,#0x7`，整文件只偏移 `@10817273` 的 **1 字节** `0x5d→0x1d`；kernel sha8 `d3598dd6→766b7504`。
- 五文件其余产物：`gpu-addon.bin 950a3968`、`adt 78a6f7e9`、`trust a4a6bae6`、`boot ead4b531` 全部与 l730 相同；`payload.img` 仅替换 `KERNEL~1.MAC`，args 700/39 逐 token 相同（added=[] dropped=[]）。
- `sheng-smp=1 + nabu-wdt-arm-delay=0`、`pet600`、220/300 双槽、APFS nop、迁移修复全部保留。
- 回滚点：`hold/l730`（最佳已验证锁屏/触摸包）。恢复只需后续轮重新使用 `hold/l730`，本轮不写 `boot_a`。

## 6. 恢复预案

- 上板只走 `fastboot flash cust` → **最后** `fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin` → `fastboot boot <boot-merge-r3.img>`。
- `nabu-wdt-arm-delay=0`，`pet=600`；历史实测自动回 TWRP / PON `0x80041` 路径保持。
- 若早 panic，WDT 已 arm；若 WDT 未触发，`fastboot devices` 应仍可用；否则才需人工长按，板报提前写明。
- 不使用 `fastboot flash boot_a` / `flash boot`，任何情况下不写 recovery 所在分区。

## 门禁状态

- `audit_bootability.py`：WSL 重跑 `OK (0 fail, 0 warn)`；Windows 首跑因缺 `fsck.fat/mdir/llvm-objdump` 失败，属环境不适，已如实记录。
- `audit_args.py`：700/39 → 700/39，added=[] dropped=[]，`OK`。
- `audit_dual.py`：10/10 match，`state/audit-l732.json` 已写。
- payload FAT 长名已修复并以 `kernel.macho` 重新写入；内部 kernel/gpuaddon/trust/adt 与包外逐字节一致。
- 本板报与 `precheck.py`、`precheck_check.py`、`guard_no_boot_flash.py` 全 PASS 后才允许上板。
