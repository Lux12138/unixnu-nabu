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

---

## 七、已排除：内核 slide 不会让 offset 算错

`find_copyio_recovery_entry` 的算法是：

```c
static inline ptrdiff_t
copyio_recovery_offset(uintptr_t addr)
{
    return (ptrdiff_t)(addr - (uintptr_t)copyio_recover_table);
}
```

**它用「运行时 PC」减去「`copyio_recover_table` 符号值」。**

### 为什么它不成立

两端处在同一 slide 空间，差值自动抵消：

| 端 | 来源 | 是否含 slide |
|---|---|---|
| `get_saved_state_pc(state)` | 运行时 PC | 是（已 slide） |
| `(uintptr_t)copyio_recover_table` | C 取符号地址，编译为 `adrp/add` | 是（运行时自动 slide） |
| `cre_start` / `cre_end` | 汇编 `.quad Lcre_start - _copyio_recover_table` | 否（链接期常量） |

前两者相减，slide 抵消；结果与第三类同量纲。**本机虽然 `slide = "-0x1000000"`，但这不影响该算法。**

### 因此 `recover == 0` 只剩三种可能

| 可能 | 判据（`PCOF` 探针已布在 1980 行） |
|---|---|
| PC 真的落在所有范围之外 | `PCOF` 是可读的小值，但不在任何 `[cre_start, cre_end)` 内 |
| `find_copyio_recovery_entry` 的循环没执行 | `TB`（2060 行）为 0 或等于 `_end` |
| fault 不是从这个 PC 报的 | `PCOF` 与 `_bcopyout+0x1c` 的预期 offset（`0x2b7032`）差很远 |

**预期值可以手算：`_bcopyout = 0xfffffff00740eb44`，加 `0x1c` 得
`0xfffffff00740eb60`；减去表基址 `0xfffffff007157b2e` 得 `0x2b7032`。第 9 条表项范围
`0x2b7022..0x2b7056` 正好包含它。** 所以若 `PCOF == 0x2b7032` 而 `RV == 0`，问题就在查表循环本身。

---

## 八、🎯 已定位并修复：PAC 位混进了 copyio 表的 offset 计算

### 决定性实测（`RV` / `PCOF` / `TB` 探针）

```
RV   = 0                        recover = NULL，查表失败
PCOF = 0x230397                 算出的 offset
TB   = 0xfffffff006f6f663       表基址（正常内核地址）
PC   = 0x1999999800c2bd60       ← 不是内核地址格式！
LR   = 0x1999999800c18270
```

**`PC` 是 PAC 签名值。** ARM64 上 `get_saved_state_pc()` 返回的 PC 带指针认证位。

### 根因

```c
/* sleh.c:299 */
static inline struct copyio_recovery_entry *
find_copyio_recovery_entry(arm_saved_state_t *state)
{
    ptrdiff_t offset = copyio_recovery_offset(get_saved_state_pc(state));   // ← 少了 strip
    for (e = copyio_recover_table; e < copyio_recover_table_end; e++) {
        if (offset >= e->cre_start && offset < e->cre_end) return e;
    }
    return NULL;
}
```

**`sleh.c` 里其他每一处消费 saved PC 都做了包装：**

| 行 | 写法 |
|---|---|
| 700 | `VM_KERNEL_ADDRHIDE(far), VM_KERNEL_UNSLIDE(get_saved_state_pc(state))` |
| 959 | 同上 |
| 2350 | `VM_KERNEL_UNSLIDE(get_saved_state_pc(state))` |

**只有 `find_copyio_recovery_entry` 直接用了裸 PC。** 用它减去链接期常量表基址，得到的 offset
落不进任何 `[cre_start, cre_end)`，`recover` 恒为 NULL，于是每次 copyio 缺页都走 panic 而不是 recovery。

### 修法

```c
- ptrdiff_t offset = copyio_recovery_offset(get_saved_state_pc(state));
+ ptrdiff_t offset = copyio_recovery_offset(VM_KERNEL_STRIP_PTR(get_saved_state_pc(state)));
```

**为什么是 `STRIP_PTR` 而不是 `UNSLIDE`：**

```
copyio_recovery_offset(addr) = addr - (uintptr_t)copyio_recover_table
```

