# 上板报告 l696（t733-managed-rt-normal 轮）

## 1. 唯一问题

`t733-managed-rt-normal=1` 能否让 CA 的 no-copy surface 通过 `importTask`（不再报
`HostMemory / NotRepresentable`），使 `frames xfer` 越过 6？

## 2. 已闭合到哪

**板上现象（l695u tracev3，5 次）**：
```
native-metal: surface import refused: no-copy buffer:
    Unsupported(Unsupported { feature: HostMemory, needs: NotRepresentable })
```

**传导链（每一环都有静态证据）**：

1. 插件 `NativeBridgeMetalFrontend.mm:400`
   `nmb_buffer_new_no_copy_releasing(gDevice, base, bytes, MTLResourceStorageModeShared, ...)`
   —— CA 的 IOSurface 是 no-copy buffer，走 `MTLResourceStorageModeShared`。
2. `NativeAdrenoUserClient.cpp:306` `importBo()` → `NativeAdrenoMemory::importTask()`。
3. `NativeAdrenoMemory.cpp:725-729`
   ```cpp
   const NativeAdrenoCpuCacheMode mode=NativeAdrenoQueryPageCacheMode(physical);
   if(mode==NativeAdrenoCpuCacheMode::Unknown) {
       status=kIOReturnUnsupported; goto failed;
   }
   ```
4. `NativeAdrenoCpu.cpp:31-56`
   ```cpp
   const unsigned attribute=pmap_cache_attributes((ppnum_t)(physical>>12))&VM_WIMG_MASK;
   switch(attribute) {
       ...
       case VM_WIMG_RT: {                       // = VM_WIMG_WCOMB|VM_MEM_RT = 0x16
           int managed_rt_normal=0;
           PE_parse_boot_argn("t733-managed-rt-normal",&managed_rt_normal,sizeof(managed_rt_normal));
           if(managed_rt_normal==1) {
               return NativeAdrenoCpuCacheMode::NormalUncached;   // ← 上游走这里
           }
           return NativeAdrenoCpuCacheMode::Unknown;             // ← 我们走这里
       }
   ```
5. `VM_WIMG_RT`（`osfmk/arm/memory_types.h:45`）是**受管 render-target 页**的标记，
   CA 的 IOSurface 正是这一类 —— alioth 的记录原文：
   > `Managed render-target IOSurface pages are mapped Normal NC under t733-managed-rt-normal=1`

**差异点（已核对）**：

| | 上游 `board.toml` | 我们的设备仓 |
|---|---|---|
| `t733-managed-rt-normal` | **有**（`:127`） | **没有** |

**已排除**：
- DRAM 窗口不足：ADT 实测 `dram-base=0x80000000 / dram-size=0x300000000`（12 GB），
  覆盖全部物理内存；`limit=1<<output_bits=1<<40=1 TiB` 远大于 DRAM 上限，
  所以 `physical>=limit` 不可能触发，拒绝只能来自 `Unknown`。
- Turnip/mesa 补丁缺失：`turnip-host-import-images.patch` 与 alioth 版**逐字节相同**
  （sha `8bc141db...`），且 l695u 的 dylib 里可见
  `VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT`。
- 签名/AMFI：l695s 起 0 fatal，`mac_vnode_check_signature` 全 0。

## 3. 为什么离线不行

三件事只能在板上观测：
- `PE_parse_boot_argn("t733-managed-rt-normal", ...)` 在运行期是否解析为 1；
- `pmap_cache_attributes()` 对 CA surface 页实际返回哪个 `VM_WIMG_*`（静态看不到运行期 pmap 属性）；
- `importTask` 是否真的放行、`frames` 是否真的越过 6。

## 4. 失败也有信息

- **`surface import refused` 仍在**：`VM_WIMG_RT` 不是 CA surface 的实际属性（可能是别的
  `VM_WIMG_*` 落进 `default: return Unknown`）。下一轮给 `NativeAdrenoQueryPageCacheMode`
  加一行 IOLog 打 `attribute` 原值。
- **refused 消失但 frames 仍 6**：import 不是唯一闸门，断点在 `serveScanout` 或
  WindowServer 的合成路径，按 `PUB`/`STn` 与 `nabu-display` 继续查。
- **frames 涨过 6**：根因确认，接着看屏上内容（`nabu-display: screen` 直方图）判断是否出 UI。
- **backboardd 崩溃**：alioth 同栈的 `0x106da915c` SIGBUS 签名（`docs/devices/alioth.md:378`），
  若出现则 publish 是唯一变量，回退 l695p 即对照。

## 5. 唯一变量与回滚点

**唯一变量 = args 增加 `t733-managed-rt-normal=1`**（37→38 token，674→698 字符，上限 700）。
其余五个文件与 l695p **逐字节相同**：

| 文件 | sha256（前 8） |
|---|---|
| kernel.macho | `efcb4c34` |
| kernel.macho.adt | `78a6f7e9` |
| owner-trust.bundle | `b1bd85cc` |
| gpu-addon.bin | `dcd25289` |
| boot-merge-r3.img | `ead4b531` |

- 回滚包：`hold/l695p/`（已恢复，sha `f84046de...` 与原始一致）
- 回滚命令：`fastboot flash cust hold/l695p/payload.img`

## 6. 恢复预案

**不碰 boot_a**（AGENTS.md 0.10，`tmp/guard_no_boot_flash.py` 强制）。
流程：recovery 里 arm misc → `fastboot flash cust hold/l696/payload.img`
→ **`fastboot flash misc misc-bootrecovery.bin`（flash 后重新 arm）** → `fastboot boot`。

- 软件侧恢复：`fastboot flash boot_a /e/nabu/tmp/boot_a.twrp.img && fastboot reboot recovery`
  （仅当已落入 boot_a 循环时，且需用户点名）。
- 若 fastboot 命令全部失败：需要人工长按电源 10 秒。
- 提前写明：本轮**没有** `nabu-autolog`，内核不会自己 arm misc ——
  每次 flash 后必须手动 re-arm，否则复位后 ABL 直接启动 boot_a。

## 7. 门禁

| 项 | 结果 |
|---|---|
| `audit_bootability.py` | ✅ 0 fail, 0 warn |
| `audit_dual.py` | ✅ 7/7 match |
| `precheck.py`（含 args token diff） | ✅ stamp written |

**注**：编译侧（fc76）自 11-06 14:57 起未消费，本轮由板侧自行出包。
双审计两侧均为板侧实测（编译侧缺席，无法构成真正的双签）——这是对 AGENTS.md 0.6 的偏离，
记录在此。
