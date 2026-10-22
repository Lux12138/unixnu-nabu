# 最新进展（2026-10-23 06:01）

## 突破：DarwinBoot 移交成功了

上一版卡在：

```
darwinboot: boot refused: Compromised Data; firmware retained
```

现在：

```
DARWINBOOT: hand-off to the kernel

kernel  va 0xFFFFFFF00427C000  pa 0x0C007C0000
entry   pa 0xC31EB488
video   base 0x9C000000  1600x2560  row 6400 bytes  32 bpp
ID_AA64MMFR0_EL1 0x101122  (PARange 2, 4 KiB granule supported)

Exiting boot services. A green bar across the top
means the kernel entry code ran.
```

**绿条已在，面板几何也报对了（1600x2560，真机分辨率）。**

## 根因与修法

`DarwinBoot.c` 里：

```c
if (Request->Ramdisk != NULL) {
    if ((RamdiskOffset == MAX_UINTN) || ...) {
        Status = EFI_COMPROMISED_DATA;   // 我们命中这里
        goto Fail;
    }
}
```

`RamdiskOffset` 只有在 `AdtNode()` 于 `/chosen/memory-map` 下找到
名为 `RAMDisk`（16 字节、两半全 0）的属性时才会被设置。

而 `make-nabu-adt.py` **只有带 `--data-ramdisk` 才生成这个属性**，
我们的打包脚本从来没传过。

**加 `--data-ramdisk` → ADT 里出现 `RAMDisk` 节点（13340 字节）→ 移交成功。**

## 顺带发现：官方流程的缺口

`board.toml` 的 `[payload.adt]` 只有 `packed_data_ramdisk` 一个布尔，
**没有普通 `data_ramdisk`**。也就是说：

- 走官方 `unixnu build` 流程，用**普通 FAT ramdisk** 当根，**无法生成 `RAMDisk` 节点**
- 只有 packed Data 格式才被 board.toml 支持

这解释了为什么之前一直卡在 `Compromised Data`：我们用的是普通 ramdisk，
而可配置的只有 packed 那条路。

**这条值得给 UniXNU 提 issue / PR。**

## 现在的问题

内核 entry 跑了，但**还没有内核日志出现在屏幕上**。
下一步是确认它到了哪个探针 —— 需要再拍一张（日志画在 framebuffer，不是串口）。

## 已有链路的完整状态

| 环节 | 状态 |
|---|---|
| CI 构建内核 | ✅ |
| payload 打包（kernel + ADT + ramdisk）| ✅ |
| DarwinBoot 加载 loader protocol | ✅ |
| **DarwinBoot 移交内核** | ✅ **本轮突破** |
| mockfs 编入内核 | ✅ |
| `IOStorageFamily` 回到 prelink（提供 IOMedia）| ✅ |
| 内核跑到 bsd_autoconf 之后 | 🟡 **待确认当前探针** |
| 根文件系统挂载 | ⏳ 待测（ramdisk 已就位）|
| launchd | ⏳ 远 |
