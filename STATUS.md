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

#
## 屏幕输出：已经修好了

### 两个根因（都在画字代码里，不在行号）

| 问题 | 代码 | 后果 |
|---|---|---|
| 每画一行清掉 **450px** | `nabu_mark_text`: `fill(0, y-12, g_w, 10*lh, BLACK)`，`lh = 45` | 行距小于 450px 的行互相擦除，怎么排位都没用 |
| console 滚到**屏幕最底** | `nabu_mark_putc`: `g_ty > g_h - 40` 才回卷 | console 会盖掉任何位置的状态行 |

### 修法

```c
/* nabu_early_mark.c —— 每行只清自己 */
fill(0, y > 20 ? y - 12 : 0, g_w, lh + 16, 0xFF000000);

/* nabu_early_mark.c —— console 停在状态区之上 */
if (g_ty > (g_h > 200 ? g_h - 780 : 600)) { g_ty = 16; }
```

### 最终布局（panel 1600x2560）

```
 16 ────────────────┐
      console        │  滚到 1780 为止
1780 ────────────────┘
                    ← 780px 保留区
1850  guard 0/1/2
1920  walk N
1990  <类名>
2060  <实例名>
2130  IOFindBSDRoot entered
2200  A call wfs  →  B returned 0/1
```

**六行等距 70px、互不干扰、console 进不来。** 这一块已经稳定，实机验证通过。

### 还剩下的屏幕问题

console 日志仍然显示。原因查清了：`PE_kputc` 有**三处**赋值，前两处我已经关掉：

1. `osfmk/arm/nabu_early_mark.c` —— `if (0)` 包住，已失效
2. `pexpert/arm/pe_kprintf.c` 的 regex patch —— 因为 `PE_kputc` 是函数指针而非函数定义，regex 匹配不到，走了 WARN 分支
3. **`patches/xnu/zzzz-nabu-kprintf-default-on.patch:29`** —— 无条件执行，**这才是日志还在的真正来源**，下一轮处理它

另外 `IOsurfaceRoot` 和蓝底 `UnI XNU` 横幅已经通过「映射后整屏填黑」清掉了。


## 根因找到了：ADT 缺一个 `interrupt-controller` 属性

**这是整条链的真正源头，比之前追的 IODTNVRAM 过滤要根本得多。**

屏幕上有这么一行白字：

```
pe_arm_map_interrupt_controller: failed to find the interrupt-controller.
```

`pexpert/arm/pe_identify_machine.c:206` 是这样找中断控制器的：

```c
if (SecureDTFindEntry("interrupt-controller", "master", &entryP) == kSuccess) {
    SecureDTGetProperty(entryP, "reg", (void const **)&reg_prop, &prop_size);
    gPicBase = ml_io_map(soc_phys + *reg_prop, *(reg_prop + 1));
}
if (gPicBase == 0) {
    kprintf("...failed to find the interrupt-controller.\n");
}
```

`SecureDTFindEntry` 的签名是 **(propName, propValue, entryP)**，同文件里其它调用可以印证：

```c
SecureDTFindEntry("name", "arm-io", ...)          // 找 name = "arm-io"
SecureDTFindEntry("device_type", "timer", ...)    // 找 device_type = "timer"
SecureDTFindEntry("name", "uart0", ...)           // 找 name = "uart0"
```

所以它找的是：**带 `interrupt-controller` 属性、且该属性值为 `master` 的节点**。

而 `make-nabu-adt.py` 生成的 `intc` 节点只有三个属性：

```python
node('intc', [('compatible', b'arm,gic-v3\0'),
              ('reg', ...),
              ('redistributor-stride', ...)])
```

**没有 `interrupt-controller` 标记，所以永远找不到。**

### 为什么这一个属性就能卡死整个 boot

```
ADT 缺 interrupt-controller
  → gPicBase = 0，中断子系统完全不可用
    → IODTNVRAM 读属性要走 SPI/I2C，等不到中断，无限阻塞
      → instanceMatch 卡在 matchInternal 里
        → waitForService(IOMedia+UUID) 的 5 秒超时也轮不到（它持着 IOCatalogue 锁）
          → boot 停住
```

**UFS 读盘要走中断，也是同一个病根。** 之前把卡点归到 IODTNVRAM、UFS、apfs、GPU 都不对，
它们都只是「等中断」的不同受害者。

