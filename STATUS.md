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


## 这一轮我反复犯的错（写给下次的自己）

这一晚在 `bsd_init.c` 的探针上耗了十几轮构建，**没有一次是内核的问题，全部是流程问题**。
逐条记下来，下次不再犯。

### 1. 同一个动作重复十几次而不换方法

每次都是「猜行号 → 写 diff → 构建 70 秒 → 失败」，持续了十几轮。

**`git apply` 对偏移过大的 hunk 是静默丢弃的**，不报错也不失败。所以「构建成功」不等于
「patch 应用了」。我直到用 `strings kernel.macho | grep NABU` 数出来一直是 **13 个**而
不是 14 个，才发现 `vfs_mountroot` 那个 hunk 从来没进去过。

**规则：同一个工具失败三次就换方法，不要继续调参。** 最后改成按唯一锚点插入文本
（`scripts/nabu-bsd-init-probes.py`），一次成功，而且幂等。

### 2. 不先验证就构建

改完 patch 应该先 `git apply --check`，我却直接构建，用 70 秒换一个「还是不行」。

**规则：能在一秒内验证的事，不要用一分钟的构建去验证。**

### 3. 行号量错了对象

我量的是「我的 patch 已经应用之后」的位置，却当成「应用之前」的基线写进 diff。
更糟的是中途换了三次参考状态（pristine / `unixnu tree` 之后 / build 失败之后），
每次得到的数字都不一样：

```
pristine                      calling bsd_autoconf = 816
unixnu tree 之后（没打 patch）  = 816
build 失败之后（有前面 64 个）  = 924
我的 patch 应用之后            = 925
```

**规则：写 diff 前先明确「我要的是哪一个状态的坐标」，并在同一次测量里取全部锚点。**

### 4. WSL 里用 Windows 路径

`generate_patch.py` 里写 `r'C:\Users\...'`，在 WSL 里那是**字面文件名**，于是新 patch
写进了一个叫 `C:\Users\...` 的怪文件，真正的 patch 一个字都没变，构建自然还是旧的。

**规则：WSL 里一律 `/mnt/c/...`。**

### 5. 改了文件没提交，WSL 的 `reset --hard` 就把它吃掉了

出现过至少三次。`build` 脚本第一步就是 `git reset --hard FETCH_HEAD`。

**规则：改完立刻 commit + push，再让 WSL 拉。**

### 6. 把「探针不存在」当成「探针没输出」

我说过「`NABU: loopattach` 没出来，所以卡在 loopattach」——
**那条探针当时根本不存在**。判断「某条日志没出现」之前，必须先确认它在二进制里。

**规则：`strings kernel.macho | grep '^NABU: '` 数一遍，再解读屏幕。**

### 7. 追 `IOsurfaceRoot` 的源码

花时间在树里搜 `installMemoryRegions`，只搜到二进制。
`nm kernel.macho` 里其实**一个 `iosurfaceroot` 符号都没有** —— 它来自预链接进
`kernelcache.decompressed` 的 Apple kext，改不了。用户直接叫停了这条死路。

**规则：先 `nm`/`strings` 确认符号归属，再决定要不要找源码。**

### 8. 为了「对齐别人」把已验证的值改坏

把 `darwinboot-memory-size` 从 4.75 GiB 改成 1PJL 的 512 MiB，直接从「走到 launchd」
退回到「卡在 `kminit`」。那是 6 GiB 机型的值，而这台是 **12 GiB 扩容机**。

**规则：能跑通的配置别动。别人的参数要按自己机器的物理内存换算。**

### 9. 忽略了构建缓存

改了 patch，但 `bsd_init.o` 因为 `OWNER_ROOT_STAMP` 没变而没重编，探针自然进不去。

**规则：动 `bsd_init.c` 就必须 `find build -name 'bsd_init.o' -delete`。**

### 10. 每次 `unixnu tree` 都会清掉 `.cache/xnu-work` 里的手工改动

包括 DriverKit 头（`IORPC.h`），于是 `archive-03` 报 `IORPC.h file not found`。
现在统一入口 `build_one.sh` 里会自动补，但这条要记住。

