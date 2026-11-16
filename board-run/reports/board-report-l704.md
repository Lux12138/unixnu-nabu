# 上板报告 l704（唯一功能变量：打开内核已有 GPU hand-off 开关 `sheng-fb-gpu=1`）

> 本报告是**上板前**门禁文件；本会话未运行 adb/fastboot、未上板、未改 `hold/l703/`（只读模板）、未写 `logs/rounds/l704.json`。
> 按 `E:\nabu\AGENTS.md` 0.5 / 0.6 / 0.7 / 0.9 / 0.10 编写；格式照 `state/board-report-l703.md`。

## 0. 先过 args 预算（≤700 硬上限，先算才出包）

l703 实测 **698 字符 / 38 token**（`tmp/l703-build-20261110/extract-l703/UNIXNU~1TOM.bin`）。直接加
` sheng-fb-gpu=1`（15 字符含分隔空格）⇒ **713 字符 / 39 token**，超限 13。逐项核过：

| 候选 | 长度 | 省 | 历史/内核核对 | 结论 |
|---|---|---|---|---|
| `sheng-build-tag=l700`（用户所称 `tag=l700`，历史轮次标记） | 20 | **21**（含分隔空格） | `hold/l703/kernel.macho` 中 `sheng-build-tag` **0 命中**（boot/ADT/addon/trust 也 0），仅被 CDC 整行回显（`logs/l703-cdc.bin` 偏移 93949）当带内凭据；`bridge/inbox-33dd/20261103-0105-…l456b.md:29`「改短、别删，内核确实不消费它」；`tmp/args_swap.py` 有 `--drop-tag`，`tmp/led_l348_l349.py:31` 明记「sheng-build-tag dropped to buy the 14 chars; tag is behaviour-neutral」，`bridge/inbox-33dd/20261104-0230-8714-l515e-yi7-values.md:9` 有实删先例 | **选中：原位替换**（20 B → 14 B）。缩短成 `tag=l704` 会在 `audit_args` 里多出一个 `added`，与门禁③「`added=['sheng-fb-gpu=1']`」冲突；保留则必须删受保护功能开关，不可。去掉后带内识别由本轮唯一的 `sheng-fb-gpu=1`（同一 CDC args 行）承担 |
| `nabu-fbcon=0` | 12 | 13（正好 700） | 内核 2 命中；`bridge/outbox/20261104-2240-d46a-fbcon-comment-vs-code.md:22`「标成**不可删**（和 `serial=3` 同级），删掉整轮像素读数作废」；`tmp/args_swap.py` LIVE 保护表记录 w840/l563b 事故；`HANDOVER-20261105-compile-side-fc76.md:104` 亦列不可删 | **保留**：本轮 `screen.bgrx` 是主判据 |
| `debug=0x14e` | 11 | 12 | 省不够 13；调试输出掩码，panic/CDC 可读性靠它；历史各轮 args 表逐字沿用 | 保留 |
| `serial=3` | 8 | 9 | 省不够；串口控制台/CDC 正控，与上同列「不可删」 | 保留 |
| `-v` | 2 | 3 | 省不够；`HANDOVER-20261105-compile-side-fc76.md:104` 列不可删 | 保留 |
| 其余 33 token | — | — | 12 个受保护开关（`nabu-gpu-observe=1`、`t733-managed-rt-normal=1`、`sheng-gpu-addon=1`、`nabu-wdt-arm-delay=0`、`-apfs_shared_datavolume`、`offline-legacy-*`、`native-ufs-*`、`sheng-smp=1`、`nabu-usb-reboot=1`、`sheng-aks=1`）及日志/UFS/PM 项，kernel 内各有 1–2 处消费者 | 全保留 |

**可行**：把 `sheng-build-tag=l700`（20 B）**原位缩短替换**为 `sheng-fb-gpu=1`（14 B）⇒
**698 → 692 字符 / 38 token**（≤700），`added=['sheng-fb-gpu=1']`、`dropped=['sheng-build-tag=l700']`，
其余 37 token 逐字节同序相同，功能语义零丢失（删掉的只是内核 0 命中的标签），
toml `1124 → 1118` B 仍在 2048 B 单簇 12688 内。**没有硬凑、没有删关键开关、没有改内核**。

