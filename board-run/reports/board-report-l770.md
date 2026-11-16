# board-report l770（GPU WAIT_SWAP 独占竞争确认轮）

## 1. 唯一问题
在 l769 已达成真桌面的包上，只把 kernel `VA 0xfffffff007a806d0 / file offset 0xa7c6d0` 的 4B `bl kernel_thread_start` 改为 `nop`，禁止 l750/l769 独有内核 `nabuScanoutClient` 线程抢先注册 handoff 独占 `client_`；用户态 `gpuscanout::Client` 能否越过 `P3 ee00002c2`，出现 `P4/PF/PX` 与 `nabu_scanout_waits/completed>0`，并在保持 `SBStartupTransitionToHome / UI Locked:NO / 主屏 PNG` 的前提下改善帧延迟？本轮不调 args、不改 plist、不改 ADT/addon、不写 boot 分区。

## 2. 已闭合到哪
- 日志优先：l769 CDC `P3 ee00002c2` 18 次，`P2/P4/PF/PX` 全 0，`nabu_scanout_waits/completed` 全 0；l750 同类 16 次。l738/l739 是 `P3 e0` + `no-copy buffer refused` / `import_stage=19`，说明旧构建线曾越过 scanout 映射、墙在 BoImport；现有日志对「l769 去掉内核竞争线程后用户态客户端能否接管」沉默。
- 用户态链：`GpuScanoutClient.h:44-68` 先 MODE、再 `mapScanout`、再 `Presenter::ready`；`NativeMetalBridgePhase.h:117` 调 `IOConnectMapMemory64(conn,0x5343,mach_task_self(),address,length,0x101)`；`l876-client-open-dis.py` 证明 MODE 输出 0x28、`P3` 而非 `P2`，故 MODE/40B ABI PASS。
- 内核链：AGX 对象 vptr `0xfffffff0072624e8` 的 `+0x590` 槽 = `AGXDeviceUserClient::clientMemoryForType @0xfffffff007a5d294`；`0xa5d3b0-0xa5d3cc` 的 `0x5343` 分支调 `integration.displayMemory(current_task(),this)`。`is_io_connect_map_memory_into_task @0xfffffff0079b99c8` 在 `mapClientMemory64()==NULL` 时折叠成 `0xe00002c2`。`clientMemoryForType` 内部 `kIOReturnNotReady` 会被这个折叠隐藏。
- 决定性竞争：l769 `native_legacy_gop_mapping_observed @0xfffffff007a80338` 在 `0xa806ac` 发布 handoff 后，`0xa806bc-0xa806d0` 启动 `kernel_thread_start(nabuScanoutClient, nullptr, &nabuScanoutThread)`；`nabuScanoutClient @0xfffffff007a806f8` 用 `kernel_task + &nabuScanoutThread` 循环调 `WAIT_SWAP`，`NabuGpuScanoutHandoff.h:112-120` 把 `client_` 设为该内核 client；`displayMemory @0xfffffff007a8009c` 的 `0xa80190-0xa801b4` 比较 `client_/clientTask_`，backboardd 必然不等，返回 null。l738 kernel 无 `nabuScanoutClient/Thread` 字面量；l750/l769 各 2 处；`funcdiff-all.txt:3-4` 标记二者为 `ONLY final`。
- 已撤回/排除：DPU 直出互斥禁用（错，l769 有 `scanout hand-off online gop=9c000000/fa0000 1600x2560 stride=1900`）、vtable 重载错配（错）、`MAP_MEM_IO` 高位必被 `vm_map_enter_mem_object_helper` 拒绝（错，l769 实际 prot 已与低权限位相与）、`P3 ee00002c2` 证明 descriptor 已返回（错，MIG 折叠）、DeviceLost/ring poison（l769 0）、MODE/ABI（P3 非 P2）、XNU VM/cache/WIMG（三版 `mapClientMemory64`/MIG 哈希相同且 `displayMemory` 同构）。
- 安全回退：禁线程后 `native_legacy_gpu_offer()` 因 `client_==nullptr` 返回 0，`NativeLegacyDisplay.cpp:705-767` 自动走原有 CPU 拷贝回退；用户态 `gpuscanout::Client::open()` 只有 `mapScanout` 与 `Presenter::ready()` 都成功才进入 `serveOne()`，不会因 NOP 本身抢写 GOP。
- 包级单变量：`l769 kernel 234eeb09f31e1814 → l770 9a5ead640d6ddf1a`，125628416B 等长，仅 `0xa7c6d0` 4B `76fae297→1f2003d5` / 1 簇；startup plist 区域 sha16 `3dc98beeec943898` 不变。反汇编确认 `0xa806d0 nop`、前后 `adr/adrp` 不变。payload `a12398ed8d51d71a` 内部仅 kernel DIFF；ADT `ddb096bf` / gpuaddon `950a3968` / trustcache `a4a6bae6` / scratch `3cd3d0d3` 全 SAME；args `ee42777c0c3a802e`，700 chars / 39 tokens，added/dropped 均空；外部 boot-merge/ADT/addon/trust 与 l769 全同。`mcopy` 重排 FAT 簇链属既有打包语义，以内部文件哈希和 args 逐 token 为准。
- 第二道墙：l769 CDC 无 `c4641cbd / HostMemory / NotRepresentable / import_stage=21 / stage17 / no-copy buffer refused`；`import_stage=19` 8 次、`reason=10` 28 次均为 `status=00000000` 的普通 slow_flags；`funcdiff` 把 `NativeAdrenoMemory::importTask` / `importFirmwareScanout` 标为 SAME；设备仓 `NativeAdrenoMemory.cpp:763-792` 为两遍扫描，`NativeAdrenoCpu.cpp:48-54` 含 `t733-managed-rt-normal=1` 的 RT 分支。旧 BoImport 拒绝证据不在 l769，但 scanout target 的 `newBufferWithBytesNoCopy` 结果仍需真机确认。
- 链文档：`state/display-latency-chain-20261114.md`、`state/l770-candidate-static-gate.md`、`state/l929-kclient-dis.txt`、`state/l930-handoff-call-wait-dis.txt`、`state/l932-gop-mapping-observed-dis.txt`。

