# 状态快照（2026-10-23 14:51）

## 决定性结论：IOSurface 不是障碍

**证据**：胶片上彩色方块从 16 个变成 20 个，正好对应新增的四个探针。

| 探针 | 位置 | 状态 |
|---|---|---|
| 65 | `bsd_autoconf()` 返回后 | ✅ |
| 66 | `loopattach()` 之后 | ✅ |
| 67 | `ether_family_init()` 之后 | ✅ |
| 68 | `inittodr()` 之前 | ✅ |
| 69 | `inittodr()` 之后 | ✅ |

**`NABU_MARK(69)` 就在 `vfs_mountroot()` 的正上方。**

```
bsd_autoconf() ✅ → loopattach ✅ → ether_family_init ✅ → inittodr ✅
                                                              ↓
                                                      vfs_mountroot()   ← 现在卡在这
```

### 之前为什么判断错了

`max_hit` 是**最大** checkpoint，而 ulock 探针（120–123）把它顶到了 123，**把 65–69 全盖住了**。
屏幕上的 `123` 一直在说「内核走到了 123」，但我们盯着最后一行**文字**看，把
`IOsurfaceRoot::installMemoryRegions()` 当成了终点。

**它是最后一条 kprintf，不是内核停下的地方。** 它之后紧接的 `dtrace_postinit()`、
`inittodr()` 和条件编译的网络初始化全都不打日志。

**教训**：数字探针和文字日志必须分开读；`max_hit` 会掩盖低编号的进度。

## 当前真正的卡点

**`vfs_mountroot()`** —— 没有可用的根设备。

预期错误：`cannot mount root, errno = N`（`bsd_init.c` 里 `setconf()` 失败后的路径）。

## 下一步：根文件系统（唯一在走的路）

依据 `/guide/userdata` + `/concepts/userdata`：

```
1. board.toml 的 [ipsw] fetch 加 "OS" 和 "Cryptex1,SystemOS"    ← 已加，未验证
2. ./unixnu ipsw fetch            # 拉两个组件（几 GB）
3. ./unixnu build userdata        # SHNGROOT manifest + 两个窗口
4. [payload.adt] 设 root_match_volume_uuid = <OS 卷 UUID>
5. fastboot flash userdata out/nabu/userdata/userdata-root.img
6. 看根卷是否挂上、launchd 是否起来
```

**已知**：`[userdata]` 的 LBA 早实测好
（`first_lba = 0x288C40` / `last_lba = 0x77BF3FA` / `lba_bytes = 4096`）。

**不做什么**：不碰 defconfig、不碰 IOSurface、不启用任何「合格基线」开关。

## 可复现基线

| 项 | 值 |
|---|---|
| 构建 | `39911803391` / success |
| 内核 | 49.2 MB |
| 探针 | checkpoint 1–123，其中 65–69 证明已到 `vfs_mountroot()` |
| defconfig | 9 个开关 |
| `[kernel] archives` | 8 个 |