| 端 | 运行时是否 slide |
|---|---|
| `addr`（保存的 PC） | 是 |
| `copyio_recover_table`（C 取符号，`adrp/add`） | 是 |

**两端都 slide → 相减抵消。所以只需要去掉 PAC 位，不能动 slide。**
`UNSLIDE` 会额外减去 slide，反而把对齐破坏掉（第一版就是这么写的，已改正）。

### 这一条解释力

**它同时解释了此前所有异常观测：**

| 观测 | 解释 |
|---|---|
| 缺页地址合法、在分配区间内 | 地址本身没问题 |
| 早先几次 copyout 成功 | 那些页已映射，不触发缺页 |
| 一次 `copyout` 一字节就 panic | 缺页必然经过 `find_copyio_recovery_entry`，必然返回 NULL |
| `exec_prefault_data` 提前无效 | 它只 prefault 数据段，栈仍缺页 |
| `vm_page_free_count` 变成 9100 万 | panic 路径的副作用 |
| 表完整（0x120 字节、12 条） | 表没问题，是查询参数错了 |

### 验证方式

`RV`（1900 行）变成非 0 即修复生效 —— 缺页会被 `handle_kernel_abort_recover` 接住并跳到
`copyio_error`，`copyoutptr` 返回 `EFAULT` 而不是让内核崩掉。

---

## 九、STRIP_PTR 修复已生效 —— 故障点后移

### 修复前后的 `PCOF` 对比

| 启动 | `PCOF` | `RV` | 判定 |
|---|---|---|---|
| 修复前 | `0x230397` | **0** | 对 PAC 签名值算 offset，落不进任何表项 |
| **修复后** | **`0x2b7027`** | **`0xfffffff006f6f779`** | **offset 正确，查表成功** |

**验算：**

```
RV - TB      = 192 = 8 × 24          → 第 9 条表项（0-based #8）
表项 9 范围   = 0x2b7022 .. 0x2b7056
实测 PCOF    = 0x2b7027              → 落在范围内 ✓
```

**所以 `find_copyio_recovery_entry` 现在能返回有效指针了。这是本轮唯一的实质性修复，成功了。**

### 但 panic 依旧 —— 故障点移到了 `handle_kernel_abort_recover`

`handle_kernel_abort_recover` 的 asm 逻辑（`sleh.c:1716`）：

```asm
ldr  x1, [recover, CRE_RECOVERY]   // x1 = cre_recovery，相对偏移
add  x1, x1, x6                    // x1 += copyio_recover_table → 绝对地址
str  x1, [x0, SS64_PC]             // 写回 saved PC，跳过去
```

**计算本身是对的（`TB + 0x2b6eb6` 即 `copyio_error`）。** 新增探针查实际值：

| 行 | 标签 | 含义 |
|---|---|---|
| 2140 | `JT` | `recover->cre_recovery` —— 即将跳转到的地址 |
| 2220 | `RJ` | `recover` 指针，用于与 `RV` 对照 |

### 下一步的三种判读

| `JT` 值 | 结论 | 动作 |
|---|---|---|
| `TB + 0x2b6eb6`（正常） | 跳转目标正确，panic 发生在 `copyio_error` 返回之后 | 查 `copyoutptr` 的返回值路径与 exec 的 `badtoolate` |
| 异常小值或 0 | 表项内容被读错 | 查 `CRE_RECOVERY` 的 `offsetof` 与结构布局 |
| 与 `TB` 无关的值 | `recover` 指针虽然非 0 但不在表内 | 重新检查 `find_copyio_recovery_entry` 的边界比较 |

### 本轮总结

**从「每次 copyio 缺页必崩」到「查表能成功」，机制上跨了一步。** 剩下的是恢复之后的路径问题，
不再是「表查不到」这一类。

---

## 十、🎯 execve 的缺页已经跨过去了 —— 现在是 launchd 被 SIGKILL

### 证据链

```
R2 (panic 点的 recover)  没画  →  handle_kernel_abort 没走到那里
JT (recovery 跳转目标)   没画  →  但 RV 非 0，recover 确实找到了
                              →  handle_kernel_abort_recover 执行，跳回 copyio_error
                              →  copyoutptr 返回 EFAULT，execve 继续
                              →  launchd 起来了
                              →  然后被 SIGKILL
```

**盘踞一整天的 `_bcopyout` 缺页 panic 已经消失。** PAC 修复（`VM_KERNEL_STRIP_PTR`）是有效的那一刀。

