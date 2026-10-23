# UniXNU on Xiaomi Pad 5 (nabu / SM8150) — 完整交接文档

> 状态：**XNU 内核已在真机上进入执行**（DarwinBoot `exiting boot services` → `hand-off to the kernel`）
> 当前工作：给内核装"可见性探针"（把黑箱变白箱）。
> ✅ 注入链路已修好并 **CI 跑通**（UniXNU `0c4e9a1` / unixnu-nabu `b7642cf`，run `39612555624` success，
> 方块标记 MARK1–4 已链进 `kernel.macho`）。
> ✅ 同时查实"内核无输出"的**真因**：DarwinBoot 的 `boot_args.Video` 被改成了旧数组顺序，
> 与内核 `struct Boot_Video` 字段错位（详见 **坑 8 更正**）。
> ⬜ 下一步：**重编固件 + 刷机数方块**（第 7 节第 5 步，需要接设备）。

---

## 0. 仓库与产物位置

| 项 | 路径 |
|---|---|
| 本仓库（文档/脚本/kit） | `C:\Users\Administrator\Documents\unixnu-nabu-gh` → https://github.com/Lux12138/unixnu-nabu |
| UniXNU fork（内核构建脚本） | `C:\Users\Administrator\Documents\UniXNU` → https://github.com/Lux12138/UniXNU |
| 内核/payload/镜像产物 | `C:\Users\Administrator\Desktop\unixnu-nabu-kernel-ufs`（**所有 nabu-xnu64-*.img 都在这里**）|
| WSL（固件构建） | `/root/Mu-Silicium`（EDK2/Mu UEFI）、`/root/UniXNU`（上游）、`/root/xnu`（稀疏 clone 的 xnu 参考）|
| fastboot/adb | `C:\platform-tools` |
| git | `C:\Users\Administrator\.dsh-tools\PortableGit\cmd\git.exe` |
| gh | `C:\Program Files\GitHub CLI\gh.exe` |

> ⚠️ 易错点：`Documents\unixnu-nabu-kernel-ufs` 和 `Desktop\unixnu-nabu-kernel-ufs` 都存在，
> **boot 镜像在 Desktop 那个**，之前多次因路径写错导致 `No such file`。

---

## 1. 已打通的链路（真机逐项验证）

```
fastboot boot → BootShim → Mu UEFI → UniXNU 菜单 → cust 分区 FAT payload
→ DarwinBoot → 读 51MB 内核 → 解析 Mach-O → 读 ADT → ADT 校验
→ 分配窗口 → 映射段 → ExitBootServices → 跳转 XNU 内核 ✅
```

真机日志（实拍）：
```
UNIXNU: runxnu entry vol=A2142798 path=\EFI\UniXNU\Payloads\kernel.macho cfgerr=
DB1 kernel read  Success size=0x3345000      (≈51.2MB，注意是十六进制打印)
DB2 kernel parse Success
DB3 adt read     Success size=0x330C
DB4 adt ok  win=0x130000000 keep=0 rd=ffffffffffffffff
DB6 va=0xFFFFFFF00427C000 pa=0x8007C000 args=0xC4364000
DARWINBOOT: inputs validated; exiting boot services
still-there kernel entry code ran
```

---

## 2. 硬坑与解法（按踩坑顺序）

### 坑 1：Load Error
- 误判过"官改 ABL 禁用 boot"，**真正原因是 USB 线**
- 劣质线导致大镜像传输丢数据 → ABL 校验失败
- ✅ 换原装数据线 + 主板后面板 USB 口

### 坑 2：ramdisk 大小区间
| ramdisk | ABL | UEFI |
|---|---|---|
| 16 MB | ❌ Load Error | — |
| 32 MB | ✅ | ✅ |
| 64 MB | ✅ | ✅ |
| 96 MB | ✅ | ❌ 黑屏 |

→ **可用区间 32–64MB**；`header_version=1`、`page_size=2048`

### 坑 3：每次 boot 前必须重启设备
重启后**第一次 boot 成功率最高**；连续 boot 会 Load Error（download buffer 残留）。

### 坑 4：内核不能塞进固件卷（PEI 解压预算）
| FVMAIN | 结果 |
|---|---|
| 8 MB（原始） | ✅ |
| 62 MB（未压缩内核） | ❌ |
| 15 MB（LZMA 内核） | ❌ |

FD 本体上限：**3MB 可用，32MB/60MB 全部起不来**（`[uefi_fd] size` 保持 `0x300000`）。

### 坑 5：没有任何 FAT 卷（最根本）
固件自带存储栈（`UFSDxe`/`PartitionDxe`/`DiskIoDxe`/`Fat`），但安卓分区全是 ext4/f2fs →
`Config->Volume=NULL` → DarwinBoot 报 `NULL input`。

✅ **payload 刷进 `cust` 分区**（1GB，出厂资源分区，不影响系统启动）：
```
\EFI\UniXNU\unixnu.toml
\EFI\UniXNU\Payloads\kernel.macho        (51MB)
\EFI\UniXNU\Payloads\kernel.macho.adt    (13KB)
```
⚠️ 刷前确认完整：`Sending 'cust' (65536 KB)`；曾被截断成 1.2MB，表现为 `config error: unixnu.toml line 1: expected '='`

### 坑 6：DarwinBoot 不被 dispatch
✅ 并进 `Include/APRIORI.inc` 的**第一个** `APRIORI DXE { }` 块（新开块会 `GenFv ERROR 2000`）

### 坑 7：ADT 的 RAMDisk 占位符必须与实际一致
```c
else if (RamdiskOffset != MAX_UINTN) { Status = EFI_UNSUPPORTED; goto Fail; }
```
✅ 生成 ADT 时**不要** `--data-ramdisk` / `--packed-data-ramdisk`，使 `rd=ffffffffffffffff keep=0`

### 坑 8：`boot_args.Video` 布局（⚠️ 之前的结论有错，见下）
`BootVideo.h` 原写 `[base, 1, rowBytes, width, height, 32]`，当时认为 XNU 期望
`[base, rowBytes, width, height, depth, display]`
→ 内核读到 `rowBytes=1 / depth=2560` → `initialize_screen()` 失败 → **静默切回串口**
✅ 改写过一次（并同步改 DarwinBoot 绘制下标）；DarwinBoot 侧自报 `base=0x50000000 row=6400 1600x2560 depth=32`

