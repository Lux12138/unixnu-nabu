# 参考与来源

本项目不是一个从零开始的移植。它站在相当多的上游工作上，其中一部分是直接依赖（代码原样使用），
一部分是参考（读了设计、对照过实现，或者踩过同样的坑）。

**本文的目的**：把「哪些是我们的、哪些是别人的」说清楚，不留含糊。

---

## 一、直接依赖（代码原样使用，按许可证分发）

### 内核与用户态基础

| 项目 | 许可 | 我们用了什么 |
|---|---|---|
| [Apple XNU](https://github.com/apple-oss-distributions/xnu) `10063.141.1` | Apple Public Source License 2.0 | 内核源码基线。我们只加补丁（`patches/xnu/`，23 个），不改主干结构 |
| [Apple **libplatform** / **libdispatch** / **libpthread** / **corecrypto** / **AvailabilityVersions** / **IOStorageFamily**](https://github.com/apple-oss-distributions) | 各自 APSL / Apache-2.0 | 内核链接所需的基础库，按 `feeds.toml` 的 pin 拉取 |
| iPadOS 17.7.11 (21H461) IPSW | Apple 专有 | 用户态（`/System`、dyld shared cache）、`kernelcache.decompressed`。**不入库**，用户自备 |

### UniXNU 框架与板级组件

全部来自 [UniXNU 组织](https://github.com/UniXNU)，按 `components.toml` / `feeds.toml` 的 revision pin 拉取：

| 组件 | 上游仓库 | 我们做了什么 |
|---|---|---|
| **ArmCpu** | `UniXNU/ArmCpu` | 加 8 核 SMP bring-up（`smp` 参数、PSCI CPU_ON、GIC 重分发器、per-core monitor） |
| **ArmDTPlatform** | `UniXNU/ArmDTPlatform` | 用于解析 ADT |
| **DarwinBoot** | `UniXNU/DarwinBoot` | XNU 加载器（Mach-O 解析、ADT 校验、ExitBootServices、跳转）|
| **ShengUfs**（我们叫 `NativeUfs`） | `UniXNU/ShengUfs` | 加诊断导出区、`NABULOG1`、DPU 探针、WDT pet 路径、`writable[]` 属主表 |
| **GopFramebuffer** | `UniXNU/GopFramebuffer` | 加 `startDirect()`/`flip()` 做 DPU 直出；`NabuSmmuMapper` 是新增 |
| **Adreno740** | `UniXNU/Adreno740` | 加两遍扫描的 `importTask`、WIMG 位掩码匹配 |
| **AdrenoMetal** | `UniXNU/AdrenoMetal` | Metal 前端经 metal2vulkan + Turnip 服务用户态 |
| **ShengNovatekTouch** | `UniXNU/ShengNovatekTouch` | NT36523 触摸协议 |
| **QcomGeniSpi** | `UniXNU/QcomGeniSpi` | GENI SPI 控制器 |
| **AppleKeyStoreState** | `UniXNU/AppleKeyStoreState` | KeyStore 事件替身 |
| **XnuPrelink** | `UniXNU/XnuPrelink` | prelink 工具 |
| **VerifiedRoot** / **RamBlock** / **VirtioMmioBlock** / **SoftwareAES** | `UniXNU/*` | 按原样使用 |

### UEFI 固件

| 项目 | 许可 | 说明 |
|---|---|---|
| [Project Silicium / Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) | BSD-2-Clause | `uefi-nabu` 的基座。`Platforms/Xiaomi/nabuPkg` 与 `Resources/Configs/nabu.toml` 来自这里 |
| [Microsoft Project Mu](https://github.com/microsoft/mu) | BSD-2-Clause-Patent | `mu_basecore` / `mu_plus` / `mu_oem_sample`（`release/202608`）|
| [Project Silicium Device-Binaries](https://github.com/Project-Silicium/Device-Binaries) | 厂商二进制 | nabu 的 QcomPkg 驱动 |
| [Mu-Silicium 的 Silicium-ACPI](https://github.com/Project-Silicium/Silicium-ACPI) | BSD-2-Clause | ACPI 表 |
| [OpenSSL](https://github.com/openssl/openssl) | Apache-2.0 | EDK II 依赖，pin `67b5686b` |
| **UniXNU/uefi-sheng** | BSD-2-Clause-Patent | `uefi-nabu` 的 fork 来源 |

### Mesa / Turnip（GPU 用户态）

| 项目 | 许可 | 说明 |
|---|---|---|
| [Mesa](https://gitlab.freedesktop.org/mesa/mesa) + [Turnip](https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/freedesktop/turnip) | MIT | Adreno 640 的 Vulkan 后端。我们打 20 条补丁（`patches/mesa/`），关键两条：`turnip-host-import-images.patch`（HostMemory 导入）、`turnip-darwin-cached-coherent-memory.patch`（`memoryTypeBits = 1u`） |
| **metal2vulkan** | MIT | Metal → Vulkan 翻译层，commit `624bab39`（与 alioth 同版） |

### 构建工具链

| 项目 | 许可 | 说明 |
|---|---|---|
| [LLVM / Clang](https://github.com/llvm/llvm-project) | Apache-2.0 with LLVM exceptions | 打 2 条补丁：`llvm-xnu-builtin-type.patch`（实现 `__builtin_xnu_type_*`）、`llvm-clang-arm64-crypto-asm.patch` |
| [cctools-port](https://github.com/tpoechtrager/cctools-port) | APSL / MIT | `ld64` / `libtool` / `nm` / `lipo`。**必须 1030.6.3-ld64-956.6**，上游 master 的 `ld64-609` 会损坏内核链接 |
| [bootstrap_cmds](https://github.com/apple-oss-distributions/bootstrap_cmds) | APSL | `mig` |
| [iig-port](https://github.com/Nyktis/iig-port) | 见上游 | DriverKit `iig` 的 Linux 实现，我们加 `iig-port-darwin-target.patch` |
| [lzfse](https://github.com/lzfse/lzfse) | BSD-3-Clause | 解 kernelcache |
| [xnu-linux-toolchain](https://github.com/Allenwdk/xnu-linux-toolchain) | 见上游 | 早期工具链方案，我们 fork 后加了 nabu 专用的 awk-parens 补丁 |

---

## 二、参考（读过、对照过，但代码不是复制来的）

这一节比上一节更需要说清楚，因为「参考」的边界最容易含糊。

### 同 SoC 的其他 nabu 移植（[unixnu.com/devices/models/nabu](https://unixnu.com/devices/models/nabu) 登记的 8 个）

| 项目 | 我们参考了什么 | 我们没做什么 |
|---|---|---|
| [**zzweiqing/UniXNU-nabu**](https://github.com/zzweiqing/UniXNU-nabu) | ① `porting.toml` 的「能力域 + 证据 + limits」记录格式，我们的 `CURRENT-STATE.md` 分层结构受了它启发<br>② `NABULOG2` 的**完整性设计思路**（sequence / boot id / build id / CRC / loss counter）——我们目前 CDC 还没做，是明确短板<br>③ `adrenometal-nabu-completion-wait.patch` 发现的 `dispatch_group_wait(_handlers, FOREVER)` 死锁模式——**这条我们尚未核对自己实现是否有同样问题** | 没有复制任何代码。它的构建链是 macOS + Xcode + rEFInd，我们走 fastboot + cust |
| [**mytiantian001/UniXNU-nabu**](https://github.com/mytiantian001/UniXNU-nabu) | ① 「八色条」阶段的 GOP 内存 provider 验证方法<br>② `docs/prior-art-mytiantian001-ipados-nabu.md` 是我们**为它写的**对比文档（在我们仓库的 `docs/`）<br>③ 它发现的 `disableConsoleOutput` 导致图形阶段 USB 断流——我们核过自己有类似现象 | 没有复制代码 |
| [**hanqioopp-lab/unixnu-nabu**](https://github.com/hanqioopp-lab/unixnu-nabu) 与 [**XingJiaoStudio/unixnu-nabu-sweet**](https://github.com/XingJiaoStudio/unixnu-nabu-sweet) | ① `sweetusb=2` 的「面板模式」思路（USB 线程兼当屏幕，每 20 秒重绘过滤后的摘要块）<br>② `sweet-panel-report-state.patch` 里两个 bug 的教训：「诊断不能在看起来正常时停止」 | 都没复制。它们是移植 raphael 的 SWEET 方案，**且在 nabu 上没有实测证据** |
| [**3214791900-del/unixnu-nabu-build**](https://github.com/3214791900-del/unixnu-nabu-build) | 「六个硬坑」（USB 线导致 Load Error、PEI 解压预算、ADT 的 DRAM 必须实测等）——我们早期踩过同样的坑，它的记录帮我们确认了现象 | 它停在 PMGR 阶段，没到用户态 |
| [**lee-kenneth/unixnu-nabu**](https://github.com/lee-kenneth/unixnu-nabu) | **它是我们这个仓库的 fork**（parent = `Lux12138/unixnu-nabu`），不是我们参考它 | — |
| `zzc775/unixnu-nabu` | 仓库已删除/私有，无法参考 | — |
| [**glebenekt/ios-nabu**](https://github.com/glebenekt/ios-nabu) | `PLAN.md` 记录过：它用 **xnu-8796 + 83 个 patch** 的阶段划分给了我们**反面参考**——我们最终选 xnu-10063，那些补丁全部作废（现在在 `docs/history/xnu-8796-route-abandoned/`）| 没用它的代码 |
| [**oosexgang/hackios**](https://github.com/oosexgang/hackios) | 同上，`PLAN.md` 记录过它「单文件 119 KB，锁屏/桌面/设置/Safari 可用」，是我们早期的目标参照 | 没用它的代码 |

### 其他 SoC 的 UniXNU 移植

| 项目 | 参考了什么 |
|---|---|
| [**JohnLeeeeeee678/UniXNU-raphael**](https://github.com/JohnLeeeeeee678/UniXNU-raphael)（红米 K20 Pro，同 SM8150） | 「同 SoC 可复用」的证据。l738 的 diff-copy、DPU 直出思路受它启发 |
| [**wochgn/UniXNU-guacamole**](https://github.com/wochgn/UniXNU-guacamole)（一加 7 Pro，同 SM8150） | SM8150 原始突破。`porting.toml` 里多处记录它的成果 |
| [**Project Aloha / mu_aloha_platforms**](https://github.com/Project-Aloha/mu_aloha_platforms) | SM8150 UEFI 的参考实现（官方有 `xiaomi-nabu` 目录）|
| [**WOA-Project / mu_andromeda_platforms**](https://github.com/WOA-Project/mu_andromeda_platforms) | 早期 UEFI 基座参考 |
| **alioth** 系（小米 11 / SM8250 同栈） | ① 它的 Metal 插件**能画**（`GPUFB nonblack 992/1000`），是我们的对照基准<br>② turnip 补丁集 9 条 vs 我们 20 条，多出的 11 条里 `turnip-darwin-cached-coherent-memory.patch` 是它没有的 |
| [**1PJL/unixnu-nabu**](https://github.com/1PJL/unixnu-nabu) | UFS 卷诊断三步法（`Recover GPT` / `Build Volume` / `Repack`）|

### 工具与方法的参考

| 项目 | 参考了什么 |
|---|---|
| [**macOS `unifiedlog_iterator`**](https://github.com/mandiant/macos-UnifiedLogs)（macul）| os_log 解码。没有它，本轮 19 天里所有用户态判读都做不了 |
| **dnl / IPSW 解包工具** | 从 IPSW 提取 `kernelcache` / `DeviceTree` / `dyld_shared_cache` |

---

## 三、我们的原创部分

不是为了自夸，而是让边界清楚：

| 类别 | 内容 |
|---|---|
| **内核补丁** | `l730` 的迁移完成态修复（67 B）、`l769` 的 `SBDontLockAfterCrash` 数据补丁（44 B）、`l770` 的 `client_` 竞争 4 B NOP、`l771` 的 GOP carveout WIMG 4 B |
| **驱动新增** | `NabuSmmuMapper`、`NabuWdtDxe`（UEFI 侧 APSS 看门狗 + ESP 日志）、`NabuGpuScanout` handoff 状态机 |
| **诊断体系** | `NABUDIAG` 双槽导出（220 s / 300 s，含 tracev3 + uuidtext + `screen.bgrx`）、`NABULOG1` 尾部写、三通道覆盖矩阵的量化 |
| **工程方法** | 同路径 bind mount 构建（避免 `__cstring` 重排）、204 字节 / 18 段补丁账本、六问板报格式、45 轮 A/B 的判读纪律 |
| **事故记录** | `erase misc`/`vbmeta` 险变砖、`sheng-mig` 0 是成功码、`ncpy=0` ≠ 零拷贝、48.6 s CDC 黑洞 |

---

## 四、明确说明

1. **这不是「我们独立做出来的」**。内核是 Apple 的，框架是 UniXNU 的，固件基座是 Project Silicium / Microsoft Mu 的，GPU 后端是 Mesa/Turnip 的。
2. **我们做的是「在这些之上，让 nabu 这块具体板子跑起来」**：设备适配、数据补丁、驱动修补、诊断体系、以及大量失败记录。
3. **没有复制其他 nabu 移植的代码**。参考的都是**设计思路和踩坑经验**——这类东西在开源里本来就该共享。
4. **所有依赖都按各自许可证分发**。iOS/iPadOS 的 IPSW 与厂商固件属输入，不入库；`proprietary-files.txt`
   列出构建时需要的厂商文件，它们从不进仓库。
5. 如果本文有遗漏或写错，请在 issue 中指出，我们会更正——**说清来源比声称原创重要**。

---

## 五、分目录许可证

本仓库代码整体见 [`LICENSE`](LICENSE)（BSD-2-Clause-Patent）。`drivers/` 下的文件保留各自原始许可，
按 SPDX 标识统计：

| SPDX | 文件数 | 主要位置 |
|---|---:|---|
| `MIT` | 92 | `drivers/GopFramebuffer`、`drivers/NabuAdreno` 大部分 |
| `BSD-2-Clause-Patent` | 37 | `drivers/ArmCpu`、`drivers/AppleKeyStoreState`、`drivers/NativeUfs` |
| `GPL-2.0-only` | 7 | `drivers/NabuAdreno` 的 GMU / ZAP 相关（源自上游 Mesa/freedreno）|
| `Apache-2.0` | 4 | `drivers/ShengNovatekTouch` |

另有 9 个文件没有 SPDX 头：

```text
drivers/NabuAdreno/NabuGpuBuild.h
drivers/NabuAdreno/NabuLogCore.h
drivers/NabuAdreno/NabuLogFormat.h
drivers/NabuAdreno/nabu_adreno_fw.cpp
drivers/NabuAdreno/nabu_adreno_step1_compat.cpp
kits/nabu/NabuPlatformPower.cpp
kits/nabu/nabu_early_mark.c
kits/nabu/nabu_early_mark.h
tools/linux/TargetConditionals.h
```

这些是本项目新增或从同许可的上游文件改写而来，随本仓库的 BSD-2-Clause-Patent 分发。
`tools/linux/TargetConditionals.h` 是从 Apple Command Line Tools 的公开头文件复制的兼容层。

---

## 六、如果我们是错的

如果你发现本仓库里有**未标注来源**的代码，或某处声明与实际不符：

- 开 issue 指出具体文件与原始出处
- 或直接提 PR 补充本文

我们宁可把来源写得过于详细，也不想让任何人觉得这些东西是从别人那里悄悄拿来的。