### 新的 panic 及其出处

久违的那句屏幕文本，我一开始读成了 `unexpected STATUS!! nf %x %x with mask`，
实际是内核二进制里的：

```
unexpected SIGKILL of %s %s with reason -- namespace %d code 0x%llx description %.800s
```

出处 `bsd/kern/kern_sig.c:2077`，在 `psignal()` 里：

```c
/* catch unexpected initproc kills early for easier debuggging */
if (signum == SIGKILL && p == initproc) {
    panic_plain("unexpected SIGKILL of %s %s with reason -- ...",
        (p->p_name[0] ? p->p_name : "initproc"),
        ((proc_getcsflags(p) & CS_KILLED) ? "(CS_KILLED)" : ""),   // ← 这一列决定性质
        signal_reason->osr_namespace, signal_reason->osr_code, ...);
}
```

**PID 1 不允许被 kill，所以内核 panic 报出来。**

### 🎯 根因：两套 boot-args，生效的那套没有 AMFI

| 来源 | 内容 | AMFI |
|---|---|---|
| `make-nabu-adt.py:135`（**实际生效**） | `-enable_kprintf_spam native-ufs-root=1 native-ufs-diagnostics=1 maxmem=4864` | ❌ |
| `unixnu.toml` 的 `args`（**不生效**） | `... amfi_get_out_of_my_way=1 ...` | ✅ |

**DarwinBoot 读 ADT**（启动日志 `DB3 adt read Success`），`unixnu.toml` 的 `args` 根本到不了内核。

**`launchd` 未签名 → `CS_KILLED` → SIGKILL → panic。** 与观察完全一致。

### 修复

`make-nabu-adt.py` 的 boot-args 加上三个官方标志：

```
amfi_get_out_of_my_way=1
cs_enforcement_disable=1
amfi_allow_any_signature=1
```

已验证进入 ADT：

```
amfi_get_out_of_my_way       1
cs_enforcement_disable       1
amfi_allow_any_signature     1
maxmem                       1
native-ufs-root              1
```

### 下一步判读

| 现象 | 含义 |
|---|---|
| 不再出现 `unexpected SIGKILL` | AMFI 生效，launchd 活着 → **进入 Stage 6 后半段**（launchd 之后是 userspace 服务） |
| 仍出现且带 `(CS_KILLED)` | AMFI 标志没被识别，需要换注入路径（ADT 的 `chosen` 节点属性） |
| 仍出现但不带 `(CS_KILLED)` | 不是签名问题，是别的 exit reason —— 看 `namespace` / `code` 两个数字 |

---

## 十一、AMFI 确认生效，且**不是**代码签名问题

### 实测

```
AMx = 10
```

解包：`AMx = AMFI值 × 10 + CS_KILLED`

| 项 | 值 | 含义 |
|---|---|---|
| `AMFI 值` | **1** | `amfi_get_out_of_my_way` **确实被内核解析到了** |
| `CS_KILLED` | **0** | **`launchd` 不是因代码签名被杀的** |

**所以两条推断都被否定：**

- ❌ 「ADT 的 boot-args 没到内核」 —— 到了，`AMFI = 1`
- ❌ 「`launchd` 未签名被 `CS_KILLED`」 —— 不是，`CS_KILLED = 0`

### 这意味着什么

`psignal()` 里那个 `if (signum == SIGKILL && p == initproc)` 分支**确实被走到了**（所以有那句
`unexpected SIGKILL` 的 panic），但杀它的原因不是签名，而是别的 exit reason。

### 唯一还缺的信息

`signal_reason->osr_namespace` 和 `osr_code` —— 它们本来画在 2580 行，**超出 2560 的屏幕高度**，
所以一直看不到。

已改到 2500 行（`AMx` 的结论已拿到，让位），编码为：

```
SIG = namespace × 100000 + (osr_code % 100000)
SIG = 7777777  →  signal_reason == NULL
```

### 判读表

| `SIG` | `osr_namespace` | 含义 |
|---|---|---|
| `400000 + code` | 4 | **Jetsam** —— 内存压力，与 boot-arg 无关 |
| `300000 + code` | 3 | **EXEC** —— 执行阶段失败（`EXEC_EXIT_REASON_*`） |
| `600000 + code` | 6 | **CODESIGNING** —— 与 `CS_KILLED=0` 矛盾，需重新查 |
| `7777777` | — | `signal_reason` 为 NULL，走的是「no reason provided」那条 panic |
| 其他 | — | 按 `osr_namespace` 值查 `osfmk/kern/exit_reason.h` |