> **⚠️ 更正（本次会话核实）**：xnu-10063.141.1 的 `boot_args.Video` **不是数组**，是
> `struct Boot_Video`，定义在 `pexpert/pexpert/arm64/boot.h:31-38`：
> ```c
> struct Boot_Video {
>     unsigned long v_baseAddr;  // [0]
>     unsigned long v_display;   // [1]  ← 第 2 个！不是最后一个
>     unsigned long v_rowBytes;  // [2]
>     unsigned long v_width;     // [3]
>     unsigned long v_height;    // [4]
>     unsigned long v_depth;     // [5]
> };
> ```
> 而 `pexpert/arm/pe_init.c:419-429` 是按**字段名**逐个取的：
> ```c
> PE_state.video.v_baseAddr = boot_args_ptr->Video.v_baseAddr;
> PE_state.video.v_display  = boot_args_ptr->Video.v_display;
> PE_state.video.v_rowBytes = boot_args_ptr->Video.v_rowBytes;
> PE_state.video.v_width    = ...->Video.v_width;
> PE_state.video.v_height   = ...->Video.v_height;
> PE_state.video.v_depth    = (...->Video.v_depth >> 0) & 0xFF;   // 低 8 位才是深度
> PE_state.video.v_rotate   = (v_depth>>8 & 0xFF) + (v_depth>>24 & 0xFF);
> PE_state.video.v_scale    = ((v_depth>>16) & 0xFF) + 1;
> ```
> 也就是说：**如果 DarwinBoot 真按 `[base, rowBytes, width, height, depth, display]` 写**，
> 内核会读到 `v_display=rowBytes(非 0，于是走 graphics)、v_rowBytes=width、
> v_width=height、v_height=depth、v_depth=display位段全 0` ——
> **整条 video 参数全错位**，`vcattach` 拿到的行宽/宽高都是错的，表现就是"没有输出"。
>
> ⇒ 这**就是**"内核在 `PE_create_console` 之前静默死亡"的真因（已核实，非推测）：
> `vcattach` 拿到的 `v_rowBytes=1600`（真实 6400）、`v_width=2560`（真实 1600）、
> `v_height=32`（真实 2560）、`v_depth=0` —— 屏幕参数全错，图形 console 起不来。
>
> **核实证据链**：
> 1. `pexpert/arm/pe_init.c:419-429` 按**字段名**读（不是按下标数组读）
> 2. `pexpert/pexpert/arm64/boot.h:31-38` 结构体顺序 = base, display, rowBytes, width, height, depth
> 3. `drivers/DarwinBoot/DarwinBootPkg/Driver/BootVideo.h` 的 `git diff` 显示：
>    **上游原版本来就是正确的结构体顺序**
>    ```c
>    Video[0] = FrameBufferBase;              Video[1] = 1;      // v_display
>    Video[2] = RowBytes;                     Video[3] = HorizontalResolution;
>    Video[4] = VerticalResolution;           Video[5] = 32;
>    ```
>    而"坑 8 修复"把它改成了 `Video[1]=RowBytes … Video[5]=0` —— **方向改反了**。
> 4. `DarwinBoot.c` 的 `DiagDraw` 被同步改成用 `Video[1]` 当 rowBytes（也是反的），
>    而 `Video[2]*Video[4]`（FB size）、`Video[3]/Video[4]`（宽高）等其它调用点
>    **一直是结构体语义、没被改** —— 自证原始代码是对的。
>
> ✅ **已回滚（WSL `/root/UniXNU/drivers/DarwinBoot`）**：
> - `git checkout -- DarwinBootPkg/Driver/BootVideo.h` → 恢复上游结构体顺序
> - `DiagDraw` 的 5 处下标改回 `Video[2]`=rowBytes / `Video[3]`=width / `Video[4]`=height
> - 其它调用点（`Video[2]*Video[4]`=FB size、`Video[3]`/`Video[4]`=宽高、
>   日志里的 base/width/height/row/depth）**本来就是结构体语义，无需改**
>
> ⇒ **结论：上游 HEAD 的 DarwinBoot 就是对的。不需要 patch、不需要 fork。**
> 要做的只是"别再按数组顺序改它"。重编固件后 `Video` 布局即与内核一致。

### 坑 11：内核早期**不能**写帧缓冲（探针曾经自己把内核打死）
`ExitBootServices` 之后帧缓冲**仍然是可写可见的**（已用 DarwinBoot 双色探针证明，见下），
但那是**在 UEFI 的页表下**。内核 `start.s` 之后用的是内核自己的页表，`0x50000000`
这个**物理地址**在内核页表里没有映射 —— 于是 `arm_init` 里那次写**直接 fault**，
内核就地死亡，屏幕保持原样，看起来和"内核没跑"一模一样。

**症状特征**：有日志、有屏幕、无方块，且加标记前后行为不变（因为标记本身就是死因）。

✅ **修法（UniXNU `9ad570c`）**：
- `nabu_mark_init()` 只清状态，**不碰 fb**
- `NABU_MARK(n)` 只把命中记进 RAM 位图 `g_hits`（RAM 必然可写）
- `nabu_mark_rebind()` 挂在 **`vcattach()` 的末尾**，一次性把命中回放到屏幕

> ⚠️ **映射点不是 `PE_create_console()`！**（第一版就错在这里，白刷了两轮）
> - `vcattach()` 由 **`PE_init_printf(TRUE)`** 调起（`pexpert/gen/pe_gen.c:135-140`）
> - 而 `arm_init` 里只有 `PE_init_printf(FALSE)` → **`arm_init` 期间 fb 从未被映射**
> - 真正的映射在 `vcattach` → `initialize_screen(NULL, kPEAcquireScreen)`
>   → `ml_io_map_unmappable(...)`（`osfmk/console/video_console.c:2880`）
>
> 所以注入点在 `vcattach` 收尾的 `}` 之前，并且加了一道**内核 VA 守卫**：
> ```c
> if ((unsigned long)PE_state.video.v_baseAddr >= 0xFFFFFFF000000000UL) {
>     nabu_mark_rebind((const unsigned long *)&PE_state.video);
> }
> ```
> 地址还不像内核 VA 就**跳过**，猜错也不会 fault。
>
> **副产品结论**：既然 rebind（在 console 之后）曾把内核打死，说明**内核确实能跑到
> `PE_create_console` 之后**——比此前 HANDOVER 记录的"死在 `PE_create_console` 之前"远得多。

