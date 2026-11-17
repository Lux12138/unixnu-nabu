# 状态快照（2026-11-16）

> **这是 `CURRENT-STATE.md` 的短版仪表盘，不是它的替代。两者冲突时以
> [`CURRENT-STATE.md`](CURRENT-STATE.md) 为准。**
>
> 2026-10-24 那份同名文件停在「内核到 launchd 失败 errno 2」，
> 已被移到 [`docs/history/STATUS.md`](docs/history/STATUS.md)，作废。
> 如果你是从旧链接过来的，那份描述的是 23 天前的事，别按它行事。

## 目标层：五层已完成，卡在第六层

| 目标层 | 状态 | 判据 | 轮次 |
|---|---|---|---|
| 内核启动 → 用户态 | ✅ 100% | launchd 起来，176 个进程 | l730 之前 |
| 迁移链打通 | ✅ 100% | `Migration necessary` 0 条 | l730 |
| 系统 UI（锁屏） | ✅ 100% | 真 iPadOS 锁屏，彩色壁纸 | l730 / l750 |
| **桌面（Home Screen）** | ✅ **100%** | **os_log 官方交还事件** | **l769** |
| 触摸 / 手势送达 | ✅ 100% | `destinations: { SpringBoard.27 }` | l752–l754 |
| GPU scanout 接管 | ✅ 功能闭合 | `P4/PF` 生效、`import_stage=19` | l770 + l771 |
| **流畅度（帧率 / 延迟）** | ❌ **0%** | 1.18 fps，ctc 长尾 817 ms | **进行中** |

## 桌面已达成的证据（2026-11-16 复核）

2026-11-14 的 l769 结论当时只有 CDC（内核 printf）支撑。2026-11-16 从
NABUDIAG **seq1** 解出 213,704 行用户态 os_log，拿到官方事件，把「推断」升级成「铁证」：

```text
[265.38s] System shell "com.apple.springboard" finished startup after 18.08s.
[265.39s] First boot is finished
[265.39s] updateOverlay: pid:27 explicitlyDismissed:NO finishedStartup:YES      ← 两次
[265.39s] updateOverlay: Dismissing overlay: didFinishFirstBoot
[265.39s] systemShellDidFinishLaunching: - bundleIdentifier: com.apple.springboard;
            bundlePath: /System/Library/CoreServices/SpringBoard.app;
            jobLabel: com.apple.SpringBoard; pid: 27;
            systemIdleSleepInterval: 3.00; watchdogType: shell
[265.39s] updateOverlay: Dismissing overlay: shell launched (... pid: 27 ...)
```

来源：`com.apple.BackBoard/BootUI`（pid 56 = backboardd）。
配套：`Migration necessary` **0** 条、`Migration not necessary` 11 条、
触摸 268.3 s 起送达 `SpringBoard.27`。

**SpringBoard 自己报的显示配置也是正常的：**

```text
displayConfiguration = <FBSDisplayConfiguration: 0x3010d80f0;
                        Main; mode: "800x1280@2x 60Hz sRGB SDR">
foreground = Yes
```

## 三个必须记住的观测坑

### 坑 1：seq0 判不了桌面

```text
seq0  base=124307451  bytes=30162877   →  真实 54.8–214.8 s   ← 桌面 265.4s 才交还，永远错过
seq1  base=124340219  bytes=38460393   →  真实 54.8–291.8 s   ← 完整窗口
```

两槽 base 相差 **32,768 块 = 128 MiB**。顺序读 4×8 MiB 只能拿到 seq0。
必须按 `base` 定位：seq1 在 UFS 偏移 **509,297,537,024**（474.32 GiB）。
**下一轮出包把 `diagAt1` 设在 ≥300 s。**

### 坑 2：os_log 时基不是 epoch

**真实秒 = `time` × 52.0833 / 1e9**（×52.0833 后仍是纳秒，再除 1e9）。
用 l730 验证：算出 38.5–292.8 s，与历史记录逐字符一致。

### 坑 3：`reevaluateDisplayLink` 不是 vsync 证据

