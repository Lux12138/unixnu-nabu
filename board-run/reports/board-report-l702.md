# 上板报告 l702（publish::requested() 4 字节路径修复 + 整链重签）

> 本报告是**上板前**门禁文件；本会话未运行 adb/fastboot、未上板、未改 `hold/l702/`、未写 `logs/rounds/l702.json`。
> 按 `E:\nabu\AGENTS.md` 0.5 / 0.6 / 0.7 / 0.9 / 0.10 编写。

## 1. 唯一问题（一句话）

**0x40d4 的 4 字节路径 bug 修复（`1f140038` → `00040091`）并整链重签后，板上 `publish::requested()` 是否真读到 bundle 内 `Info.plist` 的 `NativeAdrenoPublish=true`、改走 `publish::deviceForPort()`，从而出现 `MTLDevice`/`CAContext`/`CALayer` 与非 blank swap？**

其余一切不再问：不试参数、不加 boot-arg、不新增自造探针、不重编内核、不放宽安全策略。

## 2. 已闭合到哪（日志优先 + 静态链）

**传导链（已写入 `state/precheck.json:10-11` 的 `chain` / `why_board`）**：

1. `publish::requested()` 恒 false：板上插件 exe `@0x40d4 = 1f140038` = `strb wzr,[x0],#1`（`bundle-orig/NativeAdrenoMetal` 反汇编），把 `dladdr`/`strrchr` 得到的 `dli_fname` 路径最后一个 `/` 写成 0，路径被提前截断；`NativeAdrenoMetalPlugin.mm:49-64` 的 `[NSDictionary dictionaryWithContentsOfFile:]` 读不到 bundle 内 `Info.plist`（`state/HANDOVER-20261110-l696.md:1038-1044`）。bundle 内 `Info.plist:19-20` 确有 `NativeAdrenoPublish=true`。
2. 修复后 `0x40d4 = 00040091` = `add x0,x0,#1`，只推进指针、不写内存，应拼出 `<dir>/NativeAdrenoMetal.bundle/Info.plist` 并读到 true（`HANDOVER:1045-1048`；`bundle-new/NativeAdrenoMetal` 反汇编）。
3. 修复前 `requested()=false` → `initWithAcceleratorPort:` 走 else 的 detached `adrenoUserTest` 并 `return nil`（`mm:421-427`）→ **stage-1 自测**：l699 GPU ready **+120 s** 第 4 次采样 `ext_entries=266` 次 `externalMethod` / `metal_calls=10` ABI calls / `backboardd type=00004147`（`logs/rounds/l699.json:20,23,38`；`HANDOVER:1050-1066`）。
4. `return nil` → 无 `MTLDevice` → 无 `CAContext`/`CALayer` → blank swap（`HANDOVER:1065-1066`；`logs/rounds/l701.json:30-36` os_log 的 `CAContext`/`CALayer`/`PUB`/`NativeAdrenoPublish` 全 0）。
5. l701 官方行 `nabu-gpu-addon: mounted=1 target=1 control=1 status=0 | dmn=5/0 | wlp=1/2`（`logs/l701-v1.jsonl:41984`、`logs/rounds/l701.json:20`）证明 union 全通、bundle `Info.plist` 内核侧可见；`:41983` `nabu-addon: files=8 wr=8 same=0 fail=0 err=0 publish=0`（`publish=0` 是旧版同级 `.publish` 的过时读数，`HANDOVER:1025-1036`）；`:110768` `backlight factor:0` 在 l697/l698/l701 都有，不是区分点（`HANDOVER:1086-1091`）。
6. 修复后应读 `NativeAdrenoPublish=true` 并走 `publish::deviceForPort()`（`mm:240-416` / `:421-425`），由 `native_bridge_metal_open` + monitor/telemetry/scanout/primer 产出 `PUB`/`ST`，生成 `MTLDevice`、建 `CAContext`/`CALayer` 并提交首帧。

**已排除/冻结**：§7q 撤回「sandbox/MACF deny = 最终墙」——shipped kernel 的 `zzw/zzwb` grant 已短路两道真实准入（`HANDOVER:995-1019`）；`NativeAdrenoPublish.cpp:1724` 的 deny 只是驱动诊断调用、不拒绝连接；l699 的 266 次 `externalMethod` 证明 backboardd 连接是活的，但时间限定在 **GPU ready +120 s 的 stage-1 自测**，不是启动早期 Metal 发现（`HANDOVER:1008,1068-1077`）；union / 8 条目 addon / `MetalPluginName`+`MetalPluginClassName` / trust+seal / display 帧全退役 / 背光不可判因、以及三个现成 userprobe 通道（都写进空 `nabu_capture_append`）全部离线闭合（`HANDOVER:1093-1120`）。日志对「修复后是否走 published 路径」沉默，属 0.5 允许的一次运行期判定。