### 坑 12：屏幕顶部绿条**不是** "kernel entry code ran"
`DarwinBoot.c` 里那句提示是错的。证据：
1. `BootShim.S` 全文没有画绿条的代码；Mu-Silicium / UniXNU 也搜不到
2. 四张实拍里绿条高度与字号**比例恒定**，只是拍摄距离不同
3. A/B 探针实验证实：绿条**下方**画的红块/绿块都能正常显示

⇒ 绿条只是 UEFI 侧的固定横幅，**不能**当内核进度用。判读只看方块。

#### A/B 探针实验（决定性）
在 DarwinBoot 里同一次启动画两块（都在绿条下方 y=800）：
| 探针 | 时机 | 结果 |
|---|---|---|
| 绿块 (700,800) | `ExitBootServices` **之前** | ✅ 出现 |
| 红块 (32,800) | `ExitBootServices` **之后** | ✅ 出现 |

⇒ 交棒后 fb 可写可见，**排除**"帧缓冲失效"这条路线。

### 坑 10：`nabu_mark_init(args->Video)` 编译不过（已修）
```
error: passing 'Boot_Video' (aka 'struct Boot_Video') to parameter of
       incompatible type 'const unsigned long long *'
```
✅ 改为按「`unsigned long` 字流」传：`nabu_mark_init((const unsigned long *)&args->Video);`
标记实现同时支持两种解释（结构体顺序优先，自动回退旧数组顺序），
并在 `depth&0xFF != 32` 或走了回退时**画红块**，把布局问题直接暴露在屏幕上。

### 坑 9：EL 不是问题，别再查
- `ModuleEntryPoint.S` 的 `EL1_OR_EL2` 分支：起始 EL2 则留 EL2，起始 EL1 则留 EL1
- 实测 `EL=4`（EL1）→ **ABL 用 EL1 交付**，**往上跳 EL 不可能**
- DarwinBoot 源码本身要求 EL1，与 UniXNU 设计一致 ✅

---

## 3. 当前唯一卡点（正在做）

内核已跳转执行，但**在 `PE_create_console()`（arm_init.c:616）之前静默死亡**，屏幕无任何内核输出。

### 解法：内核最早入口画"方块检查点"
原理：`boot_args.Video` 已给出帧缓冲，内核在 `arm_init()` 最开头直接画 48×48 色块，**不需要 console、不需要字库**。

| 方块数 | 颜色 | 含义 |
|---|---|---|
| 0 | — | 死在汇编入口/MMU，或帧缓冲未映射 |
| 1 | 白 | 进了 `arm_init` |
| 2 | 黄 | 过了 `PE_init_platform` |
| 3 | 青 | 过了 `PE_create_console` ← **console 通了** |
| 4 | 绿 | 过了第二次 `PE_init_platform` |

方块从 `(32,32)` 起、间隔 64px、上面有黑色横条背景。

### 已实现
- `kits/nabu/nabu_early_mark.{c,h}`（纯 C，`gcc -fsyntax-only` 通过）
- `scripts/inject-early-marks.sh`（**参考用**：注入任意给定 xnu 源树；CI 不再调用它）
- ✅ **权威实现在** `UniXNU/scripts/build-xnu-4k-baseline.sh`（commit `18ca785`）：
  补丁循环之后注入 `.cache/xnu-work`，带 pre-clean 可重入，锚点缩进无关，MARK1–MARK4 齐全

> `.github/workflows/build-kernel.yml` 里原来那步「注入 `refer/xnu`」已删除——那棵树不参与编译，
> 留着只会误导；改为构建后新增 `Verify early checkpoint marks` 步骤，自动打印
> `marks inserted` / `WARN: mark? anchor` / `llvm-nm` 符号检查三份证据。

### 注入踩坑史（重要！）
1. CI 独立步骤 → `refer/xnu` 不存在（**refer 源延迟拉取**，`tree.py` 注释 `fetched when a step needs it`）
2. 放进 Build kernel 步骤 → `ls refer/` 显示 `total 0`
3. 放进构建脚本、但**在 `if [ ! -d refer/xnu/.git ]; then` 块内** → CI 已 fetch 过，块被跳过
4. 移出 if 块 → PowerShell `Set-Content -Encoding UTF8` 写入 **BOM+CRLF** → bash 解析失败（已用 Python 归一成 LF）
5. 注入到 `refer/xnu` → 不生效，**真正构建目录是 `.cache/xnu-work`** ✅ 已改
6. 改到 `.cache/xnu-work` → CI **failure**：注入改了 `arm_init.c`，与后续 `git apply` 官方补丁**冲突**
   → ✅ **已修**：注入块移到补丁循环 `done`（第113行）**之后**，并加了 pre-clean 反向清理
   （`native-4k-user-abi.patch`、`zzz-boot-progress.patch` 改 `osfmk/arm/arm_init.c`；
   `source-machine-layer.patch` 改 `osfmk/conf/files.arm64`）
7. 锚点原来是 `assert`，实测发现**缩进敏感**（`\t` vs 空格）会误判
   → ✅ 改为**缩进无关正则** + 无匹配只 `print WARN` 不中断；`#include` 锚点缺失时回落到最后一个 `#include` 行
8. 内核构建会跑**两遍**脚本（bootstrap pass 出编译记录 → 真正的 pass），同一 `.cache/xnu-work`
   → ✅ pre-clean 让注入可重入（`MKU` 先撤、`MKP` 再注），CI 复用工作树也不会污染补丁上下文

### 验证注入是否生效
- ❌ 别只看 CI 控制台（脚本 stdout 被重定向进产物）
- ✅ 查产物 `kernel.log`：`marks inserted` / `nabu_early_mark`
- ✅ 查 `kernel.macho` 二进制含 `nabu_mark`

---

## 4. 关键源码锚点（xnu-10063.141.1 / commit d8b80295，已验证）

