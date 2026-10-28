# 当前状态 —— 重写后的新线（2026-10-28）

> **本仓库其余文档是「推倒重来」之前的记录**，只作追溯用；与本文冲突时**以本文为准**。
> 新线的代码与制品不在本仓库：串口/控制通道与无人值守流水线见
> [`nabu-usb-cdc`](https://github.com/Lux12138/nabu-usb-cdc)（`docs/07` 是控制通道与 WDT 定时停车）。

## 0. 一句话

7 个阶段里前 6 个全通，内核已进入用户态；阶段 7 内部**只差一个 `IOMobileFramebuffer` 的 provider 实例**。

## 1. 板上实测（d1b 轮，2026-10-28）

```bash
python E:/nabu/tools/pipeline.py --full --capture 60 \
    --payload-dir <制品目录> --expect-payload-sha <payload sha256>
```

| 项 | 值 | 证据 |
|---|---|---|
| 完整启动日志 | **146,042 B / 2,032 行** | `d1b-run-com.bin` |
| 守护进程 | `[hb] tag=chk n=68 sb=1 bb=1 rb=1 cfg=1` | 同上 |
| keybagd | `DEVICE HAS EPHEMERAL DATA VOLUME, SKIP INIT` + `[ex] pid=11 comm=keybagd rv=0x0` | 同上 |
| 显示族 | `[nm] mask=7 size=1336 init=1 rel=0`（第 87 行） | 同上 |
| 显示失败点 | `[ex] pid=26 comm=IOMFB_FDR_Loader rv=0x100 sig=0 code=1` | wdt-final 轮同型 |

### `mask=7` 怎么读

`mask` 是 `OSMetaClass::allocClassWithName()` 对三个名字的失败位图：
`IOMobileFramebuffer` / `IOMobileFramebufferService` / `IOMobileFramebufferLegacy`。

**抽象类的 `alloc()` 恒返回 NULL**（`OSDefineAbstractStructors` 生成的就是 `return NULL;`），
所以 `mask=7` **不能**读成“这三个类不存在”。同一行的 `size=1336 init=1` 来自
`IOMobileFramebufferUserClient`（具体类，可分配且 init 成功）。

要把“注册了没有”定死，只能改用 `getMetaClassWithName()` 对三个名字逐个判非空。

## 2. 启动链与复位

```
XBL → ABL → fastboot boot Mu UEFI → cust 里的 UniXNU payload → XNU

复位：kernel `nabu-park-after=N`
        → `[park] arm`（只设 deadline）
        → deadline 到点才调 nabu_wdt_arm_now()，打 `[park] wdt`
        → APSS WDT(0x17C10000) EN=1、BARK=BITE=2097152 ticks
        → 约 32 s 后咬合 → SoC 复位 → ABL 读 misc → recovery
```

- `WDT.CFG` 只认 `mode=N`；现行 `mode=0`（不让 Mu 预武装）。
- **咬合 = 武装后约 32 s**，不是 64 s（`2097152 ticks / 32768` 只是换算，不是实测）。
  直接观测到的只有一轮：r10z3v，deadline 82.7 s → USB LOST boot+114.8（31.3 s）。
  其余轮次的咬合时刻都是“deadline + 32”的推断。
- 想让复位更晚：**只能加大 `park-after`**（1:1）。代价是“武装时刻之前的 panic 没有硬件狗兜底”。

## 3. 根卷与用户态

- 根：只读 APFS；`/private/var` 由 **nabuvar**（内存卷）挂载；preboot 树与 dyld shared cache（536）已通。
- 沙盒目前 skip（`nabu-mac-skip-sandbox=1`）。

## 4. 无人值守与判据纪律

- 单轮 **86~129 s**，零按键，失败轮（panic / keybag 致命）也会按时回 TWRP。
- **可当事件时刻的只有三类**：port-session 自打的 `HH:MM:SS TX/RX`、真实的 USB LOST、
  `adb devices` 看到 recovery 的时刻。
  pipeline 的 `[HH:MM:SS]` 前缀、`closed`、`close <时刻>`、`auto-reset TWRP after X s`
  都是子进程返回后补的，**不能当事件时刻**。
- 每轮的 `[park] arm` 时刻都不同（实测 48.6 / 50.5 / 52.4 / 56.3 s），
  所以判读一律用当轮 `dl − ticks` 反算，不许套上一轮的常数。

## 5. 本仓库历史部分里已经失效的说法

| 旧说法（10-23 ~ 10-25） | 现状 |
|---|---|
| 内核停在 `execve("/sbin/launchd")`，`errno 2`，缺根文件系统 | 已过：根卷 + launchd 通过，用户态起来了 |
| 下一步 `./unixnu build userdata` + `fastboot flash userdata` | 已取代：只读 APFS 根 + nabuvar 挂 `/private/var` |
| 日志靠 UFS 窗口（`NABULOG1`，userdata 末尾 32 MiB）定期写盘再读回 | 主线改为 DWC3 CDC 实时串口；UFS 窗口只保留为 panic 场景旁路 |
| 复位靠 panic 路径里的 PSCI `SYSTEM_RESET` | 现在主复位源是 APSS 硬件狗（park-after 定时武装）；PSCI 仍用于 `REBOOT` 控制帧 |

## 6. 下一步（按判别力排序）

1. **`getMetaClassWithName()`**：把 `mask=7` 的歧义定死（一轮、零风险）。
2. **provider 实例**：自写 `IOMobileFramebuffer` 子类已在链接期被否证
   （缺 `gMetaClass` / ctor / dtor，基类在 prelink 的 kext 里、不参与内核链接）。
   社区路线是原版 `AppleCLCD` + 自造 `/arm-io/disp0` 节点。
3. **采集窗口**：`--capture` 放长到覆盖复位点，用真实 `USB LOST at` 时间戳
   把“武装后约 32 s”从推断变成实测。