## 1. 唯一问题（一句话）

**l703 已实证 GPU 侧 scanout 线程活着（0x4148 连接 + 15 submitted），但屏仍是 iBoot Logo；只把
`sheng-fb-gpu=1` 打开（保留 `nabu-gpu-observe=1`），内核显示驱动是否把帧交给 GPU client
（`nabu: scanout hand-off online` + GPU7 的 `P0/P1/P2/PF/PX`），`nabu-display: frames` 与
`screen.bgrx` 是否随之改变？** 其余一切不再问：不加探针、不重编内核、不放宽安全策略。

## 2. 已闭合到哪（日志优先 + 静态链）

1. **publish 侧已修、设备活跃**：`E:\nabu\logs\l703-cdc.bin` 第 **4/4** 次 +120 s：
   `NabuGPU: observation=4/4 since_ready_us=120001688 submitted=15 completed=15`；
   `metal_open_slot=2 process=backboardd type=00004148 foreign=1`、`metal_calls=738 … ext_entries=1560`、
   `ext_entries`、method 2/3/8/11 全 `failures=0`。⇒ `publish::requested()` 4 字节修复后的 published
   分支**已在真机跑起来**，scanout 线程已开 hand-off-only 0x4148 连接。
2. **但屏未变**：`E:\nabu\logs\l703-diag-tree\screen.bgrx` sha `ca18b728…` 与
   `E:\nabu\logs\l702-screen.bgrx.bin` **逐字节同**（本会话复测两值相等）；
   `nabu-display: frames xfer=8 nswap=8 nok=8 ncpy=8 nret=8`。CDC 中
   `nabu: scanout hand-off online` **0 行**（本会话复测）⇒ 缺口正是「内核没把帧 offer 出去」。
3. **静态消费者链**：
   - `E:\nabu\hold\l703\kernel.macho` 偏移 **1617122** 含 `sheng-fb-gpu` + 格式串
     `nabu: scanout hand-off online gop=%llx/%llx %ux%u stride=%llx`（本会话复测 `sheng-fb-gpu` 1 命中 @0x18ace2）。
   - `/root/merge/UniXNU-nabu/drivers/GopFramebuffer/NativeGOPPresenter.cpp:94`
     `PE_parse_boot_argn("sheng-fb-gpu",&v,…);gpuMode=(v==1)`，下一行 `if(gpuMode)gHandoff=this`；
     `:53` `NativeLegacyDisplay.cpp` 与 `:9` `NativeGOPWire.h`：开后显示驱动把帧**交给**特权 GPU client，
     由 client 做 GPU copy 再 retire swap，**不自己 copy**；`NGP_METHOD_WAIT_SWAP=2` / `COMPLETE_SWAP=3`。
   - `/root/fresh/UniXNU-latest/drivers/AdrenoMetal/NativeMetalBridgePhase.h:85,104-139`：published
     `serveScanout()` 用 `openFn(...,0x4148,...)` 拿 hand-off-only 连接 → `client.open()` →
     `while(client.serveOne(500))`，GPU7 标记流输出 `P0/P1/P2`（step）、`PF <frames>/<failures> …`、
     `PX <frames>/<failures> …`；注释明确「kernel offers them only with `sheng-fb-gpu=1`」。
     ⇒ **`sheng-fb-gpu=1` 是「显示驱动交帧」的唯一开关**；l703 的 0x4148 是客户端已就位、等内核 offer。
4. **已排除的非判据**（用户现量，本会话未重新调查）：`No Default FB Surface found`（`IOMFB` 预链 kext）
   每轮恰好 1 次、含 l652 白页轮，随后 `nodef=16384000` 说明 surface 建成，**不是墙**
   （`E:\nabu\state\HANDOVER-20261110-l696.md:233`）；`PUB/ST` 无 CDC 输出通道、
   `discovery_reads=0` 是死计数器，都不能当判据。
