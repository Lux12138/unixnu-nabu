# nabu bring-up 流程图

生成时间：2026-10-23

## 一、七阶段总览（官方 `/porting/bring-up` 划分）

```
                        ┌─────────────────────────────────────────────┐
                        │  Stage 1  UEFI 固件                         │
                        │  Mu UEFI 起来、framebuffer 有输出            │
                        └────────────────────┬────────────────────────┘
                                             │ ✅ 已过
                        ┌────────────────────▼────────────────────────┐
                        │  Stage 2  UniXNU 菜单                       │
                        │  读 unixnu.toml、列出条目                    │
                        └────────────────────┬────────────────────────┘
                                             │ ✅ 已过
                        ┌────────────────────▼────────────────────────┐
                        │  Stage 3  Payload                           │
                        │  kernel.macho + .adt 就位                    │
                        └────────────────────┬────────────────────────┘
                                             │ ✅ 已过
                        ┌────────────────────▼────────────────────────┐
                        │  Stage 4  DarwinBoot                        │
                        │  校验 kernel/ADT/trustcache → ExitBootServices│
                        └────────────────────┬────────────────────────┘
                                             │ ✅ 已过（RAMDisk 节点是关键）
                        ┌────────────────────▼────────────────────────┐
                        │  Stage 5  内核 entry → platform expert      │
                        │  内核日志、探针、IOKit 初始化                │
                        └────────────────────┬────────────────────────┘
                                             │ 🟡 进行中
                                             │ checkpoint 123
                                             │ 卡在 IOSurface
                        ┌────────────────────▼────────────────────────┐
                        │  Stage 6  存储与根                           │
                        │  block driver → 卷 → 挂根 → launchd          │
                        └────────────────────┬────────────────────────┘
                                             │ ⬜ 未开始  ← 路线 A
                        ┌────────────────────▼────────────────────────┐
                        │  Stage 7  用户态                             │
                        │  launchd → SpringBoard → 桌面                │
                        └─────────────────────────────────────────────┘
                                             ▲
                                             │ ⬜ 远期  ← 路线 B 是它的前置
```

## 二、Stage 5 内部：我们停在哪

```
arm_init
   │
   ├── nabu_mark_map（fb 映射） ✅
   │      └── UART tee：QEMU 可用 / 真机禁用（0x9000000 在 SM8150 是别的寄存器）✅
   │
   ├── STARTUP 循环（checkpoint 50 是循环入口）
   │      │
   │      ├── EARLY_BOOT / ulock_initialize        ✅ checkpoint 123
   │      ├── ml_processor_register                ✅
   │      ├── corecrypto POST（全部 PASSED）        ✅
   │      ├── SKYWALK 32 项自测                    ✅
   │      ├── bsd_autoconf → kminit                ✅
   │      └── IOsurfaceRoot::installMemoryRegions  🛑 停在这里
   │
   └── （之后应有）setconf → vfs_mountroot → load_init_program
```

## 三、当前卡点的两种解释

```
                     停在这两行：
    IOsurfaceRoot::installMemoryRegions()
    IOSurface disallowing global lookups
                              │
              ┌───────────────┴───────────────┐
              │                               │
     ┌────────▼─────────┐          ┌──────────▼──────────┐
     │ 解释 A：死路      │          │ 解释 B：只是日志中断 │
     │                  │          │                     │
     │ 闭源 IOSurface   │          │ 内核继续跑了，      │
     │ kext 在非 Apple  │          │ 但后面没有 kprintf  │
     │ 平台跑不通，     │          │ 输出到 fb           │
     │ 主动停住         │          │                     │
     └────────┬─────────┘          └──────────┬──────────┘
              │                               │
     需要换掉它                        需要用更细的探针
     （自研 surface）                  或串口日志确认
              │                               │
        ← 路线 B 的工作                  ← 下一步应先排除
```

**重要**：**解释 B 还没被排除。** 我们一直假设是死路，但没验证内核是否还在跑。
验证方法：在后面几步加探针，或在 `setconf` 附近加 kprintf。

## 四、两条路线的关系

```
                    当前基线（checkpoint 123）
                              │
              ┌───────────────┴───────────────┐
              │                               │
    ┌─────────▼──────────┐        ┌───────────▼──────────┐
    │  路线 A            │        │  路线 B              │
    │  根文件系统         │        │  显示（IOSurface）    │
    ├────────────────────┤        ├──────────────────────┤
    │ 1. 造 userdata 镜像 │        │ 1. 写 IOCoreSurfaceRoot│
    │    （OS+SystemOS）  │        │ 2. 写 AppleCLCD      │
    │ 2. 配 root UUID     │        │ 3. 注册 KernelConfig │
    │ 3. flash userdata   │        │ 4. 让 IOMFB 起来      │
    │ 4. 让根卷挂上        │        │                      │
    │ 5. launchd 起来      │        │                      │
    ├────────────────────┤        ├──────────────────────┤
    │ 周期 1–2 周         │        │ 周期 3–4 周          │
    │ 风险：低（通用路径） │        │ 风险：高（板级重写）  │
    └─────────┬──────────┘        └───────────┬──────────┘
              │                               │
              └───────────────┬───────────────┘
                              │
                    ┌─────────▼──────────┐
                    │  Stage 7 用户态     │
                    │  SpringBoard       │
                    └────────────────────┘
```

**两条路线不冲突**：A 解决「系统起不起得来」，B 解决「看不看得见」。
A 的先决条件是排除上面的「解释 B」——如果内核其实还在跑，A 会直接通向 Stage 6。

## 五、并行开发的具体做法

```
  main 分支（当前基线，永远可构建可刷机）
    │
    ├── feature/stage6-userdata      ← 路线 A
    │     · board.toml 的 [userdata] 与 root_match_volume_uuid
    │     · CI 增加 build userdata 步骤（需要拉 OS/SystemOS，几 GB）
    │     · 不碰 defconfig 的 9 个开关
    │
    └── feature/stage5-iosurface     ← 路线 B
          · iokit/Kernel/arm/NabuSurface.cpp + NabuFramebuffer.cpp
          · patches/xnu 里注册 IOCoreSurfaceRoot / AppleCLCD
          · 需要新增 [kernel] archives 条目
          · 不碰 board.toml 的 [userdata]

  规则：
    1. main 永远保持 checkpoint 123 的可刷状态
    2. 两条分支各自 CI，各自刷机，互不干扰
    3. 任何一条分支连续 2 轮构建失败 → 回 main 重新评估
    4. 不共用 defconfig 变更（那是上次烧掉十几轮的原因）
```
