# 上板报告 l705（唯一功能变量：给内核已有消费者补 `sheng-vsync=60`）

> 本报告是**上板前**门禁文件；本会话未运行 adb/fastboot、未上板、未改 `E:\nabu\hold\l704\`（只读模板）、未写 `logs/rounds/l705.json`。
> 按 `E:\nabu\AGENTS.md` 0.5 / 0.6 / 0.7 / 0.9 / 0.10 编写；格式照 `E:\nabu\state\board-report-l704.md`。

## 0. 先过 args 预算（≤700 硬上限，先算才出包）

以 `E:\nabu\hold\l704\payload.img` 内 `UNIXNU~1TOM`（1118 B，簇 12688）实测：args **692 字符 / 38 token**
（`E:\nabu\tmp\l705-build-20261110\gate3-token-by-token.txt`）。把 `nabu-gpu-observe=1`（18 B）
**等长原位替换**为 `sheng-vsync=60`（14 B）⇒ **692 → 688 字符 / 38 token**（≤700），
toml **1118 → 1114 B**，仍在 2048 B 单簇内。`added=['sheng-vsync=60']`、
`dropped=['nabu-gpu-observe=1']`，其余 **37 token 同序同字节**（逐 token 零 mismatch）。

受保护 token 全保留：`sheng-fb-gpu=1`、`t733-managed-rt-normal=1`、`sheng-gpu-addon=1`、
`sheng-smp=1` + `nabu-wdt-arm-delay=0`、`-apfs_shared_datavolume`、
`offline-legacy-{register,gop,validate,transfer,pm,phase,retire}=1`、`native-ufs-{root,diagnostics}=1`、
`nabu-fbcon=0`、`-v serial=3 debug=0x14e`、无 `nabu-autolog`。
删掉的 `nabu-gpu-observe=1` 是**本轮明确要让位的观察开关**（内核有消费者，但其输出
`NabuGPU: observation` / GPU7 通道在本轮不作判据；见 §4 注）。

## 1. 唯一问题（一句话）

**`sheng-vsync=60` 给出的 vsync tick，能否让 CoreAnimation / render server 进入帧提交循环？**
判据（运行期）：`native_legacy_vsync_observed` 是否非零、os_log
`reevaluateDisplayLink` 是否出现 `wants:YES` / 非 `no-op`、`nabu-display: frames` 是否从 **5** 持续上涨、
`screen.bgrx` 是否不再是 `ca18b728a7def68e…`。其余一切不再问：不加探针、不重编内核、不放宽安全策略。

## 2. 已闭合到哪（日志优先 + 静态链 + 为什么不是重复 l684）

1. **§7s / §7t 已闭合的前提**（`E:\nabu\state\HANDOVER-20261110-l696.md:1126-1280`）：
   - l702 修好插件 `publish::requested()` 的 4 字节 `strb`/`add` bug 并重签整链（CD→cdhash→trust→addon→payload）。
   - l703 官方 observation 实证 published GPU 设备运行：`metal_open_slot=2 backboardd type=00004148 foreign=1`、
     `submitted=15 completed=15`、`metal_calls=738`、method 1/2/3/8/11/15 `failures=0`（`logs/rounds/l703.json`）。
   - l704 加 `sheng-fb-gpu=1` 后打出 `nabu: scanout hand-off online gop=9c000000/fa0000 1600x2560 stride=1900`；
     `logs/l704-v1.jsonl`（108,087 行）证 published 早于 backboardd ~34 s、早于 WS ready ~74 s，
     `CARenderServerStart` / `CAContext` / `CALayer` / `MTLDevice` 仍全 0。
   - **但屏四轮不变**：`screen.bgrx` sha `ca18b728a7def68e`（iBoot Apple Logo 残留），
     `nabu-display: frames xfer=5 nswap=5 nsurf=2 nok=5 ncpy=5 nret=5`（l703 为 8），
     四轮 os_log 是 `BKDisplayResetFlipBookEnabled flipBookIsEnabled:NO` 与
     `reevaluateDisplayLink: wants:NO devices:NO ... no-op <main>` ⇒ **合成器注册了但没进帧循环**。
2. **静态消费者链（本会话板上内核现量）**：`E:\nabu\hold\l704\kernel.macho`（sha `fb3ce142`，
   与 l705 逐字节同）实测 `sheng-vsync` 字面量 **2 命中 @1615561 / @1617104**；`startVsync` 符号在；
   `_native_legacy_vsync_observed` @`fffffff007a80814`（文件偏移 89928030 / 92751434）、
   `_native_legacy_vsync_state`、`_native_legacy_vsync_notify` 全在
   （复核脚本输出见 `E:\nabu\tmp\l705-build-20261110\`）。⇒ **零新探针**，vsync 是内核已有消费者。
3. **为什么缺 `sheng-vsync` 就是断点**：Qoder
   `C:\Users\Administrator\.qoder-cn\projects\E--unix\memory\project-nabu-springboard-path-20261106.md:215-218`
   引 `drivers/GopFramebuffer/NativeLegacyDisplay.cpp:123-128` 原文
   「without it the render server never saw a vsync」；`startVsync()`（:371）`if(!hz || hz>240 …) return;`
   （hz 来自 `sheng-vsync`，历轮 args 从来没有）⇒ `vsyncTimer` 恒空；`armVsync()`（:354）
   `if(!vsyncTimer || vsyncArmed || !vsyncWanted()) return;` ⇒ 客户端注册 vsync 也不 arm。
   与上面四轮 `wants:NO … no-op` 自洽。
4. **为什么这一轮不是重复 l684（写进 chain / why_board）**：
   `E:\nabu\state\HANDOVER-20261110-l696.md:191,274` 与 Qoder 同文档 `:214-222` 记 l684 试过
   `sheng-vsync=60` 并判「单独不改 frames」。但 `E:\nabu\logs\rounds\l685.json` / `l686.json` 证明那是
   **AMFI/trust 旧状态**：`unsuitable CT policy 0 for this platform/device, rejecting signature`、
   `code signature validation failed`、`frames 4→5`、插件 4 字节 bug 未修、CA surface import 被
   `HostMemory/NotRepresentable` 拒、owner trust 未过、无 published 设备、无 `sheng-fb-gpu`、
   无 `t733-managed-rt-normal` 后的 handoff。今轮 §7s/§7t 的前提**全部已变**，旧否证是旧条件，不适用。
5. **并列/下游断点须并列记着**：§7t.2 的 `no-copy buffer refused: HostMemory/NotRepresentable`
   （`logs/l704-v1.jsonl:53721`）把 l704 的 GPU handoff 断点钉在 `Presenter::target_`；
   §7t.4 判主墙仍在 CA 没产出可渲染层，GPU handoff 是并列/下游断点。本轮只补 vsync tick，不动该层。

## 3. 为什么离线不行（AGENTS.md 0.5 `--why_board`）

静态能证：消费者字面量/符号与地址、`startVsync()` 的 hz 门、`armVsync()` 的 arm 门、
§7s/§7t 的 published 设备与 handoff 前提、l684 与今轮的条件差异、以及本包只改一个功能 token。
但 `native_legacy_vsync_observed` 的**计数值**、`nabu-display: frames` 是否从 5 持续上涨、
`reevaluateDisplayLink` 是否变成 `wants:YES`、`CARenderServerStart` / `CAContext` / `CALayer` 是否出现、
`screen.bgrx` 的**像素**，全是内核计时器与 render server 在真机上的运行期交互结果；离线无寄存器/时序/帧内容，
无法计算。且 Qoder 两条相反预测（vsync 必需 vs `IOMFBDisplayRefresh` 自 pace，见 §4）静态无法裁决。
⇒ 属 0.5 允许的「验证静态结论在真机上是否成立的那**一次**判定」。

## 4. 失败也有信息（三种判读 + 不隐藏的矛盾）

| 结果 | 判读 |
|---|---|
| `native_legacy_vsync_observed` **非零**，但 `frames` 仍恒 5、屏仍 `ca18b728…` | **vsync tick 已发出而 CA 不产帧** ⇒ 判据正面偏废 `sheng-vsync` 单参数解药，断点**转 CA / render server**（合成层无内容、`CAEnableDeepFramebuffer:false`、`reevaluateDisplayLink` 仍 no-op）；同时 §7t.2 的 no-copy import 仍是并列断点。下一步查 CA 层，不再重复 vsync |
| `native_legacy_vsync_observed` 非零且 `frames` **从 5 持续上涨**、os_log `wants:YES`，但屏仍 `ca18b728…` | **帧循环已起而屏不变** ⇒ 断点**转 scanout / 合成内容**（swap 内容、Presenter import、panel 路径），与 §7t.2 的 `HostMemory/NotRepresentable` 合流 |
| `frames` 涨、`reevaluateDisplayLink` 非 no-op、`screen.bgrx` **离开 `ca18b728…`** | **达成**：vsync tick 让 CA 进帧循环并换屏；本轮闭合，下一轮只读首帧质量 |
| `native_legacy_vsync_observed` 恒 0 | 开关在真机未生效 / timer 未 arm，先对账 `unixnu.toml` 与 `startVsync()`，不得把「没采到」当「没发生」 |

**矛盾记录（不隐藏）**：`C:\Users\Administrator\.qoder-cn\projects\E--unix\memory\project-nabu-display-open-current.md:271`
说 render server 用 `IOMFBDisplayRefresh`（默认 1/60）自己 pace、**vsync 非必需**，`native_legacy_vsync_observed`
是空桩；板上 `E:\nabu\hold\l704\kernel.macho` 确有 `IOMFBDisplayRefresh` 字面量 **@96465699**（本会话复核）。
这与 §2.3「缺 vsync 收不到 tick」正面冲突：若 :271 成立，本轮应是「非零事件 + frames 仍 5」；若 §2.3 成立，
应见 frames 上涨。两种预测都写进上表，**由运行期裁决**，不预设结论。

**判据排除项**：本轮**没开** `nabu-gpu-observe` ⇒ CDC 里 `NabuGPU: observation` 预期 **0 条**，
不是失败判据；`PF/PX` 走 GPU7 标记流而当前 shipped kernel 的 `nabu_capture_append` 是空实现
（§7t.2 / `nabu_adreno_step1_compat.cpp:25-29`），**无输出通道**，也不是判据；
`No Default FB Surface found`、`discovery_reads=0`、`PUB/ST` 同 §7s.3 排除。

**观察项（全部已有输出，零新增 printf/IOLog/计数器/采样/探针）**：CDC
`native_legacy_vsync_observed` / `nabu-display: frames xfer nswap nsurf nok ncpy nret` /
`nabu: scanout hand-off online` / `nabu-gpu-addon: mounted/target/control/status` / panic；
os_log `BKDisplayResetFlipBookEnabled`、`reevaluateDisplayLink`、`CARenderServerStart` / `CAContext` / `CALayer`；
`screen.bgrx`（对比 `ca18b728…`）。

## 5. 唯一变量与回滚

**唯一功能变量 = args 加 `sheng-vsync=60`，等长去掉 `nabu-gpu-observe=1`**；688/38，其余 37 token 同序同字节。
**五文件与 `hold/l704` 逐字节相同**：

| 文件 | l704 sha8 | **l705 sha8** | 说明 |
|---|---|---|---|
| kernel.macho | `fb3ce142` | `fb3ce142` | 逐字节相同（未重编） |
| kernel.macho.adt | `78a6f7e9` | `78a6f7e9` | 逐字节相同 |
| gpu-addon.bin | `950a3968` | `950a3968` | 逐字节相同（l702 4 字节修复版） |
| owner-trust.bundle | `a4a6bae6` | `a4a6bae6` | 逐字节相同 |
| boot-merge-r3.img | `ead4b531` | `ead4b531` | 逐字节相同 |
| payload.img | `e7fe1c8a` | **`aa1b47e7`** | 唯一变化；包内仅 `UNIXNU~1TOM` 变（1118→1114 B，`5bb739b0`→`9cd56086`） |

手术：`E:\nabu\tmp\l705-build-20261110\build_l705.py` 以**只读 mmap ACCESS_READ** 打开 l704 模板
（绝不写输入 payload，历史坑见 `state/board-report-l695p.md`），2048 B 簇 12688 **整簇替换**
（slack 全零、FAT 链不变）+ 目录项 size `1118 → 1114`（偏移 804028）。全 payload 仅 **289 字节不同**
（288 在 toml 簇内 + 1 字节 size 字段）。payload 内抽回复核：
`KERNEL~1MAC fb3ce142` / `KERNEL~1ADT 78a6f7e9` / `KERNEL~1GPU 950a3968` /
`KERNEL~1TRU a4a6bae6` / `SCRATC~1IMG 3cd3d0d3` 与 l704 全同，仅 `UNIXNU~1TOM` 变
（`E:\nabu\tmp\l705-build-20261110\payload-inner-compare.txt`）。

- **回滚基线**：`E:\nabu\hold\l700\`（kernel `efcb4c34`，已验证 4 分钟窗口）；
  **上一包** `E:\nabu\hold\l704\`（kernel `fb3ce142`、payload `e7fe1c8a`，同窗可用，本会话只读未污染）。
- **回滚命令（仅文字，不执行）**：`fastboot flash cust hold/l700/payload.img`。

## 6. 恢复预案 / 门禁 / 附注

**AGENTS.md 0.10 绝对红线：不碰 `boot_a`**（绝不 `fastboot flash boot` / `flash boot_a`；recovery 就在 boot 里）。
流程（**仅文字，本会话不执行**）：

```
recovery arm misc
fastboot flash cust hold/l705/payload.img
fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin   # flash 后重 arm
fastboot boot hold/l705/boot-merge-r3.img                      # 临时加载，不写任何分区
```

`E:\nabu\dwc3\build\misc-bootrecovery.bin`（4096 B，本会话确认存在）；设备 `TWRP recovery f91fa664`。
`arm-delay=0`，咬点 **380–410 s**，观察完整 **4 分钟窗口**（`AGENTS.md:57-64`），早 panic 也有重置路径。
若 fastboot 全失败：人工长按电源 10 秒。回滚见 §5。

**门禁（本会话实测，全部 PASS；证据见 §7）**：

| 项 | 结果 |
|---|---|
| ① `fsck.fat -n` 双侧 | l704/l705 均 `10 files, 67550/97913 clusters` rc=0；`python3 -I tmp/audit_bootability.py hold/l705 hold/l704 --expected=fb3ce142…` → **`OK (0 fail, 0 warn)`**（四关键文件 same、Mach-O `flags=0x0 ncmds=5776`、PRELINK 47/47、payload 201326592 同） |
| ② `audit_dual.py --tag l705` | 真实 sha `kernel=fb3ce142,payload=aa1b47e7,adt=78a6f7e9,addon=950a3968,trust=a4a6bae6,boot=ead4b531,args=688/38` → `7 items compared, 7 match, 0 differ`；`state/audit-l705.json` `equal:true` |
| ③ `audit_args.py hold/l704 hold/l705` | `692/38 → 688/38`（≤700）；`added=['sheng-vsync=60']`、`dropped=['nabu-gpu-observe=1']`；带 `--expect-added/--expect-dropped` → **`RESULT: OK`** rc=0；逐 token 37/37 同序同字节、零 mismatch；`sheng-smp=1`+`nabu-wdt-arm-delay=0` 成对仍在 |
| ④ Windows Python 3.12.10 `precheck.py` | `tmp/l705-build-20261110/run_precheck_l705.py` 驱动 `--pkg E:/nabu/hold/l705 --baseline E:/nabu/hold/l704 --expect-added sheng-vsync=60 --expect-dropped nabu-gpu-observe=1` → `stamp written: state/precheck.json`，rc=0；八字段 `question/chain/why_board/window/kernel/args/device/baseline` 全非空；`python -I tmp/precheck_check.py` → **`[gate] OK`** rc=0，`dual audit OK: audit-l705.json (7 items)` |
| ⑤ 六问板报 | 本文件 `state/board-report-l705.md` |
| 自检 | 抽回 `UNIXNU~1TOM` 逐 token 只差两 token；抽回 kernel/addon/trust/adt/scratch 与 l704 逐字节 SAME；`hold/l704/` 六文件 sha 前后未变（payload `e7fe1c8a`） |

## 7. 证据路径（全部绝对路径）

- 构建与自检：`E:\nabu\tmp\l705-build-20261110\build_l705.py`、`payload.img`、
  `extract-l705\UNIXNU~1TOM.bin`（1114 B `9cd56086`）、`run_precheck_l705.py`、
  `l704-baseline-sha.txt`、`payload-inner-compare.txt`、`gate3-token-by-token.txt`
- 门禁输出：`E:\nabu\tmp\l705-build-20261110\gate1-fsck.txt`、`gate1-bootability.txt`、
  `gate2-audit_dual.txt`、`gate3-audit_args.txt`、`gate4-precheck.txt`、`gate4-precheck_check.txt`；
  戳 `E:\nabu\state\audit-l705.json`、`E:\nabu\state\precheck.json`、`E:\nabu\state\board-report-l705.md`
- 板侧基线：`E:\nabu\logs\l704-cdc.bin`、`E:\nabu\logs\l704-diag-tree\screen.bgrx`（`ca18b728…`）、
  `E:\nabu\logs\l704-v1.jsonl`、`E:\nabu\logs\rounds\l68{4,5,6}.json`、`E:\nabu\logs\rounds\l70{2,3,4}.json`
- 静态链：`E:\nabu\hold\l704\kernel.macho` @1615561/1617104/96465699/89928030；
  `E:\nabu\state\HANDOVER-20261110-l696.md:191,274,1126-1280`；
  `C:\Users\Administrator\.qoder-cn\projects\E--unix\memory\project-nabu-springboard-path-20261106.md:214-222`；
  `C:\Users\Administrator\.qoder-cn\projects\E--unix\memory\project-nabu-display-open-current.md:271`；
  `E:\nabu\state\board-report-l695p.md:35`、`E:\nabu\state\board-report-l704.md`
- 参考：`E:\nabu\AGENTS.md:14-24,36-47,57-64,100-109`

**注**：编译侧 fc76 自 **11-06 14:57** 未消费，本轮板侧自行出包；双审计两侧均为板侧实测
（偏离 `AGENTS.md:36-47` 0.6，照 `state/board-report-l704.md:95`、`board-report-l703.md:95` 注明）。
本轮**零新探针**（`sheng-vsync` 是内核已有消费者）、**零重编内核**、**零安全策略放宽**。

## 8. 下一步（仅建议，本会话不执行）

由用户上板后自行写 `logs/rounds/l705.json`（本会话**不写**）；先按 §4 四判读表读
`native_legacy_vsync_observed` / `frames` / `reevaluateDisplayLink` / `screen.bgrx`，再决定是否继续；
不连续两轮问同一问题（`AGENTS.md:130-134`）。
