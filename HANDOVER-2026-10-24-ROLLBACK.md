# 交接文档 — 2026-10-24 晚（回滚之夜）

> 写给下一个接手的人（或我自己）。**先读这节再动手，否则会重蹈我今天的覆辙。**

## 0. 当前状态一句话

设备停在 `unexpected SIGKILL of launchd` panic —— 与昨天完全相同的基线。
两个仓库已回滚到已知良好版本，内核 md5 `e9ebb7e6…`（17:41 构建），已刷机验证。

---

## 1. 硬件与启动链（不变）

- **小米 Pad 5 (nabu / SM8150-AC / 骁龙860)**，serial `f91fa664`，12 GiB 扩容版
- 启动链：`fastboot boot nabu-xnu64-Z.img` → BootShim → Mu UEFI → UniXNU 菜单
  → **cust 分区 payload** → DarwinBoot → XNU
- 内核：xnu-10063.141.1，`KERNEL_CONFIGS=DEVELOPMENT`，`MACHINE_CONFIGS=BCM2837`
- ADT boot-args（唯一生效的一套，`make-nabu-adt.py:135`）：
  `-enable_kprintf_spam native-ufs-root=1 native-ufs-diagnostics=1 maxmem=4864 amfi_get_out_of_my_way=1 cs_enforcement_disable=1 amfi_allow_any_signature=1`
- 屏幕 1600×2560，console 上限 1780；探针必须 ≥80px 行距、避开 <1780 行

## 2. 仓库状态（全部已推送，远端已强制指回）

| 仓库 | HEAD | 内容 |
|---|---|---|
| `Lux12138/UniXNU` | **`aae83c4`** | 纯探针版：reason creator 探针（RN=/RA=，标签带 `=` 分隔）、全部诊断探针。**无 DWC3、无重启调用** |
| `Lux12138/unixnu-nabu` | **`24b73bc`** | board.toml（含 trust_cache=true）、defconfig、全部交接文档（HANDOVER-exec-fault.md 689 行） |
| `Lux12138/NativeDwc3` | `e2501a8` | **独立的 USB 驱动仓库**（今天新建）。已验证能编译链接，但因缺 GCC 时钟门控会崩溃 —— **不要接回 board.toml**，除非先做 GccUsb |
| `Lux12138/xnu-linux-toolchain` | `1270e1b` | 上游 `Allenwdk/xnu-linux-toolchain` 的 fork，补了 `zzzz-nabu-awk-parens.patch` + `docs/NABU-18-PITFALLS.md` |

**回滚时的关键教训：UniXNU 和 unixnu-nabu 必须一起回滚。** 只回滚一个，board.toml 里的
`build-arm-driver.py NativeDwc3` 会让 archive 8 每轮失败 → `KERNEL NOT REGENERATED` →
刷的是旧内核残留，而你以为改动了。

## 3. 今天搞清楚的事实（全部实测，可直接引用）

### 3.1 主线问题的进展

| 问题 | 结论 |
|---|---|
| execve 缺页 panic | ✅ **已修复**（PAC：`find_copyio_recovery_entry` 少了 `VM_KERNEL_STRIP_PTR`）。`PCOF` 从 `0x230397`→`0x2b7027`，落在表项 9 |
| `vm_page_free_count` 91M | 症状非原因，`dram_size`/`owned_memory_size` 不要动 |
| AMFI boot-args | `AMx=10` → AMFI=1 解析成功，`CS_KILLED=0`，**不是签名问题** |
| SIGKILL 的 reason | **`SIG900008` = `OS_REASON_EXEC`(9) + `EXEC_EXIT_REASON_SECURITY_POLICY`(8)**，交付点实测 `SN9 SC8 SU0` |
| 七个 reason 生产者 | 全部排除：L8019 需 `ip_mac_return≠0`（LU 从未画出）；L7950 的 `custom x18 ABI` 字符串 0 次；L8059/69 是 WRONG_PLATFORM(14)；L8097/121/128 是 CODESIGNING(3) 或 TASKGATED_OTHER(9)；`ip_cs_error` 两个外部源都是 CODESIGNING(3) |
| trust cache | ✅ 已接入：56 字节文件（launchd cdhash `1d52c69747dffc4b1937d241bdbc9cee3b22df18`）内嵌 base64 在 `make-cust-payload.sh`，ADT 有 `TrustCache` 节点。通过 DarwinBoot `ValidTrustCaches` 全部规则。**但没解决 SIGKILL** —— 因为 `load_static_trust_cache()` 需要 AMFI 接口，而 `_amfi == NULL`（没有 AppleMobileFileIntegrity kext） |

