# board-report l771（GOP carveout WIMG 分类确认轮）

## 1. 唯一问题
在 l770 已破除第一道墙、桌面不回归的基线上，只把 shipped kernel `NativeAdrenoQueryPageCacheMode` 的 out-of-window 分支从 `Unknown=0` 改成当前设备仓源码的 `Cached=1`（file offset `0xa50ebc`，4B `mov w0,#0→#1`），能否让 scanout target 的 `NativeAdrenoMemory::importTask()` 越过 `import_stage=17 / c4641cbd / HostMemory/NotRepresentable`，出现 `P4/PF/PX` 与 `nabu_scanout_waits/completed>0`，同时保持 `SBStartupTransitionToHome` / 主屏 PNG 不回归？本轮不改 args、不改 startup plist、不改 addon/ADT，不写 boot 分区。

## 2. 已闭合到哪
- 日志优先：l769 CDC `P3 ee00002c2` 18 次；l770 4B NOP 后变 `P3 e0` 18 次、`P3 ee00002c2=0`，桌面 PNG/ToHome 不回归，证明第一道墙（内核 `nabuScanoutClient` 抢 handoff 独占 `client_`）已破。l770 仍有 `P4/PF/PX=0`、`nabu_scanout_waits/completed=0`。
- 第二道墙文本：`no-copy buffer refused: no-copy buffer: Unsupported(Unsupported { feature: HostMemory, needs: NotRepresentable, location: None, symbol: None })`；`import_stage=17 fail_pa=0`；`host import reason=10 status=c4641cbd`。历史板报已确认 `c4641cbd = uint32(-1000072003) = VK_ERROR_INVALID_EXTERNAL_HANDLE`，`reason=10 = NativeAdrenoBoCacheImport`。
- shipped addon 链：`GpuScanoutPresenter.h:37-42` 用 `scanout.base` 直接调 `newBufferWithBytesNoCopy`，`ready()` 仅看 `target_ != nil`；`NativeMetalBridgePhase.h:121-123` 写 `P<step> e<hex>`；`GpuScanoutClient.h:56-66` 中 `mapScanout()` 成功但 `presenter_->ready()` 为假时 `fail(3,0)` → `P3 e0`。`NativeBridgeMetalFrontend.mm:1644-1649` 是 `no-copy buffer refused` 日志生成处。
- Rust/runtime 链：`NativeMetalObjects.rs:612-646` 调 `new_buffer_with_bytes_no_copy(HostMemory,...)`；`buffer_descriptor.rs:292-316 / 406-443` 走 `buffer_over_host_memory()`；`host_memory.rs:327-347` 的 `not_importable()` 就是 `Unsupported::not_representable(Feature::HostMemory)`，并把 `VK_ERROR_INVALID_EXTERNAL_HANDLE/UNKNOWN` 映射为同一错误；`Importer::new`（`:541-580`）还会在 `ExternalBufferProperties` 不含 `IMPORTABLE` 或带 `DEDICATED_ONLY` 时返回同一文本。
- shipped kernel 链：`NativeAdrenoMemory.cpp:697-809` 的 `importTask()` 在 stage17 进入 `firmwareImportPhysical()`；当前 args 无 `nabu-gpu-scanout=1`，`firmwareImportPhysical()`（`:665-695`）必然返回 0，随后逐页调 `NativeAdrenoQueryPageCacheMode()`；`mode==Unknown` 直接 `goto failed`，`native_adreno_import_stage` 保持 17、`fail_pa=0`。l770 反汇编 `0xa5a464 bl QueryPageCacheMode` 后 `0xa5a468 cbz w0, fail` 与源码一致。
- **源码/二进制分叉**：当前设备仓 `NativeAdrenoCpu.cpp:31-59` 在 out-of-window（GOP firmware carveout）分支返回 `Cached=1`，注释明确写这是 l747 修复；旧快照 `NativeAdrenoCpu.cpp.pre-l738-wimg:31-55` 返回 `Unknown=0`。l733/l738/l750/l769/l770 shipped kernel 的 `NativeAdrenoQueryPageCacheMode @0xfffffff007a54e84` 在 `0xa54ebc` 仍是 `mov w0,#0; ret`，即旧版 Unknown。`l944-importtask-loop-full.txt` 逐指令复验。
- 与旧轮区别：l732 改高分支 `#0x17→#0x7`、l733 改低位 `cmp w8,#0x6→#0x7`（`0xa50f35`），都只改 in-window switch，没碰 `0xa54ebc` 的 out-of-window 返回；l735/l736 改 span_capacity，l738 两遍扫描只消掉 stage21。旧轮 `P3 e0/c4641cbd` 不变不否证本次候选。
- 包级单变量：`l770 kernel 9a5ead640d6ddf1a → l771 6ed5e20a321d9aae`，125628416B 等长，仅 `0xa50ebc` 4B `00 00 80 52→20 00 80 52` / 1 簇；l770 的 `0xa7c6d0` 4B NOP 保留，startup plist 区域 sha16 `3dc98beeec943898` 不变。payload `2f2253b3771413bf` 内部仅 kernel DIFF；ADT `ddb096bf` / gpuaddon `950a3968` / trustcache `a4a6bae6` / scratch `3cd3d0d3` 全 SAME；args `ee42777c0c3a802e`、700 chars / 39 tokens，added/dropped 均空；外部 boot-merge/ADT/addon/trust 与 l770 全同。`mcopy` 重排 FAT 簇链属既有打包语义，以内部文件哈希和 args 逐 token 为准。

