# 上板报告 l710（一次性临时功能轮：内嵌 `nabu_fb` plist 同长数据补丁加 `BKHideAppleLogoOnLaunch=true`）

> 本报告是**上板前**门禁文件；本会话未运行 adb/fastboot、未上板、未写 `E:\nabu\logs\rounds\l710.json`、
> 未改 `E:\nabu\hold\l707\`（只读模板，payload `84a2ec1f`）、未污染 `E:\nabu\hold\l700\`。
> 按 `E:\nabu\AGENTS.md` 0.5 / 0.6 / 0.7 / 0.9 / 0.10 编写；格式照
> `E:\nabu\state\board-report-l709.md`。

## 0. 用户授权与性质

**用户自动授权（2026-11-11）**：「哪怕是插帧，你可以使用临时插帧，进行必要地区的插帧，义无反顾地向前推进」，
并明确「自动推进、临时插帧/插针、义无反顾向前、不要等指示」。⇒ 本轮是
**用户自动授权 + 一次性临时功能轮 + 用完回滚、不进默认 args**。

**性质：同长数据补丁，不是重编内核。** 只改 `E:\nabu\hold\l707\kernel.macho` 内嵌的
`nabu_fb` plist C 字符串（323 B → 323 B，总文件大小 125,628,416 B 不变），
**不改源码、不重编、不新增 printf/IOLog/计数器/探针、不放宽安全策略、不动 `boot_a`**。
板上 `hold/l707/kernel.macho` 的 Mach-O **ncmds=25、无 `LC_CODE_SIGNATURE`**（loadcmds 止于
`0x16b0`，plist 在 `0x226ac`）；APFS nop @`0x6cd2f68` = `1f2003d5` 远离 plist 区，补丁未触碰；
**历史 l701 已用「改 kernel.macho 字节 + 同步 payload 内 kernel.macho」安全上板**
（`E:\nabu\logs\rounds\l701.json`，281 s 正常窗口、无 panic）。

args/toml **一个字节不改**：`687/38`，与 l707 完全一致（`nabu-splash=1` 与
`offline-legacy-transfer=1` 都在），`added=[]` / `dropped=[]`、38/38 token 同序同字节、
≤700 硬上限。因此**不需要** args 预算表，改动不在 boot-args 空间。

## 1. 唯一问题（一句话）

**在 backboardd 启动前把 `BKHideAppleLogoOnLaunch=true` 写进 `com.apple.backboardd.plist`，
能否让 BootUI Apple Logo overlay 不出现？**

官方已有输出可判（零新探针）：os_log 若出现
`addOverlay(...): Not showing the boot UI overlay because we were told to hide it` /
`addUnderlay: Not showing the boot UI underlay...` 且
`Presenting overlay: <BKDisplayRenderOverlayAppleLogo>` 消失 ⇒ 键生效；
`screen.bgrx` 若不再是 `ca18b728…` ⇒ overlay 不再主导屏；
若仍 Logo / `Presenting overlay` ⇒ 键名/域不对或 BKSDefaults 不从该 plist 读，按失败收窄。
**静态只能证字符串与写入路径在，必须上板读 os_log/screen ⇒ 满足 AGENTS.md 0.5。**

## 2. 已闭合到哪（日志优先 + 静态链，`chain` 已写进 `state/precheck.json`）

1. **GOP 写路径已证**：`E:\nabu\state\HANDOVER-20261110-l696.md` §7aa.1 /
   `E:\nabu\logs\rounds\l706.json`：`sheng-fb-fill=4` ⇒ 屏全白 `e1547326…`
   （1,024,000 个 `(255,255,255,255)`）。
2. **屏上的 Logo 是 backboardd 主动呈现的 BootUI overlay**：§7aa.2/.3、
   `E:\nabu\logs\rounds\l707.json`：`nabu-splash=1` + `offline-legacy-transfer=1` ⇒
   `frames ncpy=5`，`transfer[5]!=0` 让 splash stand down；屏仍是
   `BKDisplayRenderOverlayAppleLogo`（`ca18b728…`）。§7ab 全量 119,428 行 l707 os_log：
   两次 `Presenting overlay: <BKDisplayRenderOverlayAppleLogo>`（t≈2.640/2.643）、
   `No explicit screen owner` 4 次、`Dismissing boot logo` / `finishedStartup` /
   `Not showing the boot UI overlay because we were told to hide it` **全 0**；
   §7ac：SpringBoard pid27 前台但自身初始化没走完、`finishedStartup=0` ⇒ overlay 一直 sustain。
3. **l708 / l709 把「缺时钟」钉死**：`E:\nabu\logs\rounds\l708.json` 删
   `offline-legacy-transfer=1` ⇒ frames 全 0、屏纯黑 `112b94b6…`，splash 没画；
   `l709.json` 加 `nabu-state-ring=10` 恢复 `NABUSTATE1` banner 时钟 ⇒ splash 真正画出
   近黑蓝背景 + 中心金标 + 白色进度格（`screen.bgrx` sha `00e62fe8…`）。
   ⇒ 板上代码能主动画满屏，缺的只是调用时钟 / **overlay 退场**。
4. **内核写入路径（本轮落点）**：编译树
   `/root/merge/out-v6/nabu/ipados-17.7.11-21H461/nabu/tree/.cache/xnu-work/bsd/kern/kern_exec.c:8520-8572`
   的 `static const char nabu_fb[]` 是 323 B XML plist C 字符串；`:8537-8544` 的
   `fb_prefs[]` 六个路径（mobile/root 各 `backboardd.plist` /
   `com.apple.backboardd.plist` / `.GlobalPreferences.plist`）；
   **`:8539-8543` 四个 backboardd 域 `keepIfPresent=0` 强制覆盖**（仅
   `.GlobalPreferences` 是 `keepIfPresent=1`）；`:8565`
   `nabu_kern_write_file_owner(..., nabu_fb, sizeof(nabu_fb)-1, 0644, uid, gid)` 每轮覆盖写。
   HANDOVER §7ab 记历史 5/5 上板写成功读回一致。
5. **板上字面量精确对上**：`E:\nabu\hold\l707\kernel.macho` @`0x226ac..0x227ee`
   （NUL @`0x227ef`）恰是这 323 B 字符串，含 `CAEnableDeepFramebuffer @0x2275a`、
   `CAEnableFramebufferCompression @0x22784`、`CAEnableIOSurfaceCompression @0x227b5`，
   全核唯一一处。
6. **键名/域绑定（dsc 静态证据）**：`/root/dsc/dyld_shared_cache_arm64.05`
   `com.apple.backboardd @0x1c25f9a`（文件偏移 29,515,674）、
   `BKHideAppleLogoOnLaunch @0x1c263ec`（29,516,780）、ObjC property
   `hideAppleLogoOnLaunch @0x1c263d6`（29,516,758）；BackBoard uuidtext 已有
   `addOverlay(...): Not showing the boot UI overlay because we were told to hide it`
   与 `addUnderlay: Not showing the boot UI underlay because we were told to hide i...`
   两个官方格式串；l707 板报 §2 / §7ab 记 `hideAppleLogoOnLaunch @0x72bd`。
   `com.apple.backboardd.plist` 正是该 BKSDefaults 域的落点（强制覆盖、backboardd 启动前写）。
7. **本包唯一变量与精确性见 §5**：仅 `kernel.macho` 内 plist 123 字节变化；
   三个 `CAEnable*` 键全保留（插件
   `/root/merge/UniXNU-nabu/drivers/AdrenoMetal/NativeAdrenoMetalPlugin.mm:281` 运行时
   也会写回），**没有牺牲任何 CA 键**。

## 3. 为什么离线不行（`AGENTS.md` 0.5 `--why_board`，已写进 `state/precheck.json`）

静态能证：`nabu_fb` 字面量在 `kernel.macho @0x226ac`、`kern_exec.c:8520-8572` 每轮在
backboardd 启动前**强制覆盖** `com.apple.backboardd.plist`、dsc 里
`BKHideAppleLogoOnLaunch` 键名与 `com.apple.backboardd` 域绑定存在、BackBoard uuidtext
有 `Not showing the boot UI overlay because we were told to hide it` 官方格式串、
补丁只在字符串内部且 APFS nop / 代码签名 / 文件大小全不变。

但「这个键在运行期是否被 BKSDefaults 从该 plist 读到并使 BootUI Apple Logo overlay 不呈现」
是**运行期事实**：cfprefsd 缓存、BKSDefaults 注册时机、plist 是否被后续进程覆盖、
`addOverlay` 判定分支是否走 hide 路径，离线都无法计算；os_log 的
`Presenting` / `Not showing` 两串与 `screen.bgrx` 像素只有真机能给。
现有 l707 os_log 已证 overlay 未 dismiss，但**没有 hide 键条件下的输出**，属于
「日志与静态都无法产生的新状态」。⇒ 属 0.5 允许的「验证静态结论在真机上是否成立的那**一次**判定」。

## 4. 失败也有信息（三种判读）

| 结果 | 判读 |
|---|---|
| os_log 出现 `addOverlay(...): Not showing the boot UI overlay because we were told to hide it`（或 `addUnderlay: Not showing the boot UI underlay...`），且 `Presenting overlay: <BKDisplayRenderOverlayAppleLogo>` 消失 | **键生效**：`com.apple.backboardd` 域 + `BKHideAppleLogoOnLaunch` 键名正确，BKSDefaults 确实从该 plist 读；`screen.bgrx` 应不再是 `ca18b728…`（可能是纯黑 / splash / 桌面层），据直方图继续判后续断点 |
| 仍 `Presenting overlay` 两次、屏仍 `ca18b728…` Logo，且 `Not showing ...` 全 0 | **失败收窄**：键名/域不对，或 BKSDefaults 不从 `com.apple.backboardd.plist` 读（cfprefsd 缓存 / 注册时机 / 被 cfprefsd 覆盖 / 该键其实是 `hideAppleLogoOnLaunch` property 的另一落点）。下一刀纯离线查 BKSDefaults 的域绑定与 cfprefsd 同步，不再盲改 plist |
| 出现 `Not showing ...` 但屏仍 Logo / 出现别的屏 | 混合态：`addOverlay` 被拦但 ABL 残留或 `addUnderlay` / BootUI 另一路仍主导；对账 `No explicit screen owner` / `Dismissing boot logo` 与 `screen.bgrx` 直方图，区分「overlay 没画」与「静态残留」 |

**观察项（全部已有输出，零新增 printf/IOLog/计数器/探针）**：
os_log `Presenting overlay` / `Not showing the boot UI overlay` / `addUnderlay` /
`No explicit screen owner` / `Dismissing boot logo` / `finishedStartup`；
`screen.bgrx` sha / 直方图（对比 `ca18b728…`、`e1547326…`、`112b94b6…`、`00e62fe8…`）；
CDC `nabu-display: frames xfer nswap nsurf sbytes nok ncpy nret`、
`nabu: scanout hand-off online`、`nabu-gpu-addon:`、boot-args 回显、panic /
unencrypted panic / `buf_bread_err16`。

**零新探针 / 零重编 / 零安全放宽**：本轮只用板上已有 plist 覆盖写机制 + 官方已有
os_log 格式串 + 已有 screen/CDC 通道；同长数据补丁，未改一行源码。

**fc76 未消费与双审计两侧板侧实测偏离**：编译侧 fc76 自 **11-06 14:57** 未消费；
`audit_dual.py --tag l710` 的 `--peer` / `--mine` **两侧均为板侧实测**
（7/7 equal，真实 sha 见 §6），照 `state/board-report-l707.md` 注明：偏离
`AGENTS.md:36-47` 0.6 的「编译侧 vs 板侧」原意；本轮零重编、无编译侧成果可比对。

## 5. 唯一变量与回滚

**唯一变量 = `kernel.macho` 内嵌 `nabu_fb` plist 同长数据补丁**，args/toml 零变化。

改前 plist 全文（323 B C 字符串，`E:\nabu\tmp\l710-build-20261111\plist-before.txt`）：

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>CAEnableDeepFramebuffer</key><false/><key>CAEnableFramebufferCompression</key><false/><key>CAEnableIOSurfaceCompression</key><false/></dict></plist>
```