## 3. 为什么离线不行

静态能证到指令语义、`Info.plist` 键在、代码分支、签名整链一致；但 **`dictionaryWithContentsOfFile` 在真机 sandbox 下是否真返回 true、`initWithAcceleratorPort:` 是否改走 `publish::deviceForPort()`、是否真的产生 `MTLDevice`/`CAContext`/`CALayer` 与首帧，都是运行期状态**。l701 的 os_log 对这些量全 0 且成功路径静默，离线没有读数通道（`HANDOVER:1100-1120`）；不能靠再改包去取数，只能用这一轮一次判定。

## 4. 失败也有信息（否定结果可判读）

- **官方行 / frames 与 l701 一致、UI 仍 blank**：说明 4 字节修复未改变下游 —— 要么 `requested()` 仍 false（`Info.plist` 读仍被 sandbox 静默拒），要么断点前移到 `publish::deviceForPort()` 之后（`MTLDevice` 注册 / CAContext 创建 / 首帧提交）；把「连接已建立 → 首帧」区间再收窄一段。
- **出现 `PUB`/`ST` 且 `CAContext`/`CALayer`/`NativeAdrenoMetal` 非零**：修复生效、published 路径已走；再看 frames 与 `screen.bgrx` 判首帧是否提交。
- **行 `mounted/target/control/status` 变化或 `nabu-addon: fail>0`**：addon 安装/union 被重签影响；离线已证 addon 8 条目仅 exe 变、trust 模块 1/2 完全不变，可直接对账。
- **早死 / panic / 插件未加载**：整链重签的 cdhash 与板上实际 CodeDirectory 不一致；离线已核 `ba5a873d` → `a7b7ff5b` 且 trust 模块 0 第三个 hash 同步，可据此证伪或定位签名链。
- **CDC 里 `NabuGPU: observation` 0 条**：l702 args **没有** `nabu-gpu-observe=1`（`logs/rounds/l701.json:11`、`HANDOVER:1075-1077`），预期 0 条，**不是失败判据**，不得据此判「没采到 = 没发生」。

## 5. 唯一变量与回滚点

**唯一变量 = 板上插件 exe 的 4 字节路径修复及其整链重签**（`0x40d4 1f140038` → `00040091`；exe 其余差异仅 CodeDirectory 32 字节重签；addon 8 条目中仅 `addons/NativeAdrenoMetal.bundle/NativeAdrenoMetal` 变化，`Info.plist`/`CodeResources`/`libvulkan_freedreno.dylib`/`adrenoprobed`/3 个 plist 逐字节相同、mode 不变）。

| 文件 | l701 sha8 | **l702 sha8** | 说明 |
|---|---|---|---|
| boot-merge-r3.img | `ead4b531` | `ead4b531` | 逐字节相同 |
| kernel.macho | `fb3ce142` | `fb3ce142` | 逐字节相同（未重编） |
| kernel.macho.adt | `78a6f7e9` | `78a6f7e9` | 逐字节相同 |
| gpu-addon.bin | `39dc17fe` | **`950a3968`** | 新（12,317,552 → 12,317,544，仅重压缩差 8 B） |
| owner-trust.bundle | `76783683` | **`a4a6bae6`** | 新（同为 61,456 B；3 模块，模块 0 第 3 个 cdhash `ba5a873d`→`a7b7ff5b`，模块 1/2 的 2478/76 条全同） |
| payload.img | `806f4c8f` | **`b91cfe98`** | 新（同 201,326,592 B；内部 6 条中仅 `KERNEL~1.TRU`、`KERNEL~1GPU` 变） |

**args 与 l701 逐 token 相同**：694 字符 / 38 token（≤700），含 `tag=l700`、`nabu-gpu-abi=1`、`sheng-gpu-addon=1`、`t733-managed-rt-normal=1`；`sheng-smp=1` 仍配 `nabu-wdt-arm-delay=0`（`tmp/audit_args.py` 输出 `added: []` / `dropped: []` / `RESULT: OK`；`unixnu.toml` 1120 B sha `24ebae84` 两包相同）。**没有** `nabu-gpu-observe`、`nabu-autolog`、任何新增 boot-arg。

- 回滚基线：`hold/l700/`（kernel `efcb4c34`，已验证 4 分钟窗口）；上一包 `hold/l701/`（`fb3ce142`）同窗可用。
- 回滚命令：`fastboot flash cust hold/l700/payload.img`（仅文字，本会话不执行）。

## 6. 恢复预案 / 观察项 / 门禁

**AGENTS.md 0.10 红线：不碰 `boot_a`（绝不 `fastboot flash boot` / `flash boot_a`）**。流程（仅文字）：

```
recovery arm misc
fastboot flash cust hold/l702/payload.img
fastboot flash misc misc-bootrecovery.bin   # flash 后重 arm
fastboot boot hold/l702/boot-merge-r3.img  # 临时加载，不写任何分区
```