5. **为什么旧否证不适用（条件已变）**：`E:\nabu\state\HANDOVER-20261110-l696.md:278` 把
   `sheng-fb-gpu=1` 列为「已评估、不再重复的死路」，是 **l696 旧内核 `efcb4c34` / 旧 stage-1 语境**：
   当时 `publish::requested()` 未修、插件未走 published 分支、没有 0x4148 连接，
   且 `l696` 连残留都被 blank swap 覆盖；`E:\nabu\state\board-report-l695p.md:35` 引用的
   alioth `:369`「frames stop short of the panel … sheng-fb-gpu=1 did not move the swap counter」同样是
   **旧 alioth 路径 + backboardd `0x106da915c` SIGBUS 崩溃循环**语境。本轮内核已换成 `fb3ce142`
   （与 l703 逐字节同），l703 实测 **无该崩溃、0x4148 已开、submitted=15 completed=15、
   method 2/3/8/11 failures=0**；客户端侧前提首次成立，故「内核是否 offer 帧」是**新的、未被否证过的一次判定**。

## 3. 为什么离线不行（AGENTS.md 0.5 `--why_board`）

静态能证：消费者字符串偏移、`gpuMode` 门、offer/retire 语义、0x4148/`serveOne(500)`/`P0/P1/P2/PF/PX`
输出字段、以及本包只改一个功能 token。但 `nabu: scanout hand-off online` 是否出现、`PF/PX` 的
frames/failures 读数、第 4/4 次 observation、`nabu-display: frames` 的 `xfer/nswap/nok/ncpy/nret`、
`screen.bgrx` 像素，全是内核与 GPU client 在真机上的**运行期交互结果**（帧、时序、swap 完成数），
离线无寄存器/帧内容，无法计算。l703 日志只证「设备活跃、屏未变」，对打开开关后的行为沉默。
`nabu-gpu-observe=1` 保留（官方观察通道，读 `PF/PX` 与第 4/4 次 observation）⇒ 属 0.5 允许的**一次**板上判定。

## 4. 失败也有信息（三种判读，不是「再看看」）

| 结果 | 判读 |
|---|---|
| 有 `nabu: scanout hand-off online` 且 GPU7 出现 `P2`/`PF`（frames>0, failures=0），`frames` 的 `ncpy` 让位给交帧语义、`screen.bgrx` 离开 `ca18b728…` | hand-off 链路通、UI 首次上屏；断点已闭合，下一轮只需读 CA/Metal 首帧质量 |
| `hand-off online` **有**、`PF/PX` 出现但 `frames` 仍是 `xfer=8 nswap=8 nok=8 ncpy=8 nret=8` 且 `screen.bgrx` 仍是 `ca18b728…` | 内核已 offer、client 已服务，但帧没到 panel：`PF` 的 `r<last result>` / `v<verified>/<differing rows>` 给出失败点（映射/import/verify），断点收窄到 scanout 映射与 swap retire；与 alioth 旧否证**同名但不同层**，是新事实 |
| `hand-off online` **0 行**或 GPU7 只有 `P0/P1`（`PX` 早退），frames 与 screen 全不变 | 开关在真机未生效或 offer 被拒（`nabu: scanout hand-off refused`）；须先对账 `unixnu.toml`/`gHandoff`，**不再重复试 published 侧** |

每种结果都给出可定位的新事实。附带：CDC 若 `NabuGPU: observation=4/4` 全无 ⇒ 包/装载问题，先对账再判 GPU 结论，不得把「没采到」当「没发生」。

## 5. 唯一变量与回滚点

**唯一功能变量 = args 新增 `sheng-fb-gpu=1`**；为守 700 上限把行为中性标签
`sheng-build-tag=l700` 原位缩短替换（20 B → 14 B），**698 → 692 字符 / 38 token**，
`added=['sheng-fb-gpu=1']`、`dropped=['sheng-build-tag=l700']`，其余 37 token 逐 token 同序相同；
`nabu-gpu-observe=1` 保留；`sheng-smp=1` + `nabu-wdt-arm-delay=0` 成对保留；12 个受保护开关一个不少；
无 `nabu-autolog`。手术：`E:\nabu\tmp\l704-build-20261110\build_l704.py` 以 **只读 mmap ACCESS_READ**
打开 l703 模板（绝不写输入，避免 l695p 污染），2048 B 簇 12688 **整簇替换**（slack 全零、FAT 链不变）
+ 目录项 size `1124 → 1118`（偏移 804028），输出新文件；全 payload 仅 261 字节不同（260 在 toml 簇内 + 4 字节 size 字段）。