改后 plist 全文（仍 323 B，`plist-after.txt`；`<?xml…?>` 后 62 个空格是 XML 无害空白填充）：

```xml
<?xml version="1.0" encoding="UTF-8"?>
                                                              <plist version="1.0"><dict><key>BKHideAppleLogoOnLaunch</key><true/><key>CAEnableDeepFramebuffer</key><false/><key>CAEnableFramebufferCompression</key><false/><key>CAEnableIOSurfaceCompression</key><false/></dict></plist>
```

手术：`E:\nabu\tmp\l710-build-20261111\patch_kernel_l710.py` 以 **只读 mmap ACCESS_READ**
打开 l707 模板（**绝不 mmap 输入 payload 可写**，历史坑见 §7aa.4 教训 1），
删 103 B `DOCTYPE` 行、插 41 B `<key>BKHideAppleLogoOnLaunch</key><true/>`、
用 62 B XML 空白补齐 ⇒ 长度严格不变；`ET.fromstring` 新旧 plist 均可解析，
四个键（1 新 + 3 个 `CAEnable*`）齐全。**未牺牲任何 CA 键**，理由：DOCTYPE 是可省的标准
样板行，去掉后 plist 仍合法，三个 `CAEnable* false` 键必须保留以免改变 Metal 插件运行期行为。
`cmp -l` 仅 **123 字节差异**、偏移 `0x226d3..0x22753` **全部落在 plist C 字符串范围**
（`0x226ac..0x227ee`），前 32 B Mach-O 头 / loadcmds / APFS nop / 其它字符串 / 文件长度
`125628416` 全不变（`cmp-l-kernel.txt`、`diff-bytes.txt`）。

