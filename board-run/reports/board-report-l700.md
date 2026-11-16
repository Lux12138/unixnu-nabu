# 上板报告 l700（运行期 OSMetaClass 只读诊断）

## 1. 唯一问题

`nabu-gpu-abi=1`（**只 report、不加载 kext**）能否给出运行期 OSMetaClass 表里
`IOAcceleratorES` 的真实状态：我们伪造的基类是否已注册、`classSize` / `super` 是什么？
与 l623 的 `class=0 size=0` 对照。

## 2. 已闭合到哪

**为什么需要这一轮**：
- 交接文档 §7g 把根因收敛到「iOS 侧 AGX 需要合规的 `IOAcceleratorES` provider 才会走
  `IOGPUDeviceCreate` / `IORegistryEntryCreateCFProperty(CFSTR("MetalPluginName"))`」。
- 我们 `NativeAdrenoAcceleratorPublish.cpp` 自己伪造的空 `IOAcceleratorES`
  （`OriginalAcceleratorABI.h:8-11`，零方法，`sizeof==136`）是否**真的在运行期
  metaclass 表里**，只有 `OSMetaClass::getMetaClassWithName` 能证。
- `llvm-nm l699/kernel.macho` 只证明 `__ZN15IOAcceleratorES10gMetaClassE` **符号存在**，
  不能证明 static initializer 已经把它注册进表。

**历史对照（l623，`logs/cdc-181340.bin:102-103`）**：
```
NABU GPUABI pre ctl name=... class=... size=136 class_name=IOService
NABU GPUABI pre es  name=... class=0 size=0 class_name=none super=0 super_name=none
```
⇒ 那时 `IOAcceleratorES` **不在**运行期表里。但 `NativeAdrenoAcceleratorPublish.cpp`
是 l626/l627 之后才加的，所以现在必须重测。

**`=1` 是安全的**（这是选它而不选 `=2` 的原因）：
- `GpuAbiProbe.cpp:52` 只在 `on==1 || on==2` 时进入；
- `:57` `nabu_gpu_abi_report("pre")` 只做 `OSMetaClass::getMetaClassWithName` + `IOLog`；
- **只有 `on==2`（`:60-67`）才 `OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily")`**。
- `=1` 完全不加载 kext ⇒ **没有同名 metaclass 冲突风险**。
  （`=2` 已在本阶段判定为不安全、禁止上板：真 kext 会再注册一个同名
  `IOAcceleratorES`，与我们伪造的 `gMetaClass` 撞名。）

**调用点**：`NabuTouch.cpp:1012/1018` `touchThread` 在启动早期调
`nabu_gpu_abi_probe()`，`GpuAbiProbe.o` 已编进 late archive（`build-gop-legacy-display.py`
注释：它是 `NabuTouch.o` 的同层依赖），`llvm-nm` 确认 `_nabu_gpu_abi_probe` 在内核。

## 3. 为什么离线不行

运行期 `OSMetaClass` 表的内容（哪个类注册了、classSize、superClass）是
static initializer 执行后的动态状态；静态只能证符号与调用链存在，不能证
`getMetaClassWithName` 命中哪一个。

## 4. 失败也有信息

- **`es class!=0 size=136 super=IOService`**：我们伪造的基类已在运行期表中，
  `AGXAccelerator` 继承链成立 ⇒ 问题确认在「iOS 侧没选它做 Metal provider」
  （b 分支），下一步走单文件替换上游 `NativeAdrenoAccelerator.cpp` 方向。
- **`es class=0`**（与 l623 相同）：伪造类的 static initializer **没执行**
  （可能被 `llvm-objcopy --remove-section=__DATA,__mod_init_func` 剥离而没被
  globalize）⇒ 那才是真正的断点，修法完全不同（修初始化，不是修 provider）。
- **`es size != 136`**：与 `static_assert(sizeof(IOAcceleratorES)==136)` 矛盾，
  先查运行期 classSize 来源，别急着改 provider。
- **两行都没出现**：`nabu_gpu_abi_probe()` 没被 `touchThread` 调到，或
  args 没落进 toml；先查这个再谈 metaclass。

## 5. 唯一变量与回滚点

**唯一变量 = args 把 `nabu-gpu-observe=1` 等长换成 `nabu-gpu-abi=1`**
（698→694 字符，38 token；tag `l699`→`l700`）。
l699 已把 `observation` 的 4 次采样数据拿全，本轮不需要它。

其余五个文件与 l699 **逐字节相同**：

| 文件 | sha256（前 8） |
|---|---|
| kernel.macho | `efcb4c34` |
| kernel.macho.adt | `78a6f7e9` |
| owner-trust.bundle | `76783683` |
| gpu-addon.bin | `39dc17fe` |
| boot-merge-r3.img | `ead4b531` |

payload `cc3e1dde`（仅 toml args 串与目录项 size）。
- 回滚包：`hold/l699/`
- 回滚命令：`fastboot flash cust hold/l699/payload.img`

## 6. 恢复预案

**不碰 boot_a**（AGENTS.md 0.10）。流程：recovery arm misc →
`fastboot flash cust hold/l700/payload.img` →
**`fastboot flash misc misc-bootrecovery.bin`（flash 后重 arm）** → `fastboot boot`。
本轮 args 无 `nabu-autolog`。若 fastboot 全失败：人工长按电源 10 秒。

## 7. 门禁

| 项 | 结果 |
|---|---|
| `audit_bootability.py` (base l699) | ✅ 0 fail, 0 warn |
| `audit_dual.py` | ✅ 7/7 match |
| `precheck.py` + args token diff | ✅ stamp written，added/dropped 与声明一致 |

**注**：编译侧 fc76 自 11-06 14:57 未消费，本轮板侧自行出包；双审计两侧均为板侧实测
（偏离 AGENTS.md 0.6，已记录）。