| 文件 | l703 sha8 | **l704 sha8** | 说明 |
|---|---|---|---|
| boot-merge-r3.img | `ead4b531` | `ead4b531` | 逐字节相同 |
| kernel.macho | `fb3ce142` | `fb3ce142` | 逐字节相同（未重编） |
| kernel.macho.adt | `78a6f7e9` | `78a6f7e9` | 逐字节相同 |
| gpu-addon.bin | `950a3968` | `950a3968` | 逐字节相同 |
| owner-trust.bundle | `a4a6bae6` | `a4a6bae6` | 逐字节相同 |
| payload.img | `96da4263` | **`e7fe1c8a`** | 唯一变化；包内仅 `UNIXNU~1TOM` 变（`f1d61f79` → `5bb739b0`，1124→1118 B） |

payload 内抽回复核（`E:\nabu\tmp\l704-build-20261110\extract-l704\`）：`KERNEL~1MAC fb3ce142` /
`KERNEL~1ADT 78a6f7e9` / `KERNEL~1GPU 950a3968` / `KERNEL~1TRU a4a6bae6` /
`SCRATC~1IMG 3cd3d0d3` 与 l703 **全同**；抽回 toml 逐 token 核对如上。
`hold/l703/` 六文件 sha 完成后复测未变（payload `96da4263`、addon `950a3968`、trust `a4a6bae6`、kernel `fb3ce142`）。

- **回滚基线**：`hold/l700/`（kernel `efcb4c34`，已验证 4 分钟窗口）；**上一包** `hold/l703/`
  （kernel `fb3ce142`、payload `96da4263`，同窗可用，本会话只读未污染）。
- **回滚命令（仅文字，不执行）**：`fastboot flash cust hold/l700/payload.img`。

## 6. 恢复预案 / 观察项 / 门禁

**AGENTS.md 0.10 绝对红线：不碰 `boot_a`（绝不 `fastboot flash boot` / `flash boot_a`）**。流程（仅文字）：

```
recovery arm misc
fastboot flash cust hold/l704/payload.img
fastboot flash misc misc-bootrecovery.bin   # flash 后重 arm
fastboot boot hold/l704/boot-merge-r3.img  # 临时加载，不写任何分区
```

`misc-bootrecovery.bin` 在 `E:\nabu\dwc3\build\misc-bootrecovery.bin`（4096 B，本会话已确认存在）。
设备 `TWRP recovery f91fa664`。`arm-delay=0`，咬点 **380–410 s**，观察完整 **4 分钟窗口**
（`AGENTS.md:57-64`），覆盖 +120 s 的 4/4 次 observation；早 panic 也有重置路径。
若 fastboot 全失败：人工长按电源 10 秒。回滚命令见 §5。

**观察项（全部是已有输出，零新增 printf/IOLog/计数器/采样/探针）**：
- CDC `nabu: scanout hand-off online gop=…`（及 `… refused`）；
- GPU7 标记流 `P0/P1/P2`、`PF <frames>/<failures> r… v… t…`、`PX …`（`nabu-gpu-observe=1` 官方通道保留）；
- `NabuGPU: observation=4/4`（+120 s）、`metal_open_slot=2 … type=00004148`、`metal_calls`/`submitted`/`completed`；
- `nabu-display: frames xfer/nswap/nok/ncpy/nret`（对比 l703 的 8/8/8/8/8）、`nabu-gpu-addon: mounted/target/control/status`；
- `screen.bgrx`（对比 `ca18b728…`）、panic。

**门禁（本会话实测，全部 PASS；证据见 §7）**：

| 项 | 结果 |
|---|---|
| ① `fsck.fat -n` 双侧 | l703/l704 均 `10 files, 67550/97913 clusters` rc=0；`python3 -I tmp/audit_bootability.py hold/l704 hold/l703 --expected=fb3ce142…` → **`OK (0 fail, 0 warn)`**（四关键文件 same、Mach-O `flags=0x0 ncmds=5776`、PRELINK 47/47、payload 201326592 同） |
| ② `audit_dual.py --tag l704` | 逗号分隔真实 sha：`kernel=fb3ce142,payload=e7fe1c8a,adt=78a6f7e9,addon=950a3968,trust=a4a6bae6,boot=ead4b531,args=692/38` → `7 items compared, 7 match, 0 differ`；`state/audit-l704.json` `equal:true` / 7 items |
| ③ `audit_args.py E:/nabu/hold/l703 E:/nabu/hold/l704` | `698/38` → `692/38`（≤700）；`added=['sheng-fb-gpu=1']`、`dropped=['sheng-build-tag=l700']`；带 `--expect-added/--expect-dropped` → **`RESULT: OK`**；`sheng-smp=1`+`nabu-wdt-arm-delay=0` 仍在，关键开关一个不少 |
| ④ Windows Python 3.12.10 `precheck.py` | `tmp/l704-build-20261110/run_precheck_l704.py` 驱动 `--pkg E:/nabu/hold/l704 --baseline E:/nabu/hold/l703 --expect-added sheng-fb-gpu=1 --expect-dropped sheng-build-tag=l700` → `stamp written: state/precheck.json`；`question`/`chain`/`why_board`/`window`/`kernel`/`args`/`device` 全非空；`python -I tmp/precheck_check.py` → **`[gate] OK`** rc=0，`dual audit OK: audit-l704.json (7 items)` |
| ⑤ 六问板报 | 本文件 `state/board-report-l704.md` |
| 自检 | payload 抽回 toml 逐 token 只差两 token；抽回 kernel/addon/trust/adt/scratch 与 l703 `cmp` SAME；`hold/l703/` 六文件 sha 前后未变 |

## 7. 证据路径

- 构建：`E:\nabu\tmp\l704-build-20261110\build_l704.py`（只读输入）、`run_precheck_l704.py`、
  `payload.img`、`extract-l704\UNIXNU~1TOM.bin`（1118 B `5bb739b0`）及五件抽回件
- 门禁戳：`E:\nabu\state\audit-l704.json`、`E:\nabu\state\precheck.json`、`E:\nabu\state\board-report-l704.md`
- 板侧基线：`E:\nabu\logs\l703-cdc.bin`、`E:\nabu\logs\l703-diag-tree\screen.bgrx`（`ca18b728…`）、
  `E:\nabu\logs\l702-screen.bgrx.bin`（同 sha）、`E:\nabu\state\HANDOVER-20261110-l696.md:233,278`、
  `E:\nabu\state\board-report-l695p.md:35`、`E:\nabu\state\board-report-l703.md`
- 静态链：`E:\nabu\hold\l703\kernel.macho`@1617122；
  `/root/merge/UniXNU-nabu/drivers/GopFramebuffer/NativeGOPPresenter.cpp:94`、
  `NativeLegacyDisplay.cpp:53`、`NativeGOPWire.h:9`；
  `/root/fresh/UniXNU-latest/drivers/AdrenoMetal/NativeMetalBridgePhase.h:85,104-139`；
  `E:\nabu\upstream\gpu\upstream\Adreno6xx\NativeAdrenoPublish.cpp:75-81`（0x4148 定义）
- 参考：`E:\nabu\AGENTS.md:14-24,36-47,57-64,85-98,100-109`、
  `E:\nabu\tmp\l703-build-20261110\grow_l703.py`（l695p 教训、只读 mmap、整簇替换）

**注**：编译侧 fc76 自 **11-06 14:57** 未消费，本轮板侧自行出包；双审计两侧均为板侧实测
（偏离 `AGENTS.md:36-47` 0.6，照 `state/board-report-l703.md:95`、`board-report-l702.md:86` 注明）。
本轮**零新探针**（`nabu-gpu-observe=1` 是官方观察通道、`sheng-fb-gpu=1` 是内核已有开关）、
**零重编内核**、**零安全策略放宽**。

## 8. 下一步（仅建议，本会话不执行）

由用户上板后自行写 `logs/rounds/l704.json`（本会话**不写**）；先按 §4 三判读表读
`hand-off online` / `PF/PX` / `frames` / `screen.bgrx`，再决定是否继续；
不连续两轮问同一问题（`AGENTS.md:130-134`）。
