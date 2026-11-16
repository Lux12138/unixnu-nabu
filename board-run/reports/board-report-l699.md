# 上板报告 l699（Metal 发现链诊断轮，纯观测）

## 1. 唯一问题

开启 `nabu-gpu-observe=1` 后，Metal 发现链诊断能给出什么：
**有没有人读 `MetalPluginName`、谁读的、backboardd 有没有 open `AGXAccelerator`？**

这是纯诊断轮，不改任何功能代码、不改内核，只换一个 boot-arg。

## 2. 已闭合到哪（静态链）

**l698（上一轮，新版 addon + t733）的实证**：
- CDC：`surface import refused` 0、`NABU GPUACC published cls=AGXAccelerator props=5`、
  `NativeAdrenoA6xx: service published`、`frames xfer=5`、无 panic。
- os_log（macul 解出 23069 条正文）：`MetalPluginName` **0** 命中、
  `NativeAdrenoMetal.bundle` **0**、`discovery process` **0**、
  `CAContext`/`CALayer`/`CARenderServerStart` **0**、`PUB`/`ST0` **0**。
- 新版插件 uuidtext（`4C/4C441955...`）里**有** `PUB `×2 / `ST%u` /
  `PUB no device` / `NativeAdrenoPublish` / `Info.plist` 等字符串，
  但 os_log **0 条引用该 UUID** ⇒ 插件没产生日志。
- ⇒ **Metal 发现链根本没跑到**（与 `state/lessons-tracev3-archive.md:42-44`
  对 l695u 的结论一致）。这解释了为什么换 addon、加 t733、注入触摸都不改变结果。

**内核侧的发布是好的**：`NativeAdrenoAcceleratorPublish.cpp:70-88` 设了
`MetalPluginName=NativeAdrenoMetal`、`MetalPluginClassName=NativeAdrenoMetalDevice`、
`NativeSandboxGrantProcess=backboardd` 等 5 个属性并 `registerService()`。

**缺的是观测，不是功能**：
- `NativeAdrenoPublish.cpp:1911` 的 `native_adreno_metal_dump_stats()`
  会打 `property_reads` / `name_reads` / `discovery process=<name> reads=<n>` /
  `open=` / `client=` / `selector=` 等全部关键量，但**全树只有定义、零调用者**。
- 但 `NativeAdrenoService.cpp:265-310` 已有一套等价的 **`observation` 采样**
  （`IOLog` → CDC），打 `NabuGPU: observation` / `metal_opens` /
  `discovery_reads` / `metal_calls` / `metal_open_slot`（含 process/type/macf/resolver/attach）
  / `metal_method`，由 `nabu-gpu-observe=1` 开启。
- 调用链（已静态核到行号）：`:1310` `PE_parse_boot_argn("nabu-gpu-observe")`
  → `:1311` `observation_.start(observe==1, ...)` → `:1348` `A640State::poll()`
  每轮调 `observation_.sample` → 在 GPU ready 后 **t=0/10/30/120 s 共 4 次**打印。
- `NABU_GPU_METAL=1` 已编进内核（`build-nabu-adreno-step1.py:98` 加的 `-D`，
  `enableCachedCoherentImports` 等符号实测在 kernel.macho 里）。
- 历史轮从未开过这个开关（l698 及之前 args 逐 token 核对：无 `gpu-observe`、
  无 `userprobe`），CDC 里 `NabuGPU: observation` 0 条。

## 3. 为什么离线不行

`native_adreno_metal_diag` 里的 `name_reads` / `opens` / `attach_status` 等是
**运行期累积计数器**；静态只能证明采样代码、调用点和 IOLog 输出都存在，
不能给出「有没有人读、被谁读、open 被哪个门拒绝」。这些只有板上的一次读出能回答。

## 4. 失败也有信息

- **`discovery_reads > 0` 且有 `metal_open_slot process=backboardd`**：发现链通了，
  看 `macf`/`resolver`/`attach` 哪个门拒绝 → 下一轮针对性修。
- **`discovery_reads = 0`、`metal_opens = 0`**：确认「没人来 match AGXAccelerator」，
  断点在 **iOS 侧 Metal 发现前提**（IOAcceleratorES/IOGPUFamily 匹配），
  而不是我们的插件或 addon。这直接决定后续是修内核 provider 还是修 CA 侧。
- **`observation` 行完全没出现**：则 `A640State::poll()` 的 ready 路径或
  `NABU_GPU_METAL` 编译门有问题，先查这个再谈发现链。
- 顺带读 `NabuGPU: userprobe_tick`（若 `nabu-gpu-userprobe` 未开则无）与
  `NativeAdrenoMetal: ...`（dump_stats 无调用者，预期仍 0）。

## 5. 唯一变量与回滚点

**唯一变量 = args 把 `nabu-state-ring=10` 等长换成 `nabu-gpu-observe=1`**
（698 字符 / 38 token 不变，预算 700 内；tag `l698`→`l699`）。
`nabu-state-ring` 只控制 `NABUSTATE1` 快照频率，本轮判据是 CDC 的
`NabuGPU: observation` 行 + diag os_log，不依赖它。

其余五个文件与 l698 **逐字节相同**：

| 文件 | sha256（前 8） |
|---|---|
| kernel.macho | `efcb4c34` |
| kernel.macho.adt | `78a6f7e9` |
| owner-trust.bundle | `76783683` |
| gpu-addon.bin | `39dc17fe`（新版） |
| boot-merge-r3.img | `ead4b531` |

payload `33e8b345`（仅 toml args 串与目录项 size）。
- 回滚包：`hold/l698/`
- 回滚命令：`fastboot flash cust hold/l698/payload.img`

## 6. 恢复预案

**不碰 boot_a**（AGENTS.md 0.10）。流程：recovery arm misc →
`fastboot flash cust hold/l699/payload.img` →
**`fastboot flash misc misc-bootrecovery.bin`（每次 flash 后重 arm）** → `fastboot boot`。
本轮 args 无 `nabu-autolog`，内核不自己 arm misc。
若 fastboot 全失败：人工长按电源 10 秒。

## 7. 门禁

| 项 | 结果 |
|---|---|
| `audit_bootability.py` (base l698) | ✅ 0 fail, 0 warn |
| `audit_dual.py` | ✅ 7/7 match（用真实 payload sha `33e8b345`） |
| `precheck.py` + args token diff | ✅ stamp written，added/dropped 与声明一致 |

**注**：编译侧 fc76 自 11-06 14:57 未消费，本轮板侧自行出包；双审计两侧均为板侧实测
（偏离 AGENTS.md 0.6，已记录）。