### 最终形态

```bash
bash scripts/nabu-bsd-init-probes.py .cache/xnu-work/bsd/kern/bsd_init.c
```

按锚点插入，输出每一处 `insert` / `skip`，重复运行安全。
`build-xnu-4k-baseline.sh` 在 patch 循环之后调用它。


## 根卷挂载路径：卡点已经收敛到一行调用

三天前只能看到「屏幕停住」。现在 `bsd_init.c` 和 `IOKitBSDInit.cpp` 里一共 39 个探针，
把 `inittodr` 之后的每一站都照亮了。实际走到的路径是：

```
inittodr done
mount loop iteration
setconf enter
setconf calling IOFindBSDRoot
IOFindBSDRoot enter
IOBSD wait start
IOBSD waitForMatchingService
  IOsurfaceRoot::installMemoryRegions()        <- 等待期间枚举服务，不是卡点
  IOSurface disallowing global lookups
IOBSD matched                                   <- apfs + IOStorageFamily 都注册成功了
IOBSD read /chosen
root-matching Present                           <- 我加的 UUID 读到了
didRam=0
mdevadd ramdisk base=c40d5000 size=400000       <- 4 MiB RAMDisk，就是 ramdisk.img
past ramdisk, matching=set rd=none              <- matching 有值，rd 已清空
waitForService enter                            <- ★ 卡在这里
```

### 结论

`IOFindBSDRoot` 拿着 `root-matching` 的匹配条件去等一个 IOMedia：

```xml
<dict>
  <key>IOProviderClass</key><string>IOMedia</string>
  <key>IOPropertyMatch</key>
  <dict><key>UUID</key><string>04769D26-2CA0-4825-B443-E2E5F88300F2</string></dict>
</dict>
```

**这个 UUID 是 APFS 卷的 UUID，不是 GPT 分区 UUID。** 它只有在 apfs 解析完容器、把卷作为
IOMedia 发布之后才会出现在属性里。

`IOKitBSDInit.cpp` 的等待是：

```c
968  do {
969      t.tv_sec = ROOTDEVICETIMEOUT;      // 120 或 60 秒
973      service = IOService::waitForService( matching, &t );
974      if ((!service) || (mountAttempts == 10)) { ... }
```

**超时不是致命的** —— 返回 NULL 后打印 `still waiting for root device`、睡 5 秒再重试。
所以把 `ROOTDEVICETIMEOUT` 临时改成 5 秒，每次刷机 30 秒就能拿到答案。

### 排除掉的三条死路

| 曾经怀疑 | 实际 |
|---|---|
| `IOsurfaceRoot::installMemoryRegions()` 卡死 | 它来自预链接的 Apple kext（`IOSurfaceRoot.cpp`），只是 `IOBSD wait` 期间枚举服务的副产品 |
| `native_offline_gpu_publish()` 卡死 | `XNU_NATIVE_OFFLINE_GPU` 默认 0，`strings` 里那条调用根本不存在 |
| `darwinboot-memory-size` 太小 | 4.75 GiB 足够把 4 MiB RAMDisk 映射出来（`mdevadd base=c40d5000`）。512 MiB 那次的回退是它导致的，已恢复 |
| apfs 没在 roots 里 | `IOBSD matched` 证明 apfs 和 IOStorageFamily 都注册了 |

### 下一步要看的两件事

1. **`waitForService` 是超时返回 NULL，还是根本卡死？** 5 秒超时后如果反复打印
   `waitForService=(nil)` + `still waiting for root device`，就是「UFS 没发布带该 UUID
   的 IOMedia」；如果 `=%p` 一直不打印，才是 IOCatalogue 的锁问题。
2. **UFS 驱动发布 IOMedia 时带不带 UUID。** `NativeUfs.cpp:1532` 和
   `NativeRamBlock.cpp:293` 里的 `selectVolumeSeed()` 都是**查询**方（`getMatchingServices`），
   不是发布方。真正设 `UUID` 属性的是 `IOStorageFamily` 从介质里读出来的，
   所以 UFS 一侧必须让 apfs 能认出容器头。

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
