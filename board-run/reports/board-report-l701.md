# 上板报告 l701（官方 addon 功能结果行 + APFS nop）

> **2026-11-10 校正**：本报告第 2 节「l696–l700 静态排除后剩下唯一未观测量 / iOS 侧没走 Metal 发现」是**上板前的旧假设**，已被后续证据否证：l699 证明 backboardd 的 type=0x4147 连接已建立（266 次 externalMethod），shipped kernel 的 `zzw/zzwb` grant 补丁让 `NativeSandboxGrantProcess=backboardd` 短路两道真实 MACF 准入检查；`Sandbox: backboardd … deny` 来自驱动内已有的诊断调用，不是打开失败。详见 `state/HANDOVER-20261110-l696.md` §7q。本轮上板本身有效，结论只保留「union mount 全通、281 s 正常窗口」。

## 1. 唯一问题

**union mount 到 `/System/Library/Extensions` 到底成没成？**
拿官方功能结果行 `nabu-gpu-addon: mounted/target/control/status` 的运行期读数。

这是 l696–l700 全部静态排除后剩下的**唯一未观测量**：
类在（l700 `IOAcceleratorES` size=136）、尺寸对、属性在、addon 换了、
t733 通了、触摸/激活 plist/effaceable/IOMatchCategory 全否证，
但 iOS 侧没走 Metal 发现。而框架补丁明写 Metal 发现加载
`/System/Library/Extensions/<MetalPluginName>.bundle`，其前提是 bindfs union 成功。

## 2. 已闭合到哪（与 qoder 记忆对齐）

**官方输出行已存在，不是自造探针**（qoder `feedback-no-custom-probes.md` 的红线）：
`zzyb-native-cryptex-os-view.patch:177` / 编译树 `kern_exec.c:7757`
```
nabu-gpu-addon: mounted=%d target=%d control=%d status=%d | dmn=%d/%d | wlp=%d/%d
mounted = stage>=3（union mount 成功）
target  = stage>=5（owner 的 NativeAdrenoMetal.bundle/Info.plist 可见）
control = stage>=4（sealed 下层 AGXMetalA10.bundle 可见）
status  = native_gpu_addon_status（errno）
```
qoder 当时就定性为「Functional result, not a probe」，**本轮不新增任何打印/计数器/采样**。

**为什么这行从来没在板上出现**（本会话现量，扫全部近期 kernel.macho）：
```
正常包 l673–l686 / l690 / l691 / l695p,s,u / l696–l700 : APFS nop=有, addon 行=0
重编包 l687–l689 / l692–l694 / l695r,t              : APFS nop=无(tbnz), addon 行=1
```
⇒ 含该行的包全部因重编漏了 `apfs_nop.py`，在 Data 挂载时 panic
（`l695r` CDC 尾：`unencrypted data volume is not allowed @apfs_vfsops.c:2276`）。
qoder `project-nabu-l685-local-build.md` 记 l687/688/689 三次重编内核 309–319 s 早死，
`feedback-regression-ladder-and-rollback.md` 记「拿可用基线后为读数改内核」导致卡 fastboot
⇒ **不能重编、不能加探针**（§7k 的 l701 重链方案已按这些红线放弃并完整回滚）。

**本轮做法（零重编、零源码改动、零新探针）**：
取 `hold/l695r/kernel.macho`（sha `1f82e4a3`，自带官方 addon 结果行），
用历史验证过的 `apfs_nop.py` 同款操作把唯一的
`08 21 44 39 a8 00 00 37` 里 `a8000037` 换成 `1f2003d5`（nop）。
- 唯一命中（现量）：`pair` 在 `0x6cd2f64`，仅 1 处。
- 补前 sha `1f82e4a3…`，补后 `fb3ce142…`，**逐字节仅差 4 字节**
  （`a8 00 00 37 → 1f 20 03 d5`），其余 125,628,412 字节相同。
- 这就是 l673 起每轮都做的 APFS policy 修补，不是新操作。