## 3. 为什么离线不行
静态已证明 l771 的 4B 改动会让 out-of-window GOP 页从 `Unknown` 变成 `Cached`，并让 `importTask` 的逐页 `cbz w0` 失败分支不再命中；但 `firmwareImportPhysical()==0` 后的 span 分配、Vulkan `vkGetMemoryHostPointerPropertiesEXT` 是否返回 `VK_ERROR_INVALID_EXTERNAL_HANDLE`、`ExternalBufferProperties` 的 `IMPORTABLE/DEDICATED_ONLY` 能力位、以及 Metal `Presenter::ready()` 是否最终转真，都是 shipped Turnip/Mesa 与真机物理页状态组合出的运行期量。现有 l770 CDC 因第一道墙后停在 stage17，不能静态确定越过 stage17 后是否还有第三层能力墙。按 AGENTS §0.5，这只允许做一次「验证静态结论在真机上是否成立」的确认轮，不是逐参数试包。

## 4. 失败也有信息
- `P3 e0 → P4/PF/PX`，`nabu_scanout_waits/completed>0`，`import_stage=17/c4641cbd` 消失，桌面/PNG 不回归 ⇒ out-of-window WIMG Unknown 是第二道墙，GPU WAIT_SWAP 客户端已接管；继续量化触摸后帧延迟。
- `import_stage=17→19` 但 `P3 e0/c4641cbd/HostMemory/NotRepresentable` 仍在 ⇒ 内核页分类已过，剩余墙是 Turnip/Mesa `VK_EXT_external_memory_host` 的 `IMPORTABLE/DEDICATED_ONLY` 或 `vkGetMemoryHostPointerPropertiesEXT` 驱动拒绝；按 `host_memory.rs:541-580 / :343-347` 继续静态闭链，不再改 WIMG 常量。
- 仍 `import_stage=17 fail_pa=0` ⇒ 还有另一条 Unknown 路径或 patch 未生效；用 CDC 与 kernel 反汇编判读，停止本方向。
- `P4/PF>0` 但画面黑/撕裂/WDT/槽位异常 ⇒ handoff 已接管但 cache/fence 语义不安全，回滚 l770/l769，保留桌面基线。
- 无 CDC / 早死 ⇒ 按最后 CDC 帧与 NABULOG 判读，回滚 l770，不继续调参。

## 5. 唯一变量与回滚点
唯一变量 = payload 内 `kernel.macho` 的 `0xa50ebc` 4B `mov w0,#0→#1`；l770 的 4B 内核线程 NOP、l769 的 44B `SBDontLockAfterCrash` plist、args、ADT、gpuaddon、trustcache、boot-merge、owner-trust 全部逐字节相同。回滚 = `hold/l770` payload `a12398ed8d51d71a…` / kernel `9a5ead640d6ddf1a…`（第一道墙已破、桌面不回归的安全基线）；再远可回 `hold/l769` payload `496f583ea5c2bf43…`。恢复只认用户点名 `backup-boot_a-20261103.img`，本轮绝不 `flash boot*` 或 `erase`。

## 6. 恢复预案
上板前用 `l771-run.py` 重新查 `adb devices == f91fa664 recovery`、`fastboot devices`、双槽 `slot-unbootable:a/b=no`、misc 4096B 以 `boot-recovery` 开头且 sha16 `b9f0f5a538242bf5`。允许的唯一写入顺序 = `fastboot -s f91fa664 flash cust E:/nabu/hold/l771/payload.img` → `fastboot -s f91fa664 flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin`（`flash cust` 会清 boot-recovery，必须 re-arm LAST）→ 再查槽位 → `fastboot -s f91fa664 boot E:/nabu/hold/l771/boot-merge-r3.img`。boot_a 保持 TWRP；任何 `erase` / `flash boot*` 禁止，除 cust/misc 外不写分区。CDC 采集进程在 boot 前启动，按 USB `VID_1209:PID_316D` 动态等待串口（COM3 仅回退），从首次成功打开起采 600s、每 10s flush；`sheng-smp=1` 保持 `nabu-wdt-arm-delay=0`，args 无 `nabu-wdt-pet`，不假设 pet=600。post 链拉 seq0/seq1 NABUDIAG、PNG、os_log，做 epoch/PNG 防陈旧；若早死/USB 无响应，软件侧不强拉，等人工按键回 TWRP。
