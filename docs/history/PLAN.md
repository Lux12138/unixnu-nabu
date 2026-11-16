# nabu XNU 移植：参考项目的路径与我们的可行性方案

> ## ⚠️ 历史文档（已作废）
>
> 本文写于 2026-10-2x，属于**已被取代的旧路线**。其中的「当前状态」「下一步」
> 「卡在哪里」全部失效，不要按它行事。
>
> **当前状态一律以 [`CURRENT-STATE.md`](../../CURRENT-STATE.md) 为准。**
> 项目已达成原生桌面（l769），剩余工作是流畅度与外设驱动。
>
> 保留本文仅作追溯：为什么当初选了某条路、踩过哪些坑。

---

生成时间：2026-10-22

---

## 一、参考项目的真实规模

| 项目 | 内核版本 | 补丁规模 | 到达状态 |
|---|---|---|---|
| **glebenekt/ios-nabu** | xnu-8796 | **83 个 patch / 1.58 MB** | XNU 上真机、UFS 根可读写、**launchd / SpringBoard / backboardd 启动**、显示服务注册 |
| **oosexgang/hackios** | xnu-8796 | **单文件 119 KB** | 锁屏/桌面/设置/Safari 可用（Adreno GPU）|
| **IssacL04/Pagani** | BCM2837 配置复用 | 若干 | 越过分配器断言，到 BSD setconf |
| **我们（Lux12138）** | **xnu-10063** | 约 20 个自研 patch | **内核到 bsd_autoconf 之后，卡 IOSurface** |

**关键差异**：两个成熟项目都用 **xnu-8796**，我们坚持 **10063**。
补丁**不能直接移植**，但**阶段划分和方法论可以照搬**。

---

## 二、ios-nabu 的 83 个补丁，实际是一条五阶段路径

### 阶段 1｜让内核能启动（0001–0020，20 个）

| 补丁 | 作用 |
|---|---|
| 0001/0002/0005 | 新 clang 的编译修复 |
| **0003/0010/0012** | **framebuffer serial console、早期 fb 调试、ttbr0 之后保留 console** |
| 0004/0006–0009 | machine config、开源 asm pmap、stub Apple 内部头文件、补 closed lib 空洞 |
| 0011 | physmap 对齐补齐 |
| **0013/0014** | **RNG 不依赖 corecrypto**（`vm_kern.c`，97 行）|
| **0015** | **trust cache 运行时不可用** |
| **0016/0017/0018** | **device tree platform expert、虚拟 timer IRQ、boot CPU 驱动** |
| **0019** | **builtin pthread**（不是 prelink！）|
| 0020 | portable AES-CBC |

**我们已做到的**：0003/0010/0012 的等价物（fb + UART + kprintf，今晚打通）。
**我们还缺的**：0013/0014（RNG）、0015（trust cache）、0016–0018（DT platform expert）、**0019（pthread 内置，我们错用了 prelink）**。

### 阶段 2｜根文件系统（0021–0037，17 个）

| 补丁 | 作用 |
|---|---|
| **0021** | **mockfs + RAM disk 作根**（54 行）|
| 0022/0024 | 无 IOMedia 的 ramdisk 根、mockfs 虚拟 ramdisk IO |
| 0023/0026/0027/0032/0033 | 内置 sha256 / HFS / BearSSL（md5/sha1/sha384/sha512）|
| **0025** | **自建 AMFI，不做 entitlements 检查**（`bsd/kern/nabu_amfi.c`，192 行）|
| **0028/0030/0031/0034** | UFS 根设备、写支持、读写根、DMA above 4G |
| **0029** | **unsealed root**（24 行，把 panic 改成 printf）|
| 0035/0036 | AMFI entitlements、platform binaries 签名 |
| 0037 | bindfs |

**关键**：他们**先用 mockfs + RAM disk 起步**（0021），**UFS 是后来才加的**（0028）。
我们关掉 NativeUfs 后正好需要 0021 这条路。

### 阶段 3｜运行时稳定（0038–0045，8 个）

AppleKeyStore、coredump 关闭、**iowatchdog（0040/0045）**、HFS link notes、
exec 短日志、**console crash log（0043/0044）**。

**iowatchdog 很关键**：没有它，系统起不来会被看门狗复位，看起来像"随机重启"。