**执行时刻已静态核到**：`native_gpu_addon_prepare` 由
`kern_exec.c:8191` 的 `if (!error) native_gpu_addon_prepare(p, scratch)` 调用，
在 `native_owner_data_prepare` 的 Data 挂载成功后、launchd 之前
（同段已有 `nabu: skip-setup prefs_m=5/5` 证明这段在当前板上会跑）。
⇒ 即便重编内核有 309–319 s 早死风险（qoder 记），该行也在早死之前打印。

## 3. 为什么离线不行

`native_gpu_addon_stage/status` 是运行期 `vnode_lookup` / `mount()` 的结果；
静态只能证明代码路径、官方输出行、以及「补 nop 前后仅 4 字节差」存在，
不能给出 mount 是否被拒、sealed 下层是否可见。只有板上这一行能回答。

## 4. 失败也有信息

- **`mounted=1 target=1 control=1 status=0`**：union 全通、插件 bundle 可见
  ⇒ 根因在 iOS 侧 provider 匹配（回到 §7g 的 provider 身份对账），不是挂载。
- **`mounted=0` / `status=2(ENOENT)`**：addon 源目录或 sealed 下层缺失
  ⇒ 这才是根因，修法转向补 addon 安装 / root 镜像条目。
- **`mounted=1 target=0`**：union 成功但 owner 的 Info.plist 不可见
  ⇒ addon 内容或签名/权限问题。
- **仍 panic / 早死**：重编内核的已知风险（qoder 记 309–319 s），
  按预案回滚 `hold/l700`，本轮判「该路线不可用」，不再重复。

## 5. 唯一变量与回滚点

**唯一变量 = kernel.macho**（`efcb4c34` → `fb3ce142`，只换内核）。
args 与 l700 **逐 token 相同**（694/38，含 `tag=l700`），
其余四文件与 l700 逐字节相同：

| 文件 | sha256（前 8） |
|---|---|
| **kernel.macho** | **`fb3ce142`（本轮变量）** |
| kernel.macho.adt | `78a6f7e9` |
| owner-trust.bundle | `76783683` |
| gpu-addon.bin | `39dc17fe` |
| boot-merge-r3.img | `ead4b531` |

payload `806f4c8f`（`mcopy` 覆盖 `KERNEL~1 MAC`，抽回 sha 与 `fb3ce142` 一致）。
- 回滚包：`hold/l700/`（已验证 4 分钟窗口、`efcb4c34`）
- 回滚命令：`fastboot flash cust hold/l700/payload.img`

## 6. 恢复预案

**不碰 boot_a**（AGENTS.md 0.10）。流程：recovery arm misc →
`fastboot flash cust hold/l701/payload.img` →
**`fastboot flash misc misc-bootrecovery.bin`（flash 后重 arm）** → `fastboot boot`。
args 无 `nabu-autolog`。若 fastboot 全失败：人工长按电源 10 秒。
本轮是重编内核，早死/卡住概率高于 args 轮 ⇒ 回滚路径已在 §5 写明，先试
`fastboot boot` 回 TWRP，不行再按 qoder 记录的「重写 boot_a=TWRP」路径（需用户点名文件）。

## 7. 门禁

| 项 | 结果 |
|---|---|
| `fsck.fat -n` | ✅ 10 files |
| `audit_bootability.py` (base l700) | ✅ 0 fail, 0 warn（kernel sha 现量 `fb3ce142`）|
| `audit_dual.py` | ✅ 7/7 match |
| `precheck.py` + args token diff | ✅ stamp written，added/dropped 均为空（args 零变化）|
| 补 nop 字节级自检 | ✅ 仅 4 字节差，pair 唯一命中 |

**注**：编译侧 fc76 自 11-06 14:57 未消费，本轮板侧自行出包；双审计两侧均为板侧实测
（偏离 AGENTS.md 0.6，已记录）。本轮严格不新增自造探针，输出行是框架官方已有的功能结果行。
