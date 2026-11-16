# 上板报告 l697（import 修通后重做触摸注入）

## 1. 唯一问题

l696 已把 `surface import refused` 从 5 条清零；**在这个新状态下**注入 home 手势，
能否让 CA 提交新帧（`sat` / `xfer` 变化），并在屏上出现非纯黑内容？

## 2. 已闭合到哪（静态链）

**为什么这不是重复实验**：历史所有注入轮
`l663 / l666 / l667 / l668` 的 args 都 **没有** `t733-managed-rt-normal=1`
（逐 token 核对：`t733=False`）——它们全在「CA no-copy surface 被 `HostMemory/NotRepresentable` 拒绝」
的旧状态下跑的，那时 backboardd 连 surface 都建不起来，`sat=0` 是必然结果。
**l696 是第一次在 import 修通后具备可导入 surface 的状态**，此时再测触摸才有判别力。

**l696 的板上证据**：
- os_log（macul 解出 9608 条正文）：`surface import refused` 0、`HostMemory` 0、
  `metallib Fault` 0；插件被 backboardd(pid56) 加载。
- CDC：`census2 cl=2 clients=pid 56, backboardd|pid 54, thermalmonitord`；
  `IOMFB: default_fb_surface: No Default FB Surface found`（与历史一致，surface 已建）。
- os_log：`CAWindowServerDisplay:... size:{1600,2560} scale:2 <CADisplay:LCD primary>`。
- 但 `frames xfer=6` 冻结、屏 800×1280 全 `(0,0,0,255)` 纯黑。
- 黑屏性质已定：`0xff000000` 小端 = `NativeLegacyDisplay.cpp:388` 的 blank swap；
  触发条件 `NativeLegacyRequest.h:22-28 native_legacy_blank_state()` = 所有 layer disabled。
  ⇒ **上层没往 surface 画内容**，不是没帧、不是驱动、不是 import。

**注入代码（静态，逐行）**：
- `package/NabuTouch.cpp:1092` `PE_parse_boot_argn("nabu-touch-inject", ...)`
- `:1095-1102` `t >= 200 s` 起触发，`injectStep < 3`，每步 60 ms
- `:1104-1119` 构造 `Frame{count=1}`、底边上滑 home 手势、`hid->deliverSt(f, st)`、`TLOG("inject i=..."`
- `:1130+` `sat` = latch 后 500 ms 内的 xfer 变化

**alioth 上游实践**：`docs/devices/alioth.md:258`
> 「The compositor submits frames when something asks it to ... with a touch, swaps
> 76/144/215/413/830; without one, 8 to 10. An idle panel shows whatever was drawn once.」

## 3. 为什么离线不行

import 修通后 compositor 是否响应触摸、CA render server 是否因此进入帧提交循环，
全是运行期状态；静态只能证明注入代码与事件送达路径存在，不能证明 UI 会重绘。

## 4. 失败也有信息

- **`inject i=` 出现、`sat=0`、`xfer` 仍 6**：事件送达成立 → 断点确定在
  **CA render server / display link 未进帧循环**（配合 l696 的 `CARenderServerStart` /
  `CADisplayLink` 0 命中）。下一轮转向查 backboardd 的 render server 建立条件，不再碰触摸。
- **`sat>0` 或 `frames` 越过 6**：compositor 会响应，继续看屏上内容
  （`nabu-display: screen` 直方图 / `screen.bgrx`）。
- **屏不再纯黑**：即使帧数少，只要有非黑像素就说明 UI 层开始画了。
- **backboardd 崩溃 / SIGTRAP 增多**：alioth 同栈的 `0x106da915c` 签名，回退 l696 即对照。

## 5. 唯一变量与回滚点

**唯一变量 = args 把 `nabu-state-ring=10` 等长换成 `nabu-touch-inject=1`**
（698→699 字符，38 token，预算 700 内；tag `l696`→`l697`）。

`nabu-state-ring` 只控制 `NABUSTATE1` 状态快照频率（`NativeUfs.cpp:4092-4105`），
不在 `args_swap.py` 的 LIVE 列表；本轮判据用 CDC `nabu-display: frames` + diag 导出 os_log，
不依赖该快照。

其余五个文件与 l696 **逐字节相同**：

| 文件 | sha256（前 8） |
|---|---|
| kernel.macho | `efcb4c34` |
| kernel.macho.adt | `78a6f7e9` |
| owner-trust.bundle | `b1bd85cc` |
| gpu-addon.bin | `dcd25289` |
| boot-merge-r3.img | `ead4b531` |

payload `9fb5f1dc`（仅 toml 的 args 串与目录项 size）。

- 回滚包：`hold/l696/`
- 回滚命令：`fastboot flash cust hold/l696/payload.img`

## 6. 恢复预案

**不碰 boot_a**（AGENTS.md 0.10，`guard_no_boot_flash.py` 强制）。
流程：recovery arm misc → `fastboot flash cust hold/l697/payload.img`
→ **`fastboot flash misc misc-bootrecovery.bin`（每次 flash 后重新 arm）** → `fastboot boot`。

- 本轮 args **没有** `nabu-autolog`，内核不会自己 arm misc，必须手动 re-arm。
- 软件侧恢复：`fastboot flash boot_a tmp/boot_a.twrp.img && fastboot reboot recovery`
  （仅当已落入 boot_a 循环且用户点名时）。
- 若 fastboot 命令全失败：人工长按电源 10 秒。

## 7. 门禁

| 项 | 结果 |
|---|---|
| `fsck.fat -n` | ✅ 10 files |
| `audit_bootability.py` | ✅ 0 fail, 0 warn |
| `audit_dual.py` | ✅ 7/7 match |
| `precheck.py`（含 args token diff） | ✅ stamp written，added/dropped 与声明完全一致 |

**注**：编译侧（fc76）自 11-06 14:57 起未消费，本轮由板侧自行出包；
双审计两侧均为板侧实测（已记录偏离 AGENTS.md 0.6）。