### 修复

```python
node('intc', [('compatible', b'arm,gic-v3\0'),
              ('interrupt-controller', b'master\0'),    # ← 新增，SafeDTFindEntry 找的就是它
              ('name', b'interrupt-controller\0'),
              ('reg', struct.pack('<4Q',
                                  GIC_DIST - ARMIO_BASE, GIC_DIST_SIZE,
                                  GIC_REDIST - ARMIO_BASE, GIC_REDIST_SIZE)),
              ('redistributor-stride', struct.pack('<Q', 0x40000))])
```

基址本来就对得上：

```
ARMIO_BASE = 0x17000000        (arm-io 的 ranges，即 gSocPhys)
GIC_DIST   = 0x17a00000        (SM8150 sm8150.dtsi intc@17a00000)
reg 偏移   = GIC_DIST - ARMIO_BASE = 0xA00000
gPicBase   = soc_phys + 偏移 = 0x17a00000   ✓
```

## 下一步要看的两件事

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

> ### 一个会静默毁掉所有构建的坑（已修）
>
> WSL 的全局 gitconfig 里曾经有：
>
> ```
> url.https://gh.llkk.cc/https://github.com/.insteadof  https://github.com/
> ```
>
> 那个加速镜像已经挂了，于是**每次 `git fetch` 都静默失败**（`gnutls_handshake() failed`），
> 而构建脚本照旧用「已经检出的旧代码」继续跑。表现是：明明改了仓库，构建结果却永远不变。
> 已删除该规则，`github.com` 直连正常。
>
> **凡是「改了没反应」，第一步先查 `git log -1` 和 `git fetch` 是否真的成功。**


> ### 里程碑：块驱动第一次真正注册（walk 从 1 变成 2）
>
> 之前所有版本的屏幕都是 `walk 1` —— 遍历只碰到那个 IODTNVRAM 节点，因为 UFS 是只读探针，
> 从不发布块设备。恢复「真块驱动 + `build-ufs-original-bindings.py`」之后：
>
> ```
> keys1
> walk2      ← ★ instanceMatch 走了 2 个服务，块设备自己注册进来了
> guard2
> IOFindBSDRoot entered
> ```
>
> **这同时证明绝对地址绑定（`.globl` + `.set symbol, <addr>`）在真机上成立** —— 若地址不对，
> 内核会在更早阶段崩掉，不会走到 `IOFindBSDRoot`。
>
> ### 为什么解决了「undefined 与 duplicate」的两难
>
> `IOBlockStorageDevice` 属于 IOStorageFamily kext，不在内核里：
>
> | 做法 | 结果 |
> |---|---|
> | 不链接 | 几百个 `__ZN20IOBlockStorageDevice...` undefined |
> | 链接 `IOBlockStorageDevice-reference.o` | 注册第二个 metaclass → `postModLoad` panic |
> | `-Wl,-undefined,dynamic_lookup` | Linux 的 ld64 断言崩溃（macOS 才可用） |
> | **`build-ufs-original-bindings.py`** | **符号 `.set` 到 prelinked kext 的真实地址** |
>
> binder 读的是 `build/native-prelink/*-candidate.macho` + `.json`。那个 candidate 从不生成，
> 但 `out/nabu/kernel/kernel.macho` + `kernel.json` 满足它的全部校验
> （`candidate_sha256` 相符、`original_sha256` 等于锁定的 kernelcache、plan 里含 IOStorageFamily），
> 所以构建前把这两个文件暂存成 candidate 即可。产出 11 个符号，全部标记 `A`（absolute）。
>
> **时间线（从仓库历史恢复）**：20:40 内核到 launchd 时，块驱动与 bindings 都在位；21:37 改用
> 寄存器探针，21:41 顺手去掉 bindings，此后 `IOMedia` 恒为 0，文档却记成「不是驱动问题」。
>
> ### 待修：`IOMedia` 计数行会被 console 覆盖
>
> `nabu_mark_num("IOMedia", nmedia, 1850)` 在探针里仍在执行、内核里也有该字符串，但屏幕上不显示：
> 该行位于 console 残留白字的覆盖范围内。下一轮把它挪到 1900 以下或改为与 `keys` 同排显示。

