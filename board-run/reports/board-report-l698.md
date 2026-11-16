# 上板报告 l698（新版 addon × t733 首次组合）

## 1. 唯一问题

**把两个各自验证过的修复第一次放在一起**：
新版 addon（`Info.plist` 内 `NativeAdrenoPublish=true`，sandbox 授权路径）
＋ `t733-managed-rt-normal=1`（surface import 修通）。
插件能否进入 published 模式、CA 是否建立 context/layer、`frames` 是否越过 6、
屏上是否出现**非 iBoot-Logo** 的内容？

## 2. 已闭合到哪（静态链 + 历史判据）

**两个修复从未组合**（逐 token 核对历史 args）：

| 轮 | addon | t733 | 结果 |
|---|---|---|---|
| l695u | `39dc17fe`（新版，Info.plist key） | ❌ | `surface import refused` 5 条、`PUB/STn=0`、frames 6 |
| l696/l697 | `dcd25289`（旧版，同级 `.publish` 文件）| ✅ | import 拒绝 **5→0**，但 `PUB/STn=0`、frames 6、屏纯黑 |
| l697 追加 | 旧版 + inject | ✅ | 注入 3 帧 `rc=0`、`reports 0→3`，仍 `sat=0`、frames 冻结 |

**为什么旧 addon 可能是 l696 的短板**：
- `NativeAdrenoMetalPlugin.mm:35-48`（旧版 exe `083fa851`）的 `publish::requested()`
  读 **bundle 同级** `NativeAdrenoMetal.publish` 文件的 `access(path, R_OK)`。
- `state/lessons-l695u-metal-discovery.md` / `l695u` 板报记录：同级文件不在 backboardd
  的 sandbox 授权 subpath，`access()` 返回 EPERM ⇒ 插件**静默留在 stage 1、不注册 device**。
- 新版 exe `180ab3a0` 改为读 **bundle 内 `Info.plist` 的 `NativeAdrenoPublish` key**
  （`NativeAdrenoMetalPlugin.mm:50-63`），bundle 目录是 sandbox 授权 subpath。
- 两版 addon 的条目/签名各自自洽（解包核对：旧版有独立 `.publish` 2 B、Info.plist 622 B 无 key；
  新版无 `.publish`、Info.plist 663 B 含 key）。

**`t733` 的作用（l696 os_log 实证）**：上游 `board.toml:127` 有而我们的设备仓没有；
缺它时 `NativeAdrenoCpu.cpp:46-53` 的 `VM_WIMG_RT` 分支返回 `Unknown`
→ `NativeAdrenoMemory.cpp:726` 拒绝 import → 插件报
`Unsupported{HostMemory, NotRepresentable}`。l696 加后 5→0。

**已排除的伪墙**（详见 `state/HANDOVER-20261110-l696.md`）：
`no device answer` 6 条(harmless 常量)、`PUB/STn=0` 的 CDC 缺通道、
`dt=-8`/effaceable(`-apfs_shared_datavolume` 官方 bypass 已生效)、
`data_ark.plist`(d46a 亲核无本地落点)、SetupDone/SBDisable* plist(l696 证 5/5 写成功读回一致)、
触摸(l697 否定)、`sheng-vsync=60`(l684 否定)、`nabu-mapper=2`(l652/685/690)、
`reevaluateDisplayLink: wants:NO devices:NO`(那是 HID hit-test 的 BKDisplayLink，非主合成)。

**当前黑屏性质（已定）**：l696 屏全 `(0,0,0,255)` =
`NativeLegacyDisplay.cpp:388` blank swap 的 `0xff000000`；
`NativeLegacyRequest.h:22-28` 判定「所有 layer disabled」才走该分支
⇒ 上层没往 surface 画内容。显示驱动侧 5 帧全 `ncpy`+`nret` 成功，不是驱动问题。

## 3. 为什么离线不行

`publish::requested()` 的沙箱裁决（Info.plist 是否可读）、`deviceForPort` 能否创建、
CA 是否因此建立 `CAContext`/`CALayer`，全是运行期行为。离线只能证明两条代码路径
各自存在且自洽，不能证明它们在同一 boot 里同时成立。

## 4. 失败也有信息

- **import 仍 0 + `PUB`/`STn` 出现**：published 模式进了 → 看 `frames` 与屏内容。
- **import 仍 0、但 `CAContext`/`CALayer` 首次出现（l696/l697 全 0 命中）**：合成器开始建图层，
  即使帧数暂未涨也是实质进展。
- **import 仍 0、`PUB` 仍 0、frames 仍 6**：两修复叠加仍不足以让插件进 published 模式，
  断点在 `publish::requested()` 之外的发现链（`MetalPluginName` → bundle 加载）；
  按 `lessons-l695u-metal-discovery.md` 的现成钩子
  `native_adreno_metal_dump_stats()`（全树只有定义、无调用者）加一行调用即可定位——
  但那是改内核，需单独一轮。
- **`frames` 越过 6 / 屏上出现非 Logo 内容**：根因闭环，继续看 UI 完整性。
- **backboardd 崩溃**：alioth 同栈 `0x106da915c` SIGBUS 签名，回退 l695u 即对照。

## 5. 唯一变量与回滚点

**唯一变量 = args 增加 `t733-managed-rt-normal=1`**（674→698 字符，37→38 token，
预算 700 内；tag `l695u`→`l698`）。基线为 **l695u 全套**：

| 文件 | sha256（前 8） |
|---|---|
| kernel.macho | `efcb4c34` |
| kernel.macho.adt | `78a6f7e9` |
| owner-trust.bundle | `76783683` |
| **gpu-addon.bin** | **`39dc17fe`（新版，本轮关键）** |
| boot-merge-r3.img | `ead4b531` |

payload `41aafc51`（仅 toml 的 args 串与目录项 size）。
- 回滚包：`hold/l695u/`
- 回滚命令：`fastboot flash cust hold/l695u/payload.img`

## 6. 恢复预案

**不碰 boot_a**（AGENTS.md 0.10）。流程：recovery arm misc →
`fastboot flash cust hold/l698/payload.img` →
**`fastboot flash misc misc-bootrecovery.bin`（flash 后重新 arm）** → `fastboot boot`。
本轮 args 无 `nabu-autolog`，内核不会自己 arm misc。
若 fastboot 全失败：人工长按电源 10 秒。

## 7. 门禁

| 项 | 结果 |
|---|---|
| `audit_bootability.py` (base l695u) | ✅ 0 fail, 0 warn |
| `audit_dual.py` | ✅ 7/7 match |
| `precheck.py` + args token diff | ✅ stamp written，added/dropped 与声明一致 |

**注**：编译侧 fc76 自 11-06 14:57 未消费，本轮板侧自行出包；双审计两侧均为板侧实测
（偏离 AGENTS.md 0.6，已记录）。
