# 上板报告 l706（一次性临时插帧轮：板上内核已有的 `sheng-fb-fill=4` write-path test）

> 本报告是**上板前**门禁文件；本会话未运行 adb/fastboot、未上板、未改 `E:\nabu\hold\l705\`（只读模板）、
> 未写 `logs/rounds/l706.json`。按 `E:\nabu\AGENTS.md` 0.5 / 0.6 / 0.7 / 0.9 / 0.10 编写；
> 格式照 `E:\nabu\state\board-report-l705.md`。

## 0. 用户新授权与 args 预算（≤700 硬上限，先算才出包）

**用户新授权（改变了一条旧约束）**：「哪怕是插帧，你可以使用临时插帧，进行必要地区的插帧，义无反顾向前推进」。
⇒ 本轮允许使用**板上内核已有的** `sheng-fb-fill` write-path test。约束：**只在 l706 这一临时轮用**；
**不进默认 args**；**不污染 `hold/l700` 基线**（也不改 `hold/l705` 模板）；上板后由用户判读并写明
**「一次性、用完回滚」**。仍然禁止新增 printf / IOLog / 计数器 / 采样 / once-only 探针，
**不改任何源码、不重编内核、不放宽安全策略、不动 boot_a**。

预算表（逐项实测，见 `E:\nabu\tmp\l706-build-20261111\args-budget.md`）：

| 步骤 | 操作 | 字符 | token | ≤700 |
|---|---|---|---|---|
| 0 | l705 实测 | **688** | **38** | PASS |
| A | 直接追加 ` sheng-fb-fill=4`（16 字符含空格） | **704** | **39** | **FAIL，超 4** |
| B | 删 ` sheng-vsync=60`（15 字符含空格）⇒ 688−15 | 673 | 37 | PASS |
| C | 再加 16 ⇒ 673+16 | **689** | **38** | PASS |
| D | **实际写法**：原位替换 `sheng-vsync=60`(14) → `sheng-fb-fill=4`(15)，Δ=+1 | **689** | **38** | **PASS** |

缩短候选合法核查：

| 候选 | 省 | 证据 | 结论 |
|---|---|---|---|
| `sheng-build-tag` | — | l704 已删（`state/board-report-l704.md:0`），l705 args 内 0 命中 | 无 |
| `sheng-vsync=60` | **15** | `logs/rounds/l705.json` `verdict: DECISIVE-NEGATIVE`（vsync_events=0、frames 恒 5、屏 sha `ca18b728…` 零变化）；`state/HANDOVER-20261110-l696.md:1323-1352` §7u.2、:1376-1377 §7v；Qoder `project-nabu-fbprefs-and-pet-20261104.md`「l564d +sheng-vsync=60 屏不变 ⇒ vsync 无效」⇒ **l705 刚否证** | **选中删除** |
| `nabu-fbcon=0` | 13 | Qoder `project-nabu-fbprefs-and-pet-20261104.md`「两条判读纪律 1」明写**必须进 args 手术的「不可删」名单**：`fbcon_probe()` 默认 fb=1，`fbcon_init()` 无 state 检查会整屏清黑，删掉**整轮像素读数作废**；`bridge/outbox/20261104-2240-d46a-fbcon-comment-vs-code.md:22`、`tmp/args_swap.py` LIVE 表 w840/l563b 事故、`HANDOVER-20261105-compile-side-fc76.md:104`、`state/board-report-l704.md:14` 同 ⇒ **关系 screen.bgrx 像素读数** | **保留，绝对不删** |
| `nabu-io-throttle-off=1` 等 | — | `bridge/HANDOVER-7ba45ee0-to-next.md:20` 实测寿命 +571 s、`bridge/status/d46a.json:90`「不可摘」；其余 26 token 在 kernel.macho 内各有 1–7 字面量命中 | 全保留 |

**结论**：只删一个已否证 token，不动任何受保护开关，无硬凑。
最终 `added=['sheng-fb-fill=4']`、`dropped=['sheng-vsync=60']`，其余 **37 token 同序同字节**
（`gate3-token-by-token.txt`）。关键开关逐项实测全在：`sheng-fb-gpu=1`、
`t733-managed-rt-normal=1`、`sheng-gpu-addon=1`、`sheng-smp=1`+`nabu-wdt-arm-delay=0`、
`-apfs_shared_datavolume`、`offline-legacy-*` 7 项、`native-ufs-*` 2 项、`nabu-fbcon=0`、
`-v serial=3 debug=0x14e`。选 `=4`（白 0xffffffff）便于与 iBoot Logo / 纯黑区分。

## 1. 唯一问题（一句话）

**通过我们的 GOP 映射写整屏纯白，会不会变成 `screen.bgrx` 的白屏？**
若会 ⇒ **写路径活、墙在 CA 内容**；若不会 ⇒ **写路径 / 扫描基址 / GPU offer 有问题**。
这是运行期像素结果，静态只能证代码在，必须上板读 `screen.bgrx`（AGENTS.md 0.5）。本轮零新探针。

## 2. 已闭合到哪（日志优先 + 静态链）

1. **五轮屏不变**：`logs/l70{1,2,3,4,5}-diag-tree/screen.bgrx` 逐字节全同，
   sha `ca18b728a7def68ea23fea77bb6b91f2a224a18b481f9ec5cff862a87ba5588b`（iBoot Apple Logo 残留，
   10255 非黑 / bbox 344..457,548..688）；`l696` 纯黑是**另一状态**。
2. **但驱动在搬帧**：`logs/rounds/l70{1..5}.json` 的 `nabu-display: frames xfer=5..8`
   `nswap=5/8/5/5 nok/ncpy/nret` 同数（l701=6、l702=5、l703=8、l704=5、l705=5）；
   l704/l705 CDC 有官方 `nabu: scanout hand-off online gop=9c000000/fa0000 1600x2560 stride=1900`、
   `nabu-gpu-addon: mounted=1 target=1 control=1 status=0`、panic=0。
   ⇒ 显示驱动在搬帧而屏内容不变；缺的是「store 是否变成 panel 像素」的判定。
3. **板上内核现量（本会话复核）**：`E:\nabu\hold\l705\kernel.macho`（sha `fb3ce142`，与 l706 逐字节同）
   `sheng-fb-fill` 字面量 **1 命中 @1617052**；`llvm-nm` 实测
   `__ZL25native_legacy_fill_pixelsPVvyyjjj @fffffff007a7ebdc`、
   `_native_legacy_fill_observed @fffffff007a80870`、`_native_legacy_fill @fffffff00c19ed20`；
   字符串 `native_legacy_fill_pixels @90619638/92749962`、
   `native_legacy_fill_observed @89927100/92751491` ⇒ **零新探针、零重编**。
4. **源码链**：`/root/merge/UniXNU-nabu/drivers/GopFramebuffer/NativeLegacyDisplay.cpp:73-94`
   注释「l504: `sheng-fb-fill` is a **WRITE-PATH TEST**, not a feature … One solid colour, written every
   frame, answers that in a single round」；`native_legacy_fill_argb()` :84
   `PE_parse_boot_argn("sheng-fb-fill")`，**:90 `case 4: return 0xffffffffU`**（纯白）；
   `:629 uint32_t forcedFill=native_legacy_fill_argb()`；**:636–660 `if(!surface || forcedFill)` 分支**
   直接 `native_legacy_fill_pixels(bootMapping->getVirtualAddress(), bootMapping->getLength(),
   blank.v_rowBytes, bw, bh, forcedFill)` + `__asm__ volatile("dsb sy")` +
   `native_legacy_frame_colors_observed` + `native_legacy_fill_observed(forcedFill)` +
   `native_legacy_transfer_observed(kIOReturnSuccess,...)` + `finishTransfer(request)` ⇒
   **每一帧 swap 都绕过 surface 内容、把 GOP 写满白色并自证**。
5. **日志对本问题沉默**：历轮从未传过 `sheng-fb-fill`，也没有任何一轮 `screen.bgrx` 是纯白/纯色
   （只有 Logo 与 l696 纯黑）。属 AGENTS.md 0.5 允许的一次运行期判定。

## 3. 为什么离线不行（AGENTS.md 0.5 `--why_board`）

静态能证 write-path 代码/符号/地址、`case 4` 的颜色、`forcedFill` 分支的整屏写与 `dsb sy`、
以及本包只改一个功能 token。但「store through 我们的 GOP 映射是否真的变成 panel 扫描到的像素」是
**运行期结果**：依赖真机 GOP 物理基址 `9c000000/fa0000` 的映射属性（wcomb/cache/DSB 语义）、
panel scanout 基址是否被 GPU offer 或 DCDP 覆写、iBoot Logo 是否驻留在另一块 framebuffer、
以及 blank-swap 分支是否被 CA 覆盖；离线无寄存器/时序/像素直方图，无法计算。
⇒ 属 0.5 允许的「验证静态结论在真机上是否成立的那**一次**判定」与「日志与静态都无法产生的新状态」。

## 4. 失败也有信息（三种判读）

| 结果 | 判读 |
|---|---|
| **屏变白**（`screen.bgrx` 直方图 0xffffffff 占满、sha 离开 `ca18b728…`） | **写路径活、墙在 CA 内容**：`bootMapping` 映射与 panel scanout 基址一致，`dsb sy` 后写可见；五轮 Logo 不变是 CA 没产出可渲染层 / 没进帧提交循环，不是我们的写不生效。下一步查 CA，不再重复写路径 |
| **屏仍 iBoot Logo**（`screen.bgrx` sha `ca18b728…` 逐字节同） | **写路径/基址/GPU offer 需查**：要么 `forcedFill` 分支未在真机走到（swap 被别处 retire）、要么 `bootMapping->getVirtualAddress()` 指向的不是 panel 当前扫描的 framebuffer（DCDP / GPU handoff 改了基址）、要么写被 cache 属性吞掉（`dsb sy` 不够）；对照 `nabu-display: frames`、`native_legacy_fill_observed`（若 CDC 有格式串）、`nabu: scanout hand-off online` 三者是否同时出现 |
| **纯黑** | **blank 分支覆盖**：`if(!surface && !forcedFill && native_legacy_blank_keep())` 或 `0xff000000U` 默认黑色分支盖过 forcedFill；说明 `sheng-fb-fill` 在真机未被解析/未生效，先对账 `unixnu.toml` args 回显与 `PE_parse_boot_argn`，不得把「没采到」当「没发生」 |

**观察项（全部已有输出，零新增 printf/IOLog/计数器/采样/once-only 探针）**：
`nabu-display: frames xfer nswap nsurf nok ncpy nret`、
`native_legacy_fill_observed`（若 CDC 有格式串）、`nabu: scanout hand-off online`、
`nabu-gpu-addon:`、panic、`screen.bgrx` 直方图与 sha（对比 `ca18b728…` 与全白 0xffffffff）。

**fc76 与双审计偏离**：编译侧 fc76 自 **11-06 14:57** 未消费；双审计两侧均为板侧实测
（`audit_dual.py --tag l706 --peer … --mine …`），照 `state/board-report-l705.md:末` 与
`board-report-l704.md:95` 注明，偏离 `AGENTS.md:36-47` 0.6 的「编译侧 vs 板侧」原意。

## 5. 唯一变量与回滚

**唯一功能变量 = args 原位替换 `sheng-vsync=60` → `sheng-fb-fill=4`**；689/38，其余 37 token 同序同字节。

| 文件 | l705 sha8 | **l706 sha8** | 说明 |
|---|---|---|---|
| kernel.macho | `fb3ce142` | `fb3ce142` | 逐字节相同（未重编） |
| kernel.macho.adt | `78a6f7e9` | `78a6f7e9` | 逐字节相同 |
| gpu-addon.bin | `950a3968` | `950a3968` | 逐字节相同 |
| owner-trust.bundle | `a4a6bae6` | `a4a6bae6` | 逐字节相同 |
| boot-merge-r3.img | `ead4b531` | `ead4b531` | 逐字节相同 |
| payload.img | `aa1b47e7` | **`1d394898`** | 唯一变化；包内仅 `UNIXNU~1TOM` 变（1114→1115 B） |

手术：`E:\nabu\tmp\l706-build-20261111\build_l706.py` 以**只读 mmap ACCESS_READ** 打开 l705 模板
（绝不写输入 payload，历史坑见 `state/board-report-l695p.md`），192 MiB 用 `shutil.copyfileobj`
**流式**复制，2048 B 簇 **12688 整簇替换**（slack 全零、FAT 链不变）+ 目录项 size
`1114 → 1115`（偏移 804028）。payload 内抽回复核：`KERNEL~1MAC fb3ce142` / `KERNEL~1ADT 78a6f7e9` /
`KERNEL~1GPU 950a3968` / `KERNEL~1TRU a4a6bae6` / `SCRATC~1IMG 3cd3d0d3` 与 l705 全同，
仅 `UNIXNU~1TOM` 变（`selfcheck.txt` / `payload-inner-compare.txt`）。

- **回滚基线**：`E:\nabu\hold\l700\`（kernel `efcb4c34`，已验证 4 分钟窗口）；
  **上一包** `E:\nabu\hold\l705\`（kernel `fb3ce142`、payload `aa1b47e7`，本会话只读未污染）。
- **一次性、用完回滚**：`sheng-fb-fill=4` 只属于 l706，**不进默认 args**；判读完成后刷回
  `hold/l700/payload.img` 或 `hold/l705/payload.img`，由用户在 `logs/rounds/l706.json` 写明
  「一次性临时插帧轮、用完回滚」。回滚命令（仅文字）：`fastboot flash cust hold/l700/payload.img`。

## 6. 恢复预案 / 门禁 / 附注

**AGENTS.md 0.10 绝对红线：不碰 `boot_a`**（绝不 `fastboot flash boot` / `flash boot_a`；recovery 就在 boot 里）。
流程（**仅文字，本会话不执行**；顺序不可换）：

```
recovery arm misc
fastboot flash cust hold/l706/payload.img
fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin   # flash 后最后重 arm
fastboot boot hold/l706/boot-merge-r3.img                      # 临时加载，不写任何分区
```

`E:\nabu\dwc3\build\misc-bootrecovery.bin`（4096 B，本会话确认存在）；设备 `TWRP recovery f91fa664`。
`arm-delay=0`，咬点 **380-410 s**，观察完整 **4 分钟窗口**（`AGENTS.md:57-64`），早 panic 也有重置路径。
若 fastboot 全失败：人工长按电源 10 秒。回滚见 §5。

**门禁（本会话实测，全部 PASS）**：

| 项 | 结果 | 证据 |
|---|---|---|
| ① `fsck.fat -n` 双侧 | l706/l705 均 `10 files, 67550/97913 clusters` rc=0；`audit_bootability.py hold/l706 hold/l705 --expected=fb3ce142…` → **`OK (0 fail, 0 warn)`** | `gate1-fsck.txt`、`gate1-bootability.txt` |
| ② `audit_dual.py --tag l706` | 真实 sha `kernel=fb3ce142,payload=1d394898,adt=78a6f7e9,addon=950a3968,trust=a4a6bae6,boot=ead4b531,args=689/38` → `7 items compared, 7 match, 0 differ` | `gate2-audit_dual.txt`、`state/audit-l706.json` |
| ③ `audit_args.py hold/l705 hold/l706` | `688/38 → 689/38`（≤700）；`added=['sheng-fb-fill=4']`、`dropped=['sheng-vsync=60']` → **`RESULT: OK`** rc=0；逐 token 37/37 同序同字节、零 mismatch、关键开关全 PRESENT | `gate3-audit_args.txt`、`gate3-token-by-token.txt` |
| ④ Windows Python 3.12.10 `precheck.py` | `run_precheck_l706.py` 驱动 `--pkg E:/nabu/hold/l706 --baseline E:/nabu/hold/l705 --expect-added/--expect-dropped` → `stamp written: state/precheck.json` rc=0；八字段全非空、`token_diff=ok`；`precheck_check.py` → **`[gate] OK`** rc=0，`dual audit OK: audit-l706.json (7 items)` | `gate4-precheck.txt`、`gate4-precheck_check.txt` |
| ⑤ 六问板报 | 本文件 | `state/board-report-l706.md` |
| 自检 | 抽回 toml 逐 token 只差两 token；抽回 kernel/addon/trust/adt/scratch 与 l705 `cmp SAME`；`hold/l705/` 六文件 sha 前后未变（payload `aa1b47e7`） | `selfcheck.txt`、`l705-baseline-sha.txt`、`l705-after-sha.txt` |

## 7. 证据路径（全部绝对路径）

- 构建与预算：`E:\nabu\tmp\l706-build-20261111\build_l706.py`、`args-budget.md`、`payload.img`、
  `token_check.py`、`run_precheck_l706.py`
- 门禁输出：同目录 `gate1-fsck.txt`、`gate1-bootability.txt`、`gate2-audit_dual.txt`、
  `gate3-audit_args.txt`、`gate3-token-by-token.txt`、`gate4-precheck.txt`、
  `gate4-precheck_check.txt`、`payload-inner-compare.txt`、`selfcheck.txt`、
  `l705-baseline-sha.txt`、`l705-after-sha.txt`
- 戳与板报：`E:\nabu\state\audit-l706.json`、`E:\nabu\state\precheck.json`、
  `E:\nabu\state\board-report-l706.md`
- 包：`E:\nabu\hold\l706\`（六文件）；只读模板 `E:\nabu\hold\l705\`；回滚 `E:\nabu\hold\l700\`
- 板上证据：`E:\nabu\logs\l70{1,2,3,4,5}-diag-tree\screen.bgrx`、`E:\nabu\logs\rounds\l70{1..5}.json`、
  `E:\nabu\logs\l70{4,5}-cdc.bin`
- 静态链：`E:\nabu\hold\l705\kernel.macho` @1617052 / @90619638 / @89927100；
  `/root/merge/UniXNU-nabu/drivers/GopFramebuffer/NativeLegacyDisplay.cpp:73-94,629,636-660`；
  `E:\nabu\state\HANDOVER-20261110-l696.md:1323-1352,1376-1377`；
  `C:\Users\Administrator\.qoder-cn\projects\E--unix\memory\project-nabu-fbprefs-and-pet-20261104.md`；
  `E:\nabu\bridge\outbox\20261104-2240-d46a-fbcon-comment-vs-code.md:22`；
  `E:\nabu\tmp\args_swap.py` LIVE 表；`E:\nabu\AGENTS.md:14-24,36-47,57-64,100-109`

**注**：本轮**零新探针**（用板上已有 write-path test）、**零重编内核**、**零安全策略放宽**、
**零源码改动**、**零 boot_a 触碰**。fc76 未消费 / 双审计两侧板侧实测偏离照 l705 板报注明。

## 8. 下一步（仅建议，本会话不执行）

由用户上板后自行写 `logs/rounds/l706.json`（本会话**不写**），按 §4 三判读表读
`screen.bgrx` 直方图与 sha、`nabu-display: frames`、`native_legacy_fill_observed`，
并写明**「一次性临时插帧轮、用完回滚」**，然后回滚到 `hold/l700` 或 `hold/l705`。
