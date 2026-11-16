# board-report l749（诊断仪器轮：补 ADT panic 区，拿完整 SError 转储）

## 1. 唯一问题
补上 ADT 的 `pram` + `chosen/embedded-panic-log-size` 后，下一次 SError panic 能否留下**完整转储**（`NABUPANIC1`：panic 文本 + backtrace / 寄存器 / 内核状态），从而把死因从"仅 122B 的异常类型"推进到可定位的调用栈？本轮**不推进画面**，只补诊断通路。

**措辞修正（离线读源码发现）**：`arm64_implementation_specific_error()`（sleh.c:398）只 `panic_plain("Unhandled implementation specific error\n")`，**不打印 esr/far**；`SError esr: ... far: ...`（:436）仅在 `CONFIG_KERNEL_INTEGRITY && KERNEL_INTEGRITY_WT` 非 DEVELOPMENT 生效，本内核不适用；`panic_with_thread_kernel_state()`（打印含 esr/far 的寄存器转储，:464）只被同步异常路径调用，SError 不走它。⇒ 完整转储**可能不含 ESR/FAR**，但 panic 主路径（debug.c）仍会给出 backtrace / 寄存器 / 线程状态。判读时以实际拿到的为准，不预设 FAR。

## 2. 已闭合到哪（排除清单，逐环）
- **排除"无声硬挂"**：l747-cdc-merged 尾 = 格式化 `panic(cpu 0 caller 0xfffffff007479bac): Unhandled implementation specific error`；全仓 40+ panic 中**唯一** SError（其余为早期轮 data abort / mount / launchd）。活轮 diag 里的同字符串是 `__cstring` 常量驻留（NUL 分隔、无 `panic(cpu` 前缀），已区分。
- **XNU 机制链**：`sleh.c:398 arm64_implementation_specific_error` ← `:450 arm64_platform_error`（`PE_handle_platform_error` 失败且 `platform_error_handler==NULL`）← `:2240 sleh_serror`（PLAT_ERR_SRC_ASYNC）；`IOPlatformIO.cpp:55` 在 nabu 无 provider 返回 false。符号表：caller = `_mach_syscall_trace_exit` 入口，紧邻 `_sleh_serror`（0xfffffff007479934 +0x278）结尾 ⇒ LR 落边界，SError 从异常返回路径到达。**异步** ⇒ 日志停在无辜 daemon spawn 处。
- **排除 3 条 SMMU 假说**：散页 map 路径（`map&&!observed`）不触 SMMU MMIO；`vm_unmap=0`；budget=262144 有界（超时返回 ArmSmmu4KTimeout）。运行数据：`vm_map calls=76` 全完成、`tlb_us` 合计 286µs、`jobs=0`、`poisoned_*=0`。
- **排除 nabuScanoutClient**：l730/l731/l738/l739/l745/l746/l747/l748 共 8 个 hold 内核全为 0（含用户目验锁屏的 l730/l731）；`CommandWake=1` 仅字面量。l743 首选嫌疑作废。
- **排除 CA 已上屏/已建纹理**：l748 seq0（260s）800×1280 全黑（2 非零像素）；`xfer=0 nval=0 tex x0`；`idg=1/0/0`（导入成功）。
- **闭合 gPanicBase=0 的三路抵消**：① `0003-nabu-panic-log-region.patch` 把 `PLATFORM_PANIC_LOG_PADDR` 改 0x9e000000/0x80000，正中 GOP splash `0x9c000000..0x9e400000`；② `native-dt-panic-region.patch` 以 `XNU_DT_PANIC_REGION=y` 关掉 `#ifdef` 分支改走 ADT；③ 当前 ADT 缺 `pram`/`embedded-panic-log-size`（各 0 次）⇒ `check_for_panic_log()` 提前 return ⇒ 启动日志两次 `debug_log_init: Error!! gPanicBase is still not initialized`。**三条路互相抵消一个都不通**——这就是十几轮拿不到转储的确切原因。
- **闭合 ADT 属性语义**（对照 patch 校验逐条）：`pram.reg` 需 16B（2×u64）✓、`chosen.embedded-panic-log-size` 需 4B ✓、`len(0x80000) <= reg[1](0x80000)` ✓、`>= sizeof(embedded_panic_header)` ✓。
- **闭合地址选择**：当前 DTS `nabu.dts:22 panic_region: panic@9b000000 reg=<0 0x9b000000 0 0x80000> no-map`，在 GOP(0x9c000000) 之前**不重叠**；老脚本 `nabu-panic-log.py.bak-l321:611` 硬编码同为 0x9b000000。**l143 老配方 0x9e000000 撞 GOP，绝不可复用**。
- **闭合导入通道**：`NativeUfs.cpp:3180 native_ufs_paniclog_import()` 读 `'FUNK'` 头→写 `NABUPANIC1` 到 NABULOG 区（门=`nabu-log=1`，已在 args）；l143/l145/l152/l153 日志出现过 `NABUPANIC1` ⇒ ADT 路径确能令 `gPanicBase!=0`（否则只打印 `paniclog:no-base`）。含 ESR 的完整文本尚未在真 panic 上实证——**本轮正是补这一实证**。