> ### 下一阶段目标（当前）
>
> 构建是绿的，块驱动按设计是只读探针，都不是问题。目标收敛成三条：
>
> 1. **用仓库里这一版内核启动，确认它健康地越过 `vfs_mountroot`** —— 判据是屏幕上不再出现
>    `Metaclass ... duplicate class`、`gPICBase` 是有效地址、六行状态行干净。
> 2. **查清已经刷入的 userdata 为什么没被采用为根** —— 对照 `userdata-root.json` 里
>    `manifest_sha256 = 6684c652…` 与 OS 窗口 `offset=1048576 / size=6077546496`，
>    确认 `[payload.adt]` 的 `root_match_volume_uuid` 与 manifest 里的卷身份一致。
> 3. **只有上面两条都成立**，才重新考虑真块驱动（`XNU_NABU_UFS_REGISTER_PROBE=0`），
>    而那条路需要 verified-root bundle，bundle 需要 target image 的 volume policy。







> ### 本轮的终点：用户空间缺页无法修复（VM/PMAP 层）
>
> 从 `B returned1` 之后，内核进到 `launchd` 的 `execve`，然后在这条路径上 panic：
>
> ```
> ESR = 0x96000045     Data Abort, WnR=1(写), DFSC=0x5(translation fault L1)
> PC  = __bcopyout + 0x1c
> LR  = _copyout + 0x70
> FAR = ustk - 328     落在用户栈分配区间内
> ```
>
> 逐层排除的结果：
>
> | 已验证 | 结论 |
> |---|---|
> | `cpo0`（exec_copyout_strings） | 成功 —— 栈顶几页可用 |
> | `cp20`（dynlinker copyoutptr） | 成功 |
> | `prt16384` | 不是 `size - rlimit` 下溢，protect 正常 |
> | `freepg 91199444` / `wirepg 945550` | **都超过物理页数（12 GiB ≈ 78 万页）**，页统计不可信 |
> | 提前调用 `exec_prefault_data` | 无效 —— 它只 prefault 数据段，不含栈 |
> | 在 `ap` 上直接 `copyout` 一字节 | **本身 panic，没有返回错误** |
>
> **结论：不是地址算错、不是栈太小、不是内存耗尽，而是「用户空间缺页时内核不修页，直接 data abort」。**
> 正常内核里 `copyout` 触发缺页会由 `vm_fault` 现场建页后重试指令；这里它变成 panic。
>
> ### 下一轮的三条线索（同一个根）
>
> 1. **`MACHINE_CONFIGS=BCM2837`** —— 树莓派配置用于 SM8150，ARM64 pmap 的细节（页表属性、
>    `pmap_enter` 的 `VM_MEMORY_STACK` 分支）可能与真机要求不同；
> 2. **`copyout` 的 fault 恢复路径** —— `user_access` 异常表项是否注册正确（`fault_addr`）;
> 3. **`vm_page_bootstrap` 拿到的内存尺寸不对** —— 页统计超出物理内存，会连带影响 pmap 分配页表页。
>
> 三者指向同一件事：**VM/PMAP 在「非标准 ARM64 平台配置」下的初始化不完整。** 本轮的价值是把它从
> 「完全看不见」变成「精确到层」。
>
> ### 验证工具的三个坑（各踩过）
>
> | 坑 | 后果 | 正解 |
> |---|---|---|
> | `strings` 默认 ≥4 字符 | 2 字符探针标签看似不存在 | `strings -n 2` |
> | `nabu_mark_text` 每行清 61px | 行距 <61px 的标记互相擦除 | 行距 ≥80px |
> | xnu 目标文件是 `.cpo` | 改源码不重编 | 删 `.cpo` 不是 `.o` |

