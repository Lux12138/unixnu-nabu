# UniXNU on Xiaomi Pad 5 (nabu / SM8150)

在 **小米平板 5（nabu，骁龙 860 / SM8150）** 上通过 UEFI 启动 **苹果 XNU 内核**，
运行 **iPadOS 17.7.11 (21H461)** 用户态的完整移植记录。

---

## 现状：已达到原生桌面（2026-11-16）

| 里程碑 | 状态 |
|---|---|
| 内核启动 → 用户态 | 完成 |
| 系统 UI（锁屏） | 完成 — l730 |
| **原生 Home Screen** | **完成 — l769** |
| 触摸输入 | 完成 — 实测 `fingers 4→436`，client = `backboardd` |
| GPU scanout 接管 | 完成 — l771，`P4/PF` 生效 |
| **流畅度** | **未完成** — `present` 均值 221 ms，尾帧 844 ms |
| **外设驱动** | **多数未实现** — 电源/音频/Wi-Fi/按键/摄像头/指纹/SEP 均缺 |

**桌面截图（l769 实机，800×1280）**

![Home Screen](docs/screenshots/desktop-l769.png)

**锁屏截图（l730 实机，带系统壁纸）**

![Lockscreen](docs/screenshots/lockscreen-l730.png)

> 开机与桌面只是**第一个里程碑**，不是终点。当前显示路径是
> 「原生 GPU 合成 + 每帧 CPU 16 MiB blit 到 GOP」，理论上限约 3.5–7 fps。
>
> 除此之外还有大量驱动未实现：**电源管理（PMIC/RPMh/DVFS）、音频（ADSP/codec）、
> Wi-Fi、物理按键、摄像头、蓝牙/GPS、指纹、SEP**，以及持久化 Data 卷与
> 背光控制。完整清单与排序见下面的[路线图](#路线图已完成什么还缺什么)。

---

## 关键突破与路线（为什么能到桌面）

### 1. l730：交还系统 UI — 最后一道开机墙不是 GPU，是内核自己写错了 plist

`sheng-mig=1` 把 264 B 强制迁移态写进了 `com.apple.migration` 的 CFPreferences 域，
覆盖了 365 B 官方完成态，导致 `lastBuildVersionPref=(null)`，每轮强制重跑数据迁移，
SpringBoard 永远不 `finishedStartup`。

只改 **67 个文件字节**：保留官方完成态，把强制态写去无害路径 `com.apple.migratiox.plist`。
结果：`Migration not necessary`、`finishedStartup:YES`、`Dismissing overlay`，锁屏出现。

### 2. l738 / l750：DPU 直出——可行性已证，实现未落地

`l737` 的 `Kernel data abort (esr 0x96000010)` 是**探针地址写错**
（`0xae004000`，比真 VIG0 `0xae05000` 低 0x1000），不是 DPU 不可用；
两个数据门也确实存在：

```text
native_ufs_dpu_state = 0x80000000   (bit31)
mode = 2
```

**但直出路径当前并不工作**：`native_ufs_dpu_map()` 仍是返回 0 的桩、
`native_ufs_dpu_state=0`，`NativeLegacyDisplay::startDirect()` 的入口条件
（`mode>=2 && &map && &state && (state>>31)`）永不成立，`directOn` 恒为
false，`directSlot()` / `flip()` 是死代码。**当前显示路径仍是
「原生 GPU 合成 + 每帧 CPU 16 MiB blit 到 GOP」**，见[路线图](#路线图已完成什么还缺什么)。

> 注意：`ncpy=0` **不等于**零拷贝，该判读已撤回。

### 3. l769：桌面 — SpringBoard 硬编码要上锁

SpringBoard 开机固定走 `SBStartupTransitionToLockOut`，并硬编码
`Boot{ForceLockKey:1}`。`SBDontLockEver` 等所有旁路均已否证。

l769 把 `com.apple.springboard.plist` 里一个键做等长替换：

```text
MCForceShowCloudConfigurationUI=false  →  SBDontLockAfterCrash=true
```

`SBStartupTransitionFactory` 于是返回 `destination=1 / SBStartupTransitionToHome`，
后果：

```text
ToHome · UI Locked:NO · 无 ForceLockKey · 主屏图标 + 壁纸 + Dock
```

整个包相对 l750 只差 **44 字节 / 1 簇**。

---

## GPU scanout 接管：两堵墙

| 墙 | 根因 | 修复 |
|---|---|---|
| ① `P3 ee00002c2` | 内核 `nabuScanoutClient` 抢占了 handoff 的独占 `WAIT_SWAP client_` 槽，不是 VM/cache/WIMG 问题 | `0xa7c6d0` 处 **4 字节 NOP** |
| ② `HostMemory/NotRepresentable` | GOP carveout 的 WIMG 被识别为 `Unknown` | 改为 `Cached`，**4 字节** |

两堵墙破后：`P4/PF` 首次出现，GPU 服务 6 帧，桌面不回归。
但 latency 仍有 `commit→complete 1.30 s`、`present 221 ms 均值 / 844 ms 尾帧`。

---

## 仓库结构

| 路径 | 内容 |
|---|---|
| `drivers/` | 7 个板级驱动：`NabuAdreno`（101 文件）、`NativeUfs`（33）、`GopFramebuffer`（12）、`ArmCpu`（3）、`ShengNovatekTouch`（4）、`QcomGeniSpi`（2）、`AppleKeyStoreState`（1） |
| `patches/xnu/` | 内核补丁序列至 `0017` + `sheng-platform-power` |
| `board-run/reports/` | **73 份六问板报**，覆盖到 l771 |
| `board-run/rounds/` | **135 份结构化轮次 JSON**（到 l738，之后改用板报形式） |
| `board-run/state/` | 主交接文档（3,641 行）+ 板侧作业铁律 |
| `configs/` | 两个 defconfig |
| `tools/` | 板侧构建与采集脚本，另含 `tools/linux/` 的 Darwin 交叉工具链安装器（`setup-toolchain.py` + 8 个 shim + 2 个补丁） |
| `board.toml` | 板级配置，**保留逐轮编辑历史**（`board.toml.history` 为历史快照） |
| `docs/screenshots/` | 实机桌面（l769）与锁屏（l730） |
| `CURRENT-STATE.md` | **权威状态摘要，与旧文档冲突时以它为准** |
| `DOCS-INDEX.md` | 文档索引（**只需读 README + CURRENT-STATE**） |
| `CREDITS.md` | **参考与来源**：直接依赖的每个上游项目、参考了谁的什么思路、我们原创的是什么 |
| `docs/history/` | 已作废的旧路线文档（2026-10-2x），仅作追溯 |

---

## 启动链的四个仓库（各自负责一段）

整条链不是单一仓库能覆盖的，四段各有归属：

```text
XBL → ABL → [uefi-nabu] → UniXNU 菜单 → DarwinBoot → XNU 内核 → [drivers/] → 用户态
              ↑                                          ↑
        固件（本仓库不含）                        内核与驱动（本仓库）
```

| 仓库 | 负责 | 内容 |
|---|---|---|
| **[`uefi-nabu`](https://github.com/Lux12138/uefi-nabu)** | **UEFI 固件** | fork 自 `UniXNU/uefi-sheng`，基于 Project Silicium + Project Mu。加入 UniXNU 四个模块、固件日志导出、APSS 看门狗 |
| **本仓库** | 内核与板级驱动 | `drivers/`（7 个）、`patches/xnu/`（23 个）、`configs/` |
| [`nabu-usb-cdc`](https://github.com/Lux12138/nabu-usb-cdc) | 实时日志通道 | DWC3 CDC 串口 |
| [`xnu-linux-toolchain`](https://github.com/Lux12138/xnu-linux-toolchain) | 构建工具链 | Linux 上编 XNU 的 clang/cctools-port/mig/iig |

### UEFI 固件的关键参数

| 项 | 值 |
|---|---|
| FD base / size | `0x9FC00000` / `0x300000`（3 MiB） |
| 帧缓冲 | 1600 × 2560 × 32bpp |
| System memory base | `0x80000000` |
| UEFI stack | `0x9FF90000`，每核 256 KiB |
| Advanced Logger | `0x9FF00000`，`0x80` 页 |
| SMBIOS | Xiaomi / Pad 5 / nabu / 21051182G / K82 |
| boot image | header v1，gzip 压缩 FD，`page_size=2048` |
| `USE_CUSTOM_DISPLAY_DRIVER` | **1**（厂商显示链 `CPRDxe` + `DisplayDxe` 会加载） |

### 产物形态

`fastboot boot` 吃的是 **Android boot image**（`ANDROID!` v1），不是裸 FD：

```text
boot-merge-r3.img  3,041,280 B  sha256 ead4b531…
  ├─ header   2,048 B
  └─ kernel   3,039,032 B   ← gzip(3,145,920 B 的 UEFI FD)
     ramdisk  0（payload 在 cust 分区，不在 boot image 里）
```

刷写流程：`fastboot flash cust payload.img` → **最后**重新 arm `misc` 的
`boot-recovery` → `fastboot boot boot-merge-r3.img`。

### 构建

```bash
./unixnu build firmware --uefi ../uefi-nabu
```

独立的 `python build_uefi.py -d nabu` **不能用**——它不注入 UniXNU 的包路径，
`UniXNUPkg` 与 `DarwinBootPkg` 的组件会解析失败。

### 一个值得记的教训

`NabuWdtDxe` 的入口点**故意不碰块设备**：它在 `PartitionDxe` / `FatPkg`
起来之前运行，2026-10-26 有一次在那里做无界 `ReadBlocks`，把板子卡死
且没有复位源。所有存储访问都放在 `ReadyToBoot`。

---

## 观测通道（三条，各有明确边界）

| 通道 | 覆盖 | 说明 |
|---|---|---|
| **CDC**（DWC3 串口） | 7–335 s，0.7–1.8 MB/轮 | 实时流；只有内核 printf；51→100 s 有 48.6 s 固定黑洞 |
| **NABUDIAG 双槽** | 220 s / 300 s 两个快照 | 唯一能拿到用户态 os_log 的通道（解码后 20 万行级） |
| **UFS NABULOG1** | 滚动窗口 | 仅 USB 功能死亡时使用（l749 就是这种情况） |

**335 s 之后三条通道全空** —— 该区间不能用「没看到」推断「没发生」。

细节见 [`nabu-usb-cdc`](https://github.com/Lux12138/nabu-usb-cdc) 与
[`unixnu-nabu-autolog`](https://github.com/Lux12138/unixnu-nabu-autolog)。

---

## 板级事实（实测）

> **本机是扩容版**：物理 DRAM **12 GiB**（TWRP 实测 `MemTotal = 11766512 kB`，
> **本机是扩容版**：物理 DRAM **12 GiB**（TWRP 实测 `MemTotal = 11766512 kB`，
> 即 12 GiB 减去约 816 MB 给 modem/hyp/rmtfs），userdata **约 468 GiB**
> （`fastboot partition-size:userdata = 0x74167BB000`）。
> 所以 `dram_size = 0x300000000` 是**为这台机器测出来的**，不是标准 6/8 GB 机型的数字。



---

## 附：早期里程碑（历史，已被上面取代）

**屏幕证据**

```
Process 1 exec of /sbin/launchd failed, errno 2
/sbin/launchd
kern_exec.c

FAR=18446744042837921792
PC=18446744005117767160
LR=18446744005111703156
```

**完整链路**

```
bsd_autoconf()                            OK
  kminit()                                OK
    IOsurfaceRoot::installMemoryRegions() OK   <- 曾被误认为卡在这里
bsd_init 后半段                           OK
  vfs_mountroot()                         OK
  load_init_program()                     OK   <- 解析出 /sbin/launchd
    execve("/sbin/launchd")               errno 2 (ENOENT)
```

**`errno 2` 是文件不存在，不是内核故障。**

### 曾经误判了三天

`IOsurfaceRoot::installMemoryRegions()` 那一行**从来不是终点，只是最后一条 kprintf**。
它之后的 `dtrace_postinit()`、`inittodr()`、条件编译的网络初始化**全都不打日志**，
于是「日志停止」被读成了「内核停止」。

真正缺的一直是一个根文件系统。

---

## 二、启动链（真机验证）

```
fastboot boot -> Mu UEFI -> UniXNU 菜单 -> cust 分区 payload
-> DarwinBoot -> 读 51MB 内核 -> 解析 Mach-O -> ADT 校验
-> 分配窗口 -> 映射段 -> ExitBootServices -> 跳转 XNU -> 内核执行
```

关键日志：

```
UNIXNU: runxnu entry vol=A2142798 path=\EFI\UniXNU\Payloads\kernel.macho cfgerr=[]
DB1 kernel read  Success size=53760000
DB2 kernel parse Success
DB3 adt read     Success size=13188
DB4 adt ok  win=120000000 keep=0 rd=ffffffffffffffff
DB arm64 va=0xfffffff007ac2000 pa=0x80070000 owned=0x120000000 window=0x30000000
video base=0x... width=2560 depth=32 bpp
DARWINBOOT: EL1 installed
DARWINBOOT: inputs validated; exiting boot services
still-there kernel entry code ran
DARWINBOOT: hand-off to the kernel
```

---

## 三、本地 Linux 构建（72 秒一轮）

内核现在能在本地 WSL Ubuntu 24.04 上编译，**72 秒**一轮，对照 GitHub macOS runner 的 15–17 分钟。

| 项 | GitHub macOS runner | 本地 WSL Ubuntu |
|---|---|---|
| 单轮构建 | 15–17 分钟 | **72 秒** |
| 产物 | 49.2 MB | **49 MB** |
| 可复现 | 只能靠 CI | 本机随时跑 |

**产物验证**

```
file            Mach-O 64-bit arm64 executable, flags:<NOUNDEFS|PIE>
__TEXT vmaddr   0xfffffff007004000      与原版 kernelcache 一致
symbols         22708
nabu 探针       nabu_mark / nabu_mark_map / nabu_mark_num / nabu_mark_putc
UFS 探测        nabu_ufs_register_probe.cpp
sha256          c756ecdb88a9dd50156dc27d2cdf48a08ddf93fcf204040fa57804f814fc3c15
```

**工具链自备**：`tools/linux/setup-toolchain.py`（一次 `sudo` 运行）在 Linux 主机上装齐
内核步骤需要的全套 Darwin 交叉工具：

| 组件 | 说明 |
|---|---|
| `clang`（打补丁） | 带 `__builtin_xnu_type_*`（`llvm-xnu-builtin-type.patch`）。xnu 的 `kalloc_type` 签名没有它就没有 fallback |
| `cctools-port` 1030.6.3-ld64-956.6 | `ld64` / `libtool` / `nm` / `lipo`。**必须用这个版本**——上游 master 的 `ld64-609` 会损坏内核链接 |
| `mig` | 来自 `bootstrap_cmds` |
| `iig-port` | 作为 `iig`（`iig-port-darwin-target.patch`） |
| `lzfse` | 解内核缓存 |
| 8 个 shim | 还原 CLT/Xcode 的目录布局：`xcrun` / `codesign` / `plutil`（Python plistlib 后端）/ `install`（BSD 语义）/ `gm4`（GNU m4 的 Apple 名）/ `cc` / `clang++` / `TargetConditionals.h` |

依赖走 `apt` / `dnf` / `pacman` / `zypper`，或预配置环境。

原始参考：[`Allenwdk/xnu-linux-toolchain`](https://github.com/Allenwdk/xnu-linux-toolchain)

### 打通 Linux 时踩的 18 个坑

| 类别 | 数量 | 例子 |
|---|---|---|
| 工具链缺失 | 8 | `libdispatch` 无 Ubuntu 包；`hostcompat.h` / `libc.h` / `copyfile.h` / `setmode` 全需自写 |
| 路径硬编码 | 5 | `/usr/bin/xcrun`、`/usr/sbin/sysctl`、`compression_tool`、`dsymutil`、`lzfse` |
| BSD vs GNU | 3 | `sed -i ''`、`awk \(`、未知 `-Wno-*` |
| 逻辑冲突 | 2 | mockfs 双重链接（23 个重复符号）、`TargetConditionals.h` 重复 |

**最有价值的一个**：`makesyscalls.sh` 里 `printf("\t{ \(sy_call_t *\)...")` ——
**BSD awk 把 `\(` 当转义，gawk 保留反斜杠**，Linux 上生成的 `init_sysent.c` 全是
`\(sy_call_t *\)`，编译器报一片 `expected expression`。做成 patch（`zzzz-nabu-awk-parens.patch`），
因为 `unixnu tree` 每次都重建 `.cache`，手工改会被覆盖。

---

## 四、根文件系统：7-Zip 直接解，不用 hdiutil

`build userdata` 在 Linux 上卡在 `hdiutil`。不用自己写 UDIF 解析器 ——
**7-Zip 23.01 原生支持 DMG + LZFSE**，直接解出完整 iOS 文件系统：

```bash
sudo apt-get install -y p7zip-full
7z x -y -o/tmp/dmgout 044-76040-038.dmg
```

实测结果：

```
/sbin/launchd   561,856 bytes   Mach-O 64-bit arm64 executable
                               flags:<NOUNDEFS|DYLDLINK|TWOLEVEL|PIE>

9.7 GB / 210,828 个文件
  8.1G  System
  713M  usr
  483M  private
  313M  Applications
  139M  Library
  960K  sbin
```

**注意**：`dmg2img 1.6.7` **不支持 LZFSE**，会报
`Unsupported or corrupted block found: -2147483641` 并输出残像。用 7-Zip。

`7z l -slt` 能直接看出压缩方式：

```
Method = Copy Zero2 LZFSE CRC
Path   = 3.apfs
Size   = 6077546496         5.66 GiB
Type   = APFS
Name   = DawnUpdate21H461.J120J121J207J208OS.apfs
num_files: 165509
```

---

## 五、六道硬坑与解法（照这个顺序踩，能省几天）

### 坑 1：`fastboot boot` 报 `Failed to load/authenticate boot image: Load Error`

**不是** 官改 ABL 禁用 boot，**是 USB 线**。

- 劣质充电线会让 96MB 级镜像传输丢数据 -> ABL 校验失败
- 换原装数据线 + 插主板后面板 USB 口后，同一镜像立刻 `Booting OKAY`

### 坑 2：ramdisk 大小有上下限

| ramdisk | ABL | UEFI |
|---|---|---|
| 16 MB | Load Error | — |
| 32 MB | OK | 菜单正常 |
| 64 MB | OK | 菜单正常 |
| 96 MB | OK | 黑屏 |

可用区间 **32–64MB**；`header_version = 1`，`page_size 2048`。

### 坑 3：`fastboot boot` 前先重启设备

设备每次重启后**第一次 boot 成功率最高**；连续 boot 会出现 Load Error（download buffer 状态残留）。

### 坑 4：把内核塞进固件卷走不通（PEI 解压预算）

| FVMAIN 大小 | 结果 |
|---|---|
| 8 MB（原始） | 固件正常 |
| 62 MB（未压缩内核） | 回 fastboot |
| 15 MB（LZMA 压缩内核） | 回 fastboot |

FVMAIN 上限约 8–10MB；FD 本身也有上限。**内核不能走 boot.img 内核段，只能走存储。**

### 坑 5：没有任何 FAT 盘（最根本的一个）

固件其实自带完整存储栈（`UFSDxe` / `PartitionDxe` / `DiskIoDxe` / `Fat`），
但安卓分区全是 ext4/f2fs，UniXNU 枚举不到任何带 `unixnu.toml` 的卷。

### 坑 6：ADT 里的 DRAM 必须是实测值

```
dram-size               = 0x300000000   12 GiB（本机扩容）
darwinboot-memory-size  = 0x20000000    512 MiB 连续窗口
```

`darwinboot-memory-size` 是 DarwinBoot 向固件要的一块**连续**物理内存。
512 MiB 足够；更大的值更难从 `EfiConventionalMemory` 里找到连续区间。

---

## 路线图：已完成什么、还缺什么

**开机与桌面不是终点，只是第一个里程碑。** 下面的清单按「能力域」组织，
每项都标注是已落地、部分完成还是完全没有。

### 已完成

| 能力 | 状态 | 依据 |
|---|---|---|
| 内核启动 → 用户态（launchd） | ✅ | l730 之前完成 |
| 系统 UI（锁屏） | ✅ | l730：`Migration not necessary` + `finishedStartup:YES` |
| **原生 Home Screen** | ✅ | l769：`ToHome` / `UI Locked:NO` / 无 `ForceLockKey` |
| 触摸输入 | ✅ | `NT36523`，`fingers 4→436`，client = `backboardd` |
| GPU 合成 + scanout 接管 | ✅ | l771：`P4/PF` 生效，GPU 服务 6 帧 |
| 8 核 SMP | ✅ | 7 个副核全部 `st=7 running` |
| UFS 根卷 + RAM Data | ✅ | APFS 只读根 + `nabuvar` |
| 无人值守复位 | ✅ | `pet=600` + `pon 0x80041` 自动回 TWRP |

### 进行中 / 部分完成

| 能力 | 状态 | 缺口 |
|---|---|---|
| **流畅度** | ⚠️ 0% | `present` 均值 221 ms、尾帧 844 ms；理论上限 3.5–7 fps |
| DPU 直出 | ⚠️ 未落地 | `native_ufs_dpu_map` 仍为返回 0 的桩，`startDirect()` 恒不进入；当前走 CPU blit |
| GPU 频率策略 | ⚠️ 缺 | GMU 频率策略未实现，当前非最优档 |
| 背光控制 | ⚠️ 缺 | `backlight factor:0` 只有默认值 |
| 持久化 Data | ⚠️ 部分 | 只有 RAM 种子，掉电即失 |
| 诊断完整性 | ⚠️ 部分 | CDC 无 CRC/序号；335 s 后无通道 |

### 完全未实现（缺驱动，按难度排序）

| 能力域 | 缺什么 | 难度 |
|---|---|---|
| **电源管理** | 只有 stub：缺 PMIC、RPMh、regulator 树、CPU DVFS（EPSS）、电池电量计 | 高（牵动全局） |
| **音频** | 全部：SM8150 ADSP、codec、clock fabric 均未盘 | 高 |
| **网络（Wi-Fi）** | 全部：硬件清单与固件处理都没有 | 高 |
| **物理按键** | 按键与 key event 未接 | 中 |
| **摄像头** | 未开始 | 高 |
| **蓝牙 / GPS** | 未开始 | 中 |
| **指纹** | 未开始 | 高 |
| **SEP / 安全隔区** | 无 SEP，因此无法登录 Apple ID、无蜂窝 | 极高（需硬件级） |
| **存储高级特性** | 持久写入、多 LUN、UFS 错误恢复、power collapse | 中 |
| **SMMU 故障恢复** | 只做映射，未做 fault recovery | 中 |

### 所以「下一步」按判别力排序

1. **流畅度**（唯一影响可用性的功能项）
   - 分离 `present` 长尾因果：启动期 pipeline compile 与长尾共现，需无触摸稳态数据
   - 核实 Metal 完成等待是否存在死锁：上游同 SoC 方案定位到
     `dispatch_group_wait(_handlers, DISPATCH_TIME_FOREVER)` 的两种死锁场景，
     其补丁 26 行已打包但未上板验证；本项目尚未核对自身实现
2. **电源管理**（后面所有驱动的前置）
   - 没有 regulator/DVFS，GPU 和 CPU 都跑不到最优档，这本身也是流畅度的一部分
3. **持久化 Data**（决定能不能当日常设备用）
4. **诊断完整性**（决定后续调试效率）
   - CDC 加 CRC/序号/丢包计数，消除 48.6 s 黑洞只能靠时间跳变反推的现状
   - 把覆盖延到 335 s 之后（WDT 咬点在 380–410 s）
5. **外设**：按键 → 音频 → Wi-Fi → 蓝牙/GPS → 摄像头 → 指纹

### 已否证的方向（别再试）

| 方向 | 结论 |
|---|---|
| `nabu-gpu-scanout=1` | 数学上必然失效：GOP `0x9c000000+0xfa0000` 落在 DRAM 管理区内部 |
| `adrenoprobed` | 未被 launchd 加载，且只导 `oslog.txt.gz` |
| `PF/PX/P0..P4 = 0` 当失败判据 | 走 GPU7 标记流，本内核 capture hook 是空实现 |
| `ncpy = 0` 当零拷贝证据 | 已撤回 |
| 纯 WIMG 单字节常量轮 | l732/l733 证明无效 |
| `span_capacity` 单字节缩容 | l735/l736 证明 32000 B 仍分配失败 |
| `nabu-dpu=1/2` 探针 | l737 触发 `data abort`，DPU 第一道门关闭 |
| 纯时间常数（`pet`/`diag` 后移） | l724/l725/l728 均阴性 |

---

## 七、参考

**完整的来源与许可证说明见 [`CREDITS.md`](CREDITS.md)**——按「直接依赖 / 参考 / 我们原创」三类分开写清。

下面只列最直接相关的几个：

- [`Allenwdk/xnu-linux-toolchain`](https://github.com/Allenwdk/xnu-linux-toolchain) —— Linux 交叉工具链
- [`1PJL/unixnu-nabu`](https://github.com/1PJL/unixnu-nabu) —— UFS 卷诊断系列（`Recover GPT` / `Build Volume` / `Repack` 三步，跑在 `ubuntu-24.04` 上）
- [`Lux12138/uefi-nabu`](https://github.com/Lux12138/uefi-nabu) —— 本项目的 UEFI 固件（fork 自 `UniXNU/uefi-sheng`）
- [`Lux12138/nabu-usb-cdc`](https://github.com/Lux12138/nabu-usb-cdc) —— CDC 实时日志通道
- [`Lux12138/xnu-linux-toolchain`](https://github.com/Lux12138/xnu-linux-toolchain) —— Linux 交叉工具链
- `UniXNU` 上游 —— 内核移植框架