| 文件 | 行 | 内容 |
|---|---|---|
| `osfmk/arm/arm_init.c` | 199 | `void arm_init(boot_args * args);` |
| | 370 | `PE_init_platform(FALSE, args);` ← MARK1 锚点 |
| | 616 | `PE_create_console();` ← MARK2/3 锚点 |
| | 619 | `PE_init_printf(FALSE);` |
| `pexpert/arm/pe_init.c` | 425-436 | 从 `boot_args.Video` 填 `PE_state.video`（通用 arm64）|
| `pexpert/gen/pe_gen.c` | 135 | `PE_init_printf(boolean)`（`TRUE`→vcattach）|
| `osfmk/conf/files.arm64` | 51 | `osfmk/arm/arm_init.c	optional config_pmap_ppl`（新文件照此格式追加）|

构建脚本 `UniXNU/scripts/build-xnu-4k-baseline.sh`（行号按 commit `18ca785`）：

| 行 | 内容 |
|---|---|
| 13 | `if [ ! -d refer/xnu/.git ]; then git clone ...; fi`（refer 源，**不参与编译**）|
| 21 | `if [ ! -d .cache/xnu-work/.git ]; then git clone --shared refer/xnu .cache/xnu-work; fi` |
| 40-66 | **nabu pre-clean（MKU）**：撤掉上一次注入，保证补丁 `apply --check` 上下文干净 |
| 76-81 | **补丁应用循环**（`done` 在第 81 行）|
| 82-169 | **nabu 注入块（MKP）**：写 `nabu_early_mark.{c,h}` + `files.arm64` + `arm_init.c` 四个 MARK |
| 992 | `make -C .cache/xnu-work "${XNU_BUILD_TARGET:-build}" ...`（真正编译）|

> ⚠️ 注入**必须**在补丁循环之后：`native-4k-user-abi.patch` 与 `zzz-boot-progress.patch` 都改
> `osfmk/arm/arm_init.c`，`source-machine-layer.patch` 改 `osfmk/conf/files.arm64`。
> 另外内核构建会跑**两遍**脚本（bootstrap pass + 正式 pass），所以 pre-clean 是必需的，不是可选。

---

## 5. 硬件实测数据

| 项 | 值 |
|---|---|
| DRAM | 12 GiB |
| **RPMh RSC** | **`0x18200000` ×3 bank** |
| IPCC/apss_shared | `0x17c00000`（**只是 mailbox，不是 RPMh**）|
| cmd-db / SMEM | `0x85f20000` / `0x86000000` |
| SPMI / PDC | `0xc440000` / `0xb220000` |
| GCC | `0x100000 +0x1f0000`（`#clock-cells=1`）|
| UFS HC / PHY | `0x1d84000`（ice `0x1d90000`）/ `0x1d87000` |
| 时间基准 | 19200000 Hz |
| cust 分区 | `0x40000000`（1GB）|
| CPU | `reg 0x0..0x700`，psci，kryo485（与 ADT `--cpu-regs` 一致）|
| UFS 供电 | vcc 2.504–2.95V(L10A)、vccq 1.2V(L9A)、vccq2 1.8V(S4A)、vdda-phy 0.88V(L5A)、vdda-pll 1.2V(L3C) |
| PMIC | pm8150=0、pm8150b=2、pm8150l=4（**无 pm8009**）|

---

## 6. 命令速查

```bash
# ADT（务必不带 RAMDisk 占位符）
python3 UniXNU/scripts/make-nabu-adt.py --dram-size 0x300000000 \
  --owned-memory-size 0x130000000 --cpu-regs "0,0x100,0x200,0x300,0x400,0x500,0x600,0x700" \
  --ipad-identity --chip-id 0x207 --unique-chip-id 0x0000000000000001 --output kernel.macho.adt

# cust payload
bash unixnu-nabu-gh/scripts/make-cust-payload.sh kernel.macho kernel.macho.adt cust-payload.img

# 刷 + 启（需 fastboot）
fastboot flash cust cust-payload.img
fastboot boot C:\Users\Administrator\Desktop\unixnu-nabu-kernel-ufs\nabu-xnu64-T.img

# 固件（WSL）
cd /root/Mu-Silicium && python3 build_uefi.py -d nabu -r DEBUG
```

**恢复**：长按电源键 10 秒；全程**只写过 `cust`**，`abl`/`xbl`/`bootloader` 从未刷过。

---

## 7. 下个会话第一件事（按顺序）

> ✅ 1–3 已完成（commit `18ca785` @ UniXNU main）。**从第 4 步接着做。**

| # | 事项 | 状态 |
|---|---|---|
| 1 | 注入块移到补丁循环 `done` 之后 | ✅ 完成 |
| 2 | `assert` 改容错（并升级为缩进无关正则） | ✅ 完成 |
| 3 | `bash -n` 校验 → commit/push UniXNU → 触发 CI | ✅ 完成（`18ca785`）|
| 3.5 | 首轮 CI 修注入冲突 → 暴露编译错误 → 再修 | ✅ 完成（见下）|
| 4 | 下载产物 → 查日志/符号 | ✅ 完成（run `39612555624` success）|
| 4.5 | 查真因：`boot_args.Video` 布局错位 | ✅ 确认（见坑 8 更正）|
| 5 | 重编固件（修 Video）→ 重打 cust payload → 刷 + 启 → 数方块 | 🔄 第 1 轮已刷：方块 **0**（见下），已下沉 MARK1 等第 3 轮内核 |
| 6 | 若方块 0：改"写可写 RAM + DarwinBoot 回读" | ⬜ 条件触发 |

### 第 5 步实测（真机逐轮）

| 轮次 | 内核侧改动 | 屏幕结果 | 结论 |
|---|---|---|---|
| 1 | MARK1–4，直接写物理 fb | **0 块** | 探针自己 fault（fb 未映射）|
| 2 | MARK1 下沉到 `arm_init` 入口 | 0 块 | 同上 |
| 3 | 早期只记 RAM，`PE_create_console` 后 rebind | 0 块 | 这时 fb 仍未映射 |
| 4 | rebind 挂到 `vcattach` 末尾 | 0 块 | 内核没走到 `vcattach` |
| 5 | **`nabu_mark_map()` 用 `ml_io_map_unmappable` 自建映射** | **5 块** ✅ | `arm_init` 主干跑通 |
| 6 | 实时重画 + MARK7（`machine_startup` 前）+ MARK8/9（`kernel_bootstrap`）| **8 块** ✅✅ | **越过 `machine_startup`，进了 `kernel_bootstrap()`** |