> ### 🎯 突破：根卷找到，launchd 出现（B returned 1）
>
> ```
> IOFindBSDRoot entered
> B returned1              ← waitForService 返回了服务：带 UUID 的 IOMedia 找到了
> launchd                  ← 内核进到 load_init_program / execve
> unexpected STATUS!! ...
> ESR=0x96000045  FAR=0x16eff7eb8
> PC=0xfffffff00740eb60  LR=0xfffffff0073fb070
> ```
>
> **这是本移植第一次让 `waitForService(IOMedia+UUID)` 真的返回。** 之前所有版本都是等不到、
> 或直接空转；这一次根卷匹配成功，`mountroot` 走通，内核继续到 `launchd`。
>
> 触发条件：退休 `keys`/`walk`/`guard` 三个探针之后匹配才成功。它们每次都在
> `getMatchingServices` 里遍历整个注册表，很可能扰乱了匹配时序。
>
> ### 新的卡点：execve 写用户栈时 fault
>
> ```
> PC  = __bcopyout + 0x1c      内核在 copyout（往用户空间写）
> LR  = _copyout + 0x70        调用者
> FAR = 0x16eff7eb8            目标地址（合法的 ARM64 用户地址，但未映射）
> ESR = 0x96000045             EC=0x25 Data Abort, WnR=1 写, DFSC=0x5 translation fault L1
> ```
>
> 也就是：`/sbin/launchd` 已被加载，内核正在把 argv/envp 复制到新进程用户栈，而目标页没有建立
> 映射。**这属于 VM / execve 层，与存储路径无关。**
>
> 待查方向：
>
> 1. `execve` 里用户栈的分配路径（`exec_mach_imgact` → `allocate_user_stack`）是否因为
>    `userdata` 上的某些条件被跳过；
> 2. 用户地址空间的上限与该地址（6.1 GiB）的关系 —— 我们的 `dram_size = 0x300000000`（12 GiB），
>    用户 VA 布局可能与这个规模有关；
> 3. 是否存在「页表项建好但 pmap 未同步」的时序问题。
>
> ### 今晚完整链条
>
> ```
> boot-arg 修复        -> ufs:entered
> IODelay 换自旋       -> host:lun
> validate() 打开      -> prep:attach-done
> start() 插桩         -> start:ok
> 块设备发布           -> walk 3->5
> 时基确认             -> clock:advances
> waitForService 换忙等 -> B returned 1     ★ 根卷找到
> 退休干扰探针         -> launchd            ★ execve 开始
> ```

> ### 今晚的完整链路（从静默到精确失败）
>
> 起点：屏幕一片白字、六行探针不动、完全不知道内核停在哪。
> 终点：`mountroot` 整条路径跑完并给出 panic —— 「明确的失败」比任何静默都有价值。
>
> ```
> Failed to mount "root" device 2f5a?d      ← panic 输出（IOKitBSDInit.cpp）
>
> clock:advances      时基正常
> host:lun            UFS 命令链全通（NOP / 电源查询 / START STOP UNIT / LUN ready）
> ufs:entered         NativeUfs::start 通过三道检查
> prep:attach-done    configure + attach 成功
> keys2               匹配表 = IOMedia + UUID（正确形状）
> media:no-uuid       无签名身份，跳过 UUID 等待
> walk402             200 次轮询 × 2 个服务，正常退出
> guard1              类名匹配（非 exact）
> IOFindBSDRoot entered
> B returned0         200 次遍历，无 IOMedia 带目标 UUID
> ```
>
> ### 逐项修复（按发现顺序）
>
> | 现象 | 真因 | 处理 |
> |---|---|---|
> | 屏幕只有白字，一个探针都不动 | `NativeUfs::start()` 缺 `native-ufs-root=1`，第一行就 return false | ADT `boot-args` 补上，并加 `native-ufs-diagnostics=1` |
> | 所有探针都画在 console 区被白字压住 | 行号 1240–1640 全在 `g_h-780` 以内 | 全部搬到 1780 以下，间距 ≥ 80px（每行清 61px） |
> | 卡在 `host:nop` 一分钟 | `IODelay()` 内部等 `mach_absolute_time()`，而命令确实没完成 | 换 timer-free `yield` 自旋 + 轮询计数 |
> | 有超时代码却从不触发 | 同上，`expired()` 依赖同一把钟 | `pollUfsTransfer` 加 100 万次上限 |
> | 块设备不发布 | `validate()` 在非 verified 构建里无条件 `return false` | bring-up 期间打开（原文注释：Root publication requires owner verification in every build）|
> | `B returned` 永不出现 | `waitForService` 睡线程等中断唤醒，而睡眠在此阶段不工作 | 换成忙等轮询 `getMatchingServices` |
> | 轮询不退出（walk 涨到 4 万） | `mach_absolute_time()` 单位不是纳秒，`+5e9` 等于几千秒 | 改成 200 次尝试封顶 |
>
> ### 剩下的唯一问题
>
> **`IOStorageFamily` 没有把块设备变成可挂载的卷。** 请求正确（`keys2`）、遍历跑满（`walk402`）、
> 结果为空（`B returned 0`），于是 `mountAttempts` 累加到 10 后 panic。
>
> 最可能的原因：缺少配对的 **`IOBlockStorageDriver`**。`NativeUfsBlock` 只实现了
> `IOBlockStorageDevice` 接口，而没有驱动去枚举它上面的介质并解析 APFS 容器头 —— 所以永远不会出现
> 带 UUID 的 `IOMedia`。
>
> 下一步的判据（一次启动即可区分）：枚举所有 `IOMedia` 并打印其 `UUID` 属性
>
> | 结果 | 结论 |
> |---|---|
> | 一个 IOMedia 都没有 | 缺 `IOBlockStorageDriver`（最可能）|
> | 有 IOMedia 但 UUID 不同 | 把实际 UUID 写进 `[payload.adt]` 的 `root_match_volume_uuid` |
> | 有 IOMedia 且 UUID 正好匹配 | 匹配逻辑本身有 bug |
>
> ### 两个验证工具的坑（各踩两次）
>
> 1. `strings` 默认只输出 ≥4 字符，2 字符的探针标签（`tA`/`tD`）看起来「不在内核里」。**必须 `strings -n 2`。**
> 2. xnu 的 C++ 目标是 `.cpo` 不是 `.o`；只删 `.o` 会导致改了源码不重编。