### 阶段 4｜显示（0046–0049，4 个）

| 补丁 | 作用 |
|---|---|
| **0046** | **自研 IOSurface + IOMobileFramebuffer**（`NabuSurface.cpp` / `NabuFramebuffer.cpp`）|
| **0047** | **持久 console 日志**（4 MiB 环形，写在 userdata 末尾，TWRP 可读）|
| 0048/0049 | devnull tee、IOKit trace |

**0046 就是我们卡住的那一段**，他们的答案是**不用 Apple 闭源 kext，自己写**。

### 阶段 5｜用户态外设（0050–0083，34 个）

sandbox 策略、crash 采样、HFS rename、process sampler、AKS、fb 节流、
**HID event services（0058）**、iPhone identity（0059）、
**触摸/按键驱动（0060/0062/0063/0071/0083）**、iPhone X 显示尺寸（0061）、
**SMP PSCI（0065/0078）**、console mute、surface 查找缓存、UFS 加速、
**USB（0072–0077/0081）**、display power、net trace、UFS AES。

---

## 三、我们的实际差距（已按实际 patch 盘点修正）

初版把差距估得过重。清点 `patches/xnu/` 后，情况是：

| 能力 | 我们已有的 patch | ios-nabu 对应 | 状态 |
|---|---|---|---|
| mockfs 编译修复 | `mockfs-typed-allocation` / `mockfs-statfs` / `mockfs-intermediate-lookup` / `mockfs-memory-device-width` / `mockfs-test-file` | 0021/0024 | ✅ **已做** |
| 无 block storage 容忍 | `null-root-media` | 0021 | ✅ **已做** |
| trust cache 运行时 | `native-trust-cache-runtime` | 0015 | ✅ **已做** |
| **pthread** | `source-pthread-bootstrap`（新增 `bsd/kern/source_pthread_bootstrap.c`）| 0019 | ✅ **已有方案** |
| SMP | `native-smp-secondary` | 0065/0078 | ✅ 已有 |
| panic 区 | `native-dt-panic-region` | 0066 | ✅ 已有 |
| work interval | `native-priv-work-interval` / `native-work-interval-soft-fail` | 0064 | ✅ 已有 |
| 平台专家 / DT | `armvirt-platform-personality` / `source-machine-layer` / `sheng-platform-power` | 0016–0018 | 🟡 部分 |
| **mockfs 编入内核** | `zzzz-nabu-mockfs-root`（本轮新增）| 0021 | 🟡 **本轮验证中** |
| **RNG 不依赖 corecrypto** | — | 0013/0014 | ❌ 缺 |
| **iowatchdog** | — | 0040/0045 | ❌ 缺 |
| **AMFI 无 entitlements** | — | 0025 | ❌ 缺（或已由 `public-entitlements-header` 部分覆盖）|
| **自研 surface / framebuffer** | — | 0046 | ❌ 缺，**仍是最大差距** |
| 触摸 / USB / 显示电源 | — | 0058–0083 | ❌ 缺，远期 |

**结论：阶段 1 和阶段 2 的绝大部分我们已经有了**，真正的空白集中在
**RNG（0013）、iowatchdog（0040）、AMFI（0025）和 surface/framebuffer（0046）**。

---

## 四、可行性方案

### 方案 A｜继续 10063，按 ios-nabu 的阶段走（推荐给"要自主"的目标）

**前提**：接受 10063 是 iOS 17 内核，用户态 ABI 风险自担。

**第 1 步：补齐阶段 1 剩下的（1–2 天）**

| 动作 | 对应参考 | 说明 |
|---|---|---|
| **把 pthread 从 prelink 改回内置** | 0019 | 我们目前把 `com.apple.kec.pthread` 加进 prelink roots，方式不对 |
| RNG 不依赖 corecrypto | 0013 | 改 `osfmk/vm/vm_kern.c` |
| trust cache 运行时容忍 | 0015 | 没有 Image4 时不要 panic |
| DT platform expert | 0016–0018 | 这是让 `arm-io` 子节点能匹配驱动的前提 |

**第 2 步：拿根文件系统（2–3 天）**