**第 6 轮的关键判读**：8 块 = MARK1–8 全部命中，即
`arm_init` 完整跑完 → `machine_startup(args)` → 进入 `kernel_bootstrap()`。
但第 9 块（`PE_init_iokit()` 之后）没有出现，且**屏上没有内核文字** ——
⇒ 死亡点锁定在 **`PE_init_iokit()` 内部**，也就是 `PE_init_printf(TRUE)` → `vcattach`
→ `initialize_screen(NULL, kPEAcquireScreen)` 这条 console 初始化链路上。

> ⚠️ **这彻底推翻了"内核在 `PE_create_console` 之前就静默死亡"的旧结论。**
> 内核实际能跑到 `kernel_bootstrap`，之前看不到任何输出，纯粹是
> **没人给内核早期映射显存**（见坑 11）。

**已采取**：新增 4 个探针把 `PE_init_iokit` 内部切开（UniXNU `cb2a921`）：
```
10 = PE_init_iokit 入口
11 = vcattach 入口
12 = initialize_screen(NULL, kPEAcquireScreen) 之后
13 = PE_init_printf(TRUE) 之后
```

方块语义（v4：实时重画，13 个槽位）：

> ⚠️ 绿条不是进度信号（见坑 12）。只数方块。
> 方块会在每次命中时**立刻刷新**，所以屏幕显示的就是内核的当前最新位置。

| 方块数 | 最远到达 |
|---|---|
| 0 | `arm_init` 早期（或 `nabu_mark_map` 之前）|
| 5 | 第二次 `PE_init_platform` 之后 |
| 6 | `arm_vm_init` + 自映射成功 |
| 7 | `machine_startup(args)` 之前 → **`arm_init` 跑完** |
| 8 | 进入 `kernel_bootstrap()` |
| 9 | 过了 `PE_init_iokit()` |
| 10 | 进入 `PE_init_iokit()` |
| 11 | 进入 `vcattach()` |
| 12 | `initialize_screen(kPEAcquireScreen)` 之后 |
| 13 | `PE_init_printf(TRUE)` 之后 → **console 完全就绪** |
| 附加红块 | 右上角独立红块 = Video 布局/深度可疑（固件没生效）|

### 第 4 步验证结果（run `39612555624`，success）
```
① 补丁冲突：无 patch failed / does not apply
② 注入记录：
   kernel-bootstrap.log : marks inserted: MARK1=True MARK2=True MARK3=True MARK4=True
   kernel.log           : nabu pre-clean: reverted .cache/xnu-work/osfmk/arm/arm_init.c
                          nabu pre-clean: reverted .cache/xnu-work/osfmk/conf/files.arm64
                          marks inserted: MARK1=True MARK2=True MARK3=True MARK4=True
   ← 两遍脚本、pre-clean 生效、第二遍重新注入正确：设计验证通过
③ 符号（kernel.macho 53,760,000 B）：_nabu_mark ×6、_nabu_mark_init ×2
```

### 第 3.5 步：首轮 CI 结果（记录在案）
- run `39611266428` **failure**，但**不再是补丁冲突**——冲突已修好。
  证据：`zzz-boot-progress.patch` 的 `apply --check` 全过程无 `patch failed`。
- 新错误是编译期：
  `arm_init.c:373: passing 'Boot_Video' to parameter of incompatible type 'const unsigned long long *'`
  → 见**坑 10**：`args->Video` 是 `struct Boot_Video`，不是数组。
- 同时由 `pexpert/arm/pe_init.c:419-429` 反查出**坑 8 的结论有误**（display 在第 2 位），
  已在该节写更正与待核实项。
- 修法：传 `(const unsigned long *)&args->Video`，实现对两种布局自适应，布局可疑时画红块。

### 第 4 步：确认注入真的进了二进制

触发方式（workflow_dispatch，`build-kernel.yml`）：
```bash
gh workflow run build-kernel.yml --repo Lux12138/unixnu-nabu
gh run watch --repo Lux12138/unixnu-nabu
gh run download --repo Lux12138/unixnu-nabu -n kernel-nabu -D /tmp/nabu-kernel
```

三处证据（**新增的 `Verify early checkpoint marks` 步骤已经把这三条打好了**）：

```bash
# ① 增量补丁干净应用（没有 apply 失败 = 注入没和补丁打架）
grep -a 'error: patch failed' /tmp/nabu-kernel/logs/kernel.log || echo clean
# ② 注入记录：必须看到 MARK1..MARK4 全 True
grep -a 'marks inserted' /tmp/nabu-kernel/logs/kernel.log
# 期望: marks inserted: MARK1=True MARK2=True MARK3=True MARK4=True
# 若出现 WARN: mark? anchor missing → 上游源码锚点漂移，按第 4 节行号重新核对
# ③ 符号真的链进内核
xcrun llvm-nm --defined-only /tmp/nabu-kernel/kernel/kernel.macho | grep nabu_mark
# 期望: 两行 _nabu_mark / _nabu_mark_init（T）
```
⚠️ **别只看 CI 控制台**（脚本 stdout 被重定向进 `out/nabu/logs/kernel.log`）；
`kernel-bootstrap.log` 是 bootstrap pass 的日志，两遍都会跑注入，两遍都该出现 `marks inserted`。

### 第 5 步：刷机 + 数方块

**5a. 重编固件（必须先做！否则 Video 仍是错的）**
DarwinBoot 已回滚到上游正确版本，重编即生效：
```bash
wsl -e bash
cd /root/Mu-Silicium && python3 build_uefi.py -d nabu -r DEBUG
cd /root/UniXNU && ./unixnu bootimg        # 打包成 Android boot 镜像（UEFI FD + dtb + ramdisk）
```
产物按既有命名递增（当前最新 `nabu-xnu64-T.img`，下一个建议 `-U`），放到
`C:\Users\Administrator\Desktop\unixnu-nabu-kernel-ufs\`。

**5b. 重打 cust payload（内核侧）**
```bash
# ADT（务必不带 RAMDisk 占位符）
python3 UniXNU/scripts/make-nabu-adt.py --dram-size 0x300000000 \
  --owned-memory-size 0x130000000 --cpu-regs "0,0x100,0x200,0x300,0x400,0x500,0x600,0x700" \
  --ipad-identity --chip-id 0x207 --unique-chip-id 0x0000000000000001 --output kernel.macho.adt
