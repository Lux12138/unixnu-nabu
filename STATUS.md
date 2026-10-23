# 状态快照（2026-10-23 13:43）

## 可复现的基线

**构建**：`39905065571` / 13 分钟 / `success`
**内核**：`kernel.macho` 49.2 MB
**真机**：checkpoint **123**，16 个彩色方块，完整内核日志，停在 `IOSurface`

```
corecrypto_kext_start completed successfully
ulock_initialize>thread_max=5120, ull_hash_buckets=2048
[0]ml_processor_register>pe:find_cpu(clusid=0) returned pset 0
SCHED: running single cpu -- this is not ideal for performance, fix the platform
SKYWALK: sk_copy64_8 ... PASSED   （32 项全过）
bsd_autoconf: calling kminit
IOsurfaceRoot::installMemoryRegions()
IOSurface disallowing global lookups        <- 停在这
```

## 这套基线由什么构成

| 项 | 值 |
|---|---|
| xnu 源 | `xnu-10063.141.1`（与官方 feeds 完全一致）|
| runner | `macos-26` + Xcode 26.6（与官方一致）|
| 构建流水线 | **官方 CLI**：`defconfig → fetch → tree → ipsw fetch → build kernel → build payload` |
| defconfig 开关 | **9 个**（最小集）|
| `[kernel] archives` | **8 个** |
| prelink roots | 20 个 |

**关键修复（今天）**
1. `kprintf` 的两个开关都要关（`disable_serial_output` + `disable_kprintf_output`）
2. `PE_kputc` 必须无条件赋值（`PE_init_kprintf` 早于 fb 映射）
3. `0x9000000` 的 UART 映射**只能在 QEMU**（真机上是别的寄存器，写入即崩 —— 这就是 checkpoint 50 的真凶）
4. `RAMDisk` 节点必须在 ADT 里（否则 `boot refused: Compromised Data`）
5. Xcode 26.0.1 的 `-Wunnecessary-virtual-specifier` 在三处 flag 列表里都要抑制
6. `installhdrs` 与 kernel build 的 `OBJROOT` 不同，两条路径都要管

## 今天走错的弯路（记下来避免重犯）

**把「启用官方全部特性」当成还债，代价是十几轮构建。**

`CONFIG_XNU_BOOT_PROGRESS` / `NATIVE_UFS` / `NATIVE_AES` / `VIRTIO_BLOCK` / `OWNER_ROOT_AUDIT`
不是开关，是一整套互相依赖的合格基线：

- `boot-progress` 的实现在 `kits/sheng/BootProgress.c`（82 KB）+ 3 个探针 ≈ **136 KB**，
  builder（`build-boot-progress.py`）**两个 UniXNU 仓库都没有**
- 启用后会触发 gate 链：AES closure → CPU/pthread → owner crypto → virtio archive，
  每修一个冒出下一个
- `kernel.py` 的 bootstrap pass 故意清空这些变量，所以 gate 在冷构建时**必然**冲突

**规则**：bring-up 中途不改 defconfig；构建链排障超过 2 轮就回退。

## 官方文档参考的实效

| 文档 | 落地成果 |
|---|---|
| `/reference/cli` | 用官方 `build payload` 取代手搓 payload |
| `/xnu/boot-arguments` | 补上 `kextlog=0xfff` / `kt_fixed_zones=120` / `-no_slto_panic` / `amfi_get_out_of_my_way=1` |
| **`/porting/device-tree`** | **`RAMDisk` 占位符 —— 直接解决 `Compromised Data`** |
| `/concepts/userdata` | `[userdata]` 窗口与 `root_match_volume_uuid` 的契约 |
| `/xnu/source-and-patches` | 确认 feeds 与官方一致，排除了版本差异的怀疑 |
| 官方 `macos.yml` | 发现我们落后两个镜像（`macos-14` → `macos-26`）|
| **`UniXNU-sheng` 仓库** | roots / archives / defconfig 的真正参照物 |

**文档的两个误导点**：
1. `boot-progress` 写在 `/guide/debugging` 里像通用功能，实为 sheng 专属
2. `macos-26` 镜像上并没有 Xcode 26.6，只有 26.0/26.0.1

## 下一步：两条路，选一条做到底

| 选项 | 内容 | 周期 | 依据 |
|---|---|---|---|
| **A. 冲根文件系统**（推荐）| `unixnu build userdata` + `root_match_volume_uuid`，让根卷挂上、`launchd` 起来 | 1–2 周 | 官方文档完整、sheng 已验证、我们 `[userdata]` 的 LBA 早实测好 |
| B. 冲 IOSurface | 自研 `IOCoreSurfaceRoot` + `AppleCLCD`（照 sheng 的路子）| 3–4 周 | 板级重写 |

**推荐 A 的理由**：根文件系统是通用路径，IOSurface 是板级重写。先用通用路径把用户态拉起来，
再谈显示 —— 而且 A 做完之后，IOSurface 卡住的意义会完全不同（那时它是「看不到画面」，
不是「起不来系统」）。