### 3.2 今天的新坑（每个都烧了我至少一轮）

1. **探针行号冲突**：`LU`(2500) 被 `SIG`(2500) 覆盖、`KR`(2340) 被 `PN2` 覆盖、`EM`(2420) 被 `IOFindBSDRoot` 覆盖。**"没显示" ≠ "没执行"**，先核对行号表。
2. **读屏错误**：`PN2`+`9` 连排读成 `PN29`，`PC3`+`8` 读成 `PC38`。**已给所有标签加 `=` 分隔符**（`RN=`/`RA=`），以后读图先按标签表切分。
3. **`__builtin_return_address(0)` 在 `-O2` 下不可靠**：`RA=124812296` 固定垃圾（LR 被编译器复用）。**这条定位调用者的路作废**。
4. **`ios-obj` 缓存损坏 → 构建静默不产出新内核**。症状：内核 mtime 不变、`strings` 查不到新标签。**唯一恢复手段：`rm -rf out/nabu/tree/build/xnu/ios-obj` 全量重建**。
5. **xnu C++ 产物是 `.cpo/.cpd` 不是 `.o`**；删 `.cpo` 会破坏 link.filelist，删 `.o` 会丢失对象。**正确做法是把源文件加进 build_verify.sh 的"缓存删除清单"**（约 L54 的 find 块）。
6. **改 `scripts/*.py` 必须 push**：`build_verify.sh` 每轮 `git fetch + reset --hard FETCH_HEAD`，本地未提交的改动会被冲掉。**但 `git push` 会假成功**（见下条）。
7. **git push `exit=0` 但远端没更新**（详见 `PUSH-GOTCHA.md`）。**唯一可靠的验证**：
   ```bash
   git fetch -q origin main
   git show origin/main:<file> | grep -c '<关键改动>'
   ```
   或者干脆用 **GitHub API PUT contents**（秒回，从未失败）。
8. **验证必须选"应当存在"的判据**。我曾拿 `NABU RESTART: 0` 当"撤除成功"——但那个字符串**从没进过内核**，0 是恒真的。正确判据应是 md5 变化 + 应存在的字符串 count>0。
9. **`PEHaltRestart` 不能在 early-boot 阶段裸调**：`os_reason_create` 里调用 → `Kernel data abort`，FAR=0，崩在 `Initializing serial KDP` 之后。`PE_halt_restart` 函数指针要等平台专家初始化后才就绪。（不过当时真正崩溃源是 DWC3，见下条——这个结论待重新验证）
10. **DWC3 驱动未配时钟就映射并写 `0x0A600000` → 早期 data abort**。屏幕特征：`Kernel data abort.`，`ESR=2516582405`，`FAR=0`，PC 在 `0xfffffff01167xxxx`，停在 `Initializing serial KDP` 之后、Skywalk 自测之前。**这就是今天那两次"莫名其妙"崩溃的真凶。**

### 3.3 DWC3 驱动的现状（`Lux12138/NativeDwc3`）

已完成的：寄存器头（严格照 Android `dwc3/core.h` 转录）、stage-1 `start()`（map → GSNPSID → 软复位 → device mode → 清 SUSPHY → RUN_STOP → 读 DSTS）、Info.plist、ADT 的 `usb0` 节点、构建链全打通（白名单 + 目录映射 + force_load + config name）。

**卡住的地方（下一步从这里开始）**：
1. `CONFIG_XNU_NATIVE_DWC3=y` 传不进 `build-xnu-4k-baseline.sh`（实测 `XNU_NATIVE_DWC3=UNSET`）。nabu 的构建入口不走 `build-cpu-desktop-kernel.py` 的 names 表。**需要找到 nabu 真正的 env 注入点**（可能在 `unixnu` 的 kernel.py 或 board.toml 的 env 段）。
2. **缺 GCC 时钟门控**：即使链接成功，写寄存器也崩。需要照抄 `NativeUfs/GccUfs.h` 写 `GccUsb.h`，先开 `GCC_USB30_PRIM_*` 六个时钟 + GDSC，再碰 DWC3 寄存器。SM8150 的 GCC 基址和 UFS 相同（同一 GCC 块），bit 位从 `gcc-sm8150.h` 查。
3. `IOProviderClass=IOPlatformDevice` 需要匹配 ADT 节点 —— `usb0` 节点已加（`make-nabu-adt.py:190`），但**没验证过 IOKit 是否真的匹配并调用了 start()**（当时崩溃掩盖了这一点）。

