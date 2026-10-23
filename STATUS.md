# 状态快照

> 本机是 **扩容版 nabu**：物理 DRAM **12 GiB**，实测自 TWRP 的 `/proc/meminfo`
> （`MemTotal = 11766512 kB`，即 12 GiB 减去约 816 MB 给 modem/hyp/rmtfs）。
> 因此 `dram_size = 0x300000000`、`owned_memory_size = 0x130000000` 是**为这台机器测出来的值**，
> 不是标准 6/8 GB 机型上的数字。照抄其他项目的 `0x180000000`（6 GiB）会让 6 GiB 以上的
> 物理内存对 Darwin 不可见。

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

### 这推翻了三天来的判断

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


## 给 `.cache/xnu-work` 里的文件打补丁：六个必踩的坑

`unixnu tree` 每次都把 `.cache/xnu-work` reset 到 `d8b8029` 再按文件名顺序应用
`patches/xnu/*.patch`，所以**手工改工作树一定会被覆盖**，必须做成 patch。而 patch 本身有六个坑，
这次全踩了一遍：

| 坑 | 症状 | 正解 |
|---|---|---|
| 每个 hunk 都带一对 `--- / +++` | `git apply` 当成多个文件，输出多次 `Checking patch` | **一个文件头，hunk 堆叠** |
| 空行上下文写成真空行 | `patch` 报 `fuzz`，`git apply` 直接拒绝 | **` ` + 换行**（一个空格）|
| hunk 只有 2 行上下文 | `git apply` 默认不 fuzz | **至少 3 行**，实践中给 6–12 行 |
| 行号按干净检出的文件算 | `failed at 801` / `924` | **量「前面所有 patch 应用后」的位置** |
| 起点假设有注释块 | `bsd_autoconf(void)` 上面并没有 `/**` | 从 `kern_return_t` 开始 |
| 跳过中间某个 `#if` 块 | `#endif` 后其实还有 4 行 `XNU_NATIVE_OFFLINE_GPU` | 上下文截到 `#endif` 就停 |

### 怎么量真实行号

`unixnu tree` 失败时，`.cache` 正好停在「所有前面的 patch 已应用、当前这个未应用」的状态 ——
这就是要量的目标：

```python
import io
src = io.open('.cache/xnu-work/bsd/kern/bsd_init.c', encoding='utf-8').read().split('\n')
for needle in ('calling bsd_autoconf', 'bsd_autoconf(void)', 'kminit();'):
    print(needle, [i + 1 for i, l in enumerate(src) if needle in l][:5])
```

本次实测值（前面 patch 全部应用后）：

```
' * Version 2.0.'         80
'#include <sys/param.h>'  83
calling sysctl_mib_init  912
calling bsd_autoconf     924
bsd_autoconf(void)      1291     ← 323 是前置声明，不是定义
kminit();               1294
```

### 验证补丁能应用，别靠构建

```bash
cd .cache/xnu-work
git apply --check /path/to.patch && git apply /path/to.patch
grep -n 'NABU:' bsd/kern/bsd_init.c
```

**注意不要先 `git checkout --` 那个文件** —— 那会把它还原成 pristine，行号又变回另一套。


## `bsd_autoconf` 之后为什么一片静默

内核在 `bsd_autoconf()` 返回后看起来「停住」，但这中间每一站都用 `bsd_init_kprintf` 打印，
而它在当前配置下是 **no-op**。所以卡在 `dtrace_postinit`、`loopattach`、`ether_family_init`、
`net_init_run`、`inittodr`、`vfs_mountroot` 中的任何一处，屏幕上看到的都和卡在上一行一模一样。

现在每一站都用真 `kprintf` + `NABU_MARK` 括起来了：

```
NABU: before kminit          61
NABU: after kminit           62
NABU: bsd_autoconf returned  65
NABU: dtrace_postinit
NABU: dtrace done            66
NABU: loopattach             67
NABU: loopattach done
NABU: ether_family_init      68
NABU: ether done
NABU: net_init_run           69
NABU: net done
NABU: inittodr               70
NABU: inittodr done
```

**最后一条出现在屏幕上，就是真正的卡点。** 编出来的 `kernel.macho` 里 13 条字符串全部可查：

```bash
strings out/nabu/kernel/kernel.macho | grep '^NABU: ' | sort -u
```

### 两个曾经误导判断的假线索

**`IOsurfaceRoot::installMemoryRegions()` 不是卡点。** 它来自预链接进
`kernelcache.decompressed` 的 Apple kext（`IOSurfaceRoot.cpp`），源码不在这棵树上。
`nm kernel.macho | grep -i iosurfaceroot` 是空的 —— 那行日志只是 kext 的 `start()` 顺带打印。

**`native_offline_gpu_publish()` 也没有被调用。** `XNU_NATIVE_OFFLINE_GPU` 默认 0，
`#if` 整体为假，`nm kernel.macho | grep native_offline_gpu` 是 0 次。它的源码在
`drivers/NativeMetal/NativeOfflineGPU.cpp`，但这份内核里没有它。

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