**K710 = `d9b5a17405b2aaac767eeddd82f3914b7e753bb2753262032ccb045d740246e3`**（前 8 `d9b5a174`）。

| 文件 | l707 sha8 | **l710 sha8** | 说明 |
|---|---|---|---|
| kernel.macho | `fb3ce142` | **`d9b5a174`** | 唯一变化；同长 plist 数据补丁 |
| kernel.macho.adt | `78a6f7e9` | `78a6f7e9` | 逐字节相同 |
| gpu-addon.bin | `950a3968` | `950a3968` | 逐字节相同 |
| owner-trust.bundle | `a4a6bae6` | `a4a6bae6` | 逐字节相同 |
| boot-merge-r3.img | `ead4b531` | `ead4b531` | 逐字节相同 |
| payload.img | `84a2ec1f` | **`391510e3`** | 随 kernel 同步；内仅 `KERNEL~1.MAC` 变 |

payload 同步：`E:\nabu\tmp\l710-build-20261111\build_pkg_l710.py` 先以 ACCESS_READ mmap
+ 192 MiB 流式 `copyfileobj` 从 l707 复制到新 `hold/l710/`（l707 全程只读），
再 **`mcopy -D o` 覆盖长名 `kernel.macho`（即短名 `KERNEL~1.MAC`）**：先列 10 个目录项确认
长短名（`dir-entries-10.txt`、`mdir-l710.txt`；`KERNEL~1.MAC/kernel.macho`、
`KERNEL~1.TRU/kernel.macho.trustcache`、`KERNEL~1.GPU/kernel.macho.gpuaddon`、
`KERNEL~1.ADT/kernel.macho.adt`、`SCRATC~1IMG`、`UNIXNU~1TOM`、3 目录 + volume）。
用长名是必要的：直接对 `KERNEL~1.MAC` 短名 `-D o` 会删掉 LFN 并留一个 `0xE5` 项；
长名写法保留 LFN、零删除项、`fsck.fat -n` 仍 `10 files`。mcopy 会按 mtools 惯例
重分配 kernel FAT 链（`12689 → 86227`），与历史 l701 `mcopy` 行为一致
（`l700→l701` payload 同为 125 M 字节 diff，已安全上板）；文件大小
`201326592` 不变，**抽回 `KERNEL~1.MAC` sha == K710**，其余 inner files 全 SAME。

