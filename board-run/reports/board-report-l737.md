# l737 六问板报（2026-11-12，DPU/MDP EL1 只读判定轮）

## 1. 唯一问题

`nabu-dpu=2` 在 l730 kernel 已有的一次只读探针，能否从 EL1 安全读取 VIG 窗口并给出 `dpu:vig-va/len`、`dpu:aim-*`、`dpu:read`、`n_ssp/ssp` 而不触发同步外部 abort？这是 DPU display-SMMU 路线的第一道硬门槛；本轮不声称闭合 bank2/clock/CTL/INTF/SMMU。

## 2. 已闭合到哪

- l730 kernel 有 `nabu-dpu`、`dpu:vig-/dma-/aim-/read` 格式串；`NativeUfs.cpp:715-775` 的 `nabu_dpu_probe_once()` 已在 `NativeUfs.cpp:4674` 被调用，输出走既有 `nabu_log_puts/hex` 与 `ssp=/n_ssp=` 状态行，不是新探针。
- `nabu-dpu=2` 把 `NativeUfs.cpp:774-798` 限为 1 个窗口 / 1 个 pipe / 1 个寄存器（pipe0 的 `0x00`）；只读 `0xae004000/0x7000` VIG 窗口与 `0xae24000/0x7000` DMA 窗口的映射元数据，随后一次总线读。
- 静态 SM8150 catalog：MDP `0xae01000`、VIG0/1 `0xae05000/0xae07000`、CTL0 `0xae02000`、CTL_FLUSH `0xae02018`、INTF `0xae6b000..`。现有 probe 的 `0xae004000` 比真 VIG0 低 `0x1000`，因此本轮还同时检验窗口是否错位。
- `NativeLegacyDisplay.cpp:265` 的 `kCtlFlush=0x15018`、`kBootStride=0x2fa0`、`kBootIova=0xb8000000` 均有错或未证，但本轮不改/不启用 direct scanout，只是为后续路线收集第一手可读性证据。
- 历史 `nabu-dpu=1` 曾在 `nabu_dpu_probe_once+0x264` 触发 `esr 0x96000010`，`far=VA+0x4000`；`=2` 是官方已有的最小化读取模式。`arm-delay=0`、pet600、misc boot-recovery 保证早 panic 也自动回 TWRP。
- args 等长替换已预演并抽回验证：`nabu-state-ring=10` → `nabu-dpu=2` + 8 个空格，仍 700 字节 / 39 token。`nabu-state-ring` 只控制每 10 s 的 `NABUSTATE1`/mapper census；`nabuLogThread` 的 2 s `lg:tick` 与 `nabu_wdt_pet()` 不依赖它。
- 五文件除 payload 内 toml args 外全部与 l730 一致；kernel sha8 仍 `d3598dd6`。

## 3. 为什么离线不行

`AGENTS.md` 允许在静态链闭合后，为「寄存器在 EL1 是否可读、firmware 是否点亮 clock/GDSC」这类运行期量做一次判定。当前静态只能定位窗口与格式串，不能证明 l730 运行态 MDP 总线已上电、MMIO 映射可读、单次读是否 abort；必须一次上板。

## 4. 失败也有信息

- 出现 `dpu:vig-va/len`、`dpu:aim-*`、`dpu:read`、`n_ssp=1/...`、无 panic ⇒ MDP 窗口可读，后续可核正确 VIG/CTL/clock/SMMU 值。
- `ssp=none|oob`、`dpu:map-fail`、`dpu:vig-len=0` ⇒ 窗口映射/长度不对或 clock/GDSC 未点亮，DPU 路线需先修电源/窗口，不继续写 DPU 寄存器。
- `panic` + `esr 0x96000010`/`far` ⇒ MDP EL1 不可读，停止 DPU 路线，回到完整源码流水线或接受 l730 当前体验。
- 无论阳性/阴性，220/300 s 双槽、PON `0x80041`、WDT 自动回 TWRP、无 APFS/buf_bread 都是安全判据。

## 5. 唯一变量与回滚点

- 唯一变量：payload 内 `unixnu.toml` 的 args 等长替换，新增 `nabu-dpu=2`、删除 `nabu-state-ring=10`，其余 38 token 同序同字节；kernel/ADT/addon/trust/boot 五文件与 l730 逐字节一致。
- 不新增格式串、探针、打印；不改 kernel；不碰 `boot_a`；`sheng-smp=1 + nabu-wdt-arm-delay=0` 保持；args 700/39。
- 回滚点：`hold/l730`。l737 一次性，不进默认 args；上板后显式 `flash cust` 回 l730 并最后重 arm misc。

## 6. 恢复预案

- 上板只走 `fastboot flash cust` → **最后** `fastboot flash misc E:/nabu/dwc3/build/misc-bootrecovery.bin` → `fastboot boot <boot-merge-r3.img>`。
- `pet=600`、`arm=0`；历史 DPU abort 轮也由 WDT/PON 自动回 TWRP。
- 若早 panic，WDT 已 arm；若 WDT 未触发，`fastboot devices` 应仍可用；否则才需人工长按。
- 不使用 `fastboot flash boot_a` / `flash boot`。

## 门禁状态

- `audit_bootability.py`：WSL `OK (0 fail, 0 warn)`。
- `audit_args.py`：700/39 → 700/39，added=`['nabu-dpu=2']`、dropped=`['nabu-state-ring=10']`，`OK`。
- `audit_dual.py`：11/11 match，`state/audit-l737.json` 已写。
- 本板报、`precheck.py`、`precheck_check.py`、`guard_no_boot_flash.py`、`board_report_check.py` 全 PASS 后才允许上板。