### 3.4 自动读日志方案的定论

| 方案 | 结论 |
|---|---|
| fastboot fetch 分区 | ❌ 实测 `Device does not support fetch command` |
| xnu 自带 UART | ❌ 全是 Apple 专有宏（APPLE_UART/DOCKCHANNEL/VMAPPLE），且需 ADT 节点；sheng 也没做 |
| 物理 UART 引出 | ❌ 硬件没有 |
| **panic 后自动重启 + adb 读** | ✅ **唯一可行**，分两半：重启 + 读 |
| adb 读日志的数据源 | ✅ **现成 API**：`native_ufs_log_open/write/sync`（NativeUfs.cpp:1119-1180，预留日志 LBA 区，4K 块写，无调用者——就是为这个场景预埋的） |

**推荐实施顺序**（每步独立可验证）：
1. `nabu_mark_putc` 加内存 ring buffer（4MB），存所有 kprintf 输出 —— 纯内存，无风险
2. 找到 panic 路径**正确时机**的重启点（`kdp_machine_reboot()` 已存在，或在 `panic_trap_to_debugger` 返回前；**不在 `os_reason_create`**）—— 先只加一个"画标记后 5 秒再重启"的延时，验证时机安全
3. panic 前把 ring buffer 写进 `native_ufs_log_write`（预留区在盘尾，不碰根卷）
4. 重启后（进 Android），`adb shell dd if=/dev/block/by-name/<日志分区> of=/sdcard/log.bin` + `adb pull`，host 侧脚本解析
5. 全流程打包成一个脚本：`build → flash → boot → 等 panic → 等重启 → adb pull log → 解析关键值`

## 4. 复现当前基线（3 条命令）

```bash
# WSL 内
cd ~/src/UniXNU && git checkout aae83c4 2>/dev/null
cd ~/src/unixnu-nabu && git checkout 24b73bc 2>/dev/null
bash /mnt/c/Temp/auto/build_pack_short.sh   # 产出 /mnt/c/Temp/cust-final2.img
```
```powershell
# Windows 侧
cd C:\platform-tools
.\fastboot.exe flash cust C:\Temp\cust-final2.img
.\fastboot.exe boot C:\Users\Administrator\Desktop\unixnu-nabu-kernel-ufs\nabu-xnu64-Z.img
```
预期：60 秒后停在 `unexpected SIGKILL of ... launchd`，屏幕有 `RN8`/`RA124812296`、`ufs:entered`、`IOFindBSDRoot entered`。

**构建成功的判据**（别再用字符串 count 骗自己）：
- `/tmp/bv2.log` 出现 `built kernel at`，**没有** `KERNEL NOT REGENERATED`
- `out/nabu/kernel/kernel.macho` 的 **md5 变化**

## 5. 文件地图（都在哪）

| 文件 | 作用 |
|---|---|
| `C:\Temp\auto\build_verify.sh` | 真正的内核构建（含缓存删除清单 L54、每轮 git reset） |
| `C:\Temp\auto\build_pack_short.sh` | 构建+打包+验证一条龙 |
| `C:\Temp\auto\pack_from_board.sh` | ADT 生成（从 board.toml 读参数）+ make-cust-payload |
| `C:\Temp\auto\make-trustcache.py` | cdhash 提取 + trustcache 生成（big-endian CS blob 已修） |
| `~/src/UniXNU/scripts/nabu-reason-probes.py` | **每轮必跑的 inserter**（改 sys_reason.c，行 2100/2180）—— 加新探针优先挂这里 |
| `~/src/UniXNU/scripts/build-xnu-4k-baseline.sh` | 驱动链接（force_load 块 ~L1735）、探针 hook 点 |
| `PUSH-GOTCHA.md` | git push 假成功的坑与正确三步法 |
| `HANDOVER-exec-fault.md` | 主线 689 行完整记录（十六节） |

## 6. 下一步建议（按价值排序）

1. **内存 ring buffer + UFS 日志落盘**（§3.4 的 1/3 步）—— 不碰重启逻辑，纯增益，任何 panic 都能留下完整 log
2. **GccUsb.h**（DWC3 的时钟门控）—— 打通 USB 的前提
3. **重启时机**：在 `panic_trap_to_debugger` 尾部（此时平台已就绪）加延时重启，boot-arg 门控
4. 主线 SIGKILL：若 §3.4 的 log 能落盘，一次 panic 就能看到完整的 exec 路径，`SECURITY_POLICY` 的真正来源大概率一目了然