- **回滚基线**：`E:\nabu\hold\l700\`（kernel `efcb4c34`，已验证 4 分钟窗口）；
  **上一包 / 只读模板** `E:\nabu\hold\l707\`（kernel `fb3ce142`、payload `84a2ec1f`，
  本会话六文件 sha 前后未变，见 `l707-baseline-sha.txt` / `l707-after-sha.txt`）。
- **一次性、用完回滚**：`BKHideAppleLogoOnLaunch` 只属于 l710，**不进默认 args**；
  判读完成后刷回 `hold/l700/payload.img` 或 `hold/l707/payload.img`。
- `KERNEL~1.TRU` 内容应与未变的 `owner-trust.bundle a4a6bae6` 逐字节一致（实测 True，
  `selfcheck.txt`），**未改它**。

## 6. 恢复预案 / 门禁 / 附注

**`AGENTS.md` 0.10（绝对红线）：不碰 `boot_a`**（绝不 `fastboot flash boot` / `flash boot_a`；
这块平板没有独立 recovery 分区，recovery 就在 boot 里，boot_a 里是 TWRP）。
流程（**仅文字，本会话不执行**；顺序不可换）：

```
recovery arm misc                                             # 不碰 boot_a
fastboot flash cust hold/l710/payload.img
fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin   # 最后才重 arm misc
fastboot boot hold/l710/boot-merge-r3.img                     # 临时加载，不写任何分区
```

`E:\nabu\dwc3\build\misc-bootrecovery.bin`（4096 B）；设备 `TWRP recovery f91fa664`。
`arm-delay=0`（`sheng-smp=1` 硬性配对），咬点 **380-410 s**，观察完整 **4 分钟窗口**
（`AGENTS.md:57-64`），早 panic 也有重置路径。若 fastboot 全失败：人工长按电源 10 秒。
回滚见 §5；**真正需要还原时只由用户当场点名文件**，仍不经过自动化脚本。

**门禁（本会话实测，全部 PASS）**：

| 项 | 结果 | 证据 |
|---|---|---|
| ① 同长补丁 + 六文件 | 改前/改后 plist 全文；`cmp -l` 仅 123 字节、全在 `0x226d3..0x22753` plist 区；文件长度一致；APFS nop 仍 `1f2003d5`；`hold/l707/` 六文件 sha 前后未变 | `patch_kernel_l710.py`、`plist-before.txt`、`plist-after.txt`、`diff-bytes.txt`、`cmp-l-kernel.txt`、`l707-baseline-sha.txt`、`l707-after-sha.txt` |
| ② `fsck.fat -n` + `audit_bootability.py hold/l710 hold/l707 --expected=d9b5a174…` | l710/l707 均 `10 files, 67550/97913 clusters` rc=0；→ **`OK (0 fail, 0 warn)`** RC=0（PRELINK 47/47、ncmds、payload 体积不变、仅 kernel sha 变） | `gate1-fsck.txt`、`gate1-bootability.txt` |
| ③ `audit_dual.py --tag l710` | 逗号分隔真实 sha `kernel=d9b5a174,payload=391510e3,adt=78a6f7e9,addon=950a3968,trust=a4a6bae6,boot=ead4b531,args=687/38` → **`7 items compared, 7 match, 0 differ`** | `gate2-audit_dual.txt`、`state/audit-l710.json` |
| ④ `tmp/audit_args.py l707 l710` | `687/38 → 687/38`（≤700）；**`added=[]`、`dropped=[]`** → **`RESULT: OK`** rc=0；toml 逐字节同 | `gate3-audit_args.txt`、`selfcheck.txt` |
| ⑤ Windows Python 3.12.10 `precheck.py` | `run_precheck_l710.py` 驱动 `--pkg E:/nabu/hold/l710 --baseline E:/nabu/hold/l707` → `stamp written: state/precheck.json` rc=0；八字段全非空、`token_diff=ok`；`precheck_check.py` → **`[gate] OK`** rc=0，`dual audit OK: audit-l710.json (7 items)` | `gate4-precheck.txt`、`gate4-precheck_check.txt`、`state/precheck.json` |
| ⑥ 六问板报 | 本文件 | `state/board-report-l710.md` |
| 自检 | 抽回 `KERNEL~1.MAC` == K710；抽回 `KERNEL~1.TRU` == `owner-trust.bundle` `a4a6bae6`；toml 与 l707 逐字节同；其余四文件 `78a6f7e9`/`950a3968`/`a4a6bae6`/`ead4b531` 逐字节同；`hold/l707/` 未污染（payload `84a2ec1f`） | `selfcheck.txt`、`build.log`、`mcopy-verify.txt`、`l710-sha8.txt` |

**特别注明**：这是**同长数据补丁，不是重编内核**；APFS nop 未动
（@`0x6cd2f68` = `1f2003d5`）、kernel **无 `LC_CODE_SIGNATURE`**（ncmds=25，
loadcmds 止 `0x16b0`）、历史 l701 同法安全上板（`logs/rounds/l701.json`）。
**零新探针 / 零重编 / 零安全放宽 / 零源码改动 / 零 boot_a 触碰**，
**未运行 adb/fastboot、未上板、未写 `E:\nabu\logs\rounds\l710.json`**。

## 7. 证据路径（全部绝对路径）

- 构建与补丁：`E:\nabu\tmp\l710-build-20261111\patch_kernel_l710.py`、
  `build_pkg_l710.py`、`kernel.macho`、`build.log`、`patch.log`、
  `plist-before.txt`、`plist-after.txt`、`diff-bytes.txt`、`cmp-l-kernel.txt`、
  `dir-entries-10.txt`、`dirlist.txt`、`mdir-l710.txt`、`run_precheck_l710.py`
- 门禁输出：同目录 `gate1-fsck.txt`、`gate1-bootability.txt`、
  `gate2-audit_dual.txt`、`gate3-audit_args.txt`、`gate4-precheck.txt`、
  `gate4-precheck_check.txt`、`selfcheck.txt`、`mcopy-verify.txt`、
  `l707-baseline-sha.txt`、`l707-after-sha.txt`、`l710-sha8.txt`
- 戳与板报：`E:\nabu\state\audit-l710.json`、`E:\nabu\state\precheck.json`、
  `E:\nabu\state\board-report-l710.md`
- 包：`E:\nabu\hold\l710\`（六文件）；只读模板 `E:\nabu\hold\l707\`；回滚 `E:\nabu\hold\l700\`
- 板上证据：`E:\nabu\logs\rounds\l706.json` / `l707.json` / `l708.json` / `l709.json` /
  `l701.json`、`E:\nabu\state\HANDOVER-20261110-l696.md` §7aa / §7ab / §7ac、
  `E:\nabu\state\board-report-l707.md` / `-l709.md`
- 静态链：`/root/merge/out-v6/nabu/ipados-17.7.11-21H461/nabu/tree/.cache/xnu-work/bsd/kern/kern_exec.c:8520-8572`、
  `/root/dsc/dyld_shared_cache_arm64.05`（`com.apple.backboardd @0x1c25f9a`、
  `BKHideAppleLogoOnLaunch @0x1c263ec`、`hideAppleLogoOnLaunch @0x1c263d6`）、
  `E:\nabu\AGENTS.md`

**注**：本轮**可否上板：可**（全部门禁 PASS；上板由用户执行并判读）。

## 8. 下一步（仅建议，本会话不执行）

由用户上板后自行写 `logs/rounds/l710.json`（本会话**不写**），按 §4 三判读表读
os_log 与 `screen.bgrx` sha / 直方图、CDC `nabu-display: frames`、`nabu-gpu-addon:`、
panic，写明 **「一次性临时功能轮、用完回滚」**，然后回滚到 `hold/l700` 或 `hold/l707`。
