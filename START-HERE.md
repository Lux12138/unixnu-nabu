# START HERE — nabu (小米平板5 / SM8150) XNU 移植

> 任何新会话/新窗口接手，**先读这个文件**，再读下面指定的文档，然后才动手。
> 不按顺序读就动手，会重复今天踩过的十个坑。

## 一、先读这三份（按顺序）

| 顺序 | 文件 | 内容 |
|---|---|---|
| 1 | **本文件** | 铁律 + 当前基线 + 两个仓库的 HEAD |
| 2 | `HANDOVER-2026-10-24-ROLLBACK.md` | 最新交接：今天所有实测结论 + 十个坑 + 下一步排序 |
| 3 | `HANDOVER-exec-fault.md` | 主线 689 行完整记录（execve/SIGKILL 全链条） |

`PUSH-GOTCHA.md` 是 git push 假成功那个坑，推送前扫一眼。

## 二、五条铁律（违反任何一条都会白烧一轮）

1. **结论必须量出来，不许靠代码推断。**
   今天所有重大错误都源于此：`PN2`+`9` 读成 `PN29`、`NABU RESTART: 0` 当成"撤除成功"。
2. **验证判据要选"应当存在"的东西。**
   `count = 0` 是恒真的，不能当成功证据。用 **md5 变化** + **应存在的字符串 count > 0**。
3. **改 `scripts/*.py` 必须提交，并用 `git show origin/main:<file>` 验证远端真的更新。**
   `git push` 会返回 `exit=0` 但**不生效**。构建每轮 `git reset --hard FETCH_HEAD`，本地未提交的改动会被冲掉。
4. **回滚要同时回滚 `UniXNU` 和 `unixnu-nabu` 两个仓库。**
   只回滚一个 → board.toml 仍列着已删除的驱动 → `KERNEL NOT REGENERATED` → 刷的是旧内核残留。
5. **构建没产出新内核时，先 `rm -rf out/nabu/tree/build/xnu/ios-obj` 全量重建。**
   症状：内核 mtime 不变、`strings` 查不到新标签。这是唯一恢复手段。

## 三、当前基线（2026-10-24 晚，已上机验证）

- **屏幕停在**：`unexpected SIGKILL of %s %s with reason -- launchd`，`ESR=2516582469`
- **内核**：md5 `e9ebb7e6377d4e1dfd6fe2f5fa733159`，17:41 构建，已刷机验证
- **仓库 HEAD**：
  - `Lux12138/UniXNU` → **`aae83c4`**（纯探针，无 DWC3、无重启调用）
  - `Lux12138/unixnu-nabu` → **`105e1bb`**（含 trust_cache、全部文档）
- **已修复**：execve 缺页（PAC `VM_KERNEL_STRIP_PTR`）、AMFI boot-args、trust cache 接入
- **未解**：launchd 被 `OS_REASON_EXEC + SECURITY_POLICY` 杀掉，七个生产者全部排除

## 四、复现基线（3 条命令）

```bash
# WSL 内
cd ~/src/UniXNU && git checkout aae83c4 2>/dev/null
cd ~/src/unixnu-nabu && git checkout 105e1bb 2>/dev/null
bash /mnt/c/Temp/auto/build_pack_short.sh
```
```powershell
# Windows 侧
cd C:\platform-tools
.\fastboot.exe flash cust C:\Temp\cust-final2.img
.\fastboot.exe boot C:\Users\Administrator\Desktop\unixnu-nabu-kernel-ufs\nabu-xnu64-Z.img
```

**构建成功的判据**（别再被骗）：
- `/tmp/bv2.log` 出现 `built kernel at`，**不出现** `KERNEL NOT REGENERATED`
- `out/nabu/kernel/kernel.macho` 的 **md5 与上一次不同**

## 五、下一步（按价值排序，详见交接文档 §6）

1. **内存 ring buffer + UFS 日志落盘** —— 纯增益、不碰重启逻辑，任何 panic 都能留下完整 log
   （`native_ufs_log_open/write/sync` API 已现成，在 NativeUfs.cpp:1119-1180，无调用者）
2. **GccUsb.h** —— DWC3 的时钟门控，USB 的前提
3. **重启时机** —— 在 `panic_trap_to_debugger` 尾部（平台已就绪）加延时重启，boot-arg 门控
4. 主线 SIGKILL —— 有了完整 log 后，一次 panic 就能定位 `SECURITY_POLICY` 的真正来源

## 六、最容易踩的坑速查

| 症状 | 真凶 |
|---|---|
| 内核 mtime 不变、新探针查不到 | `ios-obj` 缓存损坏 → 清它全量重建 |
| 屏幕上探针"没显示" | 先查行号表（行冲突/被覆盖），不是"没执行" |
| `Kernel data abort` + `FAR=0` + 停在 `Initializing serial KDP` 后 | **DWC3 驱动无时钟写寄存器**（ESR `...2405`） |
| `git push` exit=0 但改动没生效 | 用 `git show origin/main:<file>` 验证；或改用 GitHub API PUT |
| 改了 board.toml 但没效果 | defconfig 的 `CONFIG_XNU_*` 未必传进构建脚本，先打印变量值 |