## 3. 为什么离线不行
静态已证明内核线程抢 `client_` 会使 `displayMemory` 对 backboardd 返回 null，也证明 NOP 后该线程不再启动、CPU 回退仍完整；但 `client_` 在真机启动顺序下是否确实留空、用户态 `IOConnectMapMemory64(0x5343,0x101)` 是否成功、Metal `presenter_->ready()/newBufferWithBytesNoCopy` 是否接受 GOP target、`WAIT_SWAP` 是否开始服务帧、触摸后帧延迟与 PNG 是否不回归，都是运行期 Metal/RenderServer/面板状态。l769 日志因第一道墙从未产生这些读数。按 AGENTS §0.5，这是「验证静态结论是否成立」的一次确认轮，不是逐参数试包。

## 4. 失败也有信息
- `P3 ee00002c2 → P4/PF/PX` 且 `nabu_scanout_waits/completed>0`、桌面/PNG 不回归 ⇒ 内核 client 竞争为唯一第一道墙，GPU WAIT_SWAP 已接管，继续量化触摸后帧延迟。
- 变成 `P3 e0` / `PF=0` 并出现 `no-copy buffer refused/c4641cbd/import_stage=17|21` ⇒ 第一道墙已破，第二道墙是 BoImport/WIMG/span；按已有 l738 静态链继续闭链，不再调单参数。
- 仍 `P3 ee00002c2` ⇒ 还有另一个 client/close 路径或 4B patch 未生效；用 CDC 的 handoff online、open type、内核反汇编与板上 diag 判读，停止本方向。
- `P4/PF>0` 但画面黑/撕裂/WDT/槽位异常 ⇒ handoff 接管成功但 fence/cache 语义不安全，回滚 `hold/l769`，保留桌面基线。
- 无 CDC / 早死 ⇒ 按最后 CDC 帧与 NABULOG 判读，回滚 l769，不继续调参。

## 5. 唯一变量与回滚点
唯一变量 = payload 内 `kernel.macho` 的 4B `bl→nop`；l769 的 44B `SBDontLockAfterCrash` plist、args、ADT、gpuaddon、trustcache、boot-merge、owner-trust 全部逐字节相同。回滚 = `hold/l769` payload `496f583ea5c2bf43…` / kernel `234eeb09f31e1814…`，按同样 `flash cust + flash misc LAST + fastboot boot` 恢复；恢复只认用户点名 `backup-boot_a-20261103.img`，本轮绝不 `flash boot*` 或 `erase`。

## 6. 恢复预案
上板前用 `l770-run.py` 重新查 `adb devices == f91fa664 recovery`、`fastboot devices`、双槽 `slot-unbootable:a/b=no`、misc 4096B 以 `boot-recovery` 开头且 sha16 `b9f0f5a538242bf5`。允许的唯一写入顺序 = `fastboot -s f91fa664 flash cust E:/nabu/hold/l770/payload.img` → `fastboot -s f91fa664 flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin`（`flash cust` 会清 boot-recovery，必须 re-arm LAST）→ 再查槽位 → `fastboot -s f91fa664 boot E:/nabu/hold/l770/boot-merge-r3.img`。boot_a 保持 TWRP；任何 `erase` / `flash boot*` 禁止，除 cust/misc 外不写分区。CDC 采集进程在 boot 前启动，按 USB `VID_1209:PID_316D` 动态等待串口（COM3 仅回退），从首次成功打开起采 600s、每 10s flush；`sheng-smp=1` 保持 `nabu-wdt-arm-delay=0`，args 无 `nabu-wdt-pet`，不假设 pet=600。post 链拉 seq0/seq1 NABUDIAG、PNG、os_log，做 epoch/PNG 防陈旧；若早死/USB 无响应，软件侧不强拉，等人工按键回 TWRP。
