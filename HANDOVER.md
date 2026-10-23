# nabu XNU 移植：交接文档

**交接时间**：2026-10-23
**交接原因**：新窗口接手，单线推进
**当前状态**：可复现基线，checkpoint 123

> 早期路径（探针注入方案、`boot_args.Video` 的发现、MARK 方案的来历）记录在
> **`HANDOVER-legacy.md`**。本文是入口。

---

## 一、三十秒了解现状

**能做什么**：CI 构建内核（13 分钟）→ 打包 payload → 刷机 → 真机跑到
`IOsurfaceRoot::installMemoryRegions()` 停住，屏幕显示 checkpoint `123` 和 16 个彩色方块。

**不能做什么**：根文件系统没挂上，`launchd` 起不来，没有画面。

**下一步**：先确认那个「停住」是真死路还是只是日志中断（第五节）。

---

## 二、必须知道的六条硬事实

### 1. 内核日志的两个开关都要关

```c
// pe_kprintf.c
SECURITY_READ_ONLY_LATE(bool) disable_serial_output  = false;  // 必改
SECURITY_READ_ONLY_LATE(bool) disable_kprintf_output = false;  // 必改
...
if (!disable_serial_output && !disable_kprintf_output) { ... }
```

**两个默认都是 `true`**，只改一个不会有任何输出。

### 2. `PE_kputc` 必须无条件赋值

`PE_init_kprintf` 跑在 `STARTUP(KPRINTF)`，**早于** framebuffer 映射。所以不能写
`if (nabu_console_fb_ready()) PE_kputc = nabu_mark_putc;` —— 条件永远为假，hook 永不安装。

### 3. `0x9000000` 的 UART 映射只能在 QEMU

这是 **checkpoint 50 的真凶**。`0x9000000` 在 QEMU virt 是 PL011，在 SM8150 上是别的寄存器。
`ml_io_map_unmappable` 在两边**都返回成功**，第一个字符写进真实硬件就崩。

现方案：按面板尺寸判断（QEMU 800x600 / nabu 1600x2560），只在 `w <= 1024` 时映射。

### 4. ADT 里必须有 `RAMDisk` 节点

否则 DarwinBoot 报 `boot refused: Compromised Data`：

```
/chosen/memory-map/RAMDisk   16 bytes，两半全 0
```

`make-nabu-adt.py --data-ramdisk` 会生成。`board.toml` 的 `[payload.adt]` **只有
`packed_data_ramdisk`，没有普通 `data_ramdisk`** —— 走官方流程时这是个缺口。

### 5. Xcode 26.0.1 的新警告要在三处抑制

`-Wunnecessary-virtual-specifier` 在 `-Weverything -Werror` 下变错误，`OSSymbol.h` 触发。
三个独立位置各有自己的 flag 列表：

| 位置 | 处理 |
|---|---|
| `makedefs/MakeInc.def` 的 `WARNFLAGS_STD` | `build-xnu-4k-baseline.sh` 直接注入 |
| `installhdrs` 的 make 调用 | 传 `CFLAGS_EXTRA` |
| `build-arm-driver.py` / `audit-storage-interface.py` | 各自补 `-Wno-*` |

### 6. `kernel.py` 冷构建跑两遍

第一遍只生成 compile record，**故意清空** 5 个变量。任何「合格基线」gate 都会在第一遍拦截。
现方案：`build-xnu-4k-baseline.sh` 开头检测 `startup.o.json` 不存在就整体降级。

---

## 三、可复现基线

| 项 | 值 |
|---|---|
| 构建 | `39905065571` / 13 分钟 / success |
| 内核 | `kernel.macho` 49.2 MB |
| runner | `macos-26` + Xcode 26.6 |
| defconfig | **9 个开关** |
| `[kernel] archives` | **8 个** |
| prelink roots | 20 个 |
| 分支 | `main`（永远保持可构建可刷机）|

**恢复命令**：
```bash
gh run list --repo Lux12138/unixnu-nabu --limit 1 --workflow build-kernel.yml
gh run download <id> --repo Lux12138/unixnu-nabu -n kernel-nabu -D C:\Temp\nabu-base
# gh run download 常失败，用 API 直连：
#   curl.exe -sL -H "Authorization: token $(gh auth token)" -o out.zip \
#     https://api.github.com/repos/Lux12138/unixnu-nabu/actions/artifacts/<AID>/zip
wsl -e bash -c 'KM=/mnt/c/Temp/nabu-base/kernel/kernel.macho bash /mnt/c/Temp/payload2.sh'
cd C:\platform-tools
.\fastboot.exe flash cust C:\Users\Administrator\Desktop\unixnu-nabu-kernel-ufs\cust-payload.img
.\fastboot.exe boot C:\Users\Administrator\Desktop\unixnu-nabu-kernel-ufs\nabu-xnu64-Z.img
```

---

## 四、今天最大的教训

> **把「启用官方全部特性」当成还债，代价是十几轮构建。**

`CONFIG_XNU_BOOT_PROGRESS` / `NATIVE_UFS` / `NATIVE_AES` / `VIRTIO_BLOCK` / `OWNER_ROOT_AUDIT`
**不是开关**，是一整套互相依赖的合格基线：

