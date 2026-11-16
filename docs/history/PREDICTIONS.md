# 预判清单（更新于 2026-10-22 21:58）

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

按 `bsd_init.c` / `bsdinit_task()` 的执行顺序排列。撞上时直接对号。

| # | 卡点 | 触发条件 | 状态 |
|---|---|---|---|
| 1 | `panic("rootvp not authenticated after mounting")` | 非 macOS 强制 `require_rootauth = TRUE`；mockfs/ramdisk 根没有 sealed 签名 | ✅ 已提前修（`zzzz-nabu-unsealed-root.patch`）|
| 2 | `cannot mount root, errno = N` | 没有可用根设备（NativeUfs 已关，无 ramdisk）| **待撞，第一硬骨头** |
| 3 | `imageboot_setup` | `CONFIG_IMAGEBOOT` 找系统磁盘镜像 | 待观察 |
| 4 | `devfs_kernel_mount("/dev")` | devfs 未初始化 | 待观察 |
| 5 | `load_init_program` 找不到 init | 见下 | **第二硬骨头** |
| 6 | 8 核 loopattach 竞态 | 真机多核；QEMU `-smp 1` 规避 | 已知 |

## 关卡 5 的细节（已确认）

`kern_exec.c`：

```c
static const char * init_programs[] = ('/usr/appleinternal/sbin/launchd.debug', '/usr/appleinternal/sbin/launchd.development', '/sbin/launchd');
```

我们是 DEVELOPMENT 构建，所以顺序是
`launchd.development` → `/sbin/launchd`。
`appleinternal` 不会有，**必须靠根文件系统里的 `/sbin/launchd`**。

**这意味着：内核能推到这里之后，再往前完全取决于根文件系统。**
不是内核逻辑问题，是「iOS 用户态装没装进去」的问题。

## 已知的正确做法（避免重复踩）

- **boot-args 走 `unixnu.toml` 的 `[[entry]] args`**，不是 ADT、不是 `make-nabu-adt.py`
- **kprintf 要两个开关同时清**：`disable_serial_output` 和 `disable_kprintf_output`
  （门是 `if (!disable_serial_output && !disable_kprintf_output)`）
- **`PE_kputc` 要无条件赋值**：`PE_init_kprintf` 跑在 `STARTUP(KPRINTF)`，早于 fb 映射
- **新插桩一律用 patch 文件**，注入脚本已静默失效 5 次
- **`extern "C"` 保护**头文件（C++ 文件 include）
- **相对 include**：`osfmk/` 之外一律 `"../../osfmk/arm/nabu_early_mark.h"`
- **探针编号上限**已扩到 128（原来 64 会静默丢弃 65+）

## QEMU 侧已验证的事实

- 内核 entry 能跑（顶部绿条）
- 异常 trace 只有 IRQ，**无 abort** → 内核在 QEMU 里是活的
- `disable_serial_output=0 / disable_kprintf_output=0 / nabu_mark_putc` 都在二进制里验证过
- 串口仍无文本 → 正在用「固定 UART tag」区分「UART 写不通」和「kprintf 没走到」
- **QEMU 复现不了 IOSurface 卡死**（那是 IOKit 打真实硬件）


## 真机 IOSurface 卡点的本质（重要）

ios-nabu 的 `0046-nabu-iosurface-iomfb.patch` 揭示了原因：他们**没有用 Apple 闭源的
IOSurface kext**，而是自己实现了：

    iokit/Kernel/arm/NabuSurface.cpp       -> IOCoreSurfaceRoot
    iokit/Kernel/arm/NabuFramebuffer.cpp   -> AppleCLCD

并在 `iokit/KernelConfigTables.cpp` 注册这两个类，另外在 `NabuPlatformExpert` 里给
`arm-io` 子节点建 nub，让内建驱动能匹配。

**结论：`IOsurfaceRoot:installMemoryRegions()` 之后卡住，不是断言、不是内存不足，
而是闭源 IOSurface kext 在非 Apple 平台上跑不通。** 越过它需要重写
surface / framebuffer 驱动，属于周级工作量，不是一两轮 patch 能过的。

这也解释了为什么 ios-nabu 有 83 个补丁、做了几个月才到 SpringBoard。