> ### 里程碑：UFS 命令链全通（host:lun）
>
> ```
> host:lun            ← NOP OUT → 电源查询 → START STOP UNIT 唤醒 → LUN ready 轮询
> ufs:entered
> prep:attach-done
> start:ok
> media:no-uuid
> walk5
> guard2
> IOFindBSDRoot entered
> A call wfs
> ```
>
> 之前一直停在 `host:nop`，看起来像「设备不响应 NOP」。换成 timer-free 自旋后整条命令链一次走通，
> 说明设备一直响应良好 —— 卡住的是 `IODelay` 本身。
>
> 至此 UFS 侧完整可用：控制器接管 → 命令通路 → 窗口发布（walk5）→ `start:ok`。
>
> ### 仍在确认的一件事：时基是否前进
>
> `tA = 3967735401` 证明 `mach_absolute_time()` 返回非零，但差值那行始终空白 —— 改成字面字符串
> （`clock:advances` / `clock:STUCK`）以排除数字渲染路径的影响。
>
> 这个答案决定下一步方向：
>
> | 结果 | 含义 | 下一步 |
> |---|---|---|
> | `clock:advances` | 时基正常 | `waitForService` 卡在中断/调度器没有推进，去查计时器中断与 `gPicBase` 接线 |
> | `clock:STUCK` | 时基冻结 | `CNTPCT`/`CNTFRQ` 未编程；修好它可一次性解决 `IOSleep`/`IODelay`/`inittodr`/所有 IOKit 超时 |
>
> ### 验证工具的坑（今晚踩了两次）
>
> `strings` 默认只输出 ≥4 字符的字符串。探针标签 `tA` / `tD` 只有 2 个字符，于是「内核里没有探针」
> 的错误结论出现两次，白跑了两轮构建。**查 2 字符探针必须用 `strings -n 2`。**
>
> 另外 `nabu_mark_text` 每行清 61px，标记行距必须 ≥ 61px（现用 80px），否则相邻标记互相擦除 ——
> 这也是 `tD` 一开始看不见的原因之一。