## 3. 为什么离线不行
panic 的 **backtrace / 寄存器 / 内核线程状态**（以及可能存在的 ESR/FAR）**只能由真机 SError 运行期产生**；静态只能确定异常类型（异步外部中止）与处理路径，无法确定触发时的调用栈与线程现场。写不出这个量就不该上板——本轮缺的正是它。

## 4. 失败也有信息
- 仍报 `gPanicBase is still not initialized` ⇒ ADT 注入未落到 SecureDT（构建链通路问题），得到"board.toml adt 表 → SecureDT"的新事实；
- 不再报但 `NABUPANIC1` 仍空/无 `'FUNK'` ⇒ panic 文本未落盘（重启时序/清 magic 时机），得到新事实；
- 拿到完整转储 ⇒ 直接读出 FAR/ESR/栈，定位触发 SError 的物理源，进入对症修复；
- 本轮活过 400s 未死 ⇒ 得到"SError 非每轮必现"的新事实，回到存活差异面重排。

## 5. 唯一变量与回滚点
- **变量（最终包仅 1 处：`kernel.macho.adt`）**：ADT 新增 `chosen/embedded-panic-log-size=0x80000` + 根级 `pram.reg=0x9b000000/0x80000`（16B）。
- **生成过程（离线发现并补齐，均不进包）**：
  1. `l738-device/board.toml` 加上述两属性（备份 `board.toml.pre-l749`）；
  2. `scripts/unixnu/devicetree.py:377` 白名单原为 `('defaults','options','product','buttons','nabu-touch')`，**不含 `pram` ⇒ 属性会被静默丢弃**；加 `'pram'`（备份 `devicetree.py.pre-l749`）。此改动只影响 ADT 生成。
  3. 完整重建产生了 `__LINKEDIT` 构建复现性杂音（kernel d6e2f33b→dd3d6662，914 个 2B 簇），**已按红线弃用该内核**。最终包改为：以 `hold/l748/payload.img` 为基，`mcopy` 一次性替换内部 `kernel.macho.adt` + `kernel.macho`（保证 FAT 簇链自洽），所有字节级差异仅 ADT。
  4. **离线先验通过**（E:\nabu\tmp\l749-offline-verify.py / l749-payload-verify.py）：kernel.macho d6e2f33b 与 l748 SAME、boot/addon/trust SAME、payload 内 5 文件除 ADT 外全 SAME、args 同、`VERDICT: ADT-ONLY ROUND CONFIRMED`。
- **其余与 hold/l748 逐字节同**：kernel.macho（d6e2f33b，204B/18 段 l730 ledger 不动）、boot-merge-r3.img、gpu-addon.bin、owner-trust.bundle、scratch-data.img、kernel args 39 token 全同（含 `nabu-log=1 nabu-panic-restart=2 sheng-smp=1 nabu-wdt-arm-delay=0 pet=600`）、payload 生成参数全同（owner-trust-cache / gpu-addon / unique-chip-id 87f0ac79 / data-ramdisk）。
- **唯一不同产物**：kernel.macho.adt（多 2 属性）。构建后**离线先验**：ADT 内确实含两属性 + kernel.macho 哈希与 hold/l748 相同，才允许上板。
- **回滚**：`hold/l748`（同内核默认 ADT）/`hold/l730`（最佳锁屏基线）；失败即回滚。仅 `fastboot boot`，不写任何分区。诊断字段（nval/don/tr/idg）仍挂账。

## 6. 恢复预案
- 只用 `fastboot boot <boot-merge-r3.img>`（临时加载，绝不 `flash boot*`）；**任何 `fastboot erase` 禁止**。
- boot 前查 `fastboot getvar slot-unbootable`；`yes` 先 `fastboot set_active a`（非破坏）。
- **WDT 时序（离线读源码修正，勿再沿用旧数）**：`NativeUfs.cpp:3263/3279` 的 `pet=600 arm-delay=0` 是"到 600s 才停止 pet"，之后 WDT 咬（arm+58s）⇒ 咬点 ≈ **658s**（不是历史误用的 380–410s）。l747(281s)/l748(328s) 的早死是 SError/PSCI reset，非 WDT。本轮若未发生 SError，设备会活到 ~658s 才回 TWRP；采集/等待窗口按 900s 设计。SError panic 经 `nabu-panic-restart=2` 在 panic 主流程结束后 PSCI reset 回 TWRP（boot_a 即 TWRP）。恢复只用用户点名 `backup-boot_a-20261103.img`。
- **需第 2 次 `fastboot boot` 同包**导入 `NABUPANIC1`（`fastboot boot` 一次性，重启不自启 UniXNU）——同一临时启动、非破坏性；随后从 NABULOG1 窗口（userdata 尾 32MB，skip=121718715）读取。
- USB 断供由 CDC / NABULOG1 环 / NABUDIAG 三通道兜底；**无需人工断电**。
