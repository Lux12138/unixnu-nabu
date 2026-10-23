# 状态快照

## 里程碑：内核走到了 launchd

**屏幕证据（2026-10-23 晚）**

```
Process 1 exec of /sbin/launchd failed, errno 2
/sbin/launchd
kern_exec.c

FAR=18446744042837921792
PC=18446744005117767160
LR=18446744005111703156
```

**内核完整走完了这条链：**

```
bsd_autoconf()        ✅
  kminit()            ✅
    IOsurfaceRoot::installMemoryRegions()   ✅   ← 之前以为卡在这里，其实没有
bsd_init 后半段       ✅
  vfs_mountroot()     ✅
  load_init_program() ✅   ← 解析出 /sbin/launchd
    execve("/sbin/launchd")   ❌  errno 2 (ENOENT)
```

**`errno 2` 是文件不存在 —— 不是内核问题，是根文件系统里没有那个文件。**

### 这推翻了之前三个月的判断

`IOsurfaceRoot::installMemoryRegions()` 那一行**从来不是终点**，它只是最后一条 kprintf。
它之后紧接的 `dtrace_postinit()`、`inittodr()` 和条件编译的网络初始化都不打日志，所以
「日志停止」被当成了「内核停止」。

真正一直缺的是**一个真正的根**，不是自研 surface。

## 本地 Linux 构建打通（提速 12 倍）

| 项 | GitHub macOS runner | 本地 WSL Ubuntu |
|---|---|---|
| 单轮构建 | 15–17 分钟 | **72 秒** |
| 产物 | 49.2 MB | **49 MB** |
| 可复现 | 只能靠 CI | 本机随时跑 |

验证：

```
file            Mach-O 64-bit arm64 executable, flags:<NOUNDEFS|PIE>
__TEXT vmaddr   0xfffffff007004000      ← 与原版 kernelcache 一致
symbols         22708
nabu 探针       nabu_mark / nabu_mark_map / nabu_mark_num / nabu_mark_putc
UFS 探测        nabu_ufs_register_probe.cpp
sha256          c756ecdb88a9dd50156dc27d2cdf48a08ddf93fcf204040fa57804f814fc3c15
```

**工具链**：`Allenwdk/xnu-linux-toolchain`（Ubuntu 24.04 / clang 18 / 12 核）

## 一路上解决的 18 个问题

```
 1  libdispatch 无 Ubuntu 包          → 源码编译 + 手写 .pc
 2  toolchain/csh 是符号链接          → cp -L
 3  xcrun 硬编码 /usr/bin/xcrun       → 软链
 4  sysctl 硬编码 /usr/sbin/sysctl    → Darwin shim 替换
 5  /bin/sh 是 dash                   → 指向 bash
 6  bison/flex/m4 在 /usr/sbin        → 软链
 7  hostcompat.h 不存在               → 自写
 8  libc.h 不存在                     → 自写（OSAtomic / 页对齐）
 9  copyfile.h / fcopyfile            → 自写
10  setmode/getmode                   → 自写（who 位移 + mask 修正）
11  TargetConditionals.h              → 自写，且只放两处
12  sys/_types/_os_inline.h           → 自写并镜像到 compat 树
13  mach/mach_init.h                  → 自写
14  -Wno-* 不认识                     → -Wno-unknown-warning-option
15  macOS clang 26 诊断名             → CCC_OVERRIDE_OPTIONS 逐条降级
16  sed -i '' 是 BSD 语法             → 分支处理，且只在 Darwin 跑
17  awk \( 在 GNU 保留反斜杠          → 做成 patch（zzzz-nabu-awk-parens）
18  mockfs 双重链接（23 个重复符号）  → 去掉 force_load
```

## 下一步

**唯一缺的是一份真正的根文件系统。**

```
./unixnu ipsw fetch                          # OS + Cryptex1,SystemOS
./unixnu build userdata                      # SHNGROOT manifest + 两个窗口
fastboot flash userdata out/nabu/userdata/userdata-root.img
```

已知：`[userdata]` 的 LBA 早实测好（`first_lba = 0x288C40` / `last_lba = 0x77BF3FA` /
`lba_bytes = 4096`）。

**不再是编译问题，不再是驱动问题 —— 是数据问题。**