> ### 里程碑：块设备完整发布（walk 3 → 5）
>
> ```
> ufs:entered
> prep:attach-done     ← configure + attach 全部通过
> start:ok             ← registerService() 被调用
> media:no-uuid        ← 无签名身份时跳过 UUID 等待
> keys1
> walk5                ← 1 个 IODTNVRAM + 2 个 provider + 2 个 IOMedia
> guard2
> IOFindBSDRoot entered
> A call wfs
> ```
>
> `NativeUfsBlock` 的完整生命周期第一次走通：分配 → init → configure（窗口几何校验）→ attach →
> `start()` → `registerService()`。两个窗口（OS / SystemOS）都注册成功。
>
> ### 决定性的修复：`IODelay` 死等
>
> 在此之前，所有「卡住」其实卡在等待函数本身，而不是等待的条件：
>
> ```c
> void delay ()   { IODelay (2); }                            // 内部 spin 等 mach_absolute_time 前进
> bool expired () { return mach_absolute_time () >= deadline; } // 同一把钟
> ```
>
> 在这个启动阶段 `mach_absolute_time()` 不前进，于是 `IODelay` 第一次调用就永不返回。这解释了三个
> 现象同时出现：有超时代码却卡满一分钟、`rootDiagnosticFailure("NOP OUT")` 从不被调用（1810 空白）、
> 连 `native-ufs-diagnostics=1` 的 panic 都发不出来。后来加的 100 万次轮询上限也没用——循环体第一步
> 就没返回。
>
> 现在命令路径上没有任何计时器依赖：
>
> | 位置 | 原实现 | 现在 |
> |---|---|---|
> | `HardwareTransferPort::delay` | `IODelay(2)` | 200 次 `yield` 自旋 |
> | `hostWaitRegister` 节拍 | `IODelay(10)` | 400 次 `yield` 自旋 |
> | `hostWaitRegister` 循环 | `while (mach_absolute_time() < deadline)` | `for (round < 20000)` |
> | `pollUfsTransfer` 循环 | 仅 `expired()` | `+ polls >= 1000000` |
>
> 注意 `-Werror,-Wdeprecated-volatile`：自旋计数器不能带 `volatile` 限定符（`++` 对 volatile 已弃用），
> 循环体里的 `asm volatile` 足以阻止优化。
>
> ### 现在只剩一个问题：UUID 不匹配
>
> `A call wfs` 之后仍无 `B returned`，即 `waitForService(IOMedia + UUID=04769D26-…)` 未返回。
> 块设备已经存在（walk5），所以问题收窄为：**发布的 IOMedia 带不带那个 UUID**。
>
> UUID 由 IOStorageFamily 从 APFS 容器头读出，不是窗口自带的属性。下一步要确认的是窗口几何与
> `userdata-root.img` 的实际布局是否一致（`offset=1048576` / `size=6077546496`）。

> ### 里程碑：根窗口终于发布了（walk 1 → 3）
>
> ```
> keys1
> walk3        ← instanceMatch 现在遍历 3 个服务：IODTNVRAM 节点 + OS 窗口 + SystemOS 窗口
> guard2
> IOFindBSDRoot entered
> A call wfs
> ```
>
> 之前所有版本都是 `walk1`。`walk3` 说明两个 `NativeUfsBlock` 已经通过 `registerService()` 发布，
> 也就是 `IOStorageFamily` 有了可枚举的块设备。
>
> 三道门依次打开才走到这里，全部记录在案：
>
> | 门 | 位置 | 处理 |
> |---|---|---|
> | SMMU 写后回读不符 | `SmmuTakeover.h` `configureUfsSmmu()` | bring-up 期间 bypass（写仍然发生，只是不以回读定成败）|
> | `start()` 的三个前提 | `NativeUfs.cpp` `NativeUfsBlock::start()` | 每道门自报名字（`start:no-bounce` / `no-lock` / `base-false` / `ok`）|
> | **根窗口发布门禁** | `NativeUfs.cpp` `validate()` 的 `#else` 分支 | **`return false` → 打开。原文注释：Root publication requires owner verification in every build.** |
>
> **`validate()` 那道是设计上的硬门禁**：未启用 `NATIVE_VERIFIED_BLOCK` 的构建里它无条件拒绝发布，
> 这正是 defconfig 注释里那条「冷构建满足不了的门链」。bring-up 期间以未认证窗口发布，读盘尚未可信。
>
> ### 现在的状态与下一步
>
> 块设备有了，但 `B returned` 仍未出现，`A call wfs` 之后没有下文 —— `waitForService(IOMedia+UUID)`
> 还没返回。要么是 5 秒超时尚未走到，要么是遍历持有锁。
>
> 下一步只需回答一个问题：**发布的这两个 IOMedia 带不带 root volume 的 UUID**。
>
> ```
> 期望：UUID = 04769D26-2CA0-4825-B443-E2E5F88300F2（[payload.adt] root_match_volume_uuid）
> 实际：窗口只是 UFS 上的字节区间，UUID 要由 IOStorageFamily 从 APFS 容器头里读出来
> ```
>
> 若 UUID 不匹配，就要确认 `userdata-root.img` 的 OS 窗口偏移（`offset=1048576`、
> `size=6077546496`，见 `userdata-root.json`）与驱动 `configure()` 收到的窗口几何是否一致。

