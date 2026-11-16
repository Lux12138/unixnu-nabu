# l733 六问板报（2026-11-12，正确低分支 WIMG 单字节候选）

## 1. 唯一问题

把 `NativeAdrenoQueryPageCacheMode()` 低位 switch 中 `VM_WIMG_WCOMB=0x6` 的比较常量临时改为 `VM_WIMG_IO=0x7`，让 `kIOMapInhibitCache` 映射的 firmware GOP 页返回 `NormalUncached=2`，能否让已编入的 GPU handoff 客户端通过 `P3` 的 `newBufferWithBytesNoCopy` target import，进入 `serveOne()` 并出现 `P4 / PF>0 / ncpy<xfer`？

这是 l732 错误高分支被否证后、按同一静态链修正实现位置的**一次**运行期判定；不新增打印/探针、不改 args、不改五文件其余内容、不重编、不碰 `boot_a`。

## 2. 已闭合到哪

- l730 已现场确认锁屏与触摸，但 `ncpy==xfer==nret`，主瓶颈是每帧 16,384,000 B CPU blit 到 GOP。
- `gpu-addon.bin` 已含 `GpuScanoutClient::serveOne`、`NBWaitInFlight`、`PF %llu`；`WAIT_SWAP/COMPLETE_SWAP` 是编译常量，旧「客户端 0 命中」为假阴性。
- l730 CDC：`P3 e0`、`HostMemory/NotRepresentable`、`import_stage=17 fail_pa=0`、`host import reason=10 status=c4641cbd`。
- `c4641cbd == uint32(-1000072003) == VK_ERROR_INVALID_EXTERNAL_HANDLE`；`reason=10` 是 `NativeAdrenoBoCacheImport` 分类，`flags=5` 是 `miss_flags` 计数，均非额外错误。
- `NativeGOPPresenter.cpp:90` / `NativeAdrenoPublish.cpp:1129` 用 `kIOMapInhibitCache`；XNU `iokit_rpc.c:485-487` 把它映射为 `VM_WIMG_IO=0x7`。
- 板上 `NativeAdrenoQueryPageCacheMode@0xfffffff007a54e84` 反汇编：`cmp w8,#0xa; b.le low_branch`，低分支只识别 `#0x2` 与 `#0x6`，高分支才比较 `#0xb/#0x12/#0x17`。因此 `0x7` 属于低分支。
- l732 把高分支 `#0x17` 改成 `#0x7` 后仍 `P3 e0 / import_stage=17 / c4641cbd`，且 `P4/PF=0`、纯黑、`xfer=0`、只触发 seq0、约 275 s 自动回 TWRP；证明改高分支不可达，不否定 `VM_WIMG_IO` 假设本身。
- l733 改为把低分支 `cmp w8,#0x6` 改成 `cmp w8,#0x7`；该分支原本返回 `NormalUncached=2`，且当前 NabuAdreno host-import 路径没有依赖 WCOMB 分类的核心 import，是可达且语义最接近的单字节变量。
- `nabu-gpu-scanout=1` 数学必败且未启用；本轮不碰它。`nabu-gpu-observe=1` 会使 args 700→719 超限、且 l730 官方 writer 行未打开；本轮不新增参数。

## 3. 为什么离线不行

静态能证明「`VM_WIMG_IO=0x7` 会走低分支」「把 `#0x6` 改成 `#0x7` 后会在该分支返回 `NormalUncached=2`」，但不能证明真机 `BoImport` 是否成功、`Presenter::ready()` 是否转真、`WAIT_SWAP` 是否注册、`P4/PF>0` 是否出现、`ncpy<xfer` 是否成立、锁屏/触摸/WDT/双槽是否保持。这些是运行期的 GPU/RenderServer/面板行为，只能一次上板判定。

## 4. 失败也有信息

- 若仍 `P3 e0 / PF=0 / import_stage=17 / c4641cbd`：证明 `VM_WIMG_IO` 分类不是唯一阻断，停止单字节 WIMG 常量方向，转内核 NGP descriptor 表示或 Turnip/Mesa external-host-memory 能力。
- 若 `P4/PF>0 / ncpy<xfer` 但画面异常、半帧/花屏或 WDT/双槽异常：证明 GPU handoff 已打开但 descriptor/fence/cache 语义不安全，回滚并保留 `hold/l730`。
- 若 panic / APFS / `buf_bread` / 屏退化：立即判阴性，WDT 自动回 TWRP，回滚。

## 5. 唯一变量与回滚点

- 唯一变量：相对 `hold/l730`，kernel `@0xa50f35` 的 **1 字节** `0x19→0x1d`，对应指令 `cmp w8,#0x6 → cmp w8,#0x7`；kernel sha8 `d3598dd6→55e0f286`。
- 五文件其余产物：`gpu-addon.bin 950a3968`、`adt 78a6f7e9`、`trust a4a6bae6`、`boot ead4b531` 全部与 l730 相同；`payload.img` 仅替换 `KERNEL~1.MAC`，args 700/39 逐 token 相同（added=[] dropped=[]）。
- `sheng-smp=1 + nabu-wdt-arm-delay=0`、`pet600`、220/300 双槽、APFS nop、迁移修复全部保留。
- 回滚点：`hold/l730`（最佳已验证锁屏/触摸包）。l732/l733 都是一次性轮，不进默认 args；后续启动前必须显式从选定包 `flash cust` 并最后重 arm misc。

## 6. 恢复预案

- 上板只走 `fastboot flash cust` → **最后** `fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin` → `fastboot boot <boot-merge-r3.img>`。
- `nabu-wdt-arm-delay=0`，`pet=600`；l732 已实证 `PON=0x80041` 自动回 TWRP，本轮沿用。
- 若早 panic，WDT 已 arm；若 WDT 未触发，`fastboot devices` 应仍可用；否则才需人工长按，板报提前写明。
- 不使用 `fastboot flash boot_a` / `flash boot`，任何情况下不写 recovery 所在分区。

## 门禁状态

- `audit_bootability.py`：WSL `OK (0 fail, 0 warn)`。
- `audit_args.py`：700/39 → 700/39，added=[] dropped=[]，`OK`。
- `audit_dual.py`：10/10 match，`state/audit-l733.json` 已写。
- 本板报、`precheck.py`、`precheck_check.py`、`guard_no_boot_flash.py`、`board_report_check.py` 全 PASS 后才允许上板。
