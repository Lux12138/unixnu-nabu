# UniXNU on Xiaomi Pad 5 (nabu / SM8150)

在 **小米平板 5（nabu，骁龙 860 / SM8150）** 上通过 UEFI 启动 **苹果 XNU 内核** 的完整移植记录。

> **本仓库记录的是「推倒重来」之前的那条线。** 当前实际状态、以及旧文档里已经失效的说法，
> 见 [`CURRENT-STATE.md`](CURRENT-STATE.md)；与旧文档冲突时**以它为准**。

> **本机是扩容版**：物理 DRAM **12 GiB**（TWRP 实测 `MemTotal = 11766512 kB`，
> 即 12 GiB 减去约 816 MB 给 modem/hyp/rmtfs），userdata **约 468 GiB**
> （`fastboot partition-size:userdata = 0x74167BB000`）。
> 所以 `dram_size = 0x300000000` 是**为这台机器测出来的**，不是标准 6/8 GB 机型的数字。

> **状态（2026-10-28，以此为准）**：**用户态已经跑起来了。**
>
> - 根卷 + launchd 已通过：完整启动日志 **146 KB / 2,032 行**（与自动复位同一轮取得）；
> - runningboardd / backboardd / SpringBoard 都在运行，各自在自己的 mqueue 上等消息
>   （`[hb] tag=chk n=68 sb=1 bb=1 rb=1 cfg=1`），keybagd 走
>   `DEVICE HAS EPHEMERAL DATA VOLUME, SKIP INIT`；
> - 显示：`[nm] mask=7 size=1336 init=1`（三个抽象基类**名字**不可实例化，
>   `IOMobileFramebufferUserClient` 可分配且 init 成功）；运行期唯一显示相关失败是
>   `[ex] pid=26 comm=IOMFB_FDR_Loader rv=0x100 sig=0 code=1`。**缺的是一个 provider 实例**；
> - 每轮已无人值守：86~129 s 跑完一轮、零按键（CDC 控制通道 + WDT 定时停车的细节见
>   公开仓库 [`nabu-usb-cdc`](https://github.com/Lux12138/nabu-usb-cdc) 的 `docs/07`）。
>
> **历史状态（2026-10-25，已被上面取代）**：内核执行到 `execve("/sbin/launchd")`，
> `Process 1 exec of /sbin/launchd failed, errno 2`；当时缺的是装了系统文件的根文件系统。

---

## 一、里程碑：内核走到了 launchd

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

工具链来自 [`Allenwdk/xnu-linux-toolchain`](https://github.com/Allenwdk/xnu-linux-toolchain)。

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

## 六、下一步（2026-10-28 更新）

**唯一硬阻塞：给 `IOMobileFramebuffer` 提供一个 provider 实例。**

其余已全部在板上跑通（见文首状态）。当前三条待办，按判别力排序：

1. **把 `mask=7` 的歧义定死**：`allocClassWithName` 对“未注册”和“抽象类”都返回 NULL
   （抽象类生成的 `alloc()` 恒返回 NULL），单靠 mask 分不开；改用
   `getMetaClassWithName()` 对三个名字逐个判非空。
2. **provider 实例从哪来**：自写 `IOMobileFramebuffer` 子类已在链接期被否证
   （缺 `gMetaClass` / ctor / dtor——基类在 prelink 的 kext 里，不参与内核链接）。
   社区路线是原版 `AppleCLCD` + 自造 `/arm-io/disp0` 节点。
3. **把“咬合 = 武装后约 32 s”从推断变成实测**：把 `--capture` 放长到覆盖复位点，
   用真实的 `USB LOST at` 时间戳定。

> **历史（2026-10-25 的下一步，已取代）**：把解出来的根做成一个能挂的卷
> （`./unixnu build userdata` + `fastboot flash userdata`）。现在根卷走 APFS +
> **nabuvar**（内存卷挂 `/private/var`），不再需要刷 userdata。

---

## 七、参考

- [`Allenwdk/xnu-linux-toolchain`](https://github.com/Allenwdk/xnu-linux-toolchain) —— Linux 交叉工具链
- [`1PJL/unixnu-nabu`](https://github.com/1PJL/unixnu-nabu) —— UFS 卷诊断系列（`Recover GPT` / `Build Volume` / `Repack` 三步，跑在 `ubuntu-24.04` 上）
- `UniXNU` 上游 —— 内核移植与 UEFI 启动链
