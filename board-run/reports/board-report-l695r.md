# 上板报告 l695r（addon 写入诊断轮）

## 1. 唯一问题

l695p 里 `PUB`/`STn`/`P` 全为 0，三种可能分不开。这一轮只回答第一问：
**addon 到底有没有把 `addons/NativeAdrenoMetal.publish` 写进 Data？**
判据是新加的一行 `nabu-addon: files=… wr=… same=… fail=… err=… publish=…`。

## 2. 已闭合到哪

- 签名闸门仍成立（l695s/l695p：0 次 fatal，SHA-1 对照每轮 4 次）。
- l695p 的 Data 挂载正常（`nabu: owner` ×3、`disk5s1 mounting volume Data`、`data skeleton created=36`）。
- addon 写入是通用路径（`vnode_open(O_CREAT|O_TRUNC|FWRITE|O_NOFOLLOW)` + `vn_rdwr` + fsync，父目录自建，无文件名白名单），
  理论上 publish 会被写；但没有可读计数，只能靠这一行证实。
- 内核证据：`1f82e4a3…` 的 `strings` 里 `nabu-addon:` 与 `NativeAdrenoMetal.publish` 各 1 处；payload `7ba98abc…`、
  `fsck.fat -n` 干净、`gpu-addon`/`trust`/`boot`/`adt` 与 l695p 逐字节相同（唯一变量确实是内核）。

## 3. 为什么离线不行

addon 安装发生在真机内核启动的 Data 挂载之后、launchd 之前，输入是 Mu 通过 `/chosen/memory-map/GpuAddon` 传的镜像；
离线只能确认容器格式与字符串，确认不了"这一次 install 跑没跑、写没写"。

## 4. 失败也有信息

- `files=-1`：`native_gpu_addon_install()` 根本没执行（Mu 没把 addon 传进来）。
- `files=9` 且 `wr=0/fail=0/same=9`：内容被判"相同"（说明 Data 上已有同一份，publish 可能早就在）。
- `files=9` 且 `fail>0`：写入失败，`err` 就是 errno（ENOSPC/EROFS/EPERM…）。
- `publish=0`：文件确实不在 `/private/var/ngpu/addons/` 下 —— 问题在写入侧。
- `publish=1` 而 `PUB` 仍为 0：写入没问题，断点移到"沙箱读不到同级文件"或"插件没被加载"，
  下一步是 Info.plist key 那个改动（fc76 已评估：只动 publish 一条判定，其余 marker 不变）。

## 5. 唯一变量与回滚点

唯一变量 = 内核加一行只读诊断（`0017-nabu-addon-diag.patch`，`printf`），其余五个文件哈希与 l695p 完全相同。
回滚点 = `hold/l695p/`（12:54 已跑通）、`hold/l690/`。

## 6. 恢复预案

TWRP 里 arm misc → `reboot bootloader` → **只 flash cust** → 最后再 arm misc → `fastboot boot`。
**boot_a 不碰**（AGENTS.md 0.10 红线，`tmp/guard_no_boot_flash.py` 强制）；这轮 misc 最后写，复位后自动落回 TWRP。