- `boot-progress` 实现在 `UniXNU-sheng/kits/sheng/BootProgress.c`（82 KB）+ 3 个探针 ≈ **136 KB**
- 它的 builder（`build-boot-progress.py`）在 **fork 和 upstream 两个 UniXNU 仓库里都不存在**
- 启用后触发 gate 链：AES closure → CPU/pthread → owner crypto → virtio archive，**修一个冒一个**
- `kernel.py` 的 bootstrap pass 故意清空这些变量 → gate 在冷构建时**必然**冲突

**立的规矩**：
1. **bring-up 中途不改 defconfig**
2. **构建链排障超过 2 轮就回退**
3. **`main` 永远可构建**

---

## 五、下一步（按顺序，别跳）

### 第一步：排除「日志中断」的可能（半天）

**现在的假设可能是错的。** 我们一直认为 IOSurface 是死路，但**从没验证内核是否还在跑**。

```c
bsd_autoconf();                              // 当前停在这里面
kprintf("NABU: bsd_autoconf returned\n");    // 加这一行
```

- **有输出** → 内核还活着，IOSurface 只是日志断点 → **直接进第二步**
- **无输出** → 确认是死路 → 走第三步

### 第二步：根文件系统（1–2 周）

依据 `/guide/userdata` + `/concepts/userdata`：

```
1. board.toml 的 [ipsw] fetch 加 "OS" 和 "Cryptex1,SystemOS"   ← 已加，未验证
2. ./unixnu ipsw fetch          # 拉两个组件，几 GB
3. ./unixnu build userdata      # SHNGROOT manifest + 两个窗口
4. [payload.adt] 设 root_match_volume_uuid = <OS 卷的 UUID>
5. fastboot flash userdata out/nabu/userdata/userdata-root.img
6. 观察是否挂上、launchd 是否起来
```

**已知**：`[userdata]` 的 LBA 早实测好（`first_lba = 0x288C40` /
`last_lba = 0x77BF3FA` / `lba_bytes = 4096`）。

### 第三步：显示（3–4 周，只在第二步之后）

照 `UniXNU-sheng`：自研 `IOCoreSurfaceRoot` + `AppleCLCD`，注册进
`iokit/KernelConfigTables.cpp`，并在 platform expert 里给 `arm-io` 子节点建 nub。

**不要先做这一步** —— 它解决「看不看得见」，不是「起不起得来」。

---

## 六、参考物

| 参照 | 用途 |
|---|---|
| **`UniXNU-sheng`** | **唯一正确的参照**：roots / archives / defconfig 完整例子 |
| `UniXNU/UniXNU` | 框架本体；我们的 fork 落后 163 commit，且不能直接换（`-mach_msg2`）|
| `glebenekt/ios-nabu` | 同设备、iPadOS 16.6.1、83 补丁到 SpringBoard |
| `unixnu.com` | 官方文档 |

**文档的两个误导点**（踩过）：
1. `/guide/debugging` 把 `boot-progress` 写成通用功能，实为 sheng 专属 136 KB
2. `macos-26` 镜像上**没有** Xcode 26.6，只有 26.0 / 26.0.1

---

## 七、工具清单

| 工具 | 用途 |
|---|---|
| `C:\Temp\auto\watch.ps1` | 一分钟轮询构建 |
| `C:\Temp\payload2.sh` | 打包 payload（**注意**：手动调 `make-nabu-adt.py`，绕过官方 `build payload`）|
| `C:\Temp\auto\shot.ps1` | QEMU 启动 + 截图 |
| `C:\Temp\findsym.py` | 内核符号反查 |

**QEMU 参数（缺一个都不行）**：
```
-M virt -cpu max -m 8192 -smp 1
-drive if=pflash,...,file=edk2-aarch64-code.fd
-drive if=none,id=esp,...,file=fat:<ESP目录>
-device virtio-blk-pci,drive=esp -device ramfb
-display none -serial file:<log>
```
`-cpu max`（cortex-a72 不认 `MSR S3_0_C4_C1_4`）；`-m 8192`（4096 时 DarwinBoot 报
`Out of Resources`）。

---

## 八、文件地图

| 文件 | 作用 |
|---|---|
| `board.toml` | 板级配置：prelink roots / archives / userdata / payload |
| `configs/nabu_defconfig` | 9 个开关（**别乱加**）|
| `patches/xnu/*.patch` | 内核补丁，按名字排序应用 |
| `.github/workflows/build-kernel.yml` | CI（17 步，含 `build payload`）|
| `STATUS.md` | 当前状态快照 |
| `FLOW.md` | 流程图（七阶段 + 两条路线）|
| `HANDOVER-legacy.md` | 早期路径（探针方案来历）|

**外部依赖**：
- `C:\Users\Administrator\Documents\UniXNU` — `UniXNU` 仓库本地 clone，**CI 实际用的是它 push 到 fork 的版本**（这点很重要：改本地不 push，CI 看不到）
- `C:\Users\Administrator\Desktop\unixnu-nabu-kernel-ufs` — payload 与 boot image 工作目录

---

## 九、新窗口的第一条命令

```powershell
cd C:\Users\Administrator\Documents\unixnu-nabu-gh
git log --oneline -10
Get-Content STATUS.md | Select-Object -First 60
gh run list --repo Lux12138/unixnu-nabu --limit 3 --workflow build-kernel.yml
```

**然后从第五节第一步开始。**