`arm-delay` 保持 **0**（args 不变），咬点按历史 **380–410 s**，观察完整 4 分钟窗口（`AGENTS.md:57-64`）；早 panic 也有重置路径。若 fastboot 全失败：人工长按电源 10 秒。`misc-bootrecovery.bin` 在 `E:\nabu\dwc3\build\misc-bootrecovery.bin`（4096 B）。

**观察项（全部是官方已有输出，零新增 printf/IOLog/计数器/采样/探针）**：
- CDC 官方 `nabu-gpu-addon: mounted/target/control/status` 行、`nabu-addon: files/wr/fail/err` 行；
- `nabu-display: frames xfer/nswap/nsurf/nok/...` frames 行；
- os_log：`PUB`、`ST`、`CAContext`、`CALayer`、`NativeAdrenoMetal`（含 `NativeAdrenoMetal.bundle`）、`backlight factor`；
- 屏 `screen.bgrx`（比照 `logs/l696-screen.bgrx.bin` 命名保存，本会话不采）；
- CDC `NabuGPU: observation`：本轮未开 `nabu-gpu-observe`，**预期 0 条，不作失败判据**。

**门禁（本会话实测，全部 PASS）**：

| 项 | 命令 / 输出摘要 |
|---|---|
| 五项可启动性 | `python3 tmp/audit_bootability.py hold/l702 hold/l701 --expected=fb3ce142…` → `OK (0 fail, 0 warn)`：FAT `fsck.fat` 双侧 rc=0；四关键文件 `61456/106672/125628416/211568` 全 same；Mach-O `flags=0x0 ncmds=5776` 同；`__PRELINK_INFO` 组件 47/47（无 missing/extra）；payload 体积 201,326,592 同 |
| 结构/大小/defconfig/initrd/关键文件逐项对账 | `tmp/audit_bootability.py:124-194` 五项 + 包内解析：addon `NGPUADD2` count=8、header `total == len`；initrd `SCRATC~1.IMG` 106672 / `3cd3d0d3` 同；`UNIXNU~1.TOM` 1120 / `24ebae84` 同；kernel `fb3ce142`、adt `78a6f7e9`、boot `ead4b531` 逐字节同 ⇒ defconfig/内核构建配置未变（PRELINK 47/47、ncmds 5776 佐证），无新增 `__PRELINK_INFO` 组件 |
| 为什么 addon/trust 变了仍可启动 | addon 只是 8 条目归档，8 条路径/顺序/mode 不变、仅 exe 内 4 字节 + CodeDirectory 32 字节重签、总大小 -8 B 属 zlib 重压缩（解析 `off == len == total`）；trust 仍 61,456 B / 3 模块 / 各模块 `ver=2`、hash 条数 3/2478/76 不变，仅模块 0 第 3 个 cdhash 换成新 exe 的 `a7b7ff5b`，`mktrust_sha256_l702.py:30-42` 的 loader 式校验 `valid True`；kernel/ADT/boot/initrd/toml 全未变 ⇒ 启动链不变 |
| 双审计 | `python -I tmp/audit_dual.py --tag l702 --peer "kernel=fb3ce142,payload=b91cfe98,adt=78a6f7e9,addon=950a3968,trust=a4a6bae6,boot=ead4b531,args=694/38" --mine "…同值…"`（**逗号**分隔）→ `7 items compared, 7 match, 0 differ`；`E:\nabu\state\audit-l702.json:5` `equal: true` |
| args token diff | `python -I tmp/audit_args.py E:/nabu/hold/l701 E:/nabu/hold/l702` → `694/38`、`added: []`、`dropped: []`、`RESULT: OK` |
| 预检 | Windows Python 3.12.10 下 `python -I tmp/precheck.py --pkg E:/nabu/hold/l702 --baseline E:/nabu/hold/l701 …` → 内嵌 token diff OK、`stamp written: E:/nabu/state/precheck.json`；`state/precheck.json:4-14` 的 `chain`/`why_board`/`question`/`window`/`device`/`kernel`/`args` 全部非空；`python -I tmp/precheck_check.py` → `[gate] dual audit OK: audit-l702.json (7 items)`、`[gate] OK`、rc=0 |

**注**：编译侧 fc76 自 11-06 14:57 未消费，本轮板侧自行出包；双审计两侧均为板侧实测（偏离 `AGENTS.md:36-47` 0.6，照 `state/board-report-l701.md:109-110` 注明）。本轮**不新增自造探针、不重编内核、不放宽安全策略**。

## 7. 下一步（仅建议，本会话不执行）

由用户上板后自行写 `logs/rounds/l702.json`；先按 §6 观察项判读，再决定是否继续。若判定仍为否定，按 §4 收窄断点，**不连续两轮问同一问题**（`AGENTS.md:130-134`）。