**若为 EXEC（3）**：`osr_code` 会指出具体是哪一步 —— 例如 `EXEC_EXIT_REASON_COPYOUT_STRINGS`
对应我们早先看到的 `cpo0`，`EXEC_EXIT_REASON_COPYOUT_DYNLINKER` 对应 `cp2`。这也正好能验证
PAC 修复之后那次 recovery 究竟把 `copyoutptr` 变成了什么返回码。

---

## 十二、🎯 `SIG900008` 完全解码：`OS_REASON_EXEC` + `SECURITY_POLICY`

### 解码

```
SIG = 900008
    = namespace × 100000 + (code % 100000)
      namespace = 9   = OS_REASON_EXEC           （bsd/sys/reason.h:96）
      code      = 8   = EXEC_EXIT_REASON_SECURITY_POLICY  （同文件:255）
```

**`launchd` 不是被 jetsam、不是被签名、也不是被别的子系统杀的 —— 是 exec 的安全策略拒绝。**

### 完整因果链（全部实测）

```
kern_exec.c:8018   if (imgp->ip_mac_return != 0)
                     → os_reason_create(OS_REASON_EXEC, EXEC_EXIT_REASON_SECURITY_POLICY)
kern_exec.c:6773     imgp->ip_mac_return = label_update_return
kern_credential.c    kauth_proc_label_update_execve(... &label_update_return)
mac_vfs.c:876        *labelupdateerror = error
mac_vfs.c:~840       error = mac_error_select(hook(...), error)   遍历所有 policy
psignal()            SIGKILL on initproc → panic
```

**而 `machine_task_process_signature`（另一条会产生同一 reason 的路径）可以排除：**
它的错误字符串 `custom x18 ABI usage` 在内核二进制里出现 **0 次**，说明
`#if !__ARM_KERNEL_PROTECT__` 分支生效，函数返回 `KERN_SUCCESS`。

### 树里注册的两个 policy 都**没有**那个钩子

| policy | 文件 | `mpo_cred_label_update_execve` |
|---|---|---|
| `CHECK`（Check Assumptions） | `bsd/kern/policy_check.c:527` | ❌ |
| `Guarded vnode policy` | `bsd/kern/kern_guarded.c:1582` | ❌ |

`libkern/amfi/amfi.c` 只有 293 字节，内容仅 `amfi_interface_register()` —— 是**接口注册**，
真正的实现在外部 kext。

**所以 MAC 循环理论上应让 `error` 保持 0。这与「SECURITY_POLICY 被创建」矛盾。**

### 正在量的东西

新增 `LU` 探针（2500 行）画 `*labelupdateerror`：

| `LU` | 含义 | 下一步 |
|---|---|---|
| **0** | MAC 未拒绝 —— `SECURITY_POLICY` 来自别处 | 回到 `kern_exec.c:8018` 之前的其它 `goto done` 路径 |
| **非 0** | 某个 policy hook 返回了该值 | 用 errno 反查是哪个 hook（EPERM/EACCES/EAUTH…） |

### 为什么这一轮值得

**从「`launchd` 被 SIGKILL」到「exec 的安全策略拒绝，且不是签名、不是 jetsam」—— 范围已经收窄到
一个函数返回值。** 前四个 namespace 候选全部排除，剩下这一条。

---

## 十三、推断与代码矛盾 → 改为在 SIGKILL 交付点实测

### 矛盾

**实测事实：**

```
SIG = 900008  →  namespace 9 (OS_REASON_EXEC) + code 8 (EXEC_EXIT_REASON_SECURITY_POLICY)
```

**全树只有两处产生这个 reason：**

| 位置 | 触发条件 | 是否排除 |
|---|---|---|
| `kern_exec.c:7950` | `machine_task_process_signature` 返回非 `KERN_SUCCESS` | ✅ 内核里 `custom x18 ABI usage` 字符串 **0 次**，说明跑的是 `#if !__ARM_KERNEL_PROTECT__` 分支，返回 SUCCESS |
| `kern_exec.c:8019` | `imgp->ip_mac_return != 0` | ⚠️ 需要 `exec_handle_sugid` 走到 L6773 |