| 动作 | 对应参考 | 我们的具体改法 |
|---|---|---|
| **`FILESYS_BASE` 加 `mockfs`** | 0021 | 我们的 `config/MASTER.arm64.bcm2837` 当前是 `[devfs fifo fs_compression config_protect config_mnt_rootsnap config_fse routefs namedstreams bindfs]`，**没有 mockfs** |
| **容忍无 `IOBlockStorageDriver`** | 0021 | `iokit/Kernel/IOService.cpp:5261` 的 `assert(ioblockstoragedriverClass)` 要去掉（我们关了 NativeUfs，这个类不存在）|
| **unsealed root** | 0029 | ✅ **已完成**（`zzzz-nabu-unsealed-root.patch`）|
| RAM disk 作根 | 0021/0022 | `rd=md0` 之类，或用 mockfs 直接当根 |

**第 3 步：显示（1–4 周，这是真正的坎）**

| 选项 | 工作量 | 结果 |
|---|---|---|
| **自研 `IOCoreSurfaceRoot` + `AppleCLCD`** | 2–4 周 | 能画图，为 SpringBoard 铺路 |
| 放弃图形 | 0 | 内核能到 launchd，但看不见画面 |

**第 4 步：运行时 + 外设**（阶段 3/5，数周）

### 方案 B｜切到 8796，基于 ios-nabu（推荐给"要看到画面"的目标）

| 项 | 说明 |
|---|---|
| 收益 | **83 个补丁直接可用**、同设备、同 iPadOS 16.6.1、已到 SpringBoard |
| 成本 | 重搭构建管线（但我们有 `build-kernel-8796.yml` 和 87 个 patch 的底子）|
| 风险 | 需要理解并维护别人的 1.58 MB 补丁 |
| 时间 | **1 周内能看到到他们的位置**，而不是几个月 |

### 方案 C｜双线并行（最稳）

- **主线 10063**：继续验证我们自己的内核（QEMU 已到边界，真机到 IOSurface）
- **副线 8796**：用 ios-nabu 的补丁快速拿到一个**能看见画面**的系统
- 两条线共享：QEMU 环境、CI 管线、探针工具

---

## 五、我的建议

**先做方案 A 的第 1、2 步（3–5 天），因为这两步无论走哪条路都要做，而且成本可控：**

1. **pthread 改内置**（现在是错的）
2. **`FILESYS_BASE` 加 mockfs**
3. **去掉 `assert(ioblockstoragedriverClass)`**
4. **RAM disk 当根**

做完这四件事，我们有很大概率把内核推进到 **`load_init_program`**（找 `/sbin/launchd`）。
到那一步再决策 **方案 A 第 3 步（自研 surface，几周）vs 方案 B（切 8796，一周）**。

**理由**：前四件事是**两条路的公共部分**，做了不浪费；而 IOSurface 那一段是**分岔点**，
应该等拿到 `load_init_program` 的实际结果之后再决定。

---

## 六、立即可执行清单（按性价比排序）

| # | 动作 | 参考 | 预计 |
|---|---|---|---|
| 1 | `config/MASTER.arm64.bcm2837` 的 `FILESYS_BASE` 加 `mockfs` | 0021 | 10 分钟 |
| 2 | `IOService.cpp` 的 block storage assert 改成容忍 NULL | 0021 | 10 分钟 |
| 3 | pthread 从 prelink roots 移除，改用内置实现 | 0019 | 半天 |
| 4 | `vm_kern.c` 的 RNG 不依赖 corecrypto | 0013 | 半天 |
| 5 | trust cache 运行时不可用不 panic | 0015 | 2 小时 |
| 6 | RAM disk 作根，验证能到 `load_init_program` | 0021/0022 | 1 天 |
| 7 | 决策：自研 surface 还是切 8796 | — | — |

---

## 七、诚实的判断

- **1–6 做完，内核大概率能到 launchd**，但**看不见画面**
- **看见画面**需要 IOSurface 段，那一段**没法绕**
- **ios-nabu 用 83 个补丁、几个月**做到，我们自己做同样的活**不会更快**
- 唯一能显著加速的是**方案 B（切 8796 + 借补丁）**

**所以最终建议**：先做 1–6（公共部分，3–5 天），拿到 `load_init_program` 的实测结果，
再决定 IOSurface 段是自己写还是切版本。
