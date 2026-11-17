# 文档索引

> **只需要读两份：`README.md` 和 `CURRENT-STATE.md`。**
> 想快速看一眼「现在到哪了」就读 `STATUS.md`。其余都是专题或历史，按需查阅。

## 必读（3 份）

| 文件 | 内容 |
|---|---|
| [`README.md`](README.md) | 项目总览：现状、关键突破与路线、仓库结构、观测通道、**路线图（还缺哪些驱动）**、已否证方向 |
| [`CURRENT-STATE.md`](CURRENT-STATE.md) | 权威状态摘要。**与其他任何文档冲突时以它为准** |
| [`STATUS.md`](STATUS.md) | **短版仪表盘**（2026-11-16）：七层目标表、桌面 os_log 铁证、三个观测坑、ADT 差异、vsync 传递链、五条下一步、九条禁令 |
| [`CREDITS.md`](CREDITS.md) | **参考与来源**：上游依赖、参考对象、原创边界、分目录许可证 |

## 专题（按需）

| 文件 | 内容 | 什么时候读 |
|---|---|---|
| [`uefi-nabu`](https://github.com/Lux12138/uefi-nabu) | **UEFI 固件仓库**（启动链第一环）：FD 3 MiB @ `0x9FC00000`、UniXNU 四模块、APSS 看门狗 | 要改固件或复现启动时 |
| [`docs/screenshots/`](docs/screenshots/) | 实机桌面（l769）与锁屏（l730） | 想确认进度时 |
| [`board-run/reports/`](board-run/reports/) | **73 份六问板报**，覆盖到 l771。每份含：唯一问题 / 已闭合到哪 / 为什么离线不行 / 失败也有信息 / 唯一变量与回滚点 / 恢复预案 | 想复现某一轮的推理时 |
| [`board-run/rounds/`](board-run/rounds/) | 135 份结构化轮次 JSON（kernel/payload/args/verdict） | 想查某一轮的参数与结论时 |
| [`board-run/state/HANDOVER-20261110-l696.md`](board-run/state/HANDOVER-20261110-l696.md) | 主交接（3,641 行），逐轮技术细节 | 接手时 |
| [`docs/prior-art-*.md`](docs/) | 其他 nabu 移植项目的技术路线对比 | 想知道别人怎么做时 |
| [`docs/nabu-dts-extract.md`](docs/nabu-dts-extract.md) | 从设备树提取硬件参数 | 加新驱动时 |

## 历史（已作废，仅作追溯）

**`docs/history/` 下全部文档写于 2026-10-2x，属于已被取代的旧路线。**
其中「当前状态 / 下一步 / 卡在哪里」全部失效，不要按它行事。

它们保留的唯一价值：记录**为什么当初选了某条路、踩过哪些坑**。

| 文件 | 内容 |
|---|---|
| `docs/history/PLAN.md` | 早期路线选型：对比两个用 xnu-8796 的成熟项目，决定「我们坚持 10063」 |
| `docs/history/HANDOVER-2026-10-25.md` | 旧路线的最后一份交接（卡 Mi logo） |
| `docs/history/HANDOVER-exec-fault.md` | execve/SIGKILL 链条（29 KB，最详实的失败分析） |
| `docs/history/HANDOVER-legacy.md` | 最早期的历史（40 KB） |
| `docs/history/STATUS.md` | 逐轮状态流水（908 行，**已作废**；现行短版在根目录 [`STATUS.md`](STATUS.md)） |
| `docs/history/FLOW.md` / `PREDICTIONS.md` | 旧的启动流程图与预判清单 |
| `docs/history/START-HERE.md` | 旧的新手入口 |
| `docs/history/PUSH-GOTCHA.md` | ⚠️ **仍然有效**：`git push` 返回 exit=0 但远端未变的坑 + 正确三步法 |
| `docs/history/xnu-8796-route-abandoned/` | ⚠️ **作废路线**：86 个 xnu-8796 补丁。现行内基线是 xnu-10063.141.1，补丁在 `patches/xnu/` |

### 路线变更记录（为什么有两套补丁）

早期曾考虑跟随 `glebenekt/ios-nabu` 与 `oosexgang/hackios` 走 **xnu-8796（iOS 16 世代）**，
但最终选择 **xnu-10063.141.1（iOS 17 世代）**，以匹配 iPadOS 17.7.11 用户态。

因此仓库里出现过两套补丁目录：

| 目录 | 状态 |
|---|---|
| `patches/xnu/`（23 个，编号 0001–0017） | ✅ **现行**，`board.toml` 的 `patch_series` 指向这里 |
| `docs/history/xnu-8796-route-abandoned/`（86 个） | ❌ **作废**，属 xnu-8796 时代，无任何现行文件引用 |

## 仓库结构速查

```text
drivers/          7 个板级驱动（NabuAdreno 101 文件 / NativeUfs 33 / GopFramebuffer 12 /
                  ArmCpu 3 / ShengNovatekTouch 4 / QcomGeniSpi 2 / AppleKeyStoreState 1）
patches/xnu/      内核补丁序列至 0017 + sheng-platform-power
configs/          两个 defconfig
tools/            板侧构建与采集脚本（33 个）
board-run/        73 板报 + 135 轮次 JSON + 主交接
firmware/         NT36523 触摸固件（196,608 B，board.toml 用 file: 引用）
docs/             专题文档 + screenshots/ + history/
board.toml        板级配置（5.3 KB，固件已外置）
```
