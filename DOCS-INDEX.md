# 文档索引 — 每份讲什么、什么时候读

> **本仓库是「推倒重来」之前那条线的记录。**
> 当前状态、以及下面这些文档里已经失效的说法，见 [`CURRENT-STATE.md`](CURRENT-STATE.md)。
> 与旧文档冲突时**以 `CURRENT-STATE.md` 为准**。

> 新会话**必须逐份研读**下面这些（不是只看 START-HERE）。
> 同时要读**全部提交记录的 message**（`git log --oneline -40`），它们记录了
> 每一轮实测的结论和踩过的坑，很多细节只存在于 commit message 里。

## 〇、先读

| # | 文件 | 内容 |
|---|---|---|
| — | [`CURRENT-STATE.md`](CURRENT-STATE.md) | **重写后的新线现状**：板上实测、复位机制、无人值守纪律、旧说法失效清单、下一步 |

## 一、必读（按顺序，全部要读；**均为重写前的历史**）

| # | 文件 | 字节 | 内容 | 什么时候读 |
|---|---|---|---|---|
| 0 | `HANDOVER-2026-10-25.md` | 41K | **重写前的最后一份交接（10-25 深夜）**：卡 Mi logo 的现场与恢复步骤、三仓库 HEAD/子模块 pin、构建/打包/刷机/读日志全流程、主线 `SIG900008` 与 AMFI 地址疑点、DWC3 代码级现状与红线、按判别力排序的下一步、新窗口第一组命令 | 追溯用；**其中的“当前基线/下一步”已被 `CURRENT-STATE.md` 取代** |
| 1 | `START-HERE.md` | 4.1K | 铁律、当前基线、复现命令、坑速查 | 第二份 |
| 2 | `HANDOVER-2026-10-24-ROLLBACK.md` | 10.6K | 10-24 交接：全部实测结论、十个坑、DWC3 现状、自动读日志方案定论、下一步排序（部分已被 10-25 交接取代） | 第三份 |
| 3 | `HANDOVER-exec-fault.md` | 29.2K | 主线 689 行：execve/SIGKILL 完整链条，十六节 | 第三份，**最详实** |
| 4 | `HANDOVER.md` | 8.5K | 权威初始交接（227 行）：两个 kprintf 开关、PE_kputc、ADT 需要 RAMDisk、0x9000000 UART 只在 QEMU、FVMAIN 8–10MB 上限、kernel.py 冷构建跑两次 | 基础事实核对 |
| 5 | `HANDOVER-legacy.md` | 40.1K | 早期历史（40K，最长）：从沉默内核到能启动的全过程 | 需要追溯某个决定"当初为什么这么做"时 |
| 6 | `STATUS.md` | 40.7K | 逐轮状态流水（908 行），含五点 `vm_page_free_count` 二分 | 查某轮到底发生了什么 |
| 7 | `HANDOVER-2026-10-24.md` | 5.2K | 10-24 上午交接（root 找到、launchd 到达） | 对照上午/下午差异 |
| 8 | `PUSH-GOTCHA.md` | 1.8K | `git push` 返回 exit=0 但不生效的坑 + 正确三步法 | **每次推送前** |
| 9 | `README.md` | 8.0K | 本机 12GiB 扩容、启动链、7-Zip 解 LZFSE DMG、launchd 561856 字节 | 环境事实 |

## 二、专题文档（`docs/` 与根目录）

| 文件 | 字节 | 内容 |
|---|---|---|
| `PLAN.md` | 9.9K | 整体计划 |
| `FLOW.md` | 10.0K | 启动流程 |
| `PREDICTIONS.md` | 3.3K | 预测与验证记录 |
| `docs/early-console-plan.md` | 7.2K | 早期控制台方案（屏幕输出是怎么做出来的） |
| `docs/nabu-dts-extract.md` | 5.1K | 从设备树提取硬件参数 |
| `docs/pmgr-plan.md` | 3.8K | 电源管理计划 |
| `docs/firmware-patches.md` | 2.2K | 固件补丁 |
| `docs/prior-art-0xDive-ios-nabu.md` | 3.4K | 前人的 nabu iOS 移植 |
| `docs/prior-art-mytiantian001-ipados-nabu.md` | 5.4K | 前人 iPadOS on nabu |
| `docs/prior-art-*.md` | — | **社区方案**：别人怎么做 nabu，值得对照 |

## 三、另一个仓库的文档

| 仓库 | 文件 |
|---|---|
| `Lux12138/UniXNU` | `START-HERE.md`（指向本仓库 + 三条硬规矩） |
| `Lux12138/xnu-linux-toolchain` | `docs/LINUX-BUILD.md`（34K，上游工具链）、`docs/NABU-18-PITFALLS.md`（72 秒本地构建 + 18 坑） |

## 四、提交记录怎么读

```bash
cd ~/src/unixnu-nabu && git log --oneline -60
cd ~/src/UniXNU      && git log --oneline -60
```

**每条 commit message 都要看**，它们不是套话，而是当轮的实测结论。举几个例子：

| commit | 信息量 |
|---|---|
| `5488c9e docs: PAC bits broke the copyio recovery lookup` | **PAC 修复的完整机制** —— `find_copyio_recovery_entry` 漏了 `VM_KERNEL_STRIP_PTR` |
| `bcedff4 vm: max_mem and vm_page_pages are correct` | `vm_page_free_count` 异常是**后果不是原因**，别动 dram_size |
| `9686059 vm: fault repair does not work for user pages` | 用户页缺页修复失败的精确诊断 |
| `6373972 docs: the ten process mistakes behind the probe detour` | 探针弯路背后的十个流程错误 |
| `ded3190 adt: restore the 4.75 GiB window and the RAMDisk node` | ADT 缺节点 = 功能静默失效（后来 TrustCache 也是同一个模式） |
| `5b095ac / a5b70fb usb: ...DWC3...` | **已回滚**，读它们是为了知道 DWC3 为什么崩、别重蹈 |

## 五、读完之后必须能回答这五个问题

答不出来就说明没读够，继续读：

1. 现在的 panic 是什么？reason 的 namespace/code 是多少，实测值从哪来？
2. execve 缺页是怎么修好的？改动在哪一行？
3. 为什么 AMFI boot-args 要改在 `make-nabu-adt.py` 而不是 `unixnu.toml`？
4. 探针"没显示"有哪三种可能？今天各踩过几次？
5. 自动读日志的四条路里，哪条可行、哪三条实测不通？