那 10 条全在 `com.apple.BackBoard/MousePointer` —— **鼠标指针**的 displayLink，
不是合成器 vsync。别拿它证明 vsync 通或不通。

## l769 / l770 / l771 只差 5 个字节

四个包的 `boot` / `gpu-addon` / `adt` / `trust` **全部相同**：

| 文件 | l769 | l770 | l771 |
|---|---|---|---|
| boot-merge-r3.img | `ead4b531` | `ead4b531` | `ead4b531` |
| gpu-addon.bin | `950a3968` | `950a3968` | `950a3968` |
| kernel.macho.adt | `ddb096bf` | `ddb096bf` | `ddb096bf` |
| owner-trust.bundle | `a4a6bae6` | `a4a6bae6` | `a4a6bae6` |
| **kernel.macho** | `234eeb09` | `9a5ead64` | `6ed5e20a` |
| payload.img | `496f583e` | `a12398ed` | `2f2253b3` |

内核字节差**总共 5 个字节**：

```text
l769 → l770   @0x00a7c6d0   4B   76fae297 → 1f2003d5    BL → NOP（禁 nabuScanoutClient）
l770 → l771   @0x00a50ebc   1B   00       → 20          WIMG Unknown → Cached
```

payload 会变是因为它**内嵌 kernel**，不是额外变量。

**「l770 进不去桌面」与包内容不符**：l770 板报 §2 原文写的是
「`P3 e0` 18 次、`P3 ee00002c2=0`，**桌面 PNG/ToHome 不回归**」，
l771 板报也称 l770 为「桌面不回归的安全基线」。4 字节 NOP 只禁一个抢
handoff 独占 `client_` 的内核线程，代价是 `ncpy=5` CPU 回退 —— 性能退化，不是掉桌面。

## ADT：211,568 与 211,712 的真实差异

**只差 144 字节**（不是早先误传的 140,304 B），唯一结构性差异在 ADT 偏移 `0x4`
的根节点子节点数：

```text
od -t x1 -N 16
211,568 / ddb096bf   06 00 00 00 09 00 00 00  name...
211,712              06 00 00 00 0a 00 00 00  name...
```

根因是 `board.toml` 里 l749 遗留的 panic region 注入：

```toml
"embedded-panic-log-size" = "0x80000"
["adt"."pram"."bytes"]
"reg" = "000000009b0000000000000000080000"
```

这段让 `unixnu tree` 往 ADT 根节点加一个子节点。**`l738`、`l750`–`l754`、`l768`–`l771`
全部是 211,568 / `ddb096bf`，只有 `l749` 是 211,712。** l1070 第一次上板倒退
（连苹果 logo 都不出）就是拿了 211,712 的 ADT。

**纪律：上板前必须核验 payload 内部 `kernel.macho.adt` = 211,568 B / `ddb096bf`。**

## 流畅度这道墙的精确形状（仍 0%）

CDC 帧时间线（2026-11-16 轮，23 个采样）：

```text
     t(s)  xfer  ncpy  fingers
      5.2     0     0        0
     52.1     3     0        0    首次出帧（BootUI）
    265.0     8     0       36    ← 桌面交还
    271.7    25     0      142
    291.2    48     0      492    +23 帧 / 19.5 s ≈ 1.18 fps
    447.5   179     0      871    最终
```

`ncpy=0` 全程，**`sbytes=16384000` 每帧恒定 16 MB**。

GPU 侧（291.77 s 快照）：`import_stage=19`（成功态）、`live=342 BO`、
`mapped_mib=368`、`submissions=24 completed=20 timeouts=0`。

`ctc`（commit-to-complete）长尾**只出现在首波触摸之后**：

```text
271.636s  ctc=817543us   ← 817 ms
271.362s  ctc=675282us   ← 675 ms
269.200s  ctc=32592us
266.400s  ctc=43374us
265.016s  ctc=36029us
```

**根本约束**（271.37 s / 281.57 s）：

```text
the buffer is a no-copy buffer, and the device makes no linear image
```

同时 `no-copy` 计数在交还后 0 → 9 → 25 —— 零拷贝路径**在用**，但 Metal 层拒绝把
no-copy buffer 当线性图像读。**这就是 16 MB/帧消不掉的直接原因**，
除非换掉读回路径。