**而 `ip_mac_return` 的来源链：**

```
kern_exec.c:6640   int label_update_return = 0;      ← 已初始化
kern_exec.c:6757   kauth_proc_label_update_execve(..., &label_update_return)
kern_exec.c:6773   imgp->ip_mac_return = label_update_return;
mac_vfs.c:876      *labelupdateerror = error;
```

**`LU` 探针（在 `*labelupdateerror = error` 处）从未画出** → `mac_cred_label_update_execve` 没被调用
→ `label_update_return` 保持 0 → `ip_mac_return` 应为 0 → L8019 不该触发。

**代码说不该发生，实测说发生了。**

### 对策：在交付点量，不再向后反推

新增 `PN` / `PC2` 探针，直接画 `psignal_with_reason` 收到的那份 reason：

```c
nb u_mark_num("PN",  exec_failure_reason ? ->osr_namespace : 7777, 2100);
nb u_mark_num("PC2", exec_failure_reason ? ->osr_code      : 7777, 2180);
psignal_with_reason(current_proc(), SIGKILL, exec_failure_reason);
```

| `PN` | `PC2` | 含义 |
|---|---|---|
| 7777 | 7777 | reason 是 NULL → 走的是 `kern_sig.c` 的「no reason provided」那条，与 `SIG900008` 矛盾，说明我解码错了 |
| **9** | **8** | 确认 `SECURITY_POLICY`，且它确实从这里发出 —— 那 `ip_mac_return` 一定非 0，需要再找第三个赋值点 |
| 其他 | 其他 | 真正的原因第一次现形 |

### 这一天的教训（写下来免得重复）

**从早上到现在，凡是「按代码推断」得出的结论，有一半被实测推翻：**

| 推断 | 实测结果 |
|---|---|
| ADT 的 dram_size 需要改 | `maxmem4864` 正确 |
| `size - rlimit` 下溢 | `prt16384` 正常 |
| 栈太小 | `usz` 4.24 MB 充足 |
| `freepg` 虚高导致缺页失败 | `freepg` 是 panic 的**后果** |
| slide 让 offset 算错 | 两端同 slide，抵消 |
| boot-args 没到内核 | `AMFI = 1`，到了 |
| 签名失败（CS_KILLED） | `CS_KILLED = 0` |

**唯一被实测证实的推断是 PAC 那条** —— 而它也是唯一被改成实际修复的一条（`PCOF` 从
`0x230397` 变成 `0x2b7027`，`RV` 从 0 变成有效指针，`_bcopyout` 的 panic 消失）。

**所以：先量，再改。屏幕上的每一个数字都要真的读出来。**

---

## 十四、`bsd/kern` 里共有**六个** `SIGKILL` 投递点，探针之前插错了

### 六个站点

| 文件:行 | reason 变量 | 能否是 `SECURITY_POLICY` |
|---|---|---|
| `kern_exec.c:2158` | `exec_failure_reason` | ❌ 该变量从未被赋成 SECURITY_POLICY（全是 BAD_MACHO / COPYOUT_STRINGS / SET_DYLD_INFO 等） |
| `kern_exec.c:4845` | `exec_failure_reason` | ❌ 同上 |
| **`kern_exec.c:8227`** | **`signature_failure_reason`** | ✅ **只有它**（L8019 赋值） |
| `kern_proc.c:4354` / `4573` | `kill_reason` | ❌ 属别的子系统 |
| `kern_sig.c:1608` | `signal_reason` | 转发路径 |
| `mach_loader.c:1400` | `load_failure_reason` | ❌ 是 `FAIRPLAY_DECRYPT`(10) / `DECRYPT`(11) |

### 我之前插错了位置

`PN`/`PC2` 探针插在 **L2158**（`badtoolate` 那条），而那条**从不触发** —— 所以屏幕上
`PN`、`PC2` 都是空白，只剩 `SIG900008`。**看起来像「探针没生效」，实际是「这条路径没走」。**

### 已改到正确位置