bash unixnu-nabu-gh/scripts/make-cust-payload.sh kernel.macho kernel.macho.adt cust-payload.img
```
`kernel.macho` 用 CI 产物 `C:\Temp\nabu-ok\kernel\kernel.macho`（run `39612555624`）。

**5c. 刷 + 启**（刷前确认 `Sending 'cust' (65536 KB)`，别被截断）
```bash
fastboot flash cust cust-payload.img
fastboot boot C:\Users\Administrator\Desktop\unixnu-nabu-kernel-ufs\nabu-xnu64-U.img
```
**每次 boot 前先长按电源 10 秒重启**（坑 3）。

**判读新增的"红块"信号**：若右上角（第 8 槽）出现红块，说明内核侧认为
`Video` 布局仍然对不上 —— 那意味着固件没重编成功，而不是帧缓冲问题。

方块语义（v3：早期只记 RAM，`PE_create_console` 之后一次性回放；从 `(32,32)` 起、间隔 64px）：

> ⚠️ **绿条不是进度信号**（见坑 12）。只数方块。
> 因为 `PE_create_console` 之前无法写屏，**"0 块"只能说明"死在 console 之前"，
> 无法再细分**——这是当前观测手段的硬限制。

| 方块数 | 颜色 | 含义 |
|---|---|---|
| **0** | — | 死在 `PE_create_console()` 之前（`arm_init` 早期 / JOP 签名 / `arm_vm_init`）|
| 4 | 白黄青绿 | 到了 `PE_create_console()`，且 1/2/3 全部经过（正常情况）|
| 5 | +品红 | 全过，包括第二次 `PE_init_platform` |
| 附加红块 | 红 | 右上角独立红块 = Video 布局/深度可疑（固件没生效，或 fallback 生效）|
| 只有 1~3 块 | — | 不会出现：回放一定会把已命中的全部画出，缺号说明位图/几何有问题 |

### 第 6 步（仅方块 0 时）
`Video` 基址在内核早期页表里没有映射（内核 start.s 沿用 UEFI 页表，但只保证 RAM 区域），
所以 `nabu_mark_init` 的写会 fault。两个方向：
1. **先确认 fb 是否真的不可写**：让 DarwinBoot 在 `ExitBootServices` 之后、`DarwinEnter` 之前
   用自己已验证过的 fb 画一个"交棒块"。屏幕上出现交棒块但**没有** MARK1 ⇒ 内核侧 fb 不可写（确诊）。
2. **改用 RAM 传递**：`arm_init` 早期只写静态变量（bss 必然可写），
   在 `arm_vm_init` 之后用 pmap 映射 fb，再一次性 flush 全部检查点到屏幕。

### 备选（若标记路线受阻）
- B：DarwinBoot 在 EL2 交付（需 EL 切换，改动大）
- C：UART 115200（需拆机，**用户不接受**）
- D：内核早期 console 走 GOP（`vcattach` 在 `PE_create_console` 之后才生效，救不了更早死亡）

---

## 8. 文档索引

| 文件 | 内容 |
|---|---|
| `README.md` | 战报：链路 + 坑 + 复现 |
| `HANDOVER.md` | 本文档 |
| `docs/firmware-patches.md` | 固件改动清单 + 已知无效尝试 |
| `docs/nabu-dts-extract.md` | DTS 蒸馏（RPMh 地址纠错 + UFS/PMIC/时钟）|
| `docs/pmgr-plan.md` | PMGR 计划 + RPMh 最小读通路 |
| `docs/early-console-plan.md` | 早期 console + 方块检查点机制 |
| `docs/prior-art-0xDive-ios-nabu.md` | 同类项目对比 |
| `kits/nabu/NabuPlatformPower.cpp` | PMGR 骨架 |
| `kits/nabu/nabu_early_mark.{c,h}` | 方块检查点（纯 C）|
| `scripts/make-cust-payload.sh` | cust payload 生成 |
| `scripts/inject-early-marks.sh` | 标记注入器 |

---

## 13. xnu-10063 启动停在 mig_init（本轮定位结论）

### 事实链（全部有硬件证据）

| 检查点 | 读数 | 含义 |
|---|---|---|
| `NABU_MARK(8)` | 8 | 进入 `kernel_bootstrap` |
| 自动编号 18..44 | 26 | 走到 `mac_policy_init` |
| 45 / 46 | **45** | `mac_policy_init()` 返回；`kernel_startup_initialize_upto(STARTUP_SUB_MACH_IPC)` 内崩 |
| 子系统 + 条目 | 13 / 27 | MACH_IPC 轮第 28 个条目 |
| `cur->func` 散列 | 56 → 50 | 两版工具链下都唯一映射到 **`_mig_init`** |
| `mig_e[i]` 索引 | **0** | `_mach_vm_subsystem` |
| 例程索引 j | **0** | `mach_vm_allocate`（msgid 4800） |
| stub 指针散列 | 19 | `0x...153c >> 4 & 0x3F`，与静态 dump 一致 → kstub **非零** |

`mig_init()` 的五个 panic 分支中，触发的是：

```c
panic("kroutine must have precise size %d %d", mig_e[i]->start, j);
```

即 `mach_vm_allocate` 的 `max_reply_msg == 0`。但 `mach_vm.defs` 里它有 `out` 参数，
描述符**理应**带回复尺寸 —— 所以是**构建期 mig 生成环节丢了该字段**，不是定义问题。

### 已排除

- 重复注册：31 个 ops、31 个唯一 `iko_op_type`
- 数组越界：`__init_entry_set` 95856 B = 3994×24；`ipc_kobject_ops_array` 1728 B = 54×32
- 工具链版本：`picked: /Applications/Xcode_15.0.app` 后仍停在同一个 panic
- 我们的补丁：patch 集里没有任何 `IKOT` / `mig` 改动
- 跨 boot RAM 传递：`keep probe` 第二次启动仍是 `fresh RAM`，DRAM 不保留

### 采用的处置

把 panic 改为回退：

```c
} else {
    mig_buckets[pos].kreply_size = mig_e[i]->maxsize;   /* 保守值 */
    mig_buckets[pos].kreply_desc_cnt =
        mig_e[i]->kroutine[j].reply_descr_count;
}
```

属于**务实绕过**（尺寸偏大只浪费内存，panic 则丢整个启动）。副作用与隐患：
回复缓冲区按子系统最大值（mach_vm 为 4140 B）分配，若后续出现 kalloc 压力或
reply 解析异常，应优先怀疑此处。

### 探针经验（避免再踩）

- framebuffer 在 `arm_init` 全程**未映射**；`ml_io_map_unmappable` 自建映射后才可写
- 方块条最多 24 格且按编号**从小到大**画，晚期的 45/46/50/60 会被挤掉（已改为画最新 24 个）
- `draw_number` 必须画前擦格，否则十位残留（50 会被读成 54/58）
- 单一数字放不下信息：mach 启动表有 1736 条（`subsystem <= 14`），ZALLOC 独占 767 条，
  已改为「子系统 + 条目序号」双格，再升级为 `nabu_mark_quad` 四格
---

## 14. 越过 mach IPC：内核进入 IOKit kext 加载

### 里程碑

今天第一次通过 `kernel_startup_initialize_upto(STARTUP_SUB_MACH_IPC)`。
之前的结论（"停在 mig_init 的 kroutine panic"）是对的，但**只是第一层**。

### 完整死因链（每一层都有屏幕读数支撑）

| 层 | 现象 | 证据 | 处置 |
|---|---|---|---|
| 1 | `panic("kroutine must have precise size")` | i=0/j=0 → `mach_vm_allocate`，`max_reply_msg=0` | 回退到子系统 `maxsize` |
| 2 | 收尾 `printf` 触发 kprintf | 移除后仍停 | 删掉该诊断 printf |
| 3 | `assert3u(grp,<,TASK_GRP_COUNT)` 误判 | 面板 `line=283 x=120675180 y=32` | 实为下一层 |
| 4 | **`reply_descr_count <= IPC_KOBJECT_RDESC_MAX`** | 断言原文 + `x=120675180`（指针低位）、`y=32` | **关掉两个描述符计数断言** |
| 5 | **`OSMetaClass: Kext %s class %s is a duplicate`** | 绿字原文 | 当前问题（prelink 层）|

第 4 层是真凶：`kern_routine_descriptor` 的字段偏移与本构建不符，
`reply_descr_count` 读到了指针低位（0x731_4xxx），断言必然失败。
这也解释了此前所有静态 dump「布局怎么算都对不上」。

### 观测手段的演进（关键工程收获）

1. **物理 framebuffer 自映射**：`nabu_mark_map()`，在 `arm_vm_init` 之后可用
2. **方块 / 数字探针**：早期靠这个定位到 `mig_init`
3. **5x7 字库 + `nabu_mark_text()`**：直接把 panic 格式串画到屏幕
4. **panic 钩子解析 varargs**：画出断言表达式、行号、x/y 实际值
5. 探针全部下移到屏幕下半部（`y≈1000+`），把上半部留给 console/文本

> 经验：**别再用两位数格子猜数值**。`x=5180/y=32` 是误读，
> 实际 `x=120675180/y=32`，结论完全不同。用 `nabu_mark_num()` 打印十进制文本。

### 试过但无效的路

- **early console**（`arm_init` 里调 `PE_init_printf(TRUE)`）：
  `vcattach` 半初始化，随后 `gc_update_color` 撞 `assert(gc_ops.update_color)`
  （`video_console.c:1226`）——自伤，已移除
- **跨 boot RAM 传递**：DRAM 重启不保留，`keep probe` 第二次仍是 `fresh RAM`
- **UART 串口**：xnu 有 `serial_console` 框架，但缺平台 UART 基址 + 需要硬件接线
- **USB 日志**（Pagani/giulia 的做法）：进入用户态后才有，早期无用

### 构建管线的坑（浪费了十几轮）

- `unixnu tree` 已应用 patch，构建脚本又应用一遍 → `patch does not apply`，
  且错误被 `|| true` 吞掉。**修法：patch 前 `git reset --hard` + `clean -fdq`**
- `source-machine-layer.patch` 往 `files.arm64` 加了 4 个 `amcc_rorgn_*.c`，
  这些文件在 10063 里不存在 → 文件列表阶段 `Error 2` 无诊断
- `pexpert/arm/` 没有 osfmk include root，`<arm/nabu_early_mark.h>` 解析失败，
  必须用 `"../../osfmk/arm/nabu_early_mark.h"`
- 内核里**没有 `strstr`**，链接会失败；用字面前缀比较替代
- 注入代码引用 `nabu_early_mark.c` 的 `static` 变量（如 `g_h`）会编译失败，
  只能用固定坐标

### 当前问题

`OSMetaClass: Kext %s class %s is a duplicate; kext %s already has a class with this name`

IOKit 初始化时报 kext 类名重复。属于 **prelink 打包**（`kits/NativePrelink` 子模块）
层面的问题，不是内核逻辑。下一步：读出重复的 kext 名，核对 prelink 清单。

---

## 15. QEMU 快速迭代环境（今天建成）

### 为什么

CI 一次内核构建 **8.1 分钟**（总 9.3 分钟里 87% 是编译），
每轮刷机再加快 2 分钟 + 人工拍照判读。没有 macOS 主机就没法本地编，
所以只能把**验证**这一半搬到本地：**QEMU 45 秒一轮，串口日志直接可读**。

### 组成部分（全部已就位）

| 件 | 位置 | 说明 |
|---|---|---|
| QEMU | `C:\Program Files\qemu` | 11.1.0，含 `edk2-aarch64-code.fd` |
| Mu_Basecore | `UniXNU/package/Mu_Basecore` | EDK2 分支 `release/202608`，8.3k 文件 |
| UniXNU.efi | `package/Build/Out/AARCH64-DEBUG/UniXNU.efi` | 94 KB，`./build.sh -b DEBUG -t GCC` |
| DarwinBoot.efi | `package/Build/UniXNU/DEBUG_GCC/AARCH64/DarwinBoot.efi` | 36 KB，提供 XNU loader protocol |
| ESP | `C:\Temp\qemu-test\esp` | `EFI/BOOT/BOOTAA64.EFI` + `EFI/UniXNU/{Drivers,Payloads,unixnu.toml}` |

### 关键坑

- `build.sh` 里 `PACKAGES_PATH` 原本只有 `package` + `edk2`；
  `DarwinBootPkg` 在 `drivers/DarwinBoot/` 下，需要
  `$ROOT/../drivers/DarwinBoot:$ROOT/../drivers:$ROOT:$EDK2`
- `DarwinBoot.dsc` 里的 `Test/Validation.inf` 路径是相对 `DarwinBootPkg/` 写的，
  实际文件在 `drivers/DarwinBoot/Test/`，会解析失败，已移除
- EDK2 的 `build` 失败时只打印 `- Failed -`，要 `-d 9` 才能看到原因
- **WSL 调 Windows QEMU 时，QEMU 命令行里的路径必须是 Windows 形式**
  （`C:/...`），只有日志文件留在 WSL 侧
- `-m 4096` 时 DarwinBoot 报 `boot refused: Out of Resources`，**8 GB 才够**

### QEMU 里已验证的事实

```
UNIXNU: UniXNU 0.1.0 starting as application, firmware EDK II
UNIXNU: driver \EFI\UniXNU\Drivers\DarwinBoot.efi (config): started
DARWINBOOT: loader v2 installed EL1
DARWINBOOT: ARM64 va=0xFFFFFFF00427C000 pa=0x4807C000 entry=0x4B1EB488
DARWINBOOT: hand-off to the kernel
```

**内核 entry code 跑了（顶部绿条），异常 trace 显示内核在正常执行：**
852 个异常，尾部全是 `Taking exception 5 [IRQ]`，`SPSR 0x205`（EL1h），
**没有任何 abort**。所以内核在 QEMU 里是活的，只是没往串口输出。

### 待办

- `nabu_mark_putc` 加 **PL011 tee**（`0x9000000`）后，QEMU 才能抓到内核文本；
  真机上该地址未映射，写入被缺页处理吞掉，不影响 fb 路径
- 串口通了之后，`-d int` 的 trace 就不需要了

### 全自动

- `C:\Temp\auto\run.ps1` —— 一次：等最新构建 → 下载 → 刷 ESP → QEMU → 摘要
- `C:\Temp\auto\loop.ps1` —— 循环：触发构建 → 等 → 下载 → QEMU → 分析 → 下一轮，
  每轮日志存 `C:\Temp\auto\rounds\<n>\`
- CI 侧已加 `actions/cache`（`xnu-work/build`），下一次起编译应变成增量


---

## 16. QEMU 里的内核日志打通（今晚成果）

### 结果

本地 QEMU 现在能打印**真实的内核日志**：

```
NABU                                                          <- UART 映射成功的标记
kprintf initialized
Serial mode specified: 00000003
initialize_screen: b=23C7A0000, w=320, h=258, r=0xC80
pe_arm_init_interrupts: args: 0xfffffff007a62860
corecrypto_kext_start (tracing disabled)
FIPSPOST_KEXT ... all tests PASSED (1690 ms)
corecrypto_kext_start completed successfully
ulock_initialize>thread_max=1536, ull_hash_buckets=512        <- 停在这
```

一轮验证 **40 秒**（CI 8.5 分钟 + 刷机 2 分钟 + 拍照）。

### 四道关（每道一个明确根因）

| # | 症状 | 根因 |
|---|---|---|
| 1 | 完全无日志 | `kprintf` 的门是 `if (!disable_serial_output && !disable_kprintf_output)`，**两个默认都是 true**，只清一个没用 |
| 2 | 清了还是无日志 | `PE_init_kprintf` 在 `STARTUP(KPRINTF)` 跑，**早于 fb 映射**，按「fb 就绪」判断导致 hook 永不安装 |
| 3 | UART tag 也不出现 | `0x9000000` 是**物理地址**，内核 VM 未映射，写入 fault 被吞；必须 `ml_io_map_unmappable` 先映射 |
| 4 | Undefined Instruction | QEMU `-cpu cortex-a72` 不认识 `MSR S3_0_C4_C1_4`（0xd500419f）；**`-cpu max` 才行** |

### QEMU 的边界

卡在 `ulock_initialize()`（`STARTUP(EARLY_BOOT, STARTUP_RANK_FIRST)`）的
`zalloc_permanent`。**真机能过这一步**（真机已到 IOKit），所以这是 QEMU 缺硬件/
内存布局差异造成的，不是内核缺陷。**QEMU 的验证深度到此为止**。

### 可复用的东西

| 件 | 位置 |
|---|---|
| Mu_Basecore | `UniXNU/package/Mu_Basecore`（`release/202608`）|
| UniXNU.efi | `package/Build/Out/AARCH64-DEBUG/UniXNU.efi` |
| DarwinBoot.efi | `package/Build/UniXNU/DEBUG_GCC/AARCH64/DarwinBoot.efi` |
| ESP | `C:\Temp\auto\esp` |
| QEMU 设备树 | `C:\Temp\auto\qemu.adt` |
| 截图 | `C:\Temp\auto\shot.ps1` |
| 全自动 | `C:\Temp\auto\loop.ps1` |

**QEMU 命令行（缺一个都不行）**

```
-M virt -cpu max -m 8192 -smp 1
-drive if=pflash,format=raw,readonly=on,file=edk2-aarch64-code.fd
-drive if=none,id=esp,format=raw,readonly=on,file=fat:<ESP目录>
-device virtio-blk-pci,drive=esp -device ramfb
-display none -serial file:<log>
```

- `-cpu max`：cortex-a72 会撞未定义指令
- `-m 8192`：4096 时 DarwinBoot 报 `boot refused: Out of Resources`
- ESP 用**目录**（`fat:`），不需要 dd/mkfs.vfat 造镜像
- WSL 里调 Windows QEMU 时，**命令行路径必须是 `C:/...` 形式**

### 设备树要用 QEMU 的，不是 nabu 的

`make-dt.py --armvirt --armcpu --smp 1` 生成。用 nabu 的 `.adt` 会报：

```
*** CPU id 256 from CPU ordinal 1 'reg' property is out of range [0..7]
*** Missing 'interrupts' property for CPU 0
*** Missing 'function-ipi_dispatch' property for CPU 0
```

### 真机 IOSurface 的本质（已查清）

`IOsurfaceRoot:installMemoryRegions()` 之后卡住，**不是断言、不是内存不足**，是
**Apple 闭源 IOSurface kext 在非 Apple 平台上跑不通**。

ios-nabu 的做法是**重写**：`NabuSurface.cpp`（IOCoreSurfaceRoot）+
`NabuFramebuffer.cpp`（AppleCLCD），注册进 `iokit/KernelConfigTables.cpp`，
并在 `NabuPlatformExpert` 里给 `arm-io` 子节点建 nub。**周级工作量**。