## vsync：假设仍未证实，且传递链已发现断裂

2026-11-16 的 `l769 + sheng-vsync=60` 单变量轮**上板失败**，原因不是包，是传递链：

> `sheng-vsync=60` 写在 payload 的 `unixnu.toml` args 里，
> 但内核 `PE_parse_boot_argn` 从 **DTB chosen / bootargs** 读，
> 而 boot image 内嵌的 DTB 没有 bootargs 节点
> ⇒ `startVsync()` 的 `hz=0` ⇒ 第一行 `return` ⇒ 定时器从未启动。

注意 `hardware.dtb`（4 KB）不是 boot image 内嵌的那份 DTB（715 KB），
修这个要改内嵌 DTB 再重建 boot image。

**判别用的埋点分层**（`NativeLegacyDisplay.cpp`）：

```cpp
:399  native_legacy_vsync_observed(4, hz)     startVsync 成功，无条件打
:376  native_legacy_vsync_observed(0, vsyncHz)  定时器真正 arm
:385  native_legacy_vsync_observed(1, vsyncHz)  每帧在发
:368  vsyncWanted() = *(this+5592) 非空（vsync 客户端链表头）
```

| CDC 看到 | 含义 |
|---|---|
| 事件 `4` | `hz` 被接受，参数通了 |
| 事件 `0` | 定时器真 arm 了（客户端已注册） |
| 事件 `1` | 每帧在发 vsync —— **修好了** |
| 只有 `4`，没有 `0/1` | `vsyncWanted()` 恒假，客户端没注册 |
| 连 `4` 都没有 | 参数根本没被解析（DTB 传递链断了） |

## 下一步（按判别力排序）

1. **修 DTB bootargs 传递链**，让 `sheng-vsync=60` 真能到内核。这是唯一能证实
   或证伪「无帧请求循环」的动作，其余优化都排它后面。
2. **分离 ctc 长尾的因果**：需要**无触摸**稳态数据，本轮已被触摸污染。
3. **换掉 16 MB/帧的读回路径**：Metal 拒读 no-copy buffer 是根本约束，
   不是频率或 cache 属性问题。
4. **8 核 SMP 的实际调度均衡**：`SMPTBL8` 已证 8 槽 `st=7 running`，
   但用户态 `hw.ncpu` 与逐核负载未测。
5. **桌面持久化**：当前仍是临时轮（一次性包），未固化到默认 args / 编译侧。

## 不要做的事

| 禁止 | 原因 |
|---|---|
| `fastboot flash boot_a` / `flash boot` | recovery 在 boot_a 里，写了丢 TWRP |
| 任何 `erase` | 2026-11-13 `erase misc`/`vbmeta` 差点变砖 |
| 新增自造探针 | 只用内核已有官方输出，禁 printf/IOLog/计数器 |
| args 超过 700 字符 | l769 正好是 39 token / **700 字符**，已经没有余量 |
| `sheng-smp=1` 不配 `nabu-wdt-arm-delay=0` | WDT 会早咬 |
| 可写 mmap 输入 payload | 曾污染 l695p |
| 不同路径构建内核 | 绝对路径字面量会移动 `__cstring`，204 字节 ledger 失效 |
| 用 seq0 判桌面 | 它永远在桌面交还之前结束 |
| 拿 211,712 的 ADT 上板 | 会重演 l1070 倒退，连苹果 logo 都不出 |

## 相关文档

| 要什么 | 去哪 |
|---|---|
| 权威状态 | [`CURRENT-STATE.md`](CURRENT-STATE.md) |
| 项目总览 + 路线图 | [`README.md`](README.md) |
| 归属与来源 | [`CREDITS.md`](CREDITS.md) |
| 导航 | [`DOCS-INDEX.md`](DOCS-INDEX.md) |
| 逐轮推理（73 份） | [`board-run/reports/`](board-run/reports/) |
| 实机截图 | [`docs/screenshots/`](docs/screenshots/) |
| 2026-10-24 的旧快照（**作废**） | [`docs/history/STATUS.md`](docs/history/STATUS.md) |