```c
kern_exec.c:8225   n a b u_mark_num("SN",  signature_failure_reason ? ->osr_namespace : 7777, 2100);
kern_exec.c:8226   n a b u_mark_num("SC",  signature_failure_reason ? ->osr_code      : 7777, 2180);
kern_exec.c:8227   n a b u_mark_num("SU",  (unsigned long)require_success, 2260);
kern_exec.c:8227   psignal_with_reason(p, SIGKILL, signature_failure_reason);
```

### 判读

| `SN` | `SC` | `SU` | 含义 |
|---|---|---|---|
| **9** | **8** | 0 或 1 | 与 `SIG900008` 一致，确认 reason 从这里发出 → `signature_failure_reason` 确实来自 L8019 → **`ip_mac_return` 非 0，存在我没找到的赋值路径** |
| 7777 | 7777 | — | reason 是 NULL → 那 `SIG900008` 不是从这个对象来的，我的解码链条要重来 |
| 其他 | 其他 | — | 真正的原因第一次现形 |

### 一条通用教训

**「探针没显示」有三种完全不同的原因，必须先区分再改：**

1. **没编译进去** —— `strings -n 2` 查不到标签（`strings` 默认 4 字符，2 字符标签永远查不到）
2. **编译进去了但那段代码没执行** —— 标签在核心里、屏幕上却没有
3. **执行了但被覆盖** —— 行号在 console 带（< 1780）内，或被相邻探针的 61px 清除区擦掉

**这一整天至少五次把 (2) 误判成 (1)，白白重构建。** 正确做法是每次都先
`strings -n 2 kernel.macho | grep -cx LABEL` 确认标签存在，再上机看。

---

## 十六、`LU` 不是没执行 —— 是被 `SIG` 覆盖

### 真相

我给 `LU`（`*labelupdateerror`）和 `SIG`（`psignal` 里的 reason）**都分配了 2500 行**。
`SIG` 在启动中晚得多才画，所以它把 `LU` 覆盖掉了。

**于是整整十几轮里，我把「被覆盖」误读成「这段代码没跑」，并以此排除了
`mac_cred_label_update_execve` —— 而后者才是真正产生 `SECURITY_POLICY` 的地方。**

### 修正

| 探针 | 原行 | 新行 | 状态 |
|---|---|---|---|
| `LU`（`*labelupdateerror`） | 2500（被覆盖） | **2100** | 可见 |
| `KR`（`machine_task_process_signature`） | 2340 | 2180 | 可见 |
| `EM`（`error_msg != NULL`） | 2420 | 2260 | 可见 |
| `SN`/`SC`/`SU` | 2100/2180/2260 | 退休 | 已回答：9 / 8 / 0 |

### 当前的完整链条（都已实测）

```
mac_cred_label_update_execve   遍历 policy，error = mac_error_select(hook(...))
   └─ *labelupdateerror = error          ← LU，现在可见
kern_exec.c:6773  imgp->ip_mac_return = label_update_return
kern_exec.c:8019  if (ip_mac_return != 0) → OS_REASON_EXEC + SECURITY_POLICY
kern_exec.c:8227  psignal_with_reason(SIGKILL, signature_failure_reason)   ← SN9 SC8 SU0 实测
psignal()         initproc 被 SIGKILL → panic
```

### 判读

| `LU` | 含义 |
|---|---|
| **非 0** | 某个 MACF policy 的 `mpo_cred_label_update_execve` 拒绝了 exec —— 用 errno 反查是哪个 hook |
| **0** | MACF 没拒绝；那 `SECURITY_POLICY` 只剩 `proc_process_signature` 一条来源（由 `KR`/`EM` 验证） |

### 一条该刻在墙上的教训

**探针的"没显示"必须先区分三种情况再下结论：**

1. 没编译进去 → `strings -n 2` 查标签（注意 2 字符标签必须 `-n 2`）
2. 编译了但那段代码没执行 → 标签在核心、屏幕无
3. **执行了但被别的探针覆盖** → 同一行或 61px 清除区重叠

**今天至少六次把 (3) 当成 (2)，白白烧掉整轮构建。** 每次上机前都该核对一遍行号表。

## 六、状态速查

**能做什么**：CI + 本地 WSL 构建（72 秒），打包 cust，刷机，真机跑到 `execve` 的 `copyoutptr`。

**不能做什么**：跨过那次缺页。`launchd` 已解析、已加载，但用户栈上有一页写不进去。

**`STATUS.md` 908 行**记录了完整历史；本文件是当前断面。
