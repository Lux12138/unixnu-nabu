# 交接：execve 缺页为何变成 panic（2026-10-24 下午）

> 承接 `HANDOVER-2026-10-24.md`。上午那份记录了「根卷找到 + launchd 加载」，
> 这份记录**那个 panic 的完整机理**。

---

## 一、已经确定的因果链

```
execve("/sbin/launchd")
  └─ vm_map_switch(get_task_map(task))        L1821  ✓ 切到用户 map
     └─ exec_copyout_strings()                L1839  ✓ cpo0  返回 0
        └─ dynlinker copyoutptr()             ✓ cp20  返回 0
           └─ copyoutptr(dynlinker_fd, ap, 8) L1925  ✗ FAULT
              └─ _bcopyout 内缺页
                 └─ handle_kernel_abort
                    └─ find_copyio_recovery_entry(state)
                       ├─ 匹配 → handle_kernel_abort_recover → 恢复，继续
                       └─ 不匹配（recover == 0）→ panic  ← 我们在这里
```

**`vm_page_free_count` 的 9100 万是这次 panic 的**后果**，不是原因。四个时点实测证明它一直正常：**

| 时点 | 值 | 判定 |
|---|---|---|
| `initialize_ram_ranges` | 0 | 页数组还不存在 |
| `pmap_startup` 返回 | 1,207,626 | ✓ 与 `max_mem/4096 = 1,245,184` 吻合 |
| `vm_page_bootstrap` 结束 | 1,207,621（wire 37,563） | ✓ |
| `IOFindBSDRoot` 入口 | 1,207,627 | ✓ UFS 驱动全程无问题 |
| execve 时 | 91,199,441 | ✗ 在这个 panic 里被写坏 |

**所以：`dram_size` / `owned_memory_size` 完全不用动。**

---

## 二、已经排除的假设（每条都有实测）

| 假设 | 排除方式 | 结论 |
|---|---|---|
| ADT 内存参数错 | `maxmem4864`、`pages1207622` | 4.75 GiB 正确，页大小 4 KB |
| `size - rlimit` 下溢 | `prt16384` = PAGE_SIZE | 未下溢 |
| 栈太小 | `usz 4.24 MB`、`rlimit 1 MB` | 分配充足 |
| `exec_prefault_data` 太晚 | 提前调用后 fault 位置不变 | 它只 prefault 数据段，不含栈 |
| kernel-to-kernel copy 路径 | **`Q40`** = 线程不在 kernel_map | 走的是有保护的 `_bcopyout` |
| `__copyio_vectors` 表丢失 | 段存在，`0x120` 字节 = 12 条 | 表完整，`cre_recovery` 指向 `copyio_error` |
| `expected_fault_handler` | 只在 `#ifdef CONFIG_XNUPOST` 里被设置 | 正常内核里恒为 NULL，不是这条路径 |

---

## 三、关键源码位置（都在 `.cache/xnu-work/`）

| 文件 | 行 | 内容 |
|---|---|---|
| `bsd/kern/kern_exec.c` | 1821 | `vm_map_switch(get_task_map(task))` |
| | 1839 | `exec_copyout_strings()` |
| | 1925 | `copyoutptr(dynlinker_fd, ap, 8)` ← fault 点 |
| `osfmk/arm64/machine_routines_asm.s` | 672 | `_bcopyout`，含 `COPYIO_RECOVER_RANGE 3f` |
| | 68 | `COPYIO_RECOVER_RANGE` 宏，写 `__copyio_vectors` |
| `osfmk/arm64/sleh.c` | 1765 | `recover = find_copyio_recovery_entry(state)` |
| | 1878/1885 | `if (recover) handle_kernel_abort_recover(...)` |
| | 1890 | `panic_fault_address = fault_addr` |
| `osfmk/arm64/sleh.c` | 299 | `find_copyio_recovery_entry`：按 PC offset 查表 |
| `osfmk/arm64/copyio.c` | 132 | `copy_validate`，`EXDEV` 时走无保护的 `copyout_kern` |

**内核里表的位置（12:54 构建）：**
```
__copyio_vectors      size 0x120  addr 0xfffffff007157b2e
_copyio_recover_table         0xfffffff007157b2e
_copyio_recover_table_end     0xfffffff007157c4e
copyio_error                  0xfffffff00740e9e4   (offset 0x2b6eb6 from base)
_bcopyout                     0xfffffff00740eb44
```

**12 条表项，第 9 条范围 `0x2b7022..0x2b7056`。`_bcopyout + 0x1c` 的 offset 是 `0x2b7032`，理论落在其中。**

---

## 四、下一个探针已经布好

在 `handle_kernel_abort` 入口画三个值（`sleh.c` 的 `nabu-sleh-probes.py`）：

| 行 | 标签 | 含义 |
|---|---|---|
| 1900 | `RV` | `recover` 指针，**0 表示查表失败** |
| 1980 | `PCOF` | `copyio_recovery_offset(PC)` |
| 2060 | `TB` | `copyio_recover_table` 基址 |

**判读：**
- `RV` 非 0 → 查表成功，panic 另有原因（往 `vm_fault` 返回值看）
- `RV` = 0 且 `PCOF` 落在某条 `[cre_start, cre_end)` 内 → 查表逻辑本身有问题
- `RV` = 0 且 `PCOF` 落在所有范围之外 → PC 不在 `COPYIO_RECOVER_RANGE` 覆盖区间，需要扩大范围

同屏还有 P0–P3 / Q4（2100–2420）与 ESR/FAR/PC/LR（1560–1820）。

---

## 五、这轮踩的工具坑（都记下来，别再踩）

| 坑 | 表现 | 解法 |
|---|---|---|
| 改 `scripts/*.py` 不 commit | `unixnu tree` 重置，改了十几轮全无效 | 每次改完立刻 commit + push |
| `strings` 默认 4 字符 | `"P0"`、`"RV"` 这些 2 字符标签查不到 | 一律 `strings -n 2` |
| 探针画在 1780 以上 | console 白字盖住 | console cap = `g_h-780`，全放 2100+ |
| `nabu_mark_text` 每行清 61px | 行距 <61px 互相擦除 | 行距 80px |
| 删 `.cpo` 重建 | `link.filelist` 缺项 → `_page_size` 消失 → archive 10 失败 | 用 `touch` 源文件，别删产物 |
| `panic` 传整数 | hook 把 vararg 当 `char*`，数字不显示 | 先 `snprintf` 再 `panic("%s", buf)` |
| 一个 panic 一轮刷机 | 每点一次全流程 | 改用 `nabu_mark_num` 画固定行，一次启动拿全部 |
| inserter 锚点带空格 | `nabu_mark_text (` vs `nabu_mark_text(` | 按实际源码写锚点 |
| C++/C 语言链接 | `extern void` 与 header 冲突 | `DECL` 按文件名条件化 |

---

## 六、状态速查

**能做什么**：CI + 本地 WSL 构建（72 秒），打包 cust，刷机，真机跑到 `execve` 的 `copyoutptr`。

**不能做什么**：跨过那次缺页。`launchd` 已解析、已加载，但用户栈上有一页写不进去。

**`STATUS.md` 908 行**记录了完整历史；本文件是当前断面。
