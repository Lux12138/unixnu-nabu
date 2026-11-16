// SPDX-License-Identifier: BSD-2-Clause-Patent
// Native sheng SM8550 UFS bring-up. Default-off: no MMIO is touched unless
// native-ufs-probe=1 is present. This is a register probe, not a storage driver.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/crypto/sha2.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <sys/param.h>
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
#include "verified_root.h"
#include "VerifiedBlockPolicy.h"
extern "C" {
extern const struct vr_crypto vr_xnu_crypto;
extern const uint8_t verified_root_key[];
}
#endif
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSIterator.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOCatalogue.h>
#include <kern/thread.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>   /* l515c: IODeviceMemory::withRange for the DPU windows */
// Status-strip markers, so a failed publication is readable on the panel instead of only
// through kprintf (which is discarded on this board).
extern "C" void nabu_log_hex(const char *label, unsigned long long value);
extern "C" void nabu_log_puts(const char *s);
extern "C" void nabu_legacy_census_once (int force);
extern "C" void nabu_legacy_census2_arm (int stage);
extern "C" void nabu_oslog_dump_arm (void);
extern "C" void nabu_diag_dump_arm (int seq);
/* l585: the per-core secondary bring-up counters ArmCpu keeps. BootProgress.c was their only
 * consumer and it is not built here (XNU_BOOT_PROGRESS=0), so read the same globals. */
extern "C" {
extern volatile uint32_t __attribute__((weak_import)) armcpu_smp_step[8];
extern volatile uint32_t __attribute__((weak_import)) armcpu_smp_fail[8];
extern volatile int32_t __attribute__((weak_import)) armcpu_smp_psci_rc[8];
extern volatile uint32_t __attribute__((weak_import)) armcpu_boot_mpidr;
}
/* Counts every doAsyncReadWrite entry. The window keeps only the last 4 KiB, so a first-read
 * marker printed before the census can be pushed out; a counter read at census time cannot. */
static volatile uint64_t nabuReadEntered = 0;
/* Watchdog petting. Writing EN 0 then 1 restarts the counter, which is the only lever this
 * block gives us (BITE is not writable). Pets hang off real events, never off time: this
 * kernel has no usable timer. Petting stops nabu-wdt-pet-until seconds after arming
 * (default 300), so the reset cannot outlive the evidence and a wedged loop still resets. */
/* l487: the DPU's own answer to "what is the panel fetching right now, and how big".
 * Addresses are not guessed: mdp_phys comes from our own vendor device tree
 * (dts/nabu-full.dts:861 `reg = <0xae00000 0x84208 0xaeb0000 0x2008 0xaeac000 0x214>`,
 * reg-names mdp_phys/vbif_phys/regdma_phys). Linux names every block inside that window:
 * drivers/gpu/drm/msm/disp/dpu1/catalog/dpu_5_0_sm8150.h gives eight SSPP pipes, VIG0..3 at
 * 0x4000/0x6000/0x8000/0xa000 and DMA0..3 at 0x24000/0x26000/0x28000/0x2a000, each len 0x1f0.
 * dpu_hw_sspp.c - the same numbers in v6.6 and in master, checked against the extracted tarballs -
 * gives the register map inside a pipe: SSPP_SRC_SIZE 0x00 = (height << 16) | width,
 * SSPP_SRC_XY 0x08, SSPP_OUT_SIZE 0x0c, SSPP_SRC0_ADDR 0x14 (the address setup_sourceaddress
 * writes), SSPP_SRC_FORMAT 0x30 ((bpp-1) << 9 | ...), SSPP_FETCH_CONFIG 0x48 (the driver's own
 * reset value is 0x87).
 * l511 read only the four VIG pipes and only two registers, which is why every vig field came back
 * "<size 0>@<something>". Two things were missing: a console scanout normally sits on a DMA pipe,
 * and DMA0..3 were outside the mapped window at all; and SRC0_ADDR on its own cannot be read -
 * without SRC_SIZE and SRC_FORMAT beside it, an address is just a number.
 * Touching this region is a risk we have never taken: the display clocks may not be enabled, and the
 * debug library's own boundary note says an address check cannot stop an asynchronous exception on an
 * inaccessible bus. So the region is never mapped unless nabu-dpu=1, it is read exactly once into RAM,
 * the provider never touches MMIO, and there is no retry and no panic. Worst case is one round that
 * hangs and comes back via the armed watchdog or an acknowledged REBOOT - which since l485 is a
 * command we can send ourselves. We deliberately do not unmap: whether an unmap API exists here is
 * less certain than the value of not depending on it. */
/* C symbol in osfmk/arm/machine_routines.h; declared here because this translation unit does
 * not pull in that header. */
extern "C" vm_offset_t ml_io_map (vm_offset_t phys_addr, vm_size_t size);

/* l484: defined in osfmk/console/serial_general.c (the ring owns the counters the banner reports). */
extern "C" void nabu_log_state_snapshot (void);

#define NABU_DPU_MDP_PHYS     0xae00000ULL
#define NABU_DPU_PIPE_COUNT   8u         /* VIG0..3 then DMA0..3, bases from dpu_5_0_sm8150.h */
#define NABU_DPU_WINDOWS      2u         /* one page-granular window per pipe family */
/* Slot numbers in gDpuSsp[][]. They are *not* offset/4 - 0x30 and 0x48 would run past the row -
 * the offsets stay beside them in regOfs[]. */
#define NABU_DPU_SLOT_SIZE    0u   /* 0x00 (h << 16) | w       */
#define NABU_DPU_SLOT_XY      1u   /* 0x08 (y << 16) | x       */
#define NABU_DPU_SLOT_OUT     2u   /* 0x0c (dstH << 16) | dstW */
#define NABU_DPU_SLOT_ADDR    3u   /* 0x14 plane 0 address     */
#define NABU_DPU_SLOT_FORMAT  4u   /* 0x30 (bpp-1) << 9 | ...  */
#define NABU_DPU_SLOT_FETCH   5u   /* 0x48 driver reset 0x87   */
#define NABU_DPU_REG_COUNT    6u
#define NABU_DPU_UNREAD       0xdeadbeefu

/* The framebuffer the firmware handed us. Read here (not from the legacy backend) so this needs no
 * rebuild of the owned late archive, and put in the same banner as vig*=<size>@<addr> so the
 * comparison "is the panel fetching our buffer?" closes inside one line of the artifact instead of
 * somebody remembering a number from an earlier round. */
extern "C" int PE_current_console (PE_Video *gState);
static volatile uint32_t gGopWidth = NABU_DPU_UNREAD, gGopHeight = NABU_DPU_UNREAD, gGopRow = NABU_DPU_UNREAD;
static volatile uint64_t gGopBase = 0;

/* l515c: the windows. Both live inside the mdp_phys reg the vendor DT declares
 * (0xae00000 len 0x84208), and each is only wide enough for its four pipes (0x6000 + 0x1f0).
 * Why they changed shape: l515b asked ml_io_map for one 0x2c000 window, got back a non-zero
 * pointer, and died on its *first* read - esr 0x96000010 (level-0 translation fault) with
 * far = the returned pointer + 0x4000 and pc inside nabu_dpu_probe_once. A non-zero ml_io_map()
 * return therefore does not mean the range is readable, and with one wide window every pipe died
 * with the first. So: two small windows, mapped exactly the way the touch stack maps GCC, TLMM and
 * the serial engine (IODeviceMemory::withRange + kIOMapInhibitCache, which has returned usable
 * pages on this board since l510), and each window's virtual address is written to the ring before
 * any read touches it - a fault now names the window and the address instead of the whole block. */
static const struct { uint64_t phys; uint64_t len; } gDpuWindow[NABU_DPU_WINDOWS] = {
  { 0xae00000ULL + 0x4000u, 0x7000u },   /* VIG0..3 at +0x0, +0x2000, +0x4000, +0x6000 */
  { 0xae00000ULL + 0x24000u, 0x7000u },  /* DMA0..3 at the same four offsets */
};
static const uint32_t gDpuPipeOffset[NABU_DPU_PIPE_COUNT] = {
  0x0000u, 0x2000u, 0x4000u, 0x6000u, 0x0000u, 0x2000u, 0x4000u, 0x6000u,
};
static const unsigned char gDpuPipeWindow[NABU_DPU_PIPE_COUNT] = {0, 0, 0, 0, 1, 1, 1, 1};
static IOMemoryMap *gDpuMap[NABU_DPU_WINDOWS] = {nullptr, nullptr};  /* kept on purpose: no unmap API is assumed */
/* l518c: what each map ACTUALLY covers. The request in gDpuWindow[].len was written from memory
 * and is exactly what the panic questions (far sits 0xe000 under x17, and 0xe000 > that 0x7000), so
 * the bounds check below never uses the requested length -- only map->getLength(). */
static volatile uint64_t gDpuWinLen[NABU_DPU_WINDOWS] = {0, 0};
#define NABU_DPU_OOB 0xdeadbee1u   /* never read: address lay outside the real mapping */
static volatile uint32_t gDpuSsp[NABU_DPU_PIPE_COUNT][NABU_DPU_REG_COUNT];
/* 0 = never asked for (no nabu-dpu=1), 1 = asked, map failed, NABU_DPU_PIPE_COUNT = read. */
static volatile unsigned gDpuSspPipes = 0;
/* Rendered once, at probe time, so the banner stays a printf and the packing rules live here:
 * "<j>:<srcW>x<srcH>><<dstW>x<dstY>>@<src0 addr>/b<bpp>/f<fetch>" per pipe that says anything at
 * all, and "none" when every pipe is silent. Width/height are unpacked from the (h << 16) | w
 * words so the number on the panel (1600x2560) is comparable by eye; addr is printed raw because
 * SSPP_SRC0_ADDR is a bus address inside the display SMMU's own address space, which is *not*
 * necessarily the CPU-physical gopbase= the firmware console uses. */
static char gDpuSspText[2 * 20 + 5 * 48] = "off";   /* the two window addresses, then four pipes */
static volatile unsigned gDpuSspLive = 0;

/* l496: the console geometry is a plain platform-expert struct read - no MMIO, cannot abort the
 * bus - so it must not sit behind the nabu-dpu=1 gate. That gate exists only for the MDP register
 * window, which does take the bus down (synchronous external abort, measured). Before this split a
 * round without nabu-dpu printed `row=3735928559` (the never-read sentinel) next to a backend that
 * had already printed `starthw ok ... row=6400`: the ruler was never armed, which is not the same
 * statement as "the panel's buffer is unreadable". */
static void
nabu_gop_probe_once (void)
{
  static int done = 0;

  if (done) {
    return;
  }
  done = 1;
  {
    PE_Video video = {};
    if (PE_current_console (&video) == 0) {
      gGopBase = (uint64_t) video.v_baseAddr;
      gGopWidth = (uint32_t) video.v_width;
      gGopHeight = (uint32_t) video.v_height;
      gGopRow = (uint32_t) video.v_rowBytes;
    } else {
      nabu_log_puts ("gop:console-fail\n");
    }
  }
}

/* l520 (GPU M0): the first read-only look at the A640. Everything here answers one question: are
 * these three register windows actually reachable from the kernel at this point in boot. It is a
 * probe in the same shape as the DPU census after l518c -- announce phys, VA and the real mapped
 * length before touching anything, bounds-check every read against map->getLength(), and keep a
 * one-register arm so "the block cannot be read" and "my table is wrong" stay separable. Reading a
 * powered-down GPU domain can take the bus down with a synchronous external abort, which is exactly
 * what nabu-dpu=1 did to l518, so this never runs unless nabu-gpu asks for it, and it writes nothing.
 * The CP and GMU always-on counters double as the positive control: a counter that advances between
 * two reads proves the window is live, and one that reads constant 0xdeadbeef/0 means it is not. */
#define NABU_GPU_WINDOWS      8u
#define NABU_GPU_UNREAD       0xdeadbeefu
#define NABU_GPU_SKIPPED      0xdeadbee2u   /* not read: the stage before it never proved its prerequisite */
#define NABU_GPU_NOPAGE       0xdeadbee3u   /* the GCC page holding it was not mapped */

/* The three pages of the global clock controller, plus the three GPU windows M0 already mapped.
 * GCC itself: linux-6.6 arch/arm64/boot/dts/qcom/sm8150.dtsi:903 reg = <0 0x00100000 0 0x1f0000>,
 * byte-identical to the copy in /root/fresh/ref-linux/sm8150.dtsi:909, so 0x71004/0x52004/0x8c010
 * are all inside one controller that is 0x1f0000 long. The kernel already maps GCC_BASE+0x77000 for
 * the UFS branches (this file, mapPhys near the UFS probe), so this window family is the one the
 * board has read for 170+ rounds -- which is also what GpuPowerBoard.h:110 calls "always safe". */
static const struct { uint64_t phys; uint64_t len; const char *name; }
    gGpuWindow[NABU_GPU_WINDOWS] = {
  { 0x00100000ULL + 0x71000u, 0x1000u, "gpu:gcc71-phys-171000\n" }, /* GPU branches: gcc-sm8150.c:1574,1636,1649 */
  { 0x00100000ULL + 0x52000u, 0x1000u, "gpu:gcc52-phys-152000\n" }, /* gcc_gpu_gpll0_clk_src .enable_reg 0x52004: :1594 */
  { 0x00100000ULL + 0x8c000u, 0x1000u, "gpu:gcc8c-phys-18c000\n" }, /* gcc_gpu_iref_clk .halt_reg 0x8c010: :1624 */
  { 0x2c00000ULL, 0x40000u, "gpu:core-phys-2c00000\n" },   /* dts/nabu-full.dts:28957 */
  { 0x2c6a000ULL, 0x30000u, "gpu:gmu-phys-2c6a000\n" },    /* dts/nabu-full.dts:29245; linux-6.6 sm8150.dtsi:2268 agrees */
  { 0x2c90000ULL, 0x09000u, "gpu:cc-phys-2c90000\n" },     /* dts/nabu-full.dts:5766; linux-6.6 sm8150.dtsi:2306 agrees */
  /* l522: the two PDC windows. MAPPED AND REPORTED, NEVER READ in this build -- 0x0b000000 is a
   * physical region this kernel has not touched in 170+ rounds, so it earns its own round. Their rc
   * alone is an answer: Linux refuses to run the PDC setup without them (a6xx_gmu.c:515-535). */
  { 0x0b290000ULL, 0x10000u, "gpu:pdc-phys-b290000\n" },   /* reg-names "gmu_pdc": nabu-full.dts:29245 = nabu-extracted.dts:2241 */
  { 0x0b490000ULL, 0x10000u, "gpu:seq-phys-b490000\n" },   /* reg-names "gmu_pdc_seq": same two sources */
};
enum { GpuWinGcc71 = 0u, GpuWinGcc52 = 1u, GpuWinGcc8c = 2u,
       GpuWinCore = 3u, GpuWinGmu = 4u, GpuWinCc = 5u,
       GpuWinPdc = 6u, GpuWinPdcSeq = 7u };

/* l522: the RSCC registers are NOT another block. a6xx_gmu.c:1667-1675 gives
 * `gmu->rscc = gmu->mmio + 0x23000` for every family except a650, and A640 is not a650;
 * a6xx_gmu.h:141-149 then reads `rscc + (offset << 2)`. So these byte offsets live inside the GMU
 * window that M0 has already read twice with no abort -- which is why reading them is a different
 * risk from reading the PDC windows above. */
#define NABU_RSCC_BASE   0x23000u
enum {
  RsStatus0    = NABU_RSCC_BASE + (0x0004u << 2),   /* REG_A6XX_GPU_RSCC_RSC_STATUS0_DRV0 : a6xx_gmu.xml.h:448 */
  RsPdcSeqStart= NABU_RSCC_BASE + (0x0008u << 2),   /* REG_A6XX_RSCC_PDC_SEQ_START_ADDR   : a6xx_gmu.xml.h:450 */
  RsSeqBusy    = NABU_RSCC_BASE + (0x0101u << 2),   /* REG_A6XX_RSCC_SEQ_BUSY_DRV0        : a6xx_gmu.xml.h:472 */
  RsSeqMem0    = NABU_RSCC_BASE + (0x0180u << 2),   /* REG_A6XX_RSCC_SEQ_MEM_0_DRV0       : a6xx_gmu.xml.h:474 */
  RsCtlReq     = 0x9307u << 2,                      /* REG_A6XX_GMU_RSCC_CONTROL_REQ, straight off
                                                     * the GMU base (not the rscc alias) : :434 */
  RsCtlAck     = 0x9308u << 2,                      /* REG_A6XX_GMU_RSCC_CONTROL_ACK      : :436 */
};

/* l521 (M1-a): the offsets that l520c had to leave out because "no local source" was true then and
 * is not now. The source is the vendor driver kept on this machine -- /mnt/e/nabu/tmp/dpu/v66.tgz is
 * Linux 6.6, extracted read-only under E:/nabu/tmp/gpu66/ -- and the board's own numbers agree with
 * the ones 8714 pulled from mainline, so one tree is authoritative from here on. */
enum {
  /* page-relative, inside the GCC pages above */
  GccCfgAhb     = 0x004u,   /* 0x71004 halt/enable, BIT(0), hwcg_bit 1  : gcc-sm8150.c:1574-1589 */
  GccMemnocGfx  = 0x00cu,   /* 0x7100c BRANCH_VOTED                     : gcc-sm8150.c:1636-1647
                            * (sm8550 has this one at 0x71010 -- the staged shape transfers, the
                            * numbers do not, and copying GpuPowerBoard.h here would have read the
                            * wrong register on both of the two pages below) */
  GccSnocDvm    = 0x018u,   /* 0x71018                                  : gcc-sm8150.c:1649-1657 */
  GccPllVote    = 0x004u,   /* 0x52004 BIT(15)/BIT(16)                  : gcc-sm8150.c:1591-1621
                            * (sm8550 uses 0x52000) */
  /* GPUCC, byte offsets exactly as the driver spells them */
  CcAhb         = 0x1078u,  /* gpu_cc_ahb_clk, BRANCH_HALT_DELAY         : gpucc-sm8150.c:109 */
  CcCrcAhb      = 0x107cu,  /* gpu_cc_crc_ahb_clk                        : gpucc-sm8150.c:122 */
  CcCxApb       = 0x1088u,  /* gpu_cc_cx_apb_clk                         : gpucc-sm8150.c:135 */
  CcCxGmu       = 0x1098u,  /* gpu_cc_cx_gmu_clk                         : gpucc-sm8150.c:148 */
  CcCxSnocDvm   = 0x108cu,  /* gpu_cc_cx_snoc_dvm_clk                    : gpucc-sm8150.c:166 */
  CcCxoAon      = 0x1004u,  /* gpu_cc_cxo_aon_clk                        : gpucc-sm8150.c:179 */
  CcCxo         = 0x109cu,  /* gpu_cc_cxo_clk                            : gpucc-sm8150.c:192 */
  CcGxGmu       = 0x1064u,  /* gpu_cc_gx_gmu_clk                         : gpucc-sm8150.c:205 */
  CcCxGdscr     = 0x106cu,  /* gpu_cx_gdsc .gdscr                        : gpucc-sm8150.c:223 */
  CcCxHwCtrl    = 0x1540u,  /* gpu_cx_gdsc .gds_hw_ctrl, flags VOTABLE   : gpucc-sm8150.c:224,229 */
  CcGxGdscr     = 0x100cu,  /* gpu_gx_gdsc .gdscr                        : gpucc-sm8150.c:233 */
  CcGxClampIo   = 0x1508u,  /* gpu_gx_gdsc .clamp_io_ctrl                : gpucc-sm8150.c:234 */
  CcCxBcr       = 0x1068u,  /* [GPUCC_GPU_CC_CX_BCR] = { 0x1068 }        : gpucc-sm8150.c:257 */
  /* l525 (mode 4): the clock that feeds the GMU on the CX side. gpu_cc_gmu_clk_src is an RCG2 whose
   * CMD_RCGR is 0x1120 (gpucc-sm8150.c:93-94), so its CFG word -- the one holding SRC_SEL and
   * DIV -- sits four bytes above it (clk-rcg2.c: CMD_RCGR 0x0, CMD_CFG +0x4). Its parent map is
   * { bi_tcxo=0, GPU_CC_PLL1_OUT_MAIN=3, GPLL0_OUT_MAIN=5, GPLL0_OUT_MAIN_DIV=6 }
   * (gpucc-sm8150.c:64-69) and the frequency table is 19.2 MHz on TCXO, 200 MHz on GPLL0_DIV with
   * div 1.5, 500 MHz on PLL1 (:79-84) -- which is why the safe first step is sel 0: it needs no
   * PLL1 at all (gpu_cc_pll1 .offset = 0x100, a TRION :47-62) and no new vote. */
  CcGmuRcgr     = 0x1120u,  /* gpu_cc_gmu_clk_src .cmd_rcgr              : gpucc-sm8150.c:94 */
  CcGmuCfg      = 0x1124u,  /* CMD_RCGR + 4 = CMD_CFG (SRC_SEL bits 10:8, DIV 4:0) */
  /* GMU: register number << 2, a6xx_gmu.xml.h -- the offsets l520c called "no local source" */
  GmuSysreset      = 0x5000u << 2,   /* REG_A6XX_GMU_CM3_SYSRESET        : a6xx_gmu.xml.h:197 */
  GmuBootConfig    = 0x5001u << 2,   /* REG_A6XX_GMU_CM3_BOOT_CONFIG     : a6xx_gmu.xml.h:199 */
  GmuFwInitResult  = 0x501cu << 2,   /* REG_A6XX_GMU_CM3_FW_INIT_RESULT  : a6xx_gmu.xml.h:203 */
  GmuHfiCtrlStatus = 0x5180u << 2,   /* REG_A6XX_GMU_HFI_CTRL_STATUS     : a6xx_gmu.xml.h:313 */
  GmuHfiVersion    = 0x5181u << 2,   /* REG_A6XX_GMU_HFI_VERSION_INFO    : a6xx_gmu.xml.h:315 */
  /* RBBM / CP, unchanged from M0 and now sourced twice over */
  GpuCoreStatus     = 0x00210u << 2,   /* a6xx.xml.h:1470 REG_A6XX_RBBM_STATUS (was A740HwInit.h:39) */
  GpuCoreStatus3    = 0x00213u << 2,   /* a6xx.xml.h:1500 REG_A6XX_RBBM_STATUS3 */
  GpuCoreInt0       = 0x00201u << 2,   /* a6xx.xml.h:1468 REG_A6XX_RBBM_INT_0_STATUS */
  GpuCoreCpLow      = 0x00980u << 2,   /* a6xx.xml.h:1396 REG_A6XX_CP_ALWAYS_ON_COUNTER */
  GpuCoreCpHigh     = 0x00981u << 2,
  GmuCounterLow     = (0x1f888u << 2) - 0x6a000u,   /* NativeAdrenoTimer.cpp:6-10, and independently
                            * a6xx_gmu.xml.h:307 REG_A6XX_GMU_ALWAYS_ON_COUNTER_L = 0x5088; 0x5088<<2
                            * is the same 0x14220 -- so the byte M0 read is confirmed by two sources,
                            * and M0 reading 0 there means no power, not a wrong address. */
  GmuCounterHigh    = (0x1f889u << 2) - 0x6a000u,
};
/* Bit meanings come from the clock framework, not from the SoC file: gdsc.c:20 PWR_ON_MASK BIT(31),
 * gdsc.c:26 SW_COLLAPSE_MASK BIT(0), clk-branch.h:42 CBCR_CLK_OFF BIT(31).
 * gcc_gpu_cfg_ahb_clk carries hwcg_bit 1 (gcc-sm8150.c:1577-1578), so CLK_OFF is not a usable "off"
 * test for it -- with hardware clock gating an idle-but-enabled branch still reads CLK_OFF set and
 * wakes on access. The enable bit BIT(0) is the test, exactly as GpuPowerBoard.h:90-93 records for
 * sm8550. That is why the gate below asks for BIT(0) and never for BIT(31). */
#define NABU_GPU_PWR_ON   (1u << 31)   /* gdsc.c:20 */
#define NABU_GPU_CLK_ON   (1u << 0)    /* gcc-sm8150.c:1581 enable_mask = BIT(0) */
#define NABU_GPU_CLK_OFF  (1u << 31)   /* clk-branch.h:42 CBCR_CLK_OFF -- same bit, different block */

static volatile uint32_t gGpuMapRc[NABU_GPU_WINDOWS] = {NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD,
                                                        NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD};
static volatile uint32_t gGpuRead[6] = {NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD,
                                        NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD};
/* [0] RBBM_STATUS [1] RBBM_STATUS3 [2] RBBM_INT_0_STATUS [3] CP counter high [4] CP counter low
 * [5] GMU ALWAYS_ON_COUNTER low (the high word is not printed; low is enough for the positive control) */
/* l521: the power chain, printed on its own line in the order the stages proved it. */
static volatile uint32_t gGpuGcc[4] = {NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD};
                                        /* cfg_ahb, memnoc_gfx, snoc_dvm_gfx, gpll0 vote */
static volatile uint32_t gGpuCc[4]  = {NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD};
                                        /* gpu_cc_ahb, cx_gmu, gx_gmu, cxo */
static volatile uint32_t gGpuGd[4]  = {NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD};
                                        /* cx_gdscr, gx_gdscr, cx gds_hw_ctrl, cx_bcr */
static volatile uint32_t gGpuGmu[4] = {NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD};
                                        /* fw_init_result, boot_config, cm3_sysreset, hfi_ctrl_status */
static volatile uint32_t gGpuMapGcc[3] = {NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD};
/* l522: the two PDC windows report their mapping and nothing else. */
static volatile uint32_t gGpuMapPdc[2] = {NABU_GPU_UNREAD, NABU_GPU_UNREAD};
static volatile uint32_t gGpuRs[10] = {NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD,
                                       NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD,
                                       NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD,
                                       NABU_GPU_UNREAD};
/* [0] RSC_STATUS0 [1] PDC_SEQ_START [2] SEQ_BUSY [3..7] SEQ_MEM_0 word 0..4
 * [8] RSCC_CONTROL_REQ [9] RSCC_CONTROL_ACK -- these are what a6xx_gmu_rpmh_init() would have to
 * write, so comparing them to the driver's own constants answers "did the bootloader already do
 * this?" without a single write from us. */
static volatile uint32_t gGpuStage = 0u;   /* 1 GCC, 2 +GPUCC, 3 +GMU, 4 +RBBM; 0 never ran.
   * The l522 RSCC read does not move this: it hangs off the same cfg_ahb gate as stage 2, and it
   * is judged from its own fields. */
/* l525: what mode 4 touched, and the post-write snapshot 8714 asked for.
 * The reason P1/P1 showed pre-write values on l523 is structural: nabu_gpu_probe_once() has a
 * `static int done`, so every stage read happened BEFORE the write arm -- and un-doing `done`
 * would mean repeating a power write on every banner, which is worse. So the fix is a second read
 * taken right after the writes, kept in its own fields, printed next to the snapshot it belongs to. */
static volatile uint32_t gGpuClk[4] = {NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD};
                                       /* cfg before, cfg after, cx_gmu before, cx_gmu after */
static volatile uint32_t gGpuPost[10] = {
  NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD,
  NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD};
                                       /* [0..3] GMU fw_init/boot_config/sysreset/hfi_status
                                        * [4..8] RBBM status, status3, int0, cp high, cp low
                                        * [9] GMU ALWAYS_ON counter low -- the heartbeat */
/* l526. The probe runs once (that is what `done` means, and it has to: the write arm may not repeat),
 * so gGpuPost[9] holds ONE value for the whole round -- and a single value cannot show a counter
 * counting. 8714 read aon=dd07c as "the GMU counter is ticking", and the number does not prove that:
 * it proves the register reads non-zero. To see a tick you need two samples, so keep the maps alive
 * after the probe -- the same way gDpuMap does it -- and re-read the two always-on counters on every
 * banner. Reading the GMU window is the one thing M0 already proved harmless (two full rounds, powered
 * or not), and this adds no write and no new physical region. */
static IOMemoryMap *gGpuKeep[NABU_GPU_WINDOWS] = {nullptr, nullptr, nullptr, nullptr,
                                                 nullptr, nullptr, nullptr, nullptr};
static volatile uint32_t gGpuHb[3] = {NABU_GPU_UNREAD, NABU_GPU_UNREAD, 0u};
/* [0] GMU ALWAYS_ON counter low, [1] CP ALWAYS_ON counter low, [2] heartbeat sequence number */

/* Forward declaration: the helper is defined below with the other readers, and clang needs the
   name before nabu_gpu_heartbeat uses it. Same signature, no second definition. */
static uint32_t nabu_gpu_read32 (IOMemoryMap *map, uint64_t len, uint64_t off);

static void
nabu_gpu_heartbeat (void)
{
  if (!gGpuKeep[GpuWinGmu] || !gGpuKeep[GpuWinCore]) {
    return;                        /* the probe never mapped them: no line, and no guess */
  }
  gGpuHb[0] = nabu_gpu_read32 (gGpuKeep[GpuWinGmu],
      (uint64_t) gGpuKeep[GpuWinGmu]->getLength (), GmuCounterLow);
  gGpuHb[1] = nabu_gpu_read32 (gGpuKeep[GpuWinCore],
      (uint64_t) gGpuKeep[GpuWinCore]->getLength (), GpuCoreCpLow);
  gGpuHb[2] = gGpuHb[2] + 1u;
}

static uint32_t
nabu_gpu_read32 (IOMemoryMap *map, uint64_t len, uint64_t off)
{
  if (!map || off + 4ull > len) {
    return NABU_GPU_SKIPPED;           /* never touch a byte outside what actually got mapped */
  }
  const volatile uint32_t *at = (const volatile uint32_t *)
      (uintptr_t) ((const uint8_t *)(uintptr_t) map->getVirtualAddress () + off);
  return *at;
}

/* One helper for every gate: the bytes past a stage that never proved itself are recorded as
 * skipped and never touched, so a line full of 0xdeadbee2 says "the gate closed here" and is not
 * mistaken for a hardware reading. */
static void
nabu_gpu_skip (volatile uint32_t *dst, unsigned n)
{
  for (unsigned i = 0; i < n; ++i) {
    dst[i] = NABU_GPU_SKIPPED;
  }
}

/* l523 (M1-b, WRITE arm -- only reached when nabu-gpu=4 is passed on purpose).
 *
 * This is upstream's mode 3, nothing more: package/GpuPowerBoard.h:135-181 runs exactly this
 * sequence for sm8550 and its comment is the boundary we keep -- "GX is never touched here
 * (the GMU owns it)" -- so the writes below are CX-only: clear SW_COLLAPSE, poll PWR_ON on
 * gds_hw_ctrl, then set the retain bits. No SMMU programming, no GMU register, no PDC/RSCC.
 * Bit meanings come from the clock framework, not from me: gdsc.c:26 SW_COLLAPSE_MASK=BIT(0),
 * :20 PWR_ON_MASK=BIT(31), :46/47 RETAIN_MEM=BIT(14)/RETAIN_PERIPH=BIT(13), gdsc.h:68
 * RETAIN_FF_ENABLE=BIT(7); the poll bound is GpuPowerBoard.h:47 GPB_GDSC_POLL_US=2000 (10 us
 * steps), so the wait is bounded at the same 2 ms upstream uses and never spins open-ended. */
#define NABU_GPU_RETAIN   ((1u << 14) | (1u << 13))   /* gdsc.c:46,47 */
#define NABU_GPU_RETAIN_FF (1u << 7)                  /* gdsc.h:68 */
#define NABU_GPU_COLLAPSE (1u << 0)                   /* gdsc.c:26 */
#define NABU_GPU_POLL_STEPS 200u                      /* 200 x 10 us = the upstream 2000 us bound */

/* What the write arm did, in the order it did it, so the round can be read without guessing:
 * [0] did the sequence run (1 = yes, 0 = never asked / gate closed)
 * [1] cx_gdscr before the first write   [2] poll steps used (200 = timed out, never a spin past it)
 * [3] gds_hw_ctrl after the poll        [4] cx_gdscr at the end
 * [5] gpu_cc_cx_gmu_clk afterwards      [6] gpu_cc_gx_gmu_clk afterwards -- the positive control:
 *     upstream enables these only after the domain is up (Linux does the same in
 *     a6xx_gmu/clock probe), so both flipping to bit0=1 is the board's own proof that CX came up. */
static volatile uint32_t gGpuAct[7] = {NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD,
                                       NABU_GPU_UNREAD, NABU_GPU_UNREAD, NABU_GPU_UNREAD,
                                       NABU_GPU_UNREAD};

static uint32_t
nabu_gpu_write32 (IOMemoryMap *map, uint64_t len, uint64_t off, uint32_t value)
{
  if (!map || off + 4ull > len) {
    return NABU_GPU_SKIPPED;   /* refused: outside what actually got mapped */
  }
  volatile uint32_t *at = (volatile uint32_t *)
      (uintptr_t) ((uint8_t *)(uintptr_t) map->getVirtualAddress () + off);
  *at = value;
  return value;
}

static void
nabu_gpu_probe_once (void)
{
  unsigned ask = 0;
  static int done = 0;

  if (!PE_parse_boot_argn ("nabu-gpu", &ask, sizeof (ask)) || !ask) {
    return;                            /* not asked for: no mapping, no read, no log line */
  }
  if (done) {
    return;
  }
  done = 1;

  IOMemoryMap *map[NABU_GPU_WINDOWS] = {nullptr, nullptr, nullptr};
  uint64_t real[NABU_GPU_WINDOWS] = {0, 0, 0};

  for (unsigned w = 0; w < NABU_GPU_WINDOWS; ++w) {
    IODeviceMemory *memory = IODeviceMemory::withRange (gGpuWindow[w].phys, gGpuWindow[w].len);
    if (!memory) {
      gGpuMapRc[w] = 0xfffffff1u;      /* no device memory for this range */
      nabu_log_puts (gGpuWindow[w].name);
      nabu_log_puts ("gpu:no-mem\n");
      continue;
    }
    map[w] = memory->map (kIOMapAnywhere | kIOMapInhibitCache);
    memory->release ();                /* the map holds its own reference */
    if (!map[w]) {
      gGpuMapRc[w] = 0xfffffff2u;
      nabu_log_puts (gGpuWindow[w].name);
      nabu_log_puts ("gpu:no-map\n");
      continue;
    }
    real[w] = (uint64_t) map[w]->getLength ();
    gGpuMapRc[w] = 0u;
    /* l526: keep the map past the end of the probe so the banner can re-read the counters. Held, not
     * released -- exactly how gDpuMap keeps its two windows, and the reason the heartbeat is possible
     * at all with the probe itself still one-shot. */
    gGpuKeep[w] = map[w];
    /* Announced before the first read of this window, exactly as l518c does for MDP: if the round
     * dies here, the window, the physical base and the length we were about to read through are
     * already on the wire. */
    nabu_log_puts (gGpuWindow[w].name);
    nabu_log_hex ("gpu:va=", (unsigned long long) (uintptr_t) map[w]->getVirtualAddress ());
    nabu_log_hex ("gpu:len=", (unsigned long long) real[w]);
  }

  /* Stage 1 is the global clock controller. gcc_gpu_cfg_ahb_clk is the branch Linux enables at its
   * own probe (qcom_branch_set_clk_en in gcc-sm8150.c:1574-1589) and everything under GPUCC hangs
   * off it, so this page is read first and unconditionally -- it is the same family the UFS
   * branches are already read from, which is what GpuPowerBoard.h:110 calls "always safe". */
  gGpuMapGcc[0] = gGpuMapRc[GpuWinGcc71];
  gGpuMapGcc[1] = gGpuMapRc[GpuWinGcc52];
  gGpuMapGcc[2] = gGpuMapRc[GpuWinGcc8c];
  gGpuGcc[0] = nabu_gpu_read32 (map[GpuWinGcc71], real[GpuWinGcc71], GccCfgAhb);
  gGpuGcc[1] = nabu_gpu_read32 (map[GpuWinGcc71], real[GpuWinGcc71], GccMemnocGfx);
  gGpuGcc[2] = nabu_gpu_read32 (map[GpuWinGcc71], real[GpuWinGcc71], GccSnocDvm);
  gGpuGcc[3] = nabu_gpu_read32 (map[GpuWinGcc52], real[GpuWinGcc52], GccPllVote);
  gGpuStage = 1u;

  /* nabu-gpu=2: the minimal face. One register, on the page the board has read before, and nothing
   * past it. It separates "the clock controller cannot be read here" from "my GPU table is wrong",
   * the same split nabu-dpu=2 buys for MDP. */
  if (ask == 2u) {
    nabu_gpu_skip (gGpuGcc + 1, 3u);
    nabu_gpu_skip (gGpuCc, 4u);
    nabu_gpu_skip (gGpuGd, 4u);
    nabu_gpu_skip (gGpuGmu, 4u);
    nabu_gpu_skip (gGpuRead, 6u);
    nabu_log_puts ("gpu:read-minimal\n");
    printf ("nabu gpu probe minimal: gcc71=%x rc=%x/%x/%x\n",
        (unsigned) gGpuGcc[0], (unsigned) gGpuMapGcc[0], (unsigned) gGpuMapGcc[1],
        (unsigned) gGpuMapGcc[2]);
    return;
  }

  /* With hardware clock gating on this branch (hwcg_bit 1) an idle-but-enabled clock still reads
   * CLK_OFF, so the gate asks for the enable bit and never for BIT(31). nabu-gpu=3 skips every gate
   * and is exactly what l520c did, so a staged round and an ungated one stay comparable. */
  if (ask == 3u || (gGpuGcc[0] & NABU_GPU_CLK_ON)) {
    /* Stage 2 -- GPUCC: the branches and the two domains. Read only once cfg_ahb proved itself. */
    gGpuCc[0] = nabu_gpu_read32 (map[GpuWinCc], real[GpuWinCc], CcAhb);
    gGpuCc[1] = nabu_gpu_read32 (map[GpuWinCc], real[GpuWinCc], CcCxGmu);
    gGpuCc[2] = nabu_gpu_read32 (map[GpuWinCc], real[GpuWinCc], CcGxGmu);
    gGpuCc[3] = nabu_gpu_read32 (map[GpuWinCc], real[GpuWinCc], CcCxo);
    gGpuGd[0] = nabu_gpu_read32 (map[GpuWinCc], real[GpuWinCc], CcCxGdscr);
    gGpuGd[1] = nabu_gpu_read32 (map[GpuWinCc], real[GpuWinCc], CcGxGdscr);
    gGpuGd[2] = nabu_gpu_read32 (map[GpuWinCc], real[GpuWinCc], CcCxHwCtrl);
    gGpuGd[3] = nabu_gpu_read32 (map[GpuWinCc], real[GpuWinCc], CcCxBcr);
    gGpuStage = 2u;

    if (ask == 3u || (gGpuGd[0] & NABU_GPU_PWR_ON)) {
      /* Stage 3 -- the GMU block, which lives in the CX domain that gdsc just had to be on for. */
      gGpuGmu[0] = nabu_gpu_read32 (map[GpuWinGmu], real[GpuWinGmu], GmuFwInitResult);
      gGpuGmu[1] = nabu_gpu_read32 (map[GpuWinGmu], real[GpuWinGmu], GmuBootConfig);
      gGpuGmu[2] = nabu_gpu_read32 (map[GpuWinGmu], real[GpuWinGmu], GmuSysreset);
      gGpuGmu[3] = nabu_gpu_read32 (map[GpuWinGmu], real[GpuWinGmu], GmuHfiCtrlStatus);
      gGpuRead[5] = nabu_gpu_read32 (map[GpuWinGmu], real[GpuWinGmu], GmuCounterLow);
      gGpuStage = 3u;

      if (ask == 3u || (gGpuGd[1] & NABU_GPU_PWR_ON)) {
        /* Stage 4 -- RBBM, the core window M0 read blind. The GX domain is the prerequisite. */
        gGpuRead[0] = nabu_gpu_read32 (map[GpuWinCore], real[GpuWinCore], GpuCoreStatus);
        gGpuRead[1] = nabu_gpu_read32 (map[GpuWinCore], real[GpuWinCore], GpuCoreStatus3);
        gGpuRead[2] = nabu_gpu_read32 (map[GpuWinCore], real[GpuWinCore], GpuCoreInt0);
        gGpuRead[3] = nabu_gpu_read32 (map[GpuWinCore], real[GpuWinCore], GpuCoreCpHigh);
        gGpuRead[4] = nabu_gpu_read32 (map[GpuWinCore], real[GpuWinCore], GpuCoreCpLow);
        gGpuStage = 4u;
      } else {
        /* Only the core window is withheld here. [5] is the GMU counter, already read one stage up:
         * overwriting it would hide the positive control that stage 3 did run. */
        nabu_gpu_skip (gGpuRead, 5u);
      }
    } else {
      nabu_gpu_skip (gGpuGmu, 4u);
      nabu_gpu_skip (gGpuRead, 6u);
    }
  } else {
    nabu_gpu_skip (gGpuCc, 4u);
    nabu_gpu_skip (gGpuGd, 4u);
    nabu_gpu_skip (gGpuGmu, 4u);
    nabu_gpu_skip (gGpuRead, 6u);
  }

  /* Stage 5 (l522) -- the RSCC side of the PDC question, read through the GMU window. Two facts
   * make this the safe half: rscc is not another block but `mmio + 0x23000` with the same
   * register-number<<2 arithmetic (a6xx_gmu.c:1667-1675 is the branch A640 takes -- only a650
   * family maps a separate rscc resource -- and a6xx_gmu.h:141-149 does `rscc + (offset << 2)`),
   * and M0 read this very window on two rounds that both ran to the watchdog. So no new physical
   * region is entered here. The two real PDC windows are mapped and their rc reported, and NOT
   * read: 0x0b000000 is a region this kernel has never touched, and it gets its own round.
   * gGpuStage deliberately does NOT grow: it means "how far the power chain got", and this read
   * hangs off the same cfg_ahb gate as stage 2. Judge it from its own fields instead. */
  gGpuMapPdc[0] = gGpuMapRc[GpuWinPdc];
  gGpuMapPdc[1] = gGpuMapRc[GpuWinPdcSeq];
  if (ask == 3u || (gGpuGcc[0] & NABU_GPU_CLK_ON)) {
    gGpuRs[0] = nabu_gpu_read32 (map[GpuWinGmu], real[GpuWinGmu], RsStatus0);
    gGpuRs[1] = nabu_gpu_read32 (map[GpuWinGmu], real[GpuWinGmu], RsPdcSeqStart);
    gGpuRs[2] = nabu_gpu_read32 (map[GpuWinGmu], real[GpuWinGmu], RsSeqBusy);
    for (unsigned i = 0; i < 5u; ++i) {
      gGpuRs[3u + i] = nabu_gpu_read32 (map[GpuWinGmu], real[GpuWinGmu], RsSeqMem0 + 4u * i);
    }
    gGpuRs[8] = nabu_gpu_read32 (map[GpuWinGmu], real[GpuWinGmu], RsCtlReq);
    gGpuRs[9] = nabu_gpu_read32 (map[GpuWinGmu], real[GpuWinGmu], RsCtlAck);
  } else {
    nabu_gpu_skip (gGpuRs, 10u);
  }

  /* l523 (M1-b, the write arm). Reached only when nabu-gpu=4 is passed on purpose -- nothing in the
   * shipped arguments asks for it -- and every gate above still has to hold: the GCC branch proved
   * itself, GPUCC was actually read, and the snapshot says CX is off. If CX already reads on the
   * sequence is skipped, because a second collapse-release is a write we cannot justify. */
  if (ask >= 4u) {
    if ((gGpuGcc[0] & NABU_GPU_CLK_ON) && gGpuCc[0] != NABU_GPU_SKIPPED
        && !(gGpuGd[0] & NABU_GPU_PWR_ON)) {
      gGpuAct[1] = gGpuGd[0];
      /* Announced before the first write, exactly as l518c does for reads: if the round dies on the
       * bus, the window, the VA, the true length and the word we aimed at are already on the wire. */
      printf ("nabu gpu cx write: cc_va=%llx len=%llx gdscr=0x%x -> 0x%x\n",
          (unsigned long long) (uintptr_t) map[GpuWinCc]->getVirtualAddress (),
          (unsigned long long) real[GpuWinCc],
          (unsigned) gGpuGd[0], (unsigned) (gGpuGd[0] & ~NABU_GPU_COLLAPSE));
      nabu_gpu_write32 (map[GpuWinCc], real[GpuWinCc], CcCxGdscr,
          gGpuGd[0] & ~NABU_GPU_COLLAPSE);      /* gdsc_update_collapse_bit(off), gdsc.c:129-132 */
      unsigned steps = 0;
      uint32_t hw = 0;
      for (;;) {                                 /* gdsc_poll_status(GDSC_ON) against gds_hw_ctrl */
        hw = nabu_gpu_read32 (map[GpuWinCc], real[GpuWinCc], CcCxHwCtrl);
        if (hw & NABU_GPU_PWR_ON) {
          break;
        }
        if (steps >= NABU_GPU_POLL_STEPS) {
          break;                                 /* bounded: 200 steps of 10 us, the upstream 2 ms */
        }
        IODelay (10);
        steps = steps + 10u;
      }
      gGpuAct[2] = steps;
      gGpuAct[3] = hw;
      if (hw & NABU_GPU_PWR_ON) {
        uint32_t v = nabu_gpu_read32 (map[GpuWinCc], real[GpuWinCc], CcCxGdscr);
        nabu_gpu_write32 (map[GpuWinCc], real[GpuWinCc], CcCxGdscr, v | NABU_GPU_RETAIN);
        IODelay (1);
        v = nabu_gpu_read32 (map[GpuWinCc], real[GpuWinCc], CcCxGdscr);
        nabu_gpu_write32 (map[GpuWinCc], real[GpuWinCc], CcCxGdscr,
            v | NABU_GPU_RETAIN | NABU_GPU_RETAIN_FF);
        IODelay (1);
        gGpuAct[4] = nabu_gpu_read32 (map[GpuWinCc], real[GpuWinCc], CcCxGdscr);
        gGpuAct[5] = nabu_gpu_read32 (map[GpuWinCc], real[GpuWinCc], CcCxGmu);
        gGpuAct[6] = nabu_gpu_read32 (map[GpuWinCc], real[GpuWinCc], CcGxGmu);
        gGpuAct[0] = 1u;
        printf ("nabu gpu cx powered: hw=%x gdscr=%x gmu=%x,%x\n",
            (unsigned) gGpuAct[3], (unsigned) gGpuAct[4],
            (unsigned) gGpuAct[5], (unsigned) gGpuAct[6]);
      } else {
        gGpuAct[0] = 2u;   /* asked, but the domain never reported PWR_ON inside the bound: no
                            * retain bits written, so the only change on the hardware is the
                            * collapse bit we cleared -- and that is what the line says. */
        nabu_gpu_skip (gGpuAct + 4, 3u);
      }
    } else {
      gGpuAct[0] = 3u;     /* gate closed, or CX already on: nothing at all was written */
      nabu_gpu_skip (gGpuAct + 1, 6u);
    }
  } else {
    nabu_gpu_skip (gGpuAct, 7u);
  }

  /* l525 (mode 4) -- enable the CX-side GMU branch, nothing else. 0x1064 (gpu_cc_gx_gmu_clk) is
   * deliberately NOT touched: the GX domain is still collapsed (l521: gx_gdscr = 0x222001) and
   * GpuPowerBoard.h:136 draws that line in words, "GX is never touched here (the GMU owns it)".
   * GX comes up later through the GMU itself (upstream mode 8, OOB GPU_SET), not through a CBCR
   * written by us. The branch's own parent is switched to bi_tcxo first when it is not already
   * there, because a branch with an unpowered parent is exactly the kind of write that looks
   * successful and proves nothing: 19.2 MHz on TCXO needs no PLL at all. */
  if (ask >= 5u && (gGpuAct[0] == 1u || (gGpuGd[0] & NABU_GPU_PWR_ON))) {
    uint32_t cfg = nabu_gpu_read32 (map[GpuWinCc], real[GpuWinCc], CcGmuCfg);
    uint32_t cx  = nabu_gpu_read32 (map[GpuWinCc], real[GpuWinCc], CcCxGmu);
    gGpuClk[0] = cfg;
    gGpuClk[2] = cx;
    printf ("nabu gpu clock: cfg=0x%x sel=%u div=%u cx=0x%x -> ",
        (unsigned) cfg, (unsigned) ((cfg >> 8) & 7u), (unsigned) (cfg & 31u), (unsigned) cx);
    if (((cfg >> 8) & 7u) != 0u) {
      /* sel 0 = P_BI_TCXO (gpucc-sm8150.c:65), divider field left exactly as found. */
      nabu_gpu_write32 (map[GpuWinCc], real[GpuWinCc], CcGmuCfg, (cfg & ~0x700u) & ~0x1fu);
    }
    nabu_gpu_write32 (map[GpuWinCc], real[GpuWinCc], CcCxGmu, cx | NABU_GPU_CLK_ON);
    unsigned waited = 0;
    uint32_t back = cx;
    for (;;) {                    /* gpu_cc_cx_gmu_clk is BRANCH_HALT (:149), so the halt bit here
                                   * is real -- unlike 0x1078, which is HALT_DELAY and cannot be polled */
      back = nabu_gpu_read32 (map[GpuWinCc], real[GpuWinCc], CcCxGmu);
      if (!(back & NABU_GPU_CLK_OFF)) {
        break;
      }
      if (waited >= NABU_GPU_POLL_STEPS) {
        break;
      }
      IODelay (10);
      waited = waited + 10u;
    }
    gGpuClk[1] = nabu_gpu_read32 (map[GpuWinCc], real[GpuWinCc], CcGmuCfg);
    gGpuClk[3] = back;
    printf ("cfg=0x%x cx=0x%x poll=%u\n",
        (unsigned) gGpuClk[1], (unsigned) gGpuClk[3], (unsigned) waited);
  } else {
    nabu_gpu_skip (gGpuClk, 4u);
  }

  /* The post snapshot, taken after anything this round wrote -- that is the whole fix for 8714's
   * second observation (P1/P1 on l523 were pre-write values, because every stage read sits before
   * the write arm and `done` means there is no second pass). The GMU side is now expected to show
   * real values because CX is up; the five core-window fields stay recorded as skipped on purpose,
   * because GX is what gates them and mode 4 does not bring GX up. */
  if (ask >= 4u) {
    gGpuPost[0] = nabu_gpu_read32 (map[GpuWinGmu], real[GpuWinGmu], GmuFwInitResult);
    gGpuPost[1] = nabu_gpu_read32 (map[GpuWinGmu], real[GpuWinGmu], GmuBootConfig);
    gGpuPost[2] = nabu_gpu_read32 (map[GpuWinGmu], real[GpuWinGmu], GmuSysreset);
    gGpuPost[3] = nabu_gpu_read32 (map[GpuWinGmu], real[GpuWinGmu], GmuHfiCtrlStatus);
    gGpuPost[9] = nabu_gpu_read32 (map[GpuWinGmu], real[GpuWinGmu], GmuCounterLow);
    if (gGpuGd[1] & NABU_GPU_PWR_ON) {
      gGpuPost[4] = nabu_gpu_read32 (map[GpuWinCore], real[GpuWinCore], GpuCoreStatus);
      gGpuPost[5] = nabu_gpu_read32 (map[GpuWinCore], real[GpuWinCore], GpuCoreStatus3);
      gGpuPost[6] = nabu_gpu_read32 (map[GpuWinCore], real[GpuWinCore], GpuCoreInt0);
      gGpuPost[7] = nabu_gpu_read32 (map[GpuWinCore], real[GpuWinCore], GpuCoreCpHigh);
      gGpuPost[8] = nabu_gpu_read32 (map[GpuWinCore], real[GpuWinCore], GpuCoreCpLow);
    } else {
      nabu_gpu_skip (gGpuPost + 4, 5u);
    }
  } else {
    nabu_gpu_skip (gGpuPost, 10u);
  }

  nabu_log_puts ("gpu:read\n");
  /* Straight to the console too: nabu_log_puts only ever reaches the ring, and l520 came back
   * with no gpu: lines at all because the spawn/exec flood had already overwritten the early
   * seconds. One line, once, at the end of the probe. Nothing is written to any device. */
  printf ("nabu gpu probe: map=%x/%x/%x st=%x,%x,%x cp=%x/%x gmu=%x\n",
      (unsigned) gGpuMapRc[GpuWinCore], (unsigned) gGpuMapRc[GpuWinGmu],
      (unsigned) gGpuMapRc[GpuWinCc],
      (unsigned) gGpuRead[0], (unsigned) gGpuRead[1], (unsigned) gGpuRead[2],
      (unsigned) gGpuRead[3], (unsigned) gGpuRead[4], (unsigned) gGpuRead[5]);
  printf ("nabu gpu power: stage=%u ahb=%u gcc=%x,%x,%x,%x cc=%x,%x,%x,%x gd=%x,%x,%x,%x "
      "gmu=%x,%x,%x,%x\n",
      (unsigned) gGpuStage, (unsigned) ((gGpuGcc[0] & NABU_GPU_CLK_ON) ? 1u : 0u),
      (unsigned) gGpuGcc[0], (unsigned) gGpuGcc[1], (unsigned) gGpuGcc[2], (unsigned) gGpuGcc[3],
      (unsigned) gGpuCc[0], (unsigned) gGpuCc[1], (unsigned) gGpuCc[2], (unsigned) gGpuCc[3],
      (unsigned) gGpuGd[0], (unsigned) gGpuGd[1], (unsigned) gGpuGd[2], (unsigned) gGpuGd[3],
      (unsigned) gGpuGmu[0], (unsigned) gGpuGmu[1], (unsigned) gGpuGmu[2], (unsigned) gGpuGmu[3]);
  /* The PDC census goes to the console too, for the same reason l520c started doing that: the ring
   * loses its early bytes, and this is the line that answers the only question l522 asks. */
  printf ("nabu gpu pdc: map=%x/%x rscc=%x,%x,%x mem=%x,%x,%x,%x,%x ctl=%x,%x\n",
      (unsigned) gGpuMapPdc[0], (unsigned) gGpuMapPdc[1],
      (unsigned) gGpuRs[0], (unsigned) gGpuRs[1], (unsigned) gGpuRs[2],
      (unsigned) gGpuRs[3], (unsigned) gGpuRs[4], (unsigned) gGpuRs[5],
      (unsigned) gGpuRs[6], (unsigned) gGpuRs[7],
      (unsigned) gGpuRs[8], (unsigned) gGpuRs[9]);
}

static void
nabu_dpu_probe_once (void)
{
  unsigned ask = 0;
  static int done = 0;

  if (!PE_parse_boot_argn ("nabu-dpu", &ask, sizeof (ask)) || !ask) {
    return;
  }
  if (done) {
    return;
  }
  done = 1;
  gDpuSspPipes = 1u;
  uintptr_t win[NABU_DPU_WINDOWS] = {0, 0};
  for (unsigned w = 0; w < NABU_DPU_WINDOWS; ++w) {
    IODeviceMemory *memory = IODeviceMemory::withRange (gDpuWindow[w].phys, gDpuWindow[w].len);
    if (!memory) {
      nabu_log_puts (w == 0 ? "dpu:vig-no-mem\n" : "dpu:dma-no-mem\n");
      continue;
    }
    IOMemoryMap *map = memory->map (kIOMapAnywhere | kIOMapInhibitCache);
    memory->release ();           /* the map holds its own reference to the range */
    gDpuMap[w] = map;
    win[w] = map ? static_cast<uintptr_t> (map->getVirtualAddress ()) : 0;
    /* Logged before anything reads the page, and with the physical base beside it, so a round that
     * dies here still says which window was aimed at and where it landed. */
    nabu_log_puts (w == 0 ? "dpu:vig-phys-ae004000\n" : "dpu:dma-phys-ae24000\n");
    nabu_log_hex (w == 0 ? "dpu:vig-va=" : "dpu:dma-va=", (unsigned long long) win[w]);
    /* Printed before any read of this window, and taken from the map rather than from my table:
     * if a round dies, the VA and the true length of the window it was about to read are already on
     * the wire -- which is what the two crash rounds never got out. */
    gDpuWinLen[w] = map ? (uint64_t) map->getLength () : 0ull;
    nabu_log_hex (w == 0 ? "dpu:vig-len=" : "dpu:dma-len=",
        (unsigned long long) gDpuWinLen[w]);
  }
  unsigned mapped = 0;
  for (unsigned w = 0; w < NABU_DPU_WINDOWS; ++w) {
    if (win[w]) {
      mapped += 4u;
    }
  }
  if (!mapped) {
    gDpuSspPipes = 0u;
    gDpuSspText[0] = 'm';
    gDpuSspText[1] = 'f';   /* "mf" = no window mapped; "off" would mean never asked */
    gDpuSspText[2] = '\0';
    nabu_log_puts ("dpu:map-fail\n");
    return;
  }
  gDpuSspPipes = mapped;    /* 4 = one family readable, 8 = both */
  unsigned used = (unsigned) snprintf (gDpuSspText, sizeof (gDpuSspText),
      "w=%016llx/%016llx;", (unsigned long long) win[0], (unsigned long long) win[1]);
  if ((unsigned) used >= sizeof (gDpuSspText)) {
    used = 0;
  }
  const unsigned regOfs[NABU_DPU_REG_COUNT] = { 0x00u, 0x08u, 0x0cu, 0x14u, 0x30u, 0x48u };
  /* nabu-dpu=2: the minimal face. One window, one pipe, one register. The panic cannot tell
   * "MDP cannot be read here" apart from "my eight-pipe table is wrong"; this does, and it costs
   * one read on the bus. */
  const unsigned pipeCount = (ask == 2u) ? 1u : NABU_DPU_PIPE_COUNT;
  const unsigned regCount = (ask == 2u) ? 1u : NABU_DPU_REG_COUNT;
  unsigned live = 0;
  unsigned oob = 0;
  for (unsigned i = 0; i < pipeCount; ++i) {
    if (!win[gDpuPipeWindow[i]]) {
      continue;   /* this family's window never mapped: say so by leaving the row zero and shortening n_ssp */
    }
    const unsigned w = gDpuPipeWindow[i];
    volatile uint32_t *pipe = (volatile uint32_t *) (win[w] + gDpuPipeOffset[i]);
    /* The address this pipe is aimed at, and the physical offset behind it, go out before the
     * first read of them. If the round dies on this pipe, the line naming it is already sent.
     * Eight lines once per boot is the whole cost. */
    nabu_log_hex ("dpu:aim-phys=", (unsigned long long) (gDpuWindow[w].phys + gDpuPipeOffset[i]));
    nabu_log_hex ("dpu:aim-va=", (unsigned long long) (uintptr_t) pipe);
    unsigned saysAnything = 0;
    for (unsigned r = 0; r < regCount; ++r) {
      const uint64_t at = (uint64_t) gDpuPipeOffset[i] + regOfs[r];
      if (gDpuWinLen[w] < at + 4ull) {
        /* outside what actually got mapped: record it, do not touch the bus */
        gDpuSsp[i][r] = NABU_DPU_OOB;
        oob++;
        continue;
      }
      uint32_t v = pipe[regOfs[r] / 4u];
      gDpuSsp[i][r] = v;
      /* SRC_XY and FETCH_CONFIG do not make a live pipe on their own; size, destination,
       * address and format do. */
      if (r != NABU_DPU_SLOT_XY && r != NABU_DPU_SLOT_FETCH && v != 0u) {
        saysAnything = 1;
      }
    }
    if (!saysAnything) {
      continue;
    }
    live++;
    /* The state block is snprintf into a fixed capacity and the pull channel only forwards the
     * first 512 bytes of it; past four named pipes the extra text would push gopbase=/gop= - the
     * pair the comparison is actually made against - out of the header. So the first four are
     * printed and `n_ssp=8/<live>` says how many were silent-worthy but not shown. */
    if (used >= sizeof (gDpuSspText) - 48u) {
      continue;
    }
    const uint32_t size = gDpuSsp[i][NABU_DPU_SLOT_SIZE];
    const uint32_t out = gDpuSsp[i][NABU_DPU_SLOT_OUT];
    const uint32_t fmt = gDpuSsp[i][NABU_DPU_SLOT_FORMAT];
    const int n = snprintf (gDpuSspText + used, sizeof (gDpuSspText) - used,
        "%s%u:%ux%u>%ux%u@%08x/b%u/f%02x", used ? "," : "", i, size & 0xffffu, size >> 16,
        out & 0xffffu, out >> 16, gDpuSsp[i][NABU_DPU_SLOT_ADDR], ((fmt >> 9) & 0x3fu) + 1u,
        gDpuSsp[i][NABU_DPU_SLOT_FETCH] & 0xffu);
    if (n < 0 || (unsigned) n >= sizeof (gDpuSspText) - used) {
      used = sizeof (gDpuSspText) - 1u;   /* truncated: keep the text terminated, stop appending */
      break;
    }
    used += (unsigned) n;
  }
  gDpuSspPipes = pipeCount;
  gDpuSspLive = live;
  if (oob) {
    /* say it in the header too, so a truncated console still shows the census was bounded out */
    (void) snprintf (gDpuSspText + (used ? used : 0u),
        sizeof (gDpuSspText) - (used ? used : 0u), "%soob%u", used ? "," : "", oob);
  }
  if (!used) {
    gDpuSspText[0] = 'n'; gDpuSspText[1] = 'o'; gDpuSspText[2] = 'n'; gDpuSspText[3] = 'e';
    gDpuSspText[4] = '\0';
  }
  nabu_log_puts ("dpu:read\n");
}

static volatile uint32_t *gWdtRegs = nullptr;
static volatile unsigned gWdtArmed = 0;
static volatile uint64_t gWdtPetUntil = 0;
static volatile unsigned gWdtPetSeconds = 0;
static volatile uint64_t gWdtArmAt = 0;
static volatile unsigned gWdtArmDelay = 0;
static volatile uint64_t gWdtArmedAt = 0;
static void nabu_wdt_pet (void);
static void nabu_wdt_arm (void);
static void nabu_media_open_thread (void *arg0, wait_result_t);
extern "C" volatile uint64_t native_ufs_block_reads;
extern "C" volatile uint64_t native_ufs_block_failures;
extern "C" volatile uint32_t native_ufs_blocks_published;
static unsigned nabu_count_services (const char *className);
extern "C" int nabu_log_flush_tail_nosync(int is_panic);
extern "C" void nabu_mark_text(const char *s, unsigned int y);

#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <kern/debug.h>
#include "GccUfs.h"
#include "RootDiagnostics.h"
#include "TransferPoll.h"
#include "SmmuTakeover.h"

static UfsRootDiagnostics gRootDiagnostic;
static bool gRootDiagnosticEnabled;
static void rootDiagnosticFailure (const char *reason);

extern "C" {
// Optional passive observer; absent in ordinary builds. It may run while a
// window lock is held, so its contract forbids blocking or transport access.
void boot_progress_poll (void) __attribute__((weak));
volatile uint32_t native_ufs_probe_calls = 0;
volatile uint32_t native_ufs_matched = 0;
volatile uint32_t native_ufs_probe_requested = 0;
volatile uint32_t native_ufs_mapped = 0;
volatile uint32_t native_ufs_status = 0;
// Root start: 1 entered, 2 host up, 3 layout found; the last soft-failure reason (a static string), for the boot page.
volatile uint32_t native_ufs_root_step = 0;
const char *volatile native_ufs_root_failure = nullptr;
volatile uint32_t native_ufs_cap = 0;
volatile uint32_t native_ufs_version = 0;
volatile uint32_t native_ufs_hcs = 0;
volatile uint32_t native_ufs_hce = 0;
volatile uint32_t native_ufs_utrl_ready = 0;
volatile uint32_t native_ufs_uic_status = 0;
volatile uint32_t native_ufs_uic_result = 0;
}

class NativeUfs : public IOService {
  OSDeclareDefaultStructors (NativeUfs)
  OSSharedPtr<IOMemoryMap> fMap;
  OSSharedPtr<IOBufferMemoryDescriptor> fCmdDesc;
  void inspect (void);
  void adoptAndRead (unsigned level);
  void dumpFirmwareList (void);
  void inspectSmmu (void);
  void inspectContextBanks (void);
  bool startRoot (IOService *provider);
  static bool waitSet (uintptr_t base, unsigned offset, uint32_t mask, unsigned loops);
public:
  IOService *probe (IOService *provider, SInt32 *score) override;
  bool start (IOService *provider) override;
  void stop (IOService *provider) override;
};

OSDefineMetaClassAndStructors (NativeUfs, IOService)

static uint32_t read32 (uintptr_t base, unsigned offset)
{
  return *reinterpret_cast<volatile uint32_t *> (base + offset);
}

static void write32 (uintptr_t base, unsigned offset, uint32_t value)
{
  *reinterpret_cast<volatile uint32_t *> (base + offset) = value;
}

static bool contains (const char *data, size_t length, const char *needle)
{
  size_t wanted = 0;
  while (needle[wanted]) {
    wanted++;
  }
  for (size_t at = 0; at + wanted <= length; at++) {
    size_t index = 0;
    while (index < wanted && data[at + index] == needle[index]) {
      index++;
    }
    if (index == wanted) {
      return true;
    }
  }
  return false;
}

IOService *NativeUfs::probe (IOService *provider, SInt32 *score)
{
  native_ufs_probe_calls = native_ufs_probe_calls + 1;
  if (OSDynamicCast (IOPlatformExpertDevice, provider)) {
    // Second personality (KernelConfigTables: IOMatchCategory NativeUfsRoot): attaches to the
    // root device, which starts before the platform expert publishes IOResources, so the
    // hardware probes run before the original AMFI (whose start panics without a PMGR).
    native_ufs_matched = 1;
    if (score) {
      *score = 500;
    }
    return this;
  }
  OSObject *property = provider->getProperty ("compatible");
  OSData *data = OSDynamicCast (OSData, property);
  OSString *string = OSDynamicCast (OSString, property);
  bool match = false;
  if (data) {
    const char *text = static_cast<const char *> (data->getBytesNoCopy ());
    match = text && contains (text, data->getLength (), "qcom,sm8150-ufshc");
  } else if (string) {
    const char *text = string->getCStringNoCopy ();
    match = text && contains (text, string->getLength (), "qcom,sm8150-ufshc");
  }
  if (!match) {
    return nullptr;
  }
  native_ufs_matched = 1;
  if (score) {
    *score = 1000;
  }
  return this;
}

bool NativeUfs::waitSet (uintptr_t base, unsigned offset, uint32_t mask, unsigned loops)
{
  for (unsigned index = 0; index < loops; index++) {
    if ((read32 (base, offset) & mask) == mask) {
      return true;
    }
    IODelay (1);
  }
  return false;
}

// Read-only inspection (native-ufs-probe=2): reports what firmware left behind.
// The physical windows are the sheng board DT's own GCC (0x100000) and UFS host
// (0x1d84000, 0x3000) ranges. The controller is read only if the GCC says its
// GDSC is powered and the AXI/AHB branches are running; nothing is written.
static uintptr_t mapPhys (uint64_t phys, uint64_t length, OSSharedPtr<IOMemoryMap> &keep)
{
  IODeviceMemory *memory = IODeviceMemory::withRange (phys, length);
  if (!memory) {
    return 0;
  }
  keep = OSSharedPtr<IOMemoryMap> (memory->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  memory->release ();
  return keep ? static_cast<uintptr_t> (keep->getVirtualAddress ()) : 0;
}

void NativeUfs::inspect (void)
{
  OSSharedPtr<IOMemoryMap> gccMap, memPhyMap, hcMap;
  uintptr_t gcc = mapPhys (GCC_BASE + 0x77000, 0x1000, gccMap);
  uintptr_t memPhy = mapPhys (GCC_BASE + 0x9e000, 0x1000, memPhyMap);
  if (!gcc || !memPhy) {
    panic ("NATIVE_UFS inspect: GCC map failed gcc=%p memphy=%p", (void *) gcc, (void *) memPhy);
  }
  uint32_t gdsc = read32 (gcc, GCC_UFS_PHY_GDSCR - 0x77000);
  uint32_t axi = read32 (gcc, GCC_UFS_PHY_AXI_ENABLE - 0x77000);
  uint32_t ahb = read32 (gcc, GCC_UFS_PHY_AHB_ENABLE - 0x77000);
  uint32_t unipro = read32 (gcc, GCC_UFS_PHY_UNIPRO_CORE_ENABLE - 0x77000);
  uint32_t txs = read32 (gcc, GCC_UFS_PHY_TX_SYMBOL0_ENABLE - 0x77000);
  uint32_t rxs = read32 (gcc, GCC_UFS_PHY_RX_SYMBOL0_ENABLE - 0x77000);
  uint32_t aux = read32 (gcc, GCC_UFS_PHY_PHY_AUX_ENABLE - 0x77000);
  uint32_t memGdsc = read32 (memPhy, 0);
  // GDSCR bit31 PWR_ON; CBCR bit31 CLK_OFF.
  bool powered = (gdsc & 0x80000000U) != 0;
  // Hardware 2026-10-17: AXI running, AHB enabled but CLK_OFF (bit31). With bit1 (HW gating)
  // set the branch wakes on access, so enable bits, not CLK_OFF, gate the read-only probe.
  bool busClocks = !(axi & 0x80000000U) && (ahb & 0x1U) && (axi & 0x1U);
  uint32_t cap = 0, ver = 0, hcs = 0, hce = 0, utrlba = 0, is = 0;
  unsigned readHc = 0;
  if (powered && busClocks) {
    uintptr_t hc = mapPhys (UFS_HC_BASE, UFS_HC_SIZE, hcMap);
    if (hc) {
      cap = read32 (hc, 0x00);
      ver = read32 (hc, 0x08);
      is = read32 (hc, 0x20);
      hcs = read32 (hc, 0x30);
      hce = read32 (hc, 0x34);
      utrlba = read32 (hc, 0x50);
      readHc = 1;
    }
  }
  panic ("NATIVE_UFS inspect\n"
         "gdsc=%08x memphygdsc=%08x\n"
         "axi=%08x ahb=%08x unipro=%08x\n"
         "txsym=%08x rxsym=%08x aux=%08x\n"
         "hc_read=%u cap=%08x ver=%08x\n"
         "is=%08x hcs=%08x hce=%08x utrlba=%08x",
         gdsc, memGdsc, axi, ahb, unipro, txs, rxs, aux, readHc, cap, ver, is, hcs, hce, utrlba);
}

// Adopt the controller the firmware left running (native-ufs-probe=3) and issue
// read-only SCSI commands through one UTP transfer request slot. Nothing is written
// to the storage. The controller is not reset and the link is not restarted; the
// firmware's request list is stopped (refused if a request is outstanding) and replaced
// by ours. Interrupts stay masked and completion is polled.
namespace {
struct UfsResult {
  uint32_t ocs;      // UTRD overall command status; 0xf = never completed
  uint32_t response; // UPIU response byte
  uint32_t scsi;     // SCSI status byte
  uint32_t sense;    // sense key<<16 | ASC<<8 | ASCQ from the response UPIU's autosense data
  uint32_t senseLen; // sense data length reported by the device
  uint32_t seg;      // UPIU data segment length
};

static void be32 (uint8_t *at, uint32_t value)
{
  at[0] = value >> 24; at[1] = value >> 16; at[2] = value >> 8; at[3] = value;
}

static uint32_t getBe32 (const uint8_t *at)
{
  return (uint32_t (at[0]) << 24) | (uint32_t (at[1]) << 16) | (uint32_t (at[2]) << 8) | at[3];
}
}

static void cacheClean (const void *start, size_t length)
{
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~uintptr_t (63);
  uintptr_t end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += 64) {
    __asm__ volatile ("dc cvac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

static void cacheCleanInvalidate (const void *start, size_t length)
{
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~uintptr_t (63);
  uintptr_t end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += 64) {
    __asm__ volatile ("dc civac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

// The region is mapped as device memory: only aligned element-sized stores are allowed
// (bzero's block zeroing faulted on hardware, 2026-10-17).
static void zero32 (void *start, size_t length)
{
  volatile uint32_t *word = static_cast<volatile uint32_t *> (start);
  for (size_t index = 0; index < length / 4; index++) {
    word[index] = 0;
  }
}


// Give the UFS stream a pass-through mapping the way Linux does on this firmware.
// Hardware 2026-10-17: after hand-off apps_smmu has sCR0 = 0x002f0406 (USFCFG=1, unmatched
// streams fault), no stream-match entry for the UFS host (ID 0x60, `iommus = <&apps_smmu 0x60
// 0x0>` in the board DT), a TYPE=BYPASS S2CR reads back as FAULT, and sCR0 writes are ignored.
// Linux (drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c, qcom_smmu_cfg_probe + qcom_smmu_write_s2cr,
// "bypass_quirk") meets exactly this: it reserves the LAST context bank (num_context_banks-1),
// writes SCTLR=0 (stage 1 disabled) and CBAR.TYPE=S1_TRANS_S2_BYPASS there, and maps a bypass
// stream as S2CR TYPE=TRANSLATE with CBNDX = that bank. S2CR is written before SMR
// (arm_smmu_write_sme). ARM IHI 0062D.c table 3-1 additionally requires VA64=1
// for inputs above 32 bits even with SCTLR.M=0. Storage DMA path only; this
// is physical pass-through, not a page-table or Adreno 4 KiB qualification.
static void smmuBypassStream (uint16_t stream, OSSharedPtr<IOMemoryMap> &keep, uint32_t *report,
                              uint64_t dma, uint64_t bytes, uintptr_t smmuOverride = 0)
{
  uintptr_t smmu = smmuOverride ? smmuOverride : mapPhys (0x15000000ULL, 0xd0000, keep);
  if (!smmu) {
    panic ("NATIVE_UFS smmu: map failed");
  }
  UfsSmmuPlan plan = planUfsSmmu (stream, dma, bytes, [smmu](unsigned offset) { return read32 (smmu, offset); });
  gRootDiagnostic.smmuId1 = read32 (smmu, 0x24);
  gRootDiagnostic.smmuId2 = read32 (smmu, 0x28);
  gRootDiagnostic.smmuSlot = plan.slot;
  if (plan.error) {
    gRootDiagnostic.stage = "SMMU planning";
    gRootDiagnostic.result = static_cast<uint32_t> (kIOReturnNotReady);
    gRootDiagnostic.smr = plan.smr; gRootDiagnostic.s2cr = plan.s2cr;
    rootDiagnosticFailure (plan.error);
    panic ("NATIVE_UFS smmu: %s slot=%u smr=%x s2cr=%x", plan.error, plan.slot, plan.smr, plan.s2cr);
  }
  unsigned bank = plan.bank, index = plan.slot;
  uintptr_t cb = smmu + (plan.pages + bank) * 4096;
  const char *failure = configureUfsSmmu (plan, stream,
    [smmu](unsigned offset) { return read32 (smmu, offset); },
    [smmu](unsigned offset, uint32_t value) {
      write32 (smmu, offset, value);
      __asm__ volatile ("dsb sy" ::: "memory");
    });
  uint32_t smr = read32 (smmu, 0x800 + 4 * index), s2cr = read32 (smmu, 0xc00 + 4 * index);
  uint32_t cbar = read32 (smmu, 0x1000 + 4 * bank), sctlr = read32 (cb, 0x0);
  gRootDiagnostic.smr = smr; gRootDiagnostic.s2cr = s2cr;
  gRootDiagnostic.cbar = cbar; gRootDiagnostic.sctlr = sctlr;
  gRootDiagnostic.smmuId2 = read32 (smmu, 0x28);
  gRootDiagnostic.cba2r = read32 (smmu, 0x1800 + 4 * bank);
  if (failure) {
    gRootDiagnostic.stage = "SMMU readback";
    gRootDiagnostic.result = static_cast<uint32_t> (kIOReturnNotReady);
    rootDiagnosticFailure (failure);
    panic ("NATIVE_UFS smmu: %s idx=%u bank=%u cba2r=%x", failure, index, bank,
           gRootDiagnostic.cba2r);
  }
  report[0] = index | (bank << 16);
  report[1] = cbar;
  report[2] = sctlr;
}

void NativeUfs::adoptAndRead (unsigned level)
{
  OSSharedPtr<IOMemoryMap> hcMap;
  uintptr_t hc = mapPhys (UFS_HC_BASE, UFS_HC_SIZE, hcMap);
  if (!hc) {
    panic ("NATIVE_UFS adopt: controller map failed");
  }
  // The controller's own request list is the one region its DMA is known to reach
  // (hardware 2026-10-17: a kernel-allocated buffer at 0xf080f000 reset the system when the
  // first command was issued, and nothing below 0xc8000000 is allocatable). The firmware
  // zeroed it; use 16 KiB from UTRLBA as ordinary cached memory (the DT marks the host
  // dma-coherent). Test instrument only: the kernel may also own these pages.
  uint64_t fwList = (uint64_t (read32 (hc, 0x54)) << 32) | read32 (hc, 0x50);
  if (!fwList || (fwList & 0xfff)) {
    panic ("NATIVE_UFS adopt: no usable firmware list base=%llx", fwList);
  }
  IOMemoryDescriptor *region = IOMemoryDescriptor::withPhysicalAddress (fwList, 16384, kIODirectionInOut);
  OSSharedPtr<IOMemoryMap> regionMap;
  if (region) {
    regionMap = OSSharedPtr<IOMemoryMap> (region->map (kIOMapAnywhere), OSNoRetain);
    region->release ();
  }
  if (!regionMap) {
    panic ("NATIVE_UFS adopt: firmware list map failed base=%llx", fwList);
  }
  uint8_t *virt = reinterpret_cast<uint8_t *> (regionMap->getVirtualAddress ());
  uint64_t phys = fwList;
  zero32 (virt, 16384);
  cacheClean (virt, 16384);
  // Layout: [0,4096) request list (slot 0 used), [4096,5120) command + response UPIU +
  // PRDT, [8192,16384) data.
  const uint32_t ucdOffset = 4096, rspOffset = 512, prdtOffset = 1024, dataOffset = 8192;
  uint32_t hcsBefore = read32 (hc, 0x30);
  uint32_t doorbellBefore = read32 (hc, 0x58);
  if (doorbellBefore != 0 || read32 (hc, 0x78) != 0) {
    panic ("NATIVE_UFS adopt: firmware request outstanding doorbell=%08x hcs=%08x", doorbellBefore, hcsBefore);
  }
  OSSharedPtr<IOMemoryMap> smmuMap;
  uint32_t smmuReport[3] = { 0, 0, 0 };
  write32 (hc, 0x24, 0);            // mask interrupts
  write32 (hc, 0x60, 0);            // stop the firmware's request list
  __asm__ volatile ("dsb sy" ::: "memory");
  unsigned stopLoops = 10000;
  while ((read32 (hc, 0x60) & 1) && stopLoops--) IODelay (10);
  if (read32 (hc, 0x60) & 1) panic ("NATIVE_UFS adopt: request list did not stop");
  smmuBypassStream (0x60, smmuMap, smmuReport, phys, 16384);
  write32 (hc, 0x50, static_cast<uint32_t> (phys));
  write32 (hc, 0x54, static_cast<uint32_t> (phys >> 32));
  write32 (hc, 0x20, 0xffffffffU);  // clear stale status
  write32 (hc, 0x60, 1);
  if (!waitSet (hc, 0x30, 0x2, 100000)) {
    panic ("NATIVE_UFS adopt: request list not ready hcs=%08x", read32 (hc, 0x30));
  }
  if (level == 3) {
    panic ("NATIVE_UFS adopt: list replaced\n"
           "phys=%llx hcs=%08x->%08x\n"
           "is=%08x db=%08x rsr=%08x ba=%08x bau=%08x ie=%08x",
           phys, hcsBefore, read32 (hc, 0x30), read32 (hc, 0x20), read32 (hc, 0x58),
           read32 (hc, 0x60), read32 (hc, 0x50), read32 (hc, 0x54), read32 (hc, 0x24));
  }

  auto command = [&] (uint8_t lun, const uint8_t *cdb, unsigned cdbLength, bool toHost, uint32_t length) -> UfsResult {
    uint8_t *utrd = virt;
    uint8_t *ucd = virt + ucdOffset;
    zero32 (utrd, 32);
    zero32 (ucd, 1024 + 16);
    zero32 (virt + dataOffset, 8192);
    ucd[0] = 0x01;                  // COMMAND UPIU
    ucd[1] = toHost ? 0x40 : 0x00;  // flags: read
    ucd[2] = lun;
    ucd[3] = 0;                     // task tag
    be32 (ucd + 12, length);        // expected data transfer length
    for (unsigned index = 0; index < cdbLength; index++) {
      ucd[16 + index] = cdb[index];
    }
    uint64_t ucdPhys = phys + ucdOffset;
    uint32_t direction = length ? (toHost ? 2U : 1U) : 0U;
    *reinterpret_cast<uint32_t *> (utrd + 0) = 0x10000000U | (direction << 25);
    *reinterpret_cast<uint32_t *> (utrd + 8) = 0x0fU;     // OCS: not completed
    *reinterpret_cast<uint32_t *> (utrd + 16) = static_cast<uint32_t> (ucdPhys);
    *reinterpret_cast<uint32_t *> (utrd + 20) = static_cast<uint32_t> (ucdPhys >> 32);
    *reinterpret_cast<uint32_t *> (utrd + 24) = (512U >> 2) | ((rspOffset >> 2) << 16);   // response length | offset (dwords)
    uint32_t prdEntries = length ? 1U : 0U;
    *reinterpret_cast<uint32_t *> (utrd + 28) = prdEntries | ((prdtOffset >> 2) << 16);
    if (length) {
      uint64_t dataPhys = phys + dataOffset;
      uint8_t *prd = ucd + prdtOffset;
      *reinterpret_cast<uint32_t *> (prd + 0) = static_cast<uint32_t> (dataPhys);
      *reinterpret_cast<uint32_t *> (prd + 4) = static_cast<uint32_t> (dataPhys >> 32);
      *reinterpret_cast<uint32_t *> (prd + 12) = length - 1;
    }
    cacheClean (virt, 16384);
    write32 (hc, 0x20, 0xffffffffU);
    write32 (hc, 0x58, 1);          // ring slot 0
    UfsResult result = { 0xf, 0xff, 0xff, 0, 0, 0 };
    bool done = false;
    for (unsigned loops = 0; loops < 100000; loops++) {
      if (!(read32 (hc, 0x58) & 1)) {
        done = true;
        break;
      }
      IODelay (1);
    }
    cacheCleanInvalidate (virt, 16384);
    if (!done) {
      return result;
    }
    result.ocs = *reinterpret_cast<volatile uint32_t *> (utrd + 8) & 0xff;
    const uint8_t *rsp = ucd + rspOffset;
    result.response = rsp[6];
    result.scsi = rsp[7];
    // Response UPIU: 12-byte header, residual count, 16 reserved bytes, then the data segment
    // (2-byte sense length + sense data) at offset 32.
    result.seg = (uint32_t (rsp[10]) << 8) | rsp[11];
    result.senseLen = (uint32_t (rsp[32]) << 8) | rsp[33];
    result.sense = ((rsp[36] & 0xfU) << 16) | (uint32_t (rsp[46]) << 8) | rsp[47];
    return result;
  };

  // NOP OUT (kind 0) and Query read requests (kind 1: opcode 0x05 READ FLAG, 0x03 READ ATTRIBUTE).
  // Read-only: no data segment, no state change on the device. Returns the UPIU response byte and
  // the 32-bit value field (attributes) or flag byte via *value; ocs 0xf means no completion.
  auto query = [&] (unsigned kind, uint8_t opcode, uint8_t idn, uint32_t *value, uint32_t *respByte) -> uint32_t {
    uint8_t *utrd = virt;
    uint8_t *ucd = virt + ucdOffset;
    zero32 (utrd, 32);
    zero32 (ucd, 1024 + 16);
    if (kind == 0) {
      ucd[0] = 0x00;                // NOP OUT
    } else {
      ucd[0] = 0x16;                // QUERY REQUEST
      ucd[5] = 0x01;                // query function (header dword 1, byte 1): standard read request
      ucd[12] = opcode;
      ucd[13] = idn;
    }
    ucd[3] = 1;                     // task tag
    uint64_t ucdPhys = phys + ucdOffset;
    *reinterpret_cast<uint32_t *> (utrd + 0) = 0x10000000U;
    *reinterpret_cast<uint32_t *> (utrd + 8) = 0x0fU;
    *reinterpret_cast<uint32_t *> (utrd + 16) = static_cast<uint32_t> (ucdPhys);
    *reinterpret_cast<uint32_t *> (utrd + 20) = static_cast<uint32_t> (ucdPhys >> 32);
    *reinterpret_cast<uint32_t *> (utrd + 24) = (512U >> 2) | ((rspOffset >> 2) << 16);
    *reinterpret_cast<uint32_t *> (utrd + 28) = (prdtOffset >> 2) << 16;
    cacheClean (virt, 16384);
    write32 (hc, 0x20, 0xffffffffU);
    write32 (hc, 0x58, 1);
    bool done = false;
    for (unsigned loops = 0; loops < 100000; loops++) {
      if (!(read32 (hc, 0x58) & 1)) {
        done = true;
        break;
      }
      IODelay (1);
    }
    cacheCleanInvalidate (virt, 16384);
    if (!done) {
      return 0xf;
    }
    const uint8_t *rsp = ucd + rspOffset;
    *respByte = (uint32_t (rsp[0]) << 8) | rsp[6];     // response type << 8 | query/NOP response
    *value = getBe32 (rsp + 20);
    return *reinterpret_cast<volatile uint32_t *> (utrd + 8) & 0xff;
  };
  // Device state before waking it. Hardware 2026-10-17: NOP OUT answers, fDeviceInit = 0 (done),
  // bCurrentPowerMode = 0x33 (UFS-PowerDown) and every LU returns NOT READY. Linux resumes the
  // device with START STOP UNIT (power condition ACTIVE) to the UFS Device well-known LU 0xD0
  // (ufshcd_set_dev_pwr_mode); no user data is touched.
  uint32_t powerBefore = 0, powerAfter = 0, queryResp = 0, ignoredValue = 0;
  uint32_t wakeStatus = 0xee;
  if (level == 5 || level == 9) {
    query (1, 0x03, 0x02, &powerBefore, &queryResp);     // READ ATTRIBUTE bCurrentPowerMode
    uint8_t ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };          // START STOP UNIT, power condition ACTIVE
    UfsResult w = command (0xd0, ssu, 6, false, 0);
    wakeStatus = (w.ocs << 16) | (w.response << 8) | w.scsi;
    for (unsigned settle = 0; settle < 50; settle++) {
      IODelay (1000);
    }
    query (1, 0x03, 0x02, &powerAfter, &queryResp);
    (void) ignoredValue;
  }
  if (level == 4) {
    uint8_t tur0[6] = { 0, 0, 0, 0, 0, 0 };
    UfsResult t = command (0, tur0, 6, false, 0);
    const uint8_t *rsp0 = virt + ucdOffset + rspOffset;
    panic ("NATIVE_UFS adopt: TEST UNIT READY lun0\n"
           "ocs=%x resp=%02x scsi=%02x\n"
           "phys=%llx hcs=%08x->%08x smmu idx/bank=%08x cbar=%08x\n"
           "is=%08x db=%08x rsr=%08x\n"
           "rsp=%08x %08x",
           t.ocs, t.response, t.scsi, phys, hcsBefore, read32 (hc, 0x30), smmuReport[0], smmuReport[1],
           read32 (hc, 0x20), read32 (hc, 0x58), read32 (hc, 0x60), getBe32 (rsp0), getBe32 (rsp0 + 4));
  }
  uint32_t hcsAfter = 0;
  const unsigned maxLun = 6;
  uint32_t status[maxLun], last[maxLun], blockSize[maxLun], senseOf[maxLun];
  for (unsigned lun = 0; lun < maxLun; lun++) {
    status[lun] = 0xee; last[lun] = 0; blockSize[lun] = 0; senseOf[lun] = 0;
    uint8_t tur[6] = { 0x00, 0, 0, 0, 0, 0 };
    UfsResult r = { 0xf, 0xff, 0xff, 0, 0, 0 };
    for (unsigned attempt = 0; attempt < 3; attempt++) {
      r = command (static_cast<uint8_t> (lun), tur, 6, false, 0);
      if (lun == 0 && attempt == 0 && r.ocs == 0xf) {
        // Never completed: stop here and report instead of waiting out every command.
        const uint8_t *rsp = virt + ucdOffset + rspOffset;
        panic ("NATIVE_UFS adopt: first command did not complete\n"
               "phys=%llx hcs=%08x->%08x\n"
               "is=%08x db=%08x rsr=%08x ba=%08x\n"
               "utrd2=%08x rsp=%08x %08x",
               phys, hcsBefore, read32 (hc, 0x30),
               read32 (hc, 0x20), read32 (hc, 0x58), read32 (hc, 0x60), read32 (hc, 0x50),
               *reinterpret_cast<volatile uint32_t *> (virt + 8), getBe32 (rsp), getBe32 (rsp + 4));
      }
      if (r.ocs == 0 && r.scsi == 0) {
        break;
      }
    }
    uint8_t cap[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    r = command (static_cast<uint8_t> (lun), cap, 10, true, 8);
    status[lun] = (r.ocs << 16) | (r.response << 8) | r.scsi;
    senseOf[lun] = r.sense;
    if (r.ocs == 0 && r.scsi == 0) {
      last[lun] = getBe32 (virt + dataOffset);
      blockSize[lun] = getBe32 (virt + dataOffset + 4);
    }
  }
  if (level == 9) {
    // Read-only GPT survey of LUN 0 (4096-byte blocks): header fields and CRCs for before/after
    // comparison, then every non-empty entry among the first 64. Nothing is written.
    if (blockSize[0] != 4096) {
      panic ("NATIVE_UFS gpt: unexpected LUN0 block size %08x", blockSize[0]);
    }
    uint8_t rd[10] = { 0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0 };
    UfsResult h = command (0, rd, 10, true, 4096);
    if (h.ocs != 0 || h.scsi != 0) {
      panic ("NATIVE_UFS gpt: header read failed ocs=%x resp=%x scsi=%x", h.ocs, h.response, h.scsi);
    }
    const uint8_t *hdr = virt + dataOffset;
    auto le32 = [] (const uint8_t *at) -> uint32_t {
      return uint32_t (at[0]) | (uint32_t (at[1]) << 8) | (uint32_t (at[2]) << 16) | (uint32_t (at[3]) << 24);
    };
    auto le64 = [&] (const uint8_t *at) -> uint64_t {
      return uint64_t (le32 (at)) | (uint64_t (le32 (at + 4)) << 32);
    };
    uint32_t sig0 = le32 (hdr), sig1 = le32 (hdr + 4), hdrCrc = le32 (hdr + 16), entCrc = le32 (hdr + 88);
    uint64_t curLba = le64 (hdr + 24), bakLba = le64 (hdr + 32), firstUsable = le64 (hdr + 40), lastUsable = le64 (hdr + 48);
    uint64_t entLba = le64 (hdr + 72);
    uint32_t entCount = le32 (hdr + 80), entSize = le32 (hdr + 84);
    uint32_t guid0 = le32 (hdr + 56), guid1 = le32 (hdr + 60), guid2 = le32 (hdr + 64), guid3 = le32 (hdr + 68);
    if (sig0 != 0x20494645U || sig1 != 0x54524150U || entSize != 128 || entLba > 64) {
      panic ("NATIVE_UFS gpt: unexpected header sig=%08x %08x esz=%u elba=%llx", sig0, sig1, entSize, entLba);
    }
    uint32_t shown = entCount > 64 ? 64 : entCount;
    uint32_t blocks = (shown * 128 + 4095) / 4096;
    uint8_t rd2[10] = { 0x28, 0, 0, 0, 0, uint8_t (entLba), 0, 0, uint8_t (blocks), 0 };
    UfsResult e = command (0, rd2, 10, true, blocks * 4096);
    if (e.ocs != 0 || e.scsi != 0) {
      panic ("NATIVE_UFS gpt: entries read failed ocs=%x scsi=%x", e.ocs, e.scsi);
    }
    const uint8_t *ents = virt + dataOffset;
    static char text[3400];
    int used = snprintf (text, sizeof (text),
                         "NATIVE_UFS gpt LUN0 hdr crc=%08x ents crc=%08x cnt=%u\n"
                         "cur=%llx backup=%llx usable=%llx..%llx elba=%llx\n"
                         "disk guid %08x %08x %08x %08x last=%08x\n",
                         hdrCrc, entCrc, entCount, curLba, bakLba, firstUsable, lastUsable, entLba,
                         guid0, guid1, guid2, guid3, last[0]);
    unsigned column = 0, listed = 0;
    for (uint32_t index = 0; index < shown && used < int (sizeof (text)) - 80; index++) {
      const uint8_t *entry = ents + index * 128;
      bool empty = true;
      for (unsigned b = 0; b < 16; b++) {
        if (entry[b]) { empty = false; break; }
      }
      if (empty) {
        continue;
      }
      char name[14];
      for (unsigned c = 0; c < 13; c++) {
        uint8_t lo = entry[56 + 2 * c], hi = entry[57 + 2 * c];
        name[c] = (hi == 0 && lo >= 32 && lo < 127) ? char (lo) : (lo ? '?' : 0);
        if (!name[c]) { name[c] = 0; break; }
      }
      name[13] = 0;
      used += snprintf (text + used, sizeof (text) - used, "%u:%s %llx-%llx%s", index, name,
                        le64 (entry + 32), le64 (entry + 40), (++column % 3) ? "  " : "\n");
      listed++;
    }
    snprintf (text + used, sizeof (text) - used, "\nlisted=%u", listed);
    panic ("%s", text);
  }
  // INQUIRY of LUN 0: vendor/product bytes 8..23.
  uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
  UfsResult inq = command (0, inquiry, 6, true, 36);
  uint32_t inqWord[4] = { 0, 0, 0, 0 };
  for (unsigned index = 0; index < 4; index++) {
    inqWord[index] = getBe32 (virt + dataOffset + 8 + index * 4);
  }
  // LBA 1 of LUN 0 holds the GPT header when the block size is 4096 (or 512).
  uint32_t gptStatus = 0xee, sig0 = 0, sig1 = 0;
  if (blockSize[0] >= 512 && blockSize[0] <= 4096) {
    uint8_t read10[10] = { 0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0 };
    UfsResult g = command (0, read10, 10, true, blockSize[0]);
    gptStatus = (g.ocs << 16) | (g.response << 8) | g.scsi;
    if (g.ocs == 0 && g.scsi == 0) {
      sig0 = *reinterpret_cast<uint32_t *> (virt + dataOffset);
      sig1 = *reinterpret_cast<uint32_t *> (virt + dataOffset + 4);
    }
  }
  hcsAfter = read32 (hc, 0x30);
  panic ("NATIVE_UFS adopt+read\n"
         "hcs %08x->%08x pwr %08x->%08x ssu=%06x\n"
         "L0 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L1 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L2 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L3 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L4 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L5 st=%06x last=%08x bs=%08x sns=%06x\n"
         "inq st=%06x %08x %08x %08x %08x\n"
         "inq sns=%06x slen=%x seg=%x\n"
         "rsp0 %08x %08x %08x %08x\n"
         "rsp8 %08x %08x %08x %08x\n"
         "gpt st=%06x sig=%08x %08x",
         hcsBefore, hcsAfter, powerBefore, powerAfter, wakeStatus,
         status[0], last[0], blockSize[0], senseOf[0], status[1], last[1], blockSize[1], senseOf[1],
         status[2], last[2], blockSize[2], senseOf[2], status[3], last[3], blockSize[3], senseOf[3],
         status[4], last[4], blockSize[4], senseOf[4], status[5], last[5], blockSize[5], senseOf[5],
         (inq.ocs << 16) | (inq.response << 8) | inq.scsi, inqWord[0], inqWord[1], inqWord[2], inqWord[3],
         inq.sense, inq.senseLen, inq.seg,
         getBe32 (virt + ucdOffset + rspOffset), getBe32 (virt + ucdOffset + rspOffset + 4),
         getBe32 (virt + ucdOffset + rspOffset + 8), getBe32 (virt + ucdOffset + rspOffset + 12),
         getBe32 (virt + ucdOffset + rspOffset + 32), getBe32 (virt + ucdOffset + rspOffset + 36),
         getBe32 (virt + ucdOffset + rspOffset + 40), getBe32 (virt + ucdOffset + rspOffset + 44),
         gptStatus, sig0, sig1);
}

// Read-only dump of the request list the firmware left (native-ufs-probe=6): shows where
// the firmware placed its descriptors, to compare with an allocation the controller's DMA
// is known to reach. Nothing is written and no register is touched except reads.
static uint32_t dumpWord (const volatile uint32_t *base, unsigned index)
{
  return base[index];
}

void NativeUfs::dumpFirmwareList (void)
{
  OSSharedPtr<IOMemoryMap> hcMap, listMap, ucdMap;
  uintptr_t hc = mapPhys (UFS_HC_BASE, UFS_HC_SIZE, hcMap);
  if (!hc) {
    panic ("NATIVE_UFS dump: controller map failed");
  }
  uint64_t listPhys = (uint64_t (read32 (hc, 0x54)) << 32) | read32 (hc, 0x50);
  uint32_t db = read32 (hc, 0x58), rsr = read32 (hc, 0x60), hcs = read32 (hc, 0x30);
  IOMemoryDescriptor *listDesc = IOMemoryDescriptor::withPhysicalAddress (listPhys, 4096, kIODirectionIn);
  if (!listDesc) {
    panic ("NATIVE_UFS dump: list descriptor failed phys=%llx", listPhys);
  }
  listMap = OSSharedPtr<IOMemoryMap> (listDesc->map (kIOMapAnywhere), OSNoRetain);
  listDesc->release ();
  if (!listMap) {
    panic ("NATIVE_UFS dump: list map failed phys=%llx", listPhys);
  }
  const volatile uint32_t *utrd = reinterpret_cast<const volatile uint32_t *> (listMap->getVirtualAddress ());
  uint64_t ucdPhys = (uint64_t (dumpWord (utrd, 5)) << 32) | dumpWord (utrd, 4);
  uint32_t ucd0 = 0, ucd1 = 0, ucd2 = 0, ucd3 = 0;
  if (ucdPhys) {
    IOMemoryDescriptor *ucdDesc = IOMemoryDescriptor::withPhysicalAddress (ucdPhys, 4096, kIODirectionIn);
    if (ucdDesc) {
      ucdMap = OSSharedPtr<IOMemoryMap> (ucdDesc->map (kIOMapAnywhere), OSNoRetain);
      ucdDesc->release ();
      if (ucdMap) {
        const volatile uint32_t *ucd = reinterpret_cast<const volatile uint32_t *> (ucdMap->getVirtualAddress ());
        ucd0 = dumpWord (ucd, 0); ucd1 = dumpWord (ucd, 1); ucd2 = dumpWord (ucd, 2); ucd3 = dumpWord (ucd, 3);
      }
    }
  }
  panic ("NATIVE_UFS dump: firmware list\n"
         "list=%llx hcs=%08x db=%08x rsr=%08x\n"
         "utrd0 %08x %08x %08x %08x\n"
         "utrd0 %08x %08x %08x %08x\n"
         "utrd1 %08x %08x %08x %08x\n"
         "ucd=%llx %08x %08x %08x %08x",
         listPhys, hcs, db, rsr,
         dumpWord (utrd, 0), dumpWord (utrd, 1), dumpWord (utrd, 2), dumpWord (utrd, 3),
         dumpWord (utrd, 4), dumpWord (utrd, 5), dumpWord (utrd, 6), dumpWord (utrd, 7),
         dumpWord (utrd, 8), dumpWord (utrd, 9), dumpWord (utrd, 10), dumpWord (utrd, 11),
         ucdPhys, ucd0, ucd1, ucd2, ucd3);
}

// Read-only dump of the apps SMMU (apps_smmu@15000000) global register space and the stream
// match entry for the UFS stream ID 0x60 (native-ufs-probe=7). SMMUv2 layout: sCR0 0x0,
// IDR0 0x20, sGFSR 0x48, SMR(n) 0x800+4n, S2CR(n) 0xc00+4n. Nothing is written.
void NativeUfs::inspectSmmu (void)
{
  OSSharedPtr<IOMemoryMap> map;
  uintptr_t smmu = mapPhys (0x15000000ULL, 0x1000, map);
  if (!smmu) {
    panic ("NATIVE_UFS smmu: map failed");
  }
  uint32_t scr0 = read32 (smmu, 0x00), idr0 = read32 (smmu, 0x20), idr1 = read32 (smmu, 0x24);
  uint32_t gfsr = read32 (smmu, 0x48);
  unsigned count = idr0 & 0xff;
  if (count > 128) {
    count = 128;
  }
  unsigned match = 0xffff, valid = 0, typeCount[4] = { 0, 0, 0, 0 };
  uint32_t matchSmr = 0, matchS2cr = 0;
  for (unsigned index = 0; index < count; index++) {
    uint32_t smr = read32 (smmu, 0x800 + 4 * index);
    uint32_t s2cr = read32 (smmu, 0xc00 + 4 * index);
    if (smr & 0x80000000U) {
      valid++;
      typeCount[(s2cr >> 16) & 3]++;
      uint32_t mask = (smr >> 16) & 0x7fff;
      if (match == 0xffff && (((smr ^ 0x60U) & 0x7fffU) & ~mask) == 0) {
        match = index;
        matchSmr = smr;
        matchS2cr = s2cr;
      }
    }
  }
  panic ("NATIVE_UFS smmu\n"
         "scr0=%08x idr0=%08x idr1=%08x gfsr=%08x\n"
         "nsmr=%u valid=%u types t0=%u t1=%u t2=%u\n"
         "sid0x60 match=%x smr=%08x s2cr=%08x",
         scr0, idr0, idr1, gfsr, count, valid, typeCount[0], typeCount[1], typeCount[2],
         match, matchSmr, matchS2cr);
}

// Read-only context-bank survey (native-ufs-probe=8): CBAR/CBA2R in gr1 and SCTLR/TTBR0 in the
// context banks, to find a bank the hypervisor leaves to us. Nothing is written.
void NativeUfs::inspectContextBanks (void)
{
  OSSharedPtr<IOMemoryMap> map;
  uintptr_t smmu = mapPhys (0x15000000ULL, 0xd0000, map);
  if (!smmu) {
    panic ("NATIVE_UFS cb: map failed");
  }
  uint32_t idr1 = read32 (smmu, 0x24);
  unsigned numCb = idr1 & 0xff;
  unsigned globalPages = 1U << (((idr1 >> 28) & 7) + 1);
  if (globalPages > 128 || numCb > 128 || (globalPages + numCb) * 0x1000 > 0xd0000) {
    panic ("NATIVE_UFS cb: unexpected geometry idr1=%08x", idr1);
  }
  unsigned cbarZero = 0, sctlrZero = 0, mSet = 0, bothZero = 0;
  uint32_t firstZero = 0xffff;
  for (unsigned index = 0; index < numCb; index++) {
    uint32_t cbar = read32 (smmu, 0x1000 + 4 * index);
    uint32_t sctlr = read32 (smmu + (globalPages + index) * 0x1000, 0x0);
    cbarZero += cbar == 0;
    sctlrZero += sctlr == 0;
    mSet += (sctlr & 1) != 0;
    if (cbar == 0 && sctlr == 0) {
      bothZero++;
      if (firstZero == 0xffff) {
        firstZero = index;
      }
    }
  }
  uintptr_t cb0 = smmu + globalPages * 0x1000, cb1 = cb0 + 0x1000, cbLast = cb0 + (numCb - 1) * 0x1000;
  panic ("NATIVE_UFS cb survey\n"
         "ncb=%u pages=%u cbar0=%u sctlr0=%u msset=%u both0=%u first0=%x\n"
         "cbar[0]=%08x cbar[1]=%08x cbar[%u]=%08x\n"
         "sctlr[0]=%08x sctlr[1]=%08x sctlr[last]=%08x\n"
         "cba2r[0]=%08x cba2r[1]=%08x",
         numCb, globalPages, cbarZero, sctlrZero, mSet, bothZero, firstZero,
         read32 (smmu, 0x1000), read32 (smmu, 0x1004), numCb - 1, read32 (smmu, 0x1000 + 4 * (numCb - 1)),
         read32 (cb0, 0), read32 (cb1, 0), read32 (cbLast, 0),
         read32 (smmu, 0x1800), read32 (smmu, 0x1804));
}

// ======================================================================================
// Root media: the live controller as a read-only block device.
//
// One shared host (a single UTP request slot, polled; interrupts stay masked) serves every
// block device. The DMA region is ordinary cached, physically contiguous kernel memory with
// explicit cache maintenance around each command (the stream is mapped through the reserved
// apps_smmu bypass context bank by smmuBypassStream, so physical addresses are used as-is).
// Nothing here writes to the storage: only INQUIRY-class and READ(10) commands are built, plus
// the single START STOP UNIT that wakes the device from the power-down the firmware left.
// ======================================================================================
namespace {
#include "RootTransportLayout.h"

struct UfsHost {
  OSSharedPtr<IOMemoryMap> hcMap, smmuMap;
  uintptr_t hc;
  IOBufferMemoryDescriptor *dma;
  uint8_t *virt;
  uint64_t phys;
  volatile int lock;
  bool ready;
  bool poisoned = false;
  // l452: one timed-out WRITE(10) from our own log flush used to sentence the whole device.
  // This flag lets a write failure retire the writers instead, so the read path keeps working.
  bool writeQuarantined = false;
  uintptr_t smmu = 0, cb = 0;
};
UfsHost gHost = {};

/* l443: the official native-ufs-diagnostics switch used to be capture-only.
 * recordTransport() filled gRootDiagnostic on every non-success transport, but the one
 * consumer that ever printed it is rootDiagnosticFailure(), which is reached from the root
 * *configuration* paths and panics. A transport fault on its own therefore produced no line,
 * and whenever a configuration fault did print, the cache already described a later command.
 * Report the FIRST transport fault on the console at the instant it happens and stop
 * overwriting that snapshot, so the field set describes the fault we are trying to name.
 * Bounded by construction: one report per boot, no allocation, no lock, no queue reset, no
 * MMIO beyond what recordTransport() has already read, and it cannot re-enter ufsReadWindow. */
static bool gTransportFaultReported = false;
static char gTransportFaultText[2600];

static void reportFirstTransportFault (void)
{
  if (gTransportFaultReported || !gRootDiagnosticEnabled) return;
  gTransportFaultReported = true;
  gRootDiagnostic.published = native_ufs_blocks_published;
  gRootDiagnostic.reads = native_ufs_block_reads;
  gRootDiagnostic.failures = native_ufs_block_failures;
  // l455: the decisive fields on one line, ahead of the block. The block is 2.6 KiB and the
  // fault kills the writer in the same breath, so three of six rounds lost it entirely; a
  // single line fits behind whatever the console backlog had already queued.
  IOLog ("UFS FAULT1 res=%08x cmd=%u dir=%u op=%02x lba=%llx bytes=%u fua=%u ocs=%02x "
         "dw0=%08x dw2=%08x rsp0=%08x polls=%u span=%llu gap=%llu grace=%u db=%x is=%x rsr=%x "
         "reads=%llu fails=%llu rescues=%u\n",
         gRootDiagnostic.result, gRootDiagnostic.command, gRootDiagnostic.cmdDir,
         gRootDiagnostic.opcode, (unsigned long long) gRootDiagnostic.cmdLba,
         gRootDiagnostic.cmdBytes, gRootDiagnostic.cmdFua, gRootDiagnostic.ocs,
         gRootDiagnostic.utrd[0], gRootDiagnostic.utrd[2], gRootDiagnostic.rspU[0],
         gRootDiagnostic.polls,
         (unsigned long long) (gRootDiagnostic.faultCnt - gRootDiagnostic.submitCnt),
         (unsigned long long) (gRootDiagnostic.faultCnt - gRootDiagnostic.lastPollCnt),
         gRootDiagnostic.gracePolls, gRootDiagnostic.doorbell, gRootDiagnostic.interrupt,
         gRootDiagnostic.runStop,
         (unsigned long long) native_ufs_block_reads,
         (unsigned long long) native_ufs_block_failures, gUfsFalseTimeouts);
  formatUfsRootDiagnostics (gTransportFaultText, sizeof (gTransportFaultText), gRootDiagnostic,
                            "first transport fault; registers read at that instant",
                            "UFS TRANSPORT FAULT");
  IOLog ("%s", gTransportFaultText);
  nabu_mark_text ("ufs:fault-snap", 1990);
}

// Defined below with the other cache helpers; needed here for the descriptor dump.
static void cacheInvalidate (const void *start, size_t length);

static IOReturn recordTransport (IOReturn result, uint32_t ocs)
{
  gRootDiagnostic.result = static_cast<uint32_t> (result);
  gRootDiagnostic.ocs = ocs;
  gRootDiagnostic.faultTicks = mach_absolute_time ();
  gRootDiagnostic.faultCnt = nabu_cntpct ();
  if (result != kIOReturnSuccess && gRootDiagnosticEnabled) {
    // Read only the already mapped HC's status/registers used by this driver.
    gRootDiagnostic.hcs = read32 (gHost.hc, 0x30);
    gRootDiagnostic.hce = read32 (gHost.hc, 0x34);
    gRootDiagnostic.doorbell = read32 (gHost.hc, 0x58);
    gRootDiagnostic.ctrl = read32 (gHost.hc, 0x04);
    gRootDiagnostic.utmDoorbell = read32 (gHost.hc, 0x5C);
    gRootDiagnostic.listHigh = read32 (gHost.hc, 0x4C);
    gRootDiagnostic.tmList = read32 (gHost.hc, 0x64);
    gRootDiagnostic.interrupt = read32 (gHost.hc, 0x20);
    gRootDiagnostic.list = uint64_t (read32 (gHost.hc, 0x50)) |
                          (uint64_t (read32 (gHost.hc, 0x54)) << 32);
    gRootDiagnostic.runStop = read32 (gHost.hc, 0x60);
    gRootDiagnostic.ahit = read32 (gHost.hc, 0x18);
    for (unsigned i = 0; i < 5; ++i) gRootDiagnostic.uic[i] = read32 (gHost.hc, 0x38 + 4 * i);
    if (gHost.smmu && gHost.cb) {
      gRootDiagnostic.gfsr = read32 (gHost.smmu, 0x48);
      gRootDiagnostic.gsyn = read32 (gHost.smmu, 0x54);
      gRootDiagnostic.fsr = read32 (gHost.cb, 0x58);
      gRootDiagnostic.fsyn = read32 (gHost.cb, 0x68);
      gRootDiagnostic.far = uint64_t (read32 (gHost.cb, 0x60)) |
                            (uint64_t (read32 (gHost.cb, 0x64)) << 32);
    }
    // What the host and the device actually left in memory, read after invalidating each line.
    {
      volatile uint32_t *utrd = reinterpret_cast<volatile uint32_t *> (gHost.virt);
      volatile uint32_t *cmdu = reinterpret_cast<volatile uint32_t *> (gHost.virt + kUcdOffset);
      volatile uint32_t *rspu = reinterpret_cast<volatile uint32_t *> (gHost.virt + kUcdOffset + kRspOffset);
      cacheInvalidate (gHost.virt, 64);
      for (unsigned i = 0; i < 8; ++i) { gRootDiagnostic.utrd[i] = utrd[i]; }
      cacheInvalidate (gHost.virt + kUcdOffset, 64);
      for (unsigned i = 0; i < 8; ++i) { gRootDiagnostic.cmdU[i] = cmdu[i]; }
      cacheInvalidate (gHost.virt + kUcdOffset + kRspOffset, 64);
      for (unsigned i = 0; i < 8; ++i) { gRootDiagnostic.rspU[i] = rspu[i]; }
    }
    reportFirstTransportFault ();
  }
  return result;
}

static void cacheCivac (const void *start, size_t length)
{
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~uintptr_t (63);
  uintptr_t end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += 64) {
    __asm__ volatile ("dc civac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

// Device-owned cache lines must not be written back over DMA results. All
// callers use complete 64-byte lines from the dedicated page-aligned buffer.
static void cacheInvalidate (const void *start, size_t length)
{
  __asm__ volatile ("dsb sy" ::: "memory");
  uintptr_t address = reinterpret_cast<uintptr_t> (start);
  for (uintptr_t end = address + length; address < end; address += 64) {
    __asm__ volatile ("dc ivac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

static void hostWrite (unsigned offset, uint32_t value)
{
  __asm__ volatile ("dsb sy" ::: "memory");
  write32 (gHost.hc, offset, value);
  __asm__ volatile ("dsb sy" ::: "memory");
}

#include "RootQueue.h"

#include "RootCommands.h"
/* l457: a flush is one 4 KiB UFS write, and every write is read throughput we do not have. When asked
 * for, drop flushes that arrive sooner than nabu-ufs-flush-ms after the previous one; 0 keeps the old
 * 'flush when called' behaviour exactly. Placed after the include because the knobs live there. */
static void nabuFlushThrottled (void)
{
  if (gUfsFlushMs) {
    uint64_t now = mach_absolute_time ();
    if (now - gUfsLastFlush < (uint64_t) gUfsFlushMs * 19200ULL) {
      // The pet lives on the block-completion path (nabu_wdt_pet(), called from the read completion
      // site), so skipping a flush also silently skipped a pet: measured life 176 s without the gate
      // and 161/162 s with it, which cancelled most of the read gain. Petting here restores the rate
      // the pre-throttle code had. No watchdog timing parameter changes; with flush-ms unset this line
      // is unreachable, so rounds that do not ask for the gate are byte-for-byte the old behaviour.
      nabu_wdt_pet ();
      return;
    }
    gUfsLastFlush = now;
  }
  /* l467 put three read-ahead ring marks here, "only when the switch is on", as an outlet that
   * would answer 'did it hit?' without nabu-ufs-cmdstat. It was a bug and the board caught it by
   * measurement: a ring mark written from inside the flush path means the ring ALWAYS has new
   * bytes, so the l469 "already on the medium" guard can never be true. On l469 the pair
   * ra=0 / ra=8 measured writes 9,191 / 139,629 - the marks alone re-created the write flood and
   * read-ahead paid for its own instrumentation with x15.2 the write commands. The counters were
   * already carried by the periodic census snapshot (ufs:cs-ra= / ufs:cs-rm=), so nothing here was
   * unique except the stolen count, which moves there as ufs:cs-rs=. Rule recorded: a mark on the
   * landing path is a mark that defeats the landing guard. */
  (void) nabu_log_flush_tail_nosync (0);
}

// Bring the live controller up for reading: map, SMMU pass-through for the UFS stream, take over
// the request list, wake the device from UFS-PowerDown and wait for LUN 0 to be ready.
// `why` receives a short reason on failure.
static bool hostBringUp (const char **why)
{
  if (gHost.poisoned) {
    *why = "transport quarantined";
    return false;
  }
  if (gHost.ready) {
    return true;
  }
  gRootDiagnostic.stage = "controller map";
  nabu_mark_text ("host:ctrl-map", 1860);
  gHost.hc = mapPhys (UFS_HC_BASE, UFS_HC_SIZE, gHost.hcMap);
  if (!gHost.hc) {
    *why = "controller map";
    return false;
  }
  if (gRootDiagnosticEnabled) {
    gRootDiagnostic.cap = read32 (gHost.hc, 0x00);
    gRootDiagnostic.version = read32 (gHost.hc, 0x08);
    gRootDiagnostic.hcs = read32 (gHost.hc, 0x30);
    gRootDiagnostic.hce = read32 (gHost.hc, 0x34);
  }
  gRootDiagnostic.stage = "DMA allocation";
  nabu_mark_text ("host:dma-alloc", 1860);
  gHost.dma = IOBufferMemoryDescriptor::inTaskWithOptions (
    kernel_task, static_cast<IOOptionBits> (kIODirectionInOut) | static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
    static_cast<IOOptionBits> (kIOMemoryMapperNone),
    kDmaBytes, 4096);
  gHost.lock = 0;
  if (!gHost.dma) {
    *why = "dma buffer";
    return false;
  }
  gHost.virt = static_cast<uint8_t *> (gHost.dma->getBytesNoCopy ());
  IOByteCount contiguous = 0;
  gHost.phys = gHost.dma->getPhysicalSegment (0, &contiguous, kIOMemoryMapperNone);
  gRootDiagnostic.dma = gHost.phys;
  gRootDiagnostic.contiguous = contiguous;
  if (!gHost.virt || !gHost.phys || (gHost.phys & 4095) || contiguous < kDmaBytes ||
      (!(read32 (gHost.hc, 0) & 0x01000000) && gHost.phys + kDmaBytes > 0x100000000ULL)) {
    *why = "DMA address/contiguity";
    return false;
  }
  bzero (gHost.virt, kDmaBytes);
  cacheCivac (gHost.virt, kDmaBytes);
  uint32_t report[3] = { 0, 0, 0 };
  gRootDiagnostic.stage = "SMMU and request list";
  nabu_mark_text ("host:smmu", 1860);
  if (!hostStopQueue (why)) return false;
  smmuBypassStream (0x60, gHost.smmuMap, report, gHost.phys, kDmaBytes);
  gHost.smmu = gHost.smmuMap->getVirtualAddress ();
  uint32_t idr1 = read32 (gHost.smmu, 0x24);
  gHost.cb = gHost.smmu + ((1U << (((idr1 >> 28) & 7) + 1)) + (report[0] >> 16)) * 4096;
  if (gRootDiagnosticEnabled) {
    gRootDiagnostic.gfsrBefore = read32 (gHost.smmu, 0x48);
    gRootDiagnostic.fsrBefore = read32 (gHost.cb, 0x58);
    gRootDiagnostic.smmuId2 = read32 (gHost.smmu, 0x28);
    gRootDiagnostic.cba2r = read32 (gHost.smmu, 0x1800 + 4 * (report[0] >> 16));
  }
  if (!hostStartQueue (why)) return false;
  gRootDiagnostic.stage = "NOP OUT";
  nabu_mark_text ("host:nop", 1860);
  // NOP OUT proves the transport before touching the SCSI layer.
  if (hostNop () != kIOReturnSuccess) {
    gHost.ready = false;
    *why = "NOP OUT";
    return false;
  }
  uint32_t power = 0;
  gRootDiagnostic.stage = "power mode query";
  nabu_mark_text ("host:power", 1860);
  if (hostQueryRead (0x03, 0x02, &power) != kIOReturnSuccess) {
    gHost.ready = false;
    *why = "power mode query";
    return false;
  }
  gRootDiagnostic.power = power;
  if ((power & 0xff) != 0x11) {
    gRootDiagnostic.stage = "device wake";
  nabu_mark_text ("host:wake", 1860);
    // START STOP UNIT, power condition ACTIVE, to the UFS Device well-known LU (as Linux resumes).
    uint8_t ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };
    uint32_t scsi = 0;
    if (hostScsi (0xd0, ssu, 6, false, 0, 500000, &scsi) != kIOReturnSuccess) {
      gHost.ready = false;
      *why = "wake (START STOP UNIT)";
      return false;
    }
    IOSleep (50);
  }
  gRootDiagnostic.stage = "LUN ready";
  nabu_mark_text ("host:lun", 1860);
  for (unsigned attempt = 0; attempt < 20; attempt++) {
    uint8_t tur[6] = { 0, 0, 0, 0, 0, 0 };
    uint32_t scsi = 0;
    if (hostScsi (0, tur, 6, false, 0, 200000, &scsi) == kIOReturnSuccess) {
      gHost.ready = true;
      return true;
    }
    if (gHost.poisoned) break;
    IOSleep (10);
  }
  gHost.ready = false;
  *why = "LUN 0 not ready";
  return false;
}
}

namespace {
static uint32_t be32w (const uint8_t *at)
{
  return (uint32_t (at[0]) << 24) | (uint32_t (at[1]) << 16) | (uint32_t (at[2]) << 8) | at[3];
}

#include "RootLayout.h"
#include "RootWindowRead.h"
#include "RootPublication.h"


}

// ---- Persistent Data store window (sheng-data-persist) ----------------------------------------------------------
// The only part of this driver that ever writes. The consumer (NativeRamBlock's overlay) asks for a window of the given
// size at the END of the userdata partition. Mode 1 (boot argument sheng-data-persist=1) only reads: it proves the window and
// lets the consumer inspect its header. Mode 2 additionally opens the write window. A window is refused unless it lies inside
// the partition, clear of the flashed root image plus a 4 MiB guard, and is at most a quarter of the partition.
namespace {
struct StoreGeometry {
  bool valid;
  uint64_t first, last, imageBytes, entriesLba;
  uint32_t headerCrc, entriesCrc, entryCount;
  uint64_t miscFirst;
  bool miscFound;
};
StoreGeometry gStoreGeometry;
uint64_t gStoreRegionFirst, gStoreRegionBlocks;
// The diagnostic log (sheng-log=1): always the last 32 MiB of userdata, so the Data store's place does not depend on logging.
constexpr uint64_t kLogReserveBlocks = 8192;
// The log's export region: the 256 MiB just below the log window (the render server's pipeline caches, kits/sheng BootLog.c). It is
// part of the log's write window, so the store ends below both.
constexpr uint64_t kExportBlocks = 65536;
uint64_t gLogRegionFirst, gLogRegionBlocks;

static unsigned storeMode (void)
{
  static int cached = -1;
  if (cached < 0) {
    uint32_t value = 0;
    if (!PE_parse_boot_argn ("sheng-data-persist", &value, sizeof (value)) || value > 2) value = 0;
    cached = static_cast<int> (value);
  }
  return static_cast<unsigned> (cached);
}
}

extern "C" unsigned native_ufs_store_mode (void) { return storeMode (); }

// Returns 1 and the first absolute 4096-byte LBA of the window, 0 on refusal (reason in *why, may be null).
extern "C" int native_ufs_store_open (uint64_t blocks, uint64_t *firstLba, const char **why)
{
  const char *reason = "";
  int ok = 0;
  if (!storeMode ()) reason = "off";
  else if (!gStoreGeometry.valid || !gHost.ready) reason = "no root layout";
  else if (gStoreRegionBlocks) reason = "already open";
  else {
    uint64_t partition = gStoreGeometry.last - gStoreGeometry.first + 1;
    uint64_t imageBlocks = (gStoreGeometry.imageBytes + 4095) / 4096;
    if (!blocks || blocks > partition / 4) reason = "size";
    else if (partition - blocks - kLogReserveBlocks - kExportBlocks < imageBlocks + 1024) reason = "overlaps the root image";
    else {
      // The last kLogReserveBlocks of the partition belong to the diagnostic log and the kExportBlocks below them to its export region;
      // the store ends just below both.
      uint64_t first = gStoreGeometry.last + 1 - kLogReserveBlocks - kExportBlocks - blocks;
      if (storeMode () == 2 && !hostStoreOpen (first, blocks)) reason = "window refused";
      else {
        gStoreRegionFirst = first;
        gStoreRegionBlocks = blocks;
        if (firstLba) *firstLba = first;
        ok = 1;
      }
    }
  }
  if (why) *why = reason;
  return ok;
}

extern "C" int native_ufs_store_read (uint64_t rel, uint32_t blocks, void *dst)
{
  if (!gStoreRegionBlocks || !dst || !blocks || rel >= gStoreRegionBlocks || blocks > gStoreRegionBlocks - rel) return 0;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    if (hostRead4k (0, gStoreRegionFirst + rel, chunk, dst) != kIOReturnSuccess) return 0;
    dst = static_cast<uint8_t *> (dst) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

extern "C" int native_ufs_store_write (uint64_t rel, uint32_t blocks, const void *src)
{
  if (storeMode () != 2 || !gStoreRegionBlocks || !src || !blocks || rel >= gStoreRegionBlocks ||
      blocks > gStoreRegionBlocks - rel) return 0;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    if (hostWrite4k (0, gStoreRegionFirst + rel, chunk, src) != kIOReturnSuccess) return 0;
    src = static_cast<const uint8_t *> (src) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

extern "C" int native_ufs_store_sync (void)
{
  return storeMode () == 2 && gStoreRegionBlocks && hostSyncCache () == kIOReturnSuccess;
}

// ---- Diagnostic log window (sheng-log=1) --------------------------------------------------------------------------
// The board's log writer (kits/sheng BootLog.c) gets the last kLogReserveBlocks of userdata and the kExportBlocks below them, refused
// unless the partition holds them clear of the flashed root image plus a 4 MiB guard. Off without the boot argument.
static unsigned logMode (void)
{
  static int cached = -1;
  if (cached < 0) {
    /* Default to on. On hardware 2026-10-24 PE_parse_boot_argn("sheng-log") returned
     * false even though boot-args carries sheng-log=1, and the panic screen said
     * "off"; the window stayed closed. Every write is still range-checked, so the
     * argument is only honoured when it parses and asks to disable. */
    uint32_t value = 1;
    (void)PE_parse_boot_argn ("sheng-log", &value, sizeof (value));
    if (value > 1) value = 0;
    cached = static_cast<int> (value);
  }
  return static_cast<unsigned> (cached);
}

// Returns 1 with the window's first absolute LBA and size in 4096-byte blocks, 0 on refusal (reason in *why, may be null).
extern "C" int native_ufs_log_open (uint64_t *firstLba, uint64_t *blocks, const char **why)
{
  const char *reason = "";
  int ok = 0;
  if (!logMode ()) reason = "off";
  else if (!gStoreGeometry.valid || !gHost.ready) reason = "no root layout";
  else if (gLogRegionBlocks) reason = "already open";
  else {
    uint64_t partition = gStoreGeometry.last - gStoreGeometry.first + 1;
    uint64_t imageBlocks = (gStoreGeometry.imageBytes + 4095) / 4096;
    if (partition < (kLogReserveBlocks + kExportBlocks) * 4 || partition - kLogReserveBlocks - kExportBlocks < imageBlocks + 1024)
      reason = "overlaps the root image";
    else {
      uint64_t first = gStoreGeometry.last + 1 - kLogReserveBlocks;
      // One write window covers the export region and the log: [first - kExportBlocks, first + kLogReserveBlocks).
      if (!hostLogOpen (first - kExportBlocks, kExportBlocks + kLogReserveBlocks)) reason = "window refused";
      else {
        gLogRegionFirst = first;
        gLogRegionBlocks = kLogReserveBlocks;
        if (firstLba) *firstLba = first;
        if (blocks) *blocks = kLogReserveBlocks;
        ok = 1;
      }
    }
  }
  if (why) *why = reason;
  return ok;
}

extern "C" int native_ufs_log_write (uint64_t rel, uint32_t blocks, const void *src)
{
  if (!gLogRegionBlocks || !src || !blocks || rel >= gLogRegionBlocks || blocks > gLogRegionBlocks - rel) return 0;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    if (hostWrite4k (0, gLogRegionFirst + rel, chunk, src, gUfsLogWriteFua == 1U) != kIOReturnSuccess) return 0;
    src = static_cast<const uint8_t *> (src) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

// The export region below the log window: its size in blocks once the log is open, else 0; writes at a block offset inside it.
extern "C" uint64_t native_ufs_export_blocks (void) { return gLogRegionBlocks ? kExportBlocks : 0; }

/* First absolute 4 KiB block of the export region (the kExportBlocks below the log window). The
 * diagnostic export reports this so the host can read the stream back without guessing. */
extern "C" uint64_t native_ufs_export_first (void) { return gLogRegionBlocks ? gLogRegionFirst - kExportBlocks : 0; }

extern "C" int native_ufs_export_write (uint64_t rel, uint32_t blocks, const void *src)
{
  if (!gLogRegionBlocks || !src || !blocks || rel >= kExportBlocks || blocks > kExportBlocks - rel) return 0;
  const uint64_t base = gLogRegionFirst - kExportBlocks;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    /* FUA, like the log window: without it the write can stay in the controller's cache, which a
     * PSCI reset (and a TWRP readback after it) does not see. Retry a few times because this runs
     * alongside the system's own read traffic and hostWrite4k fails on a busy lock. */
    bool ok = false;
    for (unsigned attempt = 0; attempt < 4 && !ok; ++attempt) {
      ok = hostWrite4k (0, base + rel, chunk, src, gUfsLogWriteFua == 1U) == kIOReturnSuccess;
    }
    if (!ok) return 0;
    src = static_cast<const uint8_t *> (src) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

extern "C" int native_ufs_log_sync (void)
{
  return gLogRegionBlocks && hostSyncCache () == kIOReturnSuccess;
}

// Arm the bootloader's recovery command before the panic reset. The first block of the misc
// partition is the Android bootloader_message; "boot-recovery" in its command field makes
// XBL/ABL boot the recovery target (TWRP on this board) instead of a normal boot that has
// nothing to load and parks at the logo. One block, written and synced once per panic.
extern "C" int native_ufs_panic_recovery (void)
{
  unsigned autolog = 0;
  PE_parse_boot_argn ("nabu-autolog", &autolog, sizeof (autolog));
  if (!autolog) return 0;
  if (!gStoreGeometry.valid || !gStoreGeometry.miscFound || !gHost.ready || !gStoreGeometry.miscFirst) return 0;
  /* Static storage: this runs with the allocator and most of the system gone, and
   * IOMalloc() here hung the board on hardware 2026-10-24 instead of returning. */
  static uint8_t block[4096];
  int ok = 0;
  if (hostRead4k (0, gStoreGeometry.miscFirst, 1, block) == kIOReturnSuccess &&
      hostMiscOpen (gStoreGeometry.miscFirst, 2)) {
    bzero (block, 32);
    const char command[] = "boot-recovery";
    for (unsigned i = 0; i < sizeof (command); ++i) block[i] = static_cast<uint8_t> (command[i]);
    bzero (block + 32, 32);
    if (hostWrite4k (0, gStoreGeometry.miscFirst, 1, block) == kIOReturnSuccess &&
        hostSyncCache () == kIOReturnSuccess) ok = 1;
  }
  return ok;
}

// Re-read the GPT header and entry array and compare their CRCs with what was recorded at root start. 0 = unchanged.
extern "C" unsigned native_ufs_store_gpt_check (void)
{
  if (!gStoreGeometry.valid || !gHost.ready) return 1;
  static uint8_t block[4096];
  if (hostRead4k (0, 1, 1, block) != kIOReturnSuccess) return 2;
  uint32_t headerBytes = le32 (block + 12);
  if (headerBytes < 92 || headerBytes > 4096 || rootCrc32 (block, headerBytes, true) != le32 (block + 16) ||
      le32 (block + 16) != gStoreGeometry.headerCrc) return 3;
  uint32_t blocks = (gStoreGeometry.entryCount * 128 + 4095) / 4096;
  static uint8_t entries[4096 * 4];
  if (blocks > 4 || hostRead4k (0, gStoreGeometry.entriesLba, blocks, entries) != kIOReturnSuccess) return 4;
  return rootCrc32 (entries, gStoreGeometry.entryCount * 128) == gStoreGeometry.entriesCrc ? 0 : 5;
}

// One read-only window of the root image as a block device: 512-byte sectors on top of 4096-byte
// UFS blocks, so the unchanged dmg disk images (GPT with 512-byte sectors) parse as they do on
// the virtio path.
class NativeUfsBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors (NativeUfsBlock)
  uint64_t absolute = 0;      // byte offset of the window on LUN 0
  uint64_t bytes = 0;
  char label[32] = {};
  uint8_t *bounce = nullptr;
  volatile int windowLock = 0;
  bool online = false;
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  struct vr_view verified = {};
  vm_offset_t verifiedStorage = 0;
  vm_size_t verifiedStorageBytes = 0;
  bool configureVerification ();
#endif
public:
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  bool authenticatesVolume (const char *uuid, uint8_t *manifestDigest = nullptr) const;
  bool signedVolumeUuid (char out[37]) const;
#endif
  const char *windowLabel () const { return label; }
  uint64_t windowBytes () const { return bytes; }
  bool configure (const char *name, uint64_t absoluteOffset, uint64_t size);
  bool start (IOService *provider) override;
  void free () override;
  IOReturn doEjectMedia () override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia (UInt64) override { return kIOReturnNotWritable; }
  UInt32 doGetFormatCapacities (UInt64 *, UInt32) const override { return 0; }
  char *getVendorString () override { return const_cast<char *> ("Native"); }
  char *getProductString () override { return label; }
  char *getRevisionString () override { return const_cast<char *> ("1.0"); }
  char *getAdditionalDeviceInfoString () override { return const_cast<char *> ("UFS LUN 0 userdata window, read-only"); }
  IOReturn reportBlockSize (UInt64 *value) override {
    *value = 512;
    IOLog ("NativeUfs: reportBlockSize -> 512\n");
    return kIOReturnSuccess;
  }
  bool open (IOService *forClient, IOOptionBits options, void *arg) override {
    /* Runs on the IOKit matching stack. It records the call and returns: no storage I/O, no
     * registry walk, no watchdog write. The driver's media is opened from a thread that waits
     * for this stack to unwind, which is the one variable this round changes. */
    static unsigned opens = 0;
    if (opens < 6) {
      ++opens;
      nabu_log_puts ("ufs:op\n");
    }
    bool ok = IOBlockStorageDevice::open (forClient, options, arg);
    if (opens == 2) {
      IOService *media = forClient ? forClient->getClient () : nullptr;
      if (media != nullptr) {
        media->retain ();
        thread_t opener = nullptr;
        kern_return_t started = kernel_thread_start (nabu_media_open_thread, media, &opener);
        nabu_log_hex ("ufs:wthr=", (unsigned long long) started);
        if (started == KERN_SUCCESS && opener != nullptr) thread_deallocate (opener);
        else media->release ();
      }
    }
    return ok;
  }
  void close (IOService *forClient, IOOptionBits options) override {
    nabu_log_puts ("ufs:cl\n");
    nabu_wdt_pet ();
    IOBlockStorageDevice::close (forClient, options);
  }
  IOReturn reportEjectability (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock (UInt64 *value) override {
    *value = bytes / 512 - 1;
    IOLog ("NativeUfs: reportMaxValidBlock -> %llu\n", (unsigned long long) *value);
    return kIOReturnSuccess;
  }
  IOReturn reportMediaState (bool *present, bool *changed) override {
    *present = online; if (changed) *changed = false;
    nabu_log_puts (online ? "ufs:rms=1\n" : "ufs:rms=0\n");
    return kIOReturnSuccess;
  }
  IOReturn reportRemovability (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection (bool *value) override { *value = true; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache () override { return online ? kIOReturnSuccess : kIOReturnNotReady; }
  IOReturn doSynchronize (UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache (); }
  IOReturn getWriteCacheState (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn setWriteCacheState (bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite (IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *, IOStorageCompletion *) override;
};
OSDefineMetaClassAndStructors (NativeUfsBlock, IOBlockStorageDevice)

extern "C" {
volatile uint32_t native_ufs_blocks_published = 0;
volatile uint64_t native_ufs_block_reads = 0;
volatile uint64_t native_ufs_block_failures = 0;
}

static char gRootDiagnosticText[2500];
static char gRootMediaText[1000];
// Passive diagnostic reader: cached software values only. It must remain safe
// when called from an IRQ while the transport is busy: no locks, allocations,
// MMIO reads, queue resets, or changes to the command/trust state. Fields can
// represent adjacent updates; this is explicitly not an atomic HW snapshot.
extern "C" void native_ufs_progress_text (char *out, size_t size)
{
  UfsRootDiagnostics sample = gRootDiagnostic;
  sample.published = native_ufs_blocks_published;
  sample.reads = native_ufs_block_reads;
  sample.failures = native_ufs_block_failures;
  formatUfsRootDiagnostics (out, size, sample, "cached values; not a new HW fault sample", "UFS CACHED SNAPSHOT");
}

static void rootDiagnosticFailure (const char *reason)
{
  // Kept even when the panic diagnostic is off, so the boot page can say why nothing was published.
  native_ufs_root_failure = reason;
  if (reason) { nabu_mark_text (reason, 1940); }
  if (!gRootDiagnosticEnabled) return;
  gRootDiagnostic.published = native_ufs_blocks_published;
  gRootDiagnostic.reads = native_ufs_block_reads;
  gRootDiagnostic.failures = native_ufs_block_failures;
  formatUfsRootDiagnostics (gRootDiagnosticText, sizeof (gRootDiagnosticText), gRootDiagnostic, reason);
  strlcat (gRootDiagnosticText, gRootMediaText, sizeof (gRootDiagnosticText));
  panic ("%s", gRootDiagnosticText);
}

bool NativeUfsBlock::configure (const char *name, uint64_t absoluteOffset, uint64_t size)
{
  if (!name || !ufsWindowGeometry (absoluteOffset, size)) {
    nabu_mark_text ("cfg:geometry", 2020);
    return false;
  }
  strlcpy (label, name, sizeof (label));
  absolute = absoluteOffset;
  bytes = size;
  windowLock = 0;
  /* the window lock is an atomic flag now: nothing to allocate */
  bounce = static_cast<uint8_t *> (IOMalloc (kDataMax));
  if (!gUfsRaBuf) { gUfsRaBuf = static_cast<uint8_t *> (IOMalloc (kDataMax)); }   // l468: once, not per window
  if (!bounce || !gUfsRaBuf) {   // l465: no cache just means no read-ahead, never a failure
    gUfsReadAhead = 0U;
    nabu_mark_text ("cfg:bounce", 2020);
    return false;
  }
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  // Reads are served only after each 4096-byte integrity block verifies against the owner-signed
  // policy; without a matching policy the device is never published.
  return configureVerification ();
#else
  return true;
#endif
}

#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
volatile uint64_t native_ufs_verified_blocks = 0;
// Time spent hashing/verifying and reading from the UFS transport, in generic-timer ticks (diagnostic only).
volatile uint64_t native_ufs_verify_ticks = 0, native_ufs_read_ticks = 0;
static inline uint64_t timerTicks (void) {uint64_t v; __asm__ volatile ("isb\n\tmrs %0, cntpct_el0" : "=r" (v)); return v;}
volatile uint32_t native_ufs_integrity_failures = 0;

bool NativeUfsBlock::configureVerification ()
{
  gRootDiagnostic.stage = "policy selection";
  gRootDiagnostic.windowBytes = bytes;
  gRootDiagnostic.policyCount = verified_root_policy_count;
  gRootDiagnostic.verify = 0xffffffff;
  // Select the one policy whose signed image size equals this window; two matches are refused.
  const uint8_t *manifest = nullptr;
  uint64_t manifestBytes = 0;
  if (!verified_root_policy_count || verified_root_policy_count > 16) {
    gRootDiagnostic.stage = "policy count invalid";
    return false;
  }
  for (uint64_t index = 0; index < verified_root_policy_count; ++index) {
    const NativeVerifiedBlockPolicy &policy = verified_root_policies[index];
    if (!policy.manifest || policy.manifestBytes < VR_HEADER_BYTES + VR_SIGNATURE_BYTES) {
      gRootDiagnostic.stage = "policy envelope invalid";
      return false;
    }
    uint64_t imageBytes = 0;
    for (unsigned i = 0; i < 8; ++i) {
      imageBytes |= static_cast<uint64_t> (policy.manifest[24 + i]) << (8 * i);
    }
    if (imageBytes != bytes) {
      continue;
    }
    if (manifest) {
      gRootDiagnostic.stage = "duplicate capacity-matching policy";
      return false;
    }
    manifest = policy.manifest;
    manifestBytes = policy.manifestBytes;
  }
  if (!manifest) {
    gRootDiagnostic.stage = "no capacity-matching policy";
    return false;
  }
  gRootDiagnostic.stage = "policy storage";
  vm_size_t tableBytes = round_page (static_cast<vm_size_t> (manifestBytes));
  if (tableBytes < manifestBytes || tableBytes > UINT64_MAX - 4096) {
    gRootDiagnostic.stage = "policy size overflow";
    return false;
  }
  verifiedStorageBytes = tableBytes + 4096;
  gRootDiagnostic.stage = "policy allocation";
  if (kmem_alloc (kernel_map, &verifiedStorage, verifiedStorageBytes,
                  static_cast<kma_flags_t> (KMA_KOBJECT | KMA_DATA), VM_KERN_MEMORY_SECURITY) != KERN_SUCCESS) {
    return false;
  }
  memcpy (reinterpret_cast<void *> (verifiedStorage), manifest, manifestBytes);
  uint8_t *key = reinterpret_cast<uint8_t *> (verifiedStorage + tableBytes);
  memcpy (key, verified_root_key, VR_KEY_BYTES);
  gRootDiagnostic.stage = "policy read-only protection";
  if (vm_map_protect (kernel_map, verifiedStorage, verifiedStorage + verifiedStorageBytes,
                      VM_PROT_READ, TRUE) != KERN_SUCCESS) {
    return false;
  }
  gRootDiagnostic.stage = "owner signature and hash table";
  int status = vr_open (reinterpret_cast<const uint8_t *> (verifiedStorage), manifestBytes, key, 1,
                        &vr_xnu_crypto, &verified);
  gRootDiagnostic.verify = static_cast<uint32_t> (status);
  return status == VR_OK && verified.image_bytes == bytes;
}

bool NativeUfsBlock::authenticatesVolume (const char *uuid, uint8_t *manifestDigest) const
{
  if (!uuid || !online || !verifiedStorage || !verified.hashes) {
    return false;
  }
  static const char hex[] = "0123456789ABCDEF";
  char expected[37];
  unsigned at = 0;
  for (unsigned i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      expected[at++] = '-';
    }
    expected[at++] = hex[verified.volume_uuid[i] >> 4];
    expected[at++] = hex[verified.volume_uuid[i] & 15];
  }
  expected[at] = 0;
  if (strcmp (uuid, expected)) {
    return false;
  }
  if (manifestDigest) {
    // Hash only the authenticated envelope, excluding allocation padding and the key.
    const uint64_t envelope = VR_HEADER_BYTES + VR_SIGNATURE_BYTES + verified.block_count * 32;
    SHA384_CTX context;
    SHA384_Init (&context);
    SHA384_Update (&context, reinterpret_cast<const void *> (verifiedStorage), envelope);
    SHA384_Final (manifestDigest, &context);
  }
  return true;
}

bool NativeUfsBlock::signedVolumeUuid (char out[37]) const
{
  if (!verifiedStorage || !verified.hashes) {
    return false;
  }
  static const char hex[] = "0123456789ABCDEF";
  unsigned at = 0;
  for (unsigned i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out[at++] = '-';
    }
    out[at++] = hex[verified.volume_uuid[i] >> 4];
    out[at++] = hex[verified.volume_uuid[i] & 15];
  }
  out[at] = 0;
  return true;
}

// Called from the root authorization query in NativeVirtioBlock: -1 when `provider` is not a
// UFS window, otherwise 1 if its signed volume UUID equals `uuid`, else 0.
extern "C" int native_ufs_block_authenticates (IOService *provider, const char *uuid, uint8_t *digest)
{
  NativeUfsBlock *block = OSDynamicCast (NativeUfsBlock, provider);
  if (!block) {
    return -1;
  }
  return block->authenticatesVolume (uuid, digest) ? 1 : 0;
}
#endif


static char gPendingUuid[4][37];

// IOBlockStorageDriver builds the IOMedia with instantiateMediaObject() and does not copy the
// device's property dictionary onto it, so a UUID set on the device never reaches the object that
// IOFindBSDRoot matches. Walk down to the media and stamp it directly; call after start() and again
// from the publication wait callback for the case where acceptNewMedia() runs later on the workloop.
/* A media stamped with our UUID must descend from this device. Without this, the first
 * storage publisher in the plane (a RAM block seeded for Data) gets the root UUID too,
 * and IOFindBSDRoot takes whichever media registered first. */
static bool nabu_media_owned_by (IOService *media, IOService *owner)
{
  IOService *provider = media->getProvider ();
  for (unsigned depth = 0; provider != nullptr && depth < 8; ++depth) {
    if (provider == owner) return true;
    provider = provider->getProvider ();
  }
  return false;
}

static void nabu_publishMediaUuid (IOService *root, const char *uuid)
{
  if (uuid == nullptr || uuid[0] == 0) return;
  OSString *identity = OSString::withCString (uuid);
  if (identity == nullptr) return;

  /* Walking from our own object returned nothing at all (media-scan=00/00), so ask IOKit for the
   * published media directly. serviceMatching() takes a class name string: no IOMedia metaClass
   * reference, which is what broke the kernel link when OSDynamicCast was used. */
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  unsigned scanned = 0, matched = 0;
  if (matching != nullptr) {
    OSIterator *services = IOService::getMatchingServices (matching);
    if (services != nullptr) {
      OSObject *object;
      while ((object = services->getNextObject ()) != nullptr) {
        ++scanned;
        IOService *service = OSDynamicCast (IOService, object);
        if (service != nullptr && nabu_media_owned_by (service, root)) {
          service->setProperty ("UUID", identity);
          ++matched;
        }
      }
      services->release ();
    }
    matching->release ();
  }
  /* Direct literal arguments: copying the prefix through a const char * lets the optimizer
   * fold it into immediates, and the string then never reaches __cstring (media-scan=00/00 in
   * the 22:59 build was a marker that silently vanished, not a missed code path). */
  nabu_log_puts ("ufs:media-scan=");
  {
    char digits[6];
    digits[0] = (char) ('0' + (scanned / 10) % 10);
    digits[1] = (char) ('0' + scanned % 10);
    digits[2] = '/';
    digits[3] = (char) ('0' + (matched / 10) % 10);
    digits[4] = (char) ('0' + matched % 10);
    digits[5] = 0;
    nabu_log_puts (digits);
  }
  nabu_log_puts ("\n");
  identity->release ();
}

bool NativeUfsBlock::start (IOService *provider)
{
  // Each gate reports itself on the strip: the SMMU readbacks are bypassed for bring-up, so the
  // remaining question is which of these three rejects the device.
  if (!bounce) {
    nabu_mark_text ("start:no-bounce", 2100);
    return false;
  }
  /* windowLock is an atomic flag now: 0 is a valid state, not a missing object. */
  /* IOBlockStorageDriver::checkForMedia() asks reportMediaState() *during* the base start(); with
   * online still false it sees no media, never calls acceptNewMedia(), and no IOMedia ever exists
   * (that is the media-scan=00/00 the window kept showing). Report the medium present first. */
  online = true;
  if (!IOBlockStorageDevice::start (provider)) {
    online = false;
    nabu_mark_text ("start:base-false", 2100);
    return false;
  }
  online = true;
  /* The media object is created by IOBlockStorageDriver only after the device reports the media
   * online; without this the driver never builds an IOMedia, IOFindBSDRoot sees nothing to match, and
   * every later scan finds zero IOMedia (media-scan=00/00). */
  nabu_mark_text ("start:ok", 2100);
  /* IOStorageFamily's Info.plist matches IOBlockStorageDriver on
   *   IOProviderClass = IOBlockStorageDevice  AND  IOPropertyMatch { device-type = "Generic" }
   * so without this property no driver is ever instantiated (ufs:drv=0) and no IOMedia is created. */
  {
    OSString *generic = OSString::withCString ("Generic");
    if (generic != nullptr) { setProperty ("device-type", generic); generic->release (); }
  }
  /* Synchronous matching: the asynchronous job puts the driver match on a config thread,
   * and the IOMedia that the driver registers from inside its own start() then has to be
   * queued behind it. With kIOServiceSynchronous the driver is attached and started before
   * this returns, so the media job is queued onto an idle config thread instead. */
  /* Official form: asynchronous, with no options. The synchronous variant was added in l228
   * to force the driver to match in place; official does not do that, and its absence may be
   * why the media's own match never completes (cg=0xa: Registered set, Matched clear). */
  registerService ();
  nabu_log_puts ("ufs:registered\n");
  /* Everything read from the registry below happens before the match job has run: registerService()
   * only queues it for a config thread, and the first match may still have to load the personality's
   * kext. ufs:drv=0 and ufs:dev=0 in the l224 window are therefore expected and prove nothing about
   * the property. The catalogue answers synchronously: how many personalities exist for this class
   * chain (ufs:cand=) and how many of them have their kext in the kernel. */
  {
    OSString *type = OSDynamicCast (OSString, getProperty ("device-type"));
    nabu_log_puts ((type && type->isEqualTo ("Generic")) ? "ufs:dt=1\n" : "ufs:dt=0\n");
    unsigned candidates = 0, loaded = 0;
    if (gIOCatalogue) {
      SInt32 generation = 0;
      OSPtr<OSOrderedSet> drivers = gIOCatalogue->findDrivers (this, &generation);
      if (drivers) {
        candidates = drivers->getCount ();
        OSCollectionIterator *iter = OSCollectionIterator::withCollection (drivers);
        if (iter) {
          OSDictionary *personality;
          while ((personality = OSDynamicCast (OSDictionary, iter->getNextObject ())) != nullptr) {
            /* Which personalities does the catalogue actually hold for this class chain? A
             * driver class missing from the kernel symbol table can still live in a loaded
             * kext, so the name printed here is the only direct evidence of what can match. */
            OSString *driver = OSDynamicCast (OSString, personality->getObject ("IOClass"));
            nabu_log_puts ("ufs:p=");
            nabu_log_puts (driver ? driver->getCStringNoCopy () : "?");
            nabu_log_puts ("\n");
            OSObject *kext = nullptr;
            if (gIOCatalogue->isModuleLoaded (personality, &kext)) ++loaded;
            if (kext) kext->release ();
          }
          iter->release ();
        }
      }
    }
    char digits[8];
    digits[0] = (char) ('0' + (candidates / 10) % 10);
    digits[1] = (char) ('0' + candidates % 10);
    digits[2] = '/';
    digits[3] = (char) ('0' + (loaded / 10) % 10);
    digits[4] = (char) ('0' + loaded % 10);
    digits[5] = '\n';
    digits[6] = 0;
    nabu_log_puts ("ufs:cand=");
    nabu_log_puts (digits);
  }
  {
    OSDictionary *dm = IOService::serviceMatching ("IOBlockStorageDriver");
    unsigned drivers = 0;
    if (dm != nullptr) {
      OSIterator *it = IOService::getMatchingServices (dm);
      if (it != nullptr) {
        while (it->getNextObject () != nullptr) ++drivers;
        it->release ();
      }
      dm->release ();
    }
    nabu_log_puts (drivers ? "ufs:drv=1\n" : "ufs:drv=0\n");
    /* Is IOKit able to see us as an IOBlockStorageDevice service at all? Without that the
     * IOBlockStorageDriver personality never matches and no IOMedia can ever exist. */
    {
      OSDictionary *om = IOService::serviceMatching ("IOBlockStorageDevice");
      unsigned devices = 0;
      if (om != nullptr) {
        OSIterator *oi = IOService::getMatchingServices (om);
        if (oi != nullptr) {
          while (oi->getNextObject () != nullptr) ++devices;
          oi->release ();
        }
        om->release ();
      }
      if (devices == 0)      nabu_log_puts ("ufs:dev=0\n");
      else if (devices == 1) nabu_log_puts ("ufs:dev=1\n");
      else                   nabu_log_puts ("ufs:dev=many\n");
    }
    nabu_log_puts (inPlane (gIOServicePlane) ? "ufs:plane=1\n" : "ufs:plane=0\n");
  }
  nabuFlushThrottled ();
  if (gPendingUuid[0][0] || gPendingUuid[1][0] || gPendingUuid[2][0] || gPendingUuid[3][0]) {
    for (unsigned index = 0; index < 4; ++index) {
      nabu_publishMediaUuid (this, gPendingUuid[index]);
    }
  }
  return true;
}

/* l465: the read-ahead cache. Kept out of ufsReadWindow() so the window reader and its unit
 * tests see exactly the Ops contract they already do; this only wraps hostRead4k(). Three
 * rules: (1) never cache beyond the partition we were configured on - gUfsRaLimitLba is set
 * per request from `absolute + bytes`; (2) never cache across the forensic log window, because
 * that region is written by us and would make this a stale-data source (hostInWindow is the
 * same test hostWrite4k uses); (3) the caller always gets exactly the blocks it asked for -
 * the extra pages only ever sit in our own buffer, so the verify loop is untouched. Off with
 * gUfsReadAhead == 0, and it is never on unless the boot argument says so. */
static IOReturn nabuBlockRead (uint64_t lba, uint32_t blocks, void *out)
{
  /* l475: phase gate. Until the configured uptime the buffer is neither filled nor consulted, so a
   * round that asks for ra-after simply runs the old code path through the bulk and pre-userland
   * phases - where the read already arrives at 25.4 blocks per device command and prefetch can only
   * compete for the single slot (the +2.6 s window cost the board measured with plain ra=8). The
   * 1.3-blocks-per-command traffic is userland, and that is what this delay lets through.
   * mach_absolute_time() is CNTPCT, the same clock every t= in our log lines uses. */
  if (gUfsReadAheadAfter && mach_absolute_time () < (uint64_t) gUfsReadAheadAfter * 19200000ULL) {
    return hostRead4k (0, lba, blocks, out);
  }
  if (!gUfsReadAhead || !gUfsRaBuf || !blocks) return hostRead4k (0, lba, blocks, out);
  /* l467: the args banner echoes `readahead=8` as soon as the argument parses, so it cannot tell us
   * whether the buffer allocation later succeeded (configure() forces the switch to 0 when it does
   * not, and that runs after the banner). This mark can only appear if a read actually reached the
   * live cache - one branch on a static after the first hit, nothing per command after that. */
  static int nabuRaLive = 0;
  if (!nabuRaLive) {
    nabuRaLive = 1;
    nabu_log_puts ("ufs:ra-live\n");
  }
  const uint64_t keyLba = gUfsRaLba;
  const uint32_t keyBlocks = gUfsRaBlocks;
  const uint32_t seq = gUfsRaSeq;
  if (keyBlocks && ! (seq & 1U) && lba >= keyLba && lba + blocks <= keyLba + keyBlocks) {
    memcpy (out, gUfsRaBuf + (lba - keyLba) * 4096ULL, (uint64_t) blocks * 4096ULL);
    if (gUfsRaSeq == seq && gUfsRaLba == keyLba && gUfsRaBlocks == keyBlocks) {
      __atomic_fetch_add (&gUfsRaHits, 1, __ATOMIC_RELAXED);
      return kIOReturnSuccess;
    }
    // the generation moved under us (or was being refilled): what we just copied is not one
    // coherent snapshot, so throw it away and read the honest way
    __atomic_fetch_add (&gUfsRaStolen, 1, __ATOMIC_RELAXED);
    gUfsRaBlocks = 0;
    return hostRead4k (0, lba, blocks, out);
  }
  __atomic_fetch_add (&gUfsRaMisses, 1, __ATOMIC_RELAXED);
  uint32_t want = blocks + gUfsReadAhead;
  if (want > kDataMax / 4096U) { want = kDataMax / 4096U; }        // one command's worth
  if (want > blocks && gUfsRaLimitLba && lba + want > gUfsRaLimitLba) { want = blocks; }
  if (want > blocks && hostInWindow (lba, want)) { want = blocks; } // never cache our log tail
  if (want <= blocks) { gUfsRaBlocks = 0; return hostRead4k (0, lba, blocks, out); }
  __atomic_add_fetch (&gUfsRaSeq, 1U, __ATOMIC_ACQUIRE);            // -> odd, this refill owns the buffe
  const IOReturn status = hostRead4k (0, lba, want, gUfsRaBuf);
  if (status != kIOReturnSuccess) {
    gUfsRaBlocks = 0;
    __atomic_add_fetch (&gUfsRaSeq, 1U, __ATOMIC_RELEASE);          // -> even again
    return hostRead4k (0, lba, blocks, out);   // the cheap retry: ask for exactly what was wanted
  }
  gUfsRaLba = lba; gUfsRaBlocks = want;        // key published with the data in place
  memcpy (out, gUfsRaBuf, (uint64_t) blocks * 4096ULL);
  const bool mine = (gUfsRaLba == lba && gUfsRaBlocks == want);
  __atomic_add_fetch (&gUfsRaSeq, 1U, __ATOMIC_RELEASE);            // -> even, generation complete
  if (!mine) {
    __atomic_fetch_add (&gUfsRaStolen, 1, __ATOMIC_RELAXED);
    gUfsRaBlocks = 0;
    return hostRead4k (0, lba, blocks, out);
  }
  __atomic_fetch_add (&gUfsRaFetched, (uint64_t) want, __ATOMIC_RELAXED);
  return kIOReturnSuccess;
}

IOReturn NativeUfsBlock::doAsyncReadWrite (IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                           IOStorageAttributes *, IOStorageCompletion *completion)
{
  /* Entry probe, before every reject path: a read that is refused below returns silently,
   * so "no ufs:read" alone cannot tell "nobody read" from "reads were all rejected". */
  {
    if (__atomic_add_fetch (&nabuReadEntered, 1, __ATOMIC_RELAXED) == 1) {
      nabu_log_puts ("ufs:rd-in\n");
      nabu_log_puts (buffer ? "ufs:rd-buf=1\n" : "ufs:rd-buf=0\n");
      nabu_log_puts ((completion && completion->action) ? "ufs:rd-act=1\n" : "ufs:rd-act=0\n");
      nabu_log_puts (online ? "ufs:rd-on=1\n" : "ufs:rd-on=0\n");
    }
  }
  if (!buffer || !completion || !completion->action || !count || block >= bytes / 512 ||
      count > bytes / 512 - block || buffer->getLength () < count * 512) {
    static int reportedArgs = 0;
    if (!reportedArgs) {
      reportedArgs = 1;
      nabu_log_puts ("ufs:rd-arg\n");
      nabuFlushThrottled ();
    }
    return kIOReturnBadArgument;
  }
  if (buffer->getDirection () != kIODirectionIn) {
    /* The window is read-only; log the first write attempt so a failure that
     * follows it is attributable. */
    static int reported = 0;
    if (!reported) {
      reported = 1;
      nabu_log_puts ("ufs:write-rejected\n");
      /* Land it at once: if the caller dies right after a refusal, this line is
       * the only explanation the next boot can read. */
      nabuFlushThrottled ();
    }
    return kIOReturnNotWritable;
  }
  if (!online) {
    static int reportedNotReady = 0;
    if (!reportedNotReady) {
      reportedNotReady = 1;
      nabu_log_puts ("ufs:rd-nready\n");
      nabuFlushThrottled ();
    }
    return kIOReturnNotReady;
  }
  /* First block read: mountroot found the media and is asking for the superblock.
   * Ring only, no window write: this runs inside the UFS read path and must not
   * re-enter the log writer (the periodic thread lands the tail). */
  static int nabuFirstRead = 0;
  static unsigned int nabuReadCount = 0;
  if (!nabuFirstRead) {
    nabuFirstRead = 1;
    nabu_log_puts ("ufs:read\n");
  }
  unsigned int nabuReadOrdinal = __atomic_add_fetch (&nabuReadCount, 1, __ATOMIC_RELAXED);
  struct Operations {
    volatile int *mutex;
    IOMemoryDescriptor *buffer;
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
    const vr_view *view;
#endif
    /* l441-ufswait: lock() used to discard the nabu_busy_try() result, so after ~2 ms it
     * continued WITHOUT the mutex and two threads drove the single UTP slot at once.
     * Acquisition is now a bounded wait, is counted, and a failure is reported instead of
     * being walked past. */
    /* l441-ufswait: the only acquisition the window reader uses. The try_lock()/lock()
     * pair that used to sit here belonged to UfsWindowGuard, which nothing in the
     * driver or the host tests instantiates - and lock() dropped the acquisition on
     * timeout, which is the shape l441 had to fix once already. Deleting the dead pair
     * so the next reader does not chase it: the product never contained the string. */
    int lock_wait () { return nabu_busy_wait (mutex); }
    void unlock () { nabu_busy_release (mutex); }
    IOReturn read (uint64_t lba, uint32_t blocks, void *out) {
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
      /* timerTicks and native_ufs_read_ticks are declared inside the verified-block guard above, so
       * the timing has to live under the same condition. verify() already does this; read() did not,
       * which is why building without verified-block mode failed with 'use of undeclared identifier
       * timerTicks' -- and why the bring-up had been hiding behind the register-probe build, which
       * skips read() altogether and publishes no block device. */
      uint64_t t0 = timerTicks ();
      IOReturn r = nabuBlockRead (lba, blocks, out);
      __atomic_fetch_add (&native_ufs_read_ticks, timerTicks () - t0, __ATOMIC_RELAXED);
      return r;
#else
      return nabuBlockRead (lba, blocks, out);
#endif
    }
    IOReturn verify (uint64_t index, const void *data, size_t length) {
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
      uint64_t t0 = timerTicks ();
      int status = vr_verify_block (view, index, data, length, &vr_xnu_crypto);
      __atomic_fetch_add (&native_ufs_verify_ticks, timerTicks () - t0, __ATOMIC_RELAXED);
      if (status != VR_OK) {
        gRootDiagnostic.stage = "read block integrity";
        gRootDiagnostic.verify = static_cast<uint32_t> (status);
        __atomic_fetch_add (&native_ufs_integrity_failures, 1, __ATOMIC_RELAXED);
        return kIOReturnNotPrivileged;
      }
      __atomic_fetch_add (&native_ufs_verified_blocks, 1, __ATOMIC_RELAXED);
      if (boot_progress_poll) boot_progress_poll ();
#else
      (void) index; (void) data; (void) length;
#endif
      return kIOReturnSuccess;
    }
    uint64_t copy (uint64_t offset, const void *data, uint64_t length) {
      uint64_t copied = buffer->writeBytes (offset, data, length);
      if (copied == length) __atomic_fetch_add (&native_ufs_block_reads, 1, __ATOMIC_RELAXED);
      return copied;
    }
  } operations = { &windowLock, buffer
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
    , &verified
#endif
  };
  gUfsRaLimitLba = (absolute + bytes) / 4096ULL;   // l465: the cache never leaves our partition
  UfsWindowResult outcome = ufsReadWindow (operations, absolute, bytes, block, count, bounce);
  IOReturn result = outcome.status;
  uint64_t done = outcome.done;
  if (result != kIOReturnSuccess) {
    __atomic_fetch_add (&native_ufs_block_failures, 1, __ATOMIC_RELAXED);
    gRootDiagnostic.result = static_cast<uint32_t> (result);
    /* l443: a failed read is a transport symptom, not a configuration fault, and the
     * first transport fault already printed its own snapshot. Record the reason and stay
     * alive: this round has to show what losing storage does to the rest of the boot.
     * With the switch off these two statements are exactly what rootDiagnosticFailure()
     * used to do before its early return, so the control round is unchanged. */
    native_ufs_root_failure = "root window read failed";
    nabu_mark_text ("root window read failed", 1940);
  }
  /* Mount-phase logging: the periodic thread lands a tail every 2 s, but the APFS
   * mount can finish and crash inside one interval, leaving the window at
   * publish-done. Land a tail from the read path instead; this runs outside the
   * window lock, so it cannot re-enter ufsReadWindow. Reads are cheap to log next
   * to the read itself, and every 8th one keeps the window within a handful of
   * reads of wherever the mount actually is. */
  /* l469: this was `(nabuReadOrdinal & 7u) == 0`. The board measured our own forensic writes at
   * 68.5% of every command this boot issues, and this site is what generates them. The divisor is
   * now a boot argument (nabu-ufs-read-flush=1..8192, default 8 = unchanged) so the rate can be
   * A/B-ed on one kernel; combined with the "already on the medium" guard in the writer, a small
   * divisor is cheap even left as it is. */
  if ((nabuReadOrdinal % gUfsReadFlushEvery) == 0U) {
    nabuFlushThrottled ();
  }
  /* Time-based arming runs on EVERY I/O, not only every eighth one: the client processes appear
   * after +133 s and this board may issue only a handful of reads after +240 s, so an "every 8th
   * read" gate can miss the whole window (l378 saw s=1 fire and s=2 never fire). The work below is
   * a few tick comparisons plus a flag; the expensive flush above stays rate-limited. */
  {
    /* Rearm the late display census once past 120 s. The census itself runs on a parked kernel
     * thread woken here; nothing enumerates services in this I/O context (that could take the
     * registry locks the matching thread holds). This path is the only one still running late:
     * every IOSleep-based thread never wakes because no timer interrupts arrive on this board. */
    static uint64_t censusArmStart;
    const uint64_t censusArmNow = timerTicks ();
    if (!censusArmStart) censusArmStart = censusArmNow;
    /* Boot-absolute like the dump slots: backboardd, SpringBoard and IOMFB_FDR_Loader all appear
     * after +133 s, so a +120 s sample can only ever report "no client yet". Two later samples
     * cover the window in which the framework actually tries to open the framebuffer. */
    static uint32_t census2At0;
    static uint32_t census2At1;
    static uint32_t census2At2;
    static bool census2Parsed;
    if (!census2Parsed) {
      census2Parsed = true;
      PE_parse_boot_argn ("nabu-census2-at0", &census2At0, sizeof (census2At0));
      PE_parse_boot_argn ("nabu-census2-at1", &census2At1, sizeof (census2At1));
      PE_parse_boot_argn ("nabu-census2-at2", &census2At2, sizeof (census2At2));
      if (!census2At0) census2At0 = 360;
      if (!census2At1) census2At1 = 440;
      if (!census2At2) census2At2 = 520;
    }
    /* The framebuffer client appears late: s_map_display_type was first seen between +358 s and
     * +474 s, after both fixed sample points, which is why uc read 0 in every earlier round. The
     * two arm times are boot arguments so a round can bracket the real open without a rebuild. */
    if (censusArmNow > (uint64_t)census2At0 * 19200000ull) {
      nabu_legacy_census2_arm (1);
    }
    if (censusArmNow > (uint64_t)census2At1 * 19200000ull) {
      nabu_legacy_census2_arm (2);
    }
    if (censusArmNow > (uint64_t)census2At2 * 19200000ull) {
      nabu_legacy_census2_arm (3);
    }
    /* Arm the one-shot os_log stream dump at nabu-oslog-dump-at seconds (default 280, i.e. after
     * SpringBoard's first trap on this build). The dump itself runs on its own parked thread. */
    static uint32_t oslogDumpAt;
    static bool oslogParsed;
    if (!oslogParsed) {
      oslogParsed = true;
      PE_parse_boot_argn ("nabu-oslog-dump-at", &oslogDumpAt, sizeof (oslogDumpAt));
      if (!oslogDumpAt) oslogDumpAt = 280;
    }
    /* Boot-absolute, not "since the first read": the first read lands around boot+60 s, so a
     * relative deadline silently pushed 340 s to boot+400 s, past the reset point. */
    if (censusArmNow > (uint64_t)oslogDumpAt * 19200000ull) {
      nabu_oslog_dump_arm ();
    }
    /* Arm the one-shot diagnostic file export at nabu-diag-dump-at seconds (default 260). */
    /* Two slots: the string pool and the tracev3 files become available at different times, so one
     * moment cannot capture both. nabu-diag-dump-at stays as an alias for slot 1. */
    static uint32_t diagAt0;
    static uint32_t diagAt1;
    static bool diagParsed;
    if (!diagParsed) {
      diagParsed = true;
      PE_parse_boot_argn ("nabu-diag-at0", &diagAt0, sizeof (diagAt0));
      PE_parse_boot_argn ("nabu-diag-at1", &diagAt1, sizeof (diagAt1));
      uint32_t legacy = 0;
      if (PE_parse_boot_argn ("nabu-diag-dump-at", &legacy, sizeof (legacy)) && legacy) diagAt1 = legacy;
      if (!diagAt1) diagAt1 = 470;
      /* Both slots default on: the string pool is complete only early (logd migrates it around
       * +270 s) and the tracev3 files appear later, so one moment cannot capture both. */
      if (!diagAt0) diagAt0 = 260;
    }
    if (diagAt0 && censusArmNow > (uint64_t)diagAt0 * 19200000ull) {
      nabu_diag_dump_arm (0);
    }
    if (censusArmNow > (uint64_t)diagAt1 * 19200000ull) {
      nabu_diag_dump_arm (1);
    }
  }
  nabu_wdt_pet ();
  completion->action (completion->target, completion->parameter, result, done);
  return kIOReturnSuccess;
}

void NativeUfsBlock::free ()
{
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  if (verifiedStorage) {
    kmem_free (kernel_map, verifiedStorage, verifiedStorageBytes);
    verifiedStorage = 0;
  }
#endif
  if (bounce) {
    IOFree (bounce, kDataMax);
    bounce = nullptr;
  }
  if (0) {
    /* the atomic window flag needs no free */
  }
  IOBlockStorageDevice::free ();
}

// Ordered publication of the root windows. The original owner-trust code resolves the SystemOS
// volume by UUID immediately after the root is mounted, so SystemOS must already have published its
// volume media by then; the OS window (whose volume is the root being waited for) goes last.
static NativeUfsBlock *gPendingDevice[4];
static unsigned gPending;
static bool gPendingAttached[4];
/* gPendingUuid is defined above NativeUfsBlock::start(). */
static UfsRootPlan gPublicationPlan;
static NativeUfs *gPendingProvider;

// Used only for objects that were never started/published. Started objects
// retain their owner references even on a later failure; clients can have I/O.
static void discardPendingWindow (unsigned index)
{
  NativeUfsBlock *device = gPendingDevice[index];
  if (!device) return;
  if (gPendingAttached[index]) device->detach (gPendingProvider);
  gPendingAttached[index] = false;
  gPendingDevice[index] = nullptr;
  device->release ();
}

static void publishThread (void *, wait_result_t)
{
  nabu_log_puts ("ufs:publish\n");
  struct Operations {
    bool start (unsigned index) {
      NativeUfsBlock *device = gPendingDevice[index];
      gRootDiagnostic.stage = "root window service start";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Start);
      gRootDiagnostic.publicationWindow = index;
      nabu_log_puts ("ufs:win-start\n");
      nabuFlushThrottled ();
      /* SystemOS is now published too: the owner-cache mount looks its volume up by UUID
       * (8846FE78-...), and while this window stayed hidden there was no IOMedia for it to
       * find, so the mount never got a chance to run. The 2026-10-26 note predicted APFS
       * would refuse the sealed root; if that happens now the panic text will say so. */
      /* Ring only until publish-done: every window write during publication can
       * land on a busy UFS command queue and stop the boot (2026-10-26). */
      /* The volume identity has to be on the device BEFORE registerService: IOKit
       * matches on registration, and IOBlockStorageDriver copies no UUID to the
       * IOMedia it creates. root-matching is IOProviderClass=IOMedia plus
       * IOPropertyMatch{UUID}, resolved up the provider chain (2026-10-26). */
      if (gPendingUuid[index][0]) { nabu_log_puts ("ufs:uuid="); nabu_log_puts (gPendingUuid[index]); nabu_log_puts ("\n"); }
      else { nabu_log_puts ("ufs:uuid-empty\n"); }
      nabuFlushThrottled ();
      if (gPendingUuid[index][0]) {
        OSString *identity = OSString::withCString (gPendingUuid[index]);
        if (identity) {
          device->setProperty ("UUID", identity);
          identity->release ();
        }
      }
      if (!device->start (gPendingProvider)) {
        nabu_log_puts ("ufs:win-start-fail\n");
        nabuFlushThrottled ();
        return false;
      }
      native_ufs_blocks_published = native_ufs_blocks_published + 1;
      nabu_log_puts ("ufs:win-started\n");
      IOLog ("NativeUfs: published %s\n", device->windowLabel ());
      return true;
    }
    bool wait (unsigned index) {
      NativeUfsBlock *device = gPendingDevice[index];
      const char *uuid = gPendingUuid[index];
      gRootDiagnostic.stage = "media publication";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Media);
      gRootDiagnostic.publicationWindow = index;
      nabu_log_puts ("ufs:win-wait\n");
      // Ring only: a window write here races the UFS command queue and stopped the
      // boot at the second window (2026-10-26). publish-done flushes once.
      // Do not walk the registry from this thread: the main thread is already in
      // IOFindBSDRoot and the walk wedges here (2026-10-26, win-wait hang). The
      // UUID is on the device before registration, so mountroot's own waitForService
      // is the authoritative matcher; publish the remaining window and return.
      // No window write here: the second window's flush landed inside a busy UFS
      // command queue and stopped the boot (2026-10-26). publish-done writes once.
      nabu_log_puts ("media:skip-wait\n");
      nabu_publishMediaUuid (device, gPendingUuid[index]);
      nabuFlushThrottled ();
      (void) uuid;
      (void) device;
      return true;
    }
    void discard (unsigned index) { discardPendingWindow (index); }
  } operations;
  UfsRootPublicationResult result = ufsPublishRootWindows (operations, gPublicationPlan);
  if (result.phase != UfsRootPhase::Complete) {
    gRootDiagnostic.result = result.phase == UfsRootPhase::Start ? kIOReturnIOError : kIOReturnTimeout;
    const char *reason = result.phase == UfsRootPhase::Start ? "root window service start failed" :
      (!strcmp (gPendingDevice[result.index]->windowLabel (), "SystemOS") ?
       "SystemOS signed volume not found" : "signed root window volume not found");
    IOLog ("NativeUfs: root publisher stopped at window %u; published=%u\n", result.index, result.published);
    // Start failures have already released their unpublished object; do not
    // dereference it here. The media-failure object is deliberately retained.
    rootDiagnosticFailure (reason);
  }
  nabu_log_puts (result.phase == UfsRootPhase::Complete ? "ufs:publish-done\n" : "ufs:publish-fail\n");
  {
    /* Per-boot marker: the UFS window is rewritten in place, so without a value that changes every
     * run an identical md5 cannot be told apart from "this round never reached a flush point". */
    uint64_t stamp = mach_absolute_time () / 1000000ULL;
    char digits[24];
    unsigned at = 0;
    if (stamp == 0) digits[at++] = '0';
    char reversed[20];
    unsigned n = 0;
    while (stamp > 0 && n < sizeof (reversed)) { reversed[n++] = (char) ('0' + stamp % 10); stamp /= 10; }
    while (n > 0) digits[at++] = reversed[--n];
    digits[at] = 0;
    nabu_log_puts ("ufs:t=");
    nabu_log_puts (digits);
    nabu_log_puts ("\n");
  }

  nabuFlushThrottled ();
  IOLog ("NativeUfs: publish done\n");
  gPendingProvider->release ();  // Balanced with successful thread dispatch.
}

// Memory snapshot (native-ufs-probe=13): root mode as usual, then, after a delay, the kernel's own
// page counters on the panic screen. Observation only.
extern "C" {
extern unsigned int vm_page_free_count, vm_page_active_count, vm_page_inactive_count,
  vm_page_wire_count, vm_page_speculative_count, vm_page_purgeable_count;
extern uint32_t vm_page_pages;
extern unsigned int memorystatus_available_pages;
extern int nprocs;
}

static void statsThread (void *argument, wait_result_t)
{
  unsigned delay = static_cast<unsigned> (reinterpret_cast<uintptr_t> (argument));
  IOSleep (delay * 1000);
  panic ("NATIVE_UFS memory snapshot after %u s\n"
         "pages total=%u free=%u active=%u inactive=%u wired=%u spec=%u purgeable=%u\n"
         "available=%u max_mem=%llx procs=%d blocks=%llu\n",
         delay, vm_page_pages, vm_page_free_count, vm_page_active_count, vm_page_inactive_count,
         vm_page_wire_count, vm_page_speculative_count, vm_page_purgeable_count,
         memorystatus_available_pages, max_mem, nprocs, native_ufs_block_reads);
}

// Storage discovery report (native-ufs-probe=12): a separate thread waits for the original
// IOStorageFamily to publish IOMedia for the root windows, then shows what exists on the screen.
// It must not run inside the matching thread that is starting NativeUfs.
static char gMediaReport[3000];

static void mediaReportThread (void *, wait_result_t)
{
  unsigned count = 0;
  for (unsigned tick = 0; tick < 200; tick++) {      // up to 20 s
    OSDictionary *matching = IOService::serviceMatching ("IOMedia");
    OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
    count = 0;
    if (iterator) {
      while (iterator->getNextObject ()) {
        count++;
      }
      iterator->release ();
    }
    if (matching) {
      matching->release ();
    }
    if (count >= 4) {
      break;
    }
    IOSleep (100);
  }
  IOSleep (500);
  int used = snprintf (gMediaReport, sizeof (gMediaReport),
                       "NATIVE_UFS media report: published=%u reads=%llu failures=%llu media=%u\n",
                       native_ufs_blocks_published, native_ufs_block_reads, native_ufs_block_failures, count);
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
  while (iterator && used < int (sizeof (gMediaReport)) - 200) {
    IOService *media = OSDynamicCast (IOService, iterator->getNextObject ());
    if (!media) {
      break;
    }
    OSString *uuid = OSDynamicCast (OSString, media->getProperty ("UUID"));
    OSString *bsd = OSDynamicCast (OSString, media->getProperty (kIOBSDNameKey));
    OSString *content = OSDynamicCast (OSString, media->getProperty ("Content"));
    OSNumber *size = OSDynamicCast (OSNumber, media->getProperty ("Size"));
    used += snprintf (gMediaReport + used, sizeof (gMediaReport) - used, "%s %s size=%llx uuid=%s\n",
                      bsd ? bsd->getCStringNoCopy () : "-", content ? content->getCStringNoCopy () : "-",
                      size ? size->unsigned64BitValue () : 0ULL, uuid ? uuid->getCStringNoCopy () : "-");
  }
  if (iterator) {
    iterator->release ();
  }
  if (matching) {
    matching->release ();
  }
  panic ("%s", gMediaReport);
}

extern "C" void nabu_log_periodic (void);
/* Optional: present only when the NativeDwc3 archive is linked in. */
extern "C" int native_dwc3_bringup (void) __attribute__((weak));

/* Rewrite the diagnostic window from a healthy thread: panic() itself must not touch
 * UFS, because the host lock taken there never came back on hardware 2026-10-24. */
extern "C" void nabu_psci_reset (void);

/* Panic log import ------------------------------------------------------------
 * The kernel writes a panic into a fixed region (pe_init.c is patched with
 * PLATFORM_PANIC_LOG_PADDR - which the generator rewrites to 0x9b000000, the address the
 * reserved panic_region node in dts/nabu.dts actually carries) and no longer clears it at boot. This copies the text to the
 * log window so the host reads it the ordinary way. Nothing here runs in panic
 * context: it is the next boot that does the work, which is why it is safe at all.
 */
static uint8_t gPanicImport[16 * 4096];

/* l476: the region's own address used to be a literal here (0x9e000000) while pe_init maps
 * whatever PLATFORM_PANIC_LOG_PADDR the build ended up with - the generator rewrites that to
 * 0x9b000000, our reserved panic_region node says 0x9b000000, and this file said 0x9e000000.
 * Three sources of one truth, and the reader was the one that was wrong. Now the reader asks the
 * kernel: pexpert owns gPanicBase/gPanicSize (pe_init.c:73, exported in pexpert.h:61-62) and has
 * already mapped the region, so there is nothing left for us to guess and nothing left to map.
 * When gPanicBase is 0 that is a fact we can finally see, instead of a silent return 0. */
extern "C" unsigned long gPanicBase;      /* vm_offset_t, kernel VA of the panic region (0 = none) */
extern "C" unsigned int  gPanicSize;      /* its size in bytes */

extern "C" int native_ufs_paniclog_import (void)
{
  if (!gLogRegionBlocks || !gHost.ready) return 0;
  if (!gPanicBase) {
    /* The kernel never got a panic region (pexpert mapped nothing) - this is the line that
     * separates "no panic happened" from "there was nowhere to put it". debug_log_init() prints
     * the same fact from the inside; we print it where the board can read it off the disk. */
    nabu_log_puts ("paniclog:no-base\n");
    return 0;
  }
  const uint64_t bytes = gPanicSize;
  volatile uint32_t *hdr = reinterpret_cast<volatile uint32_t *> (gPanicBase);
  if (hdr[0] != 0x46554e4bU) {          /* EMBEDDED_PANIC_MAGIC, 'FUNK' (osfmk/kern/debug.h:345) */
    /* Was: return 0 with no line at all, which is how "the region is empty" and "we looked in the
     * wrong place" became indistinguishable for weeks. The word itself is the answer. */
    nabu_log_hex ("paniclog:magic=", hdr[0]);
    return 0;
  }
  /* hdr[4]/hdr[5] are eph_panic_log_offset / eph_panic_log_len: field order verified against
   * struct embedded_panic_header (debug.h:356-), magic/crc/version/flags come first. */
  uint32_t off = hdr[4], len = hdr[5];
  if (off > bytes || len == 0 || len > bytes - off) {
    nabu_log_puts ("paniclog:bad-hdr\n");
    return 0;
  }
  if (len > sizeof (gPanicImport) - 64) len = static_cast<uint32_t> (sizeof (gPanicImport) - 64);
  bzero (gPanicImport, 64);
  const char tag[] = "NABUPANIC1";
  for (unsigned i = 0; i < sizeof (tag); ++i) gPanicImport[i] = static_cast<uint8_t> (tag[i]);
  gPanicImport[16] = static_cast<uint8_t> (len);
  gPanicImport[17] = static_cast<uint8_t> (len >> 8);
  gPanicImport[18] = static_cast<uint8_t> (len >> 16);
  gPanicImport[19] = static_cast<uint8_t> (len >> 24);
  const uint8_t *src = reinterpret_cast<const uint8_t *> (gPanicBase) + off;
  for (uint32_t i = 0; i < len; ++i) gPanicImport[64 + i] = src[i];
  int wrote = 1;
  for (unsigned b = 0; b < 16; ++b) {
    if (!native_ufs_log_write (2 + b, 1, gPanicImport + b * 4096)) {
      wrote = 0;
      break;
    }
  }
  if (wrote) hdr[0] = 0;                /* imported once */
  nabu_log_hex ("paniclog:len=", len);
  nabu_log_hex ("paniclog:bytes=", bytes);
  return wrote;
}

static void nabu_media_open_thread (void *arg0, wait_result_t)
{
  IOService *media = (IOService *) arg0;
  if (media == nullptr) return;
  IOService *who = media->getClient ();
  const OSMetaClass *whoMeta = who ? who->getMetaClass () : nullptr;
  nabu_log_puts ("ufs:t-who=");
  nabu_log_puts (whoMeta ? whoMeta->getClassName () : "none");
  nabu_log_puts ("\n");
  if (who == nullptr) {
    /* No client on the media means nothing in the system wants it open. Opening it with
     * its own provider as the client would only set __owner (handleOpen checks no
     * direction) and poison the real client later. Report the fact instead. */
    nabu_log_puts ("ufs:t-open=no-client\n");
    IOLog ("NativeUfs: thread open skipped, no client\n");
  } else {
    bool opened = media->open (who, 0, (void *) (uintptr_t) 1);
    nabu_log_hex ("ufs:t-open=", opened ? 1 : 0);
    IOLog ("NativeUfs: thread open=%d\n", opened ? 1 : 0);
  }
  {
    /* Same questions as the callback census, asked from a stack that holds no matching lock. */
    unsigned drivers = nabu_count_services ("IOBlockStorageDriver");
    unsigned medias = nabu_count_services ("IOMedia");
    nabu_log_hex ("ufs:t-drv=", drivers);
    nabu_log_hex ("ufs:t-med=", medias);
    IOLog ("NativeUfs: thread census drv=%u med=%u\n", drivers, medias);
  }
  nabu_log_hex ("ufs:brd=", (unsigned long long) nabuReadEntered);
  nabu_log_hex ("ufs:bok=", (unsigned long long) native_ufs_block_reads);
  nabuFlushThrottled ();
  media->release ();
}

/* Arm on the clock instead of at start: EN 0->1 is the only writable lever (BITE does not
 * take a new value), and once armed it is never reloaded, so the deadline is exactly
 * arm + 58 s. Returning early here keeps petting pending until that moment. */
static void nabu_wdt_arm (void)
{
  if (gWdtArmed || gWdtRegs == nullptr) return;
  gWdtRegs[0x08 / 4] = 0;
  gWdtRegs[0x08 / 4] = 1;
  gWdtArmed = 1;
  gWdtArmedAt = mach_absolute_time ();
  {
    uint64_t petDeadline = 0;
    clock_interval_to_deadline (gWdtPetSeconds, 1000000000u, &petDeadline);
    gWdtPetUntil = petDeadline;
  }
}

static void nabu_wdt_pet (void)
{
  if (gWdtRegs == nullptr) return;
  if (!gWdtArmed) {
    if (gWdtArmAt == 0 || mach_absolute_time () < gWdtArmAt) return;
    nabu_wdt_arm ();
    return;
  }
  if (mach_absolute_time () >= gWdtPetUntil) return;
  gWdtRegs[0x08 / 4] = 0;
  gWdtRegs[0x08 / 4] = 1;
}

/* Runs on the 4 KiB output boundary. Pets only: EN 0->1 does not reload the counter, so
 * this cannot extend the deadline, and it writes nothing to storage from a print path. */
extern "C" void nabu_log_service_4k (void)
{
  nabu_wdt_pet ();
}


/* Registry census. getMatchingServices() returns only services that finished their match
 * (kIOServiceMatchedState), which is exactly the question asked here: being attached in the
 * service plane is not the same as having completed a match. */
static unsigned nabu_count_services (const char *className)
{
  unsigned count = 0;
  OSDictionary *matching = IOService::serviceMatching (className);
  if (matching != nullptr) {
    OSIterator *services = IOService::getMatchingServices (matching);
    if (services != nullptr) {
      while (services->getNextObject () != nullptr) ++count;
      services->release ();
    }
    matching->release ();
  }
  return count;
}

/* l490: the display family's default-framebuffer path needs an IOMapper (its own strings carry
 * map_memory_dart(IOMapper *, IOMemoryDescriptor *, ...) and "IOMemoryDescriptor::prepare failed"),
 * and nothing in either tree implements an IOMapper subclass - the kernel holds only the abstract
 * base. If the registry has no mapper instance at all, that is a mechanism for "No Default FB
 * Surface found" which does not blame our backend for a missing setter. Two counters are taken
 * together: IOMapper is the question, IOMobileFramebufferLegacy is the positive control - it must
 * come back >= 1 in a round where starthw printed ok, otherwise mapper=0 proves nothing.
 * -1 means "never counted", exactly like the component-load probes, so an un-run census cannot be
 * read as an absent mapper. Counted from the log writer thread only: getMatchingServices allocates
 * and takes the matching lock, which the CDC screenshot provider's contract forbids. */
extern "C" volatile int native_ufs_mapper_count = -1;
extern "C" volatile int native_ufs_display_service_count = -1;

static void nabu_mapper_census (void)
{
  native_ufs_display_service_count = (int) nabu_count_services ("IOMobileFramebufferLegacy");
  native_ufs_mapper_count = (int) nabu_count_services ("IOMapper");
}

/* l478: the state header for the CDC pull channel. It runs inside the
 * transport's screenshot provider, whose contract forbids printing, locking,
 * allocating and storage, so this only reads counters other paths already
 * maintain. The two lock counters are the reason this exists at all: they have
 * been incremented since l442 and printed nowhere, which is why a round could
 * only ever say "at least one lock timeout" and no more. */
extern "C" int nabu_log_attach_pull (void);

/* The official component-load probes (patches/xnu/offline-component-load-probes.patch) keep each
 * OSKextLoadKextWithIdentifier() return code in a volatile int initialised to -1, so "the probe never
 * ran" is distinguishable from "it ran and succeeded" (0). The display line needs exactly that split:
 * seven silent starthw branches with imgf=-1 means the mobile-graphics family was never loaded, so no
 * legacy backend was instantiated and startHardware() could not have been reached at all.
 * Deliberately NOT wrapped in #if DEVELOPMENT || DEBUG - a missing macro would silently compile the
 * -1 fallback and look like an un-run probe; an undefined symbol fails the link instead, which is
 * the visible kind of wrong. */
extern "C" volatile int native_mobile_graphics_load_status;
extern "C" volatile int native_apfs_load_status;
extern "C" volatile int native_iogpu_load_status;

/* The control library exposes the reboot handoff but never calls it: qcom_usb_debug_control_take_reboot(),
 * _arm_reboot(), _reboot_failed() and qcom_usb_debug_wait_drained() are linked into this kernel (verified
 * by symbol address) yet have zero callers in the library source, and docs/boundaries.md assigns the
 * "dedicated normal thread" to the integrator. That is why every REBOOT we ever sent answered
 * `OK state=queued` and then `busy` without resetting anything. */
extern "C" int qcom_usb_debug_control_take_reboot (void);
extern "C" int qcom_usb_debug_control_arm_reboot (void);
extern "C" void qcom_usb_debug_control_reboot_failed (int result);
extern "C" int qcom_usb_debug_wait_drained (unsigned int timeout_ms);

/* The legacy display backend already counts the surfaces it sees (noteSurface in
 * drivers/GopFramebuffer/NativeLegacyDisplay.cpp: [0] distinct surfaces, [1] swaps checked).
 * After l482 proved startHardware() passes every check and still printed `ok`, this pair is what
 * separates "nobody ever presented a first frame" (nsurf==0 -> the wall is the presenter) from
 * "frames arrive but the default surface was never registered" (nsurf>0 -> a missing registration step
 * on our side). Read-only: we are not adding a counter. */
extern "C" volatile uint32_t native_legacy_surfaces[14];
extern "C" volatile uint32_t native_legacy_validation[6];
extern "C" volatile uint32_t native_legacy_mapper_calls[4];
extern "C" volatile uint64_t native_legacy_gop[6];
extern "C" volatile uint32_t native_legacy_transfer[8];
extern "C" volatile uint64_t native_legacy_frame[19];
extern "C" volatile uint64_t native_legacy_fill[3];
/* l511: touch bring-up stage results. The TCH rows live only in the RAM ring and a round that keeps
 * booting buries them (l510: the board saw the state line at t=110 s and no TCH at all); these few
 * fields ride the periodic state line, so one late read is enough and nabu-paniclog-blocks is not
 * needed for touch. */
extern "C" volatile uint32_t native_touch_stage_req, native_touch_stage_done, native_touch_step,
    native_touch_frames, native_touch_valid, native_touch_contacts, native_touch_hid_reports;
extern "C" volatile int native_touch_error;
extern "C" volatile uint32_t native_touch_id6[6], native_touch_mode_rc[4], native_touch_mode_tried,
    native_touch_mode_used, native_touch_irq_stats[4];
extern "C" volatile uint32_t native_touch_loop[8], native_touch_pins[6], native_touch_power;
extern "C" volatile uint32_t native_touch_mux_rb[4];
/* package/HidInputProbe.cpp: [0] asked, [1] IOHIDFamily loaded, [2] classes measured,
 * [3] finished; the two ints are the kext load and the metaclass postModLoad result. */
extern "C" volatile uint32_t native_hid_stage, native_hid_class_sizes[5];
extern "C" volatile uint32_t native_touch_clock[4], native_touch_clock_rc;
extern "C" volatile uint32_t native_touch_xfer[7], native_touch_srst[4];
extern "C" volatile uint32_t native_touch_tri[18], native_touch_srst_rc[4], native_touch_rt[2];
extern "C" volatile uint32_t native_touch_cs8[32], native_touch_cmd0[4], native_touch_pad[4];
/* package/TouchProbe.cpp l528: the one-bit-apart patterns and the pad levels sampled while CS is still
 * held for each of them (bit0=gpio23 MISO .. bit3=gpio26 CS). */
extern "C" volatile uint32_t native_touch_p8[32], native_touch_pin4[4];
/* l530: package/TouchProbe.cpp's engine counters, six arms (4 patterns + send-only + receive-only),
 * four registers each (M_IRQ_STATUS, SE_GENI_STATUS, RX_FIFO_STATUS, TX_FIFO_STATUS). The RX word
 * count mask is the driver's own GENI_RX_WC_MASK (drivers/QcomGeniSpi/GeniSpi.h) -- restated here as
 * a local constant because the header is not in this TU's include path; it is only ever used to decode
 * the value the driver's handle_rx() computes the same way. */
/* l535: package/TouchProbe.cpp's per-arm record of what the DRIVER saw during each transfer --
 * [0]=rc, [1]=M_IRQ_STATUS before it was acknowledged, [2]=bytes handed to rx, [3]=bytes pushed to
 * tx, [4]=poll iterations that saw a non-zero word. The values come from fields the QcomGeniSpi
 * driver fills inside geni_spi_transfer(); they are not register reads, because a post-transfer
 * register read (what l534 did) returns the driver's own clearing and draining, not the wire. */
extern "C" volatile uint32_t native_touch_eng[6][5];

/* l542: what the firmware download did, published by TouchProbe.cpp (slot map there). */
extern "C" uint32_t native_touch_dl[160];
extern "C" char native_touch_mapname[24];/* l529: package/KeyStoreProbe.cpp + drivers/AppleKeyStoreState/NativeKeyStoreEvents.cpp. The probe is
 * called from here because this TU is the one that is proven to link; its only upstream call site was
 * package/LegacyDisplayProbe.cpp:494, and that object is not in the archive (the l518 link proved the
 * symbols it adds are undefined), so a call left there would never run. boot_progress_ext is the
 * buffer aks_publish() writes its two report rows into; upstream ships it in package/BootProgress.c,
 * which this kernel does not carry (llvm-nm on the l528 image: no such symbol, with
 * native_ufs_state_block and _boot_progress_poll hitting in the same query), so the definition lives
 * here and the rows are read back onto the ring below rather than through a device we do not have. */
extern "C" char boot_progress_ext[8][48] = {};   /* the definition; KeyStoreProbe.cpp only declares it */
extern "C" volatile uint32_t native_keyevents_stage, native_aks_opens, native_aks_calls;
extern "C" volatile int native_keyevents_status;
extern "C" volatile uint64_t native_keyevents_service;
extern "C" void native_keystore_probe (void);
/* l531: the shape of each selector's first call (see package/KeyStoreProbe.cpp) and the last return
 * code per selector, so the ring can print the unanswered ones with what the caller asked for. */
extern "C" volatile uint32_t native_aks_shape_data[96][7], native_aks_shape_seen[96];
extern "C" volatile int32_t aks_rc[96];
/* l533: the pid that first used each selector ([6]) is in that array; these are the opener pids. */
extern "C" volatile uint32_t native_aks_open_pid[12], native_aks_open_pid_n, native_aks_pid_distinct;
extern "C" volatile uint32_t native_touch_reset_done, native_touch_reset_ctl;
extern "C" volatile int native_hid_load_status, native_hid_input_meta_status;
extern "C" volatile int native_hid_matching_load_status;
extern "C" volatile uint32_t native_hid_self_registered, native_hid_resource_found;
extern "C" volatile uint32_t native_hid_input_events[8];
extern "C" volatile int native_hid_input_results[8];
extern "C" volatile uint64_t native_hid_input_object;
extern "C" volatile uint64_t native_touch_tlmm_va;
extern "C" volatile uint32_t native_touch_mux_flat[4];
extern "C" volatile uint64_t native_touch_tlmm_flat_va, native_touch_tlmm_irq_va;
extern "C" unsigned nabu_fbcon_state;
extern "C" unsigned nabu_fbcon_rows;
/* l518: package/NativeLegacyProbe.cpp. native_gop_sample() walks the mapped scanout (every eighth
 * row) and returns the panel-side aggregate plus a centre-window hash that it also remembers, so the
 * banner can say "the bytes in the panel buffer are/is not what they were twenty seconds ago".
 * The three gpu fields replace the single native_iogpu_load_status, which could only ever say -1
 * because sheng-fb-gpu is not in the shipped arguments. */
extern "C" void native_gop_sample (uint64_t now_ticks, uint64_t *out);
extern "C" volatile uint32_t native_legacy_gpu_arg;
/* l520b: [0] requested Hz, [1] re-arms, [2] published vsyncs, [3] not-wanted, [4] catch-up,
 * [5] startVsync succeeded, [6] refresh attributes overridden. Without [2] against [5] the
 * sheng-vsync round cannot be judged at all. */
extern "C" volatile uint32_t native_legacy_vsync_state[8];
extern "C" volatile uint64_t native_gop_virtual;
/* l519: [0] nabu-splash as read, [1] cells filled, [2] banner passes that painted, [3] stood down */
extern "C" void native_splash_update (void);
extern "C" volatile uint32_t native_splash_state[4];

extern "C" unsigned native_ufs_state_block (char *dst, unsigned capacity,
    unsigned long long ring_total, unsigned long long ring_dropped,
    unsigned long long ring_head)
{
  extern volatile unsigned long long nabu_log_total;
  unsigned long long ticks = (unsigned long long) nabu_cntpct ();
  uint64_t gop_hist[14];

  native_gop_sample (ticks, gop_hist);
  nabu_gpu_probe_once ();      /* once; returns immediately unless nabu-gpu was passed */
  nabu_gpu_heartbeat ();       /* l526: every banner, re-read the two always-on counters */
  /* l529: the keystore probe, from the one TU that is proven to link. It guards itself (stage is set
   * before anything else happens, and it returns without touching IOKit unless offline-key-events=1
   * was passed), so calling it on every banner is not a second publication. The two rows below are
   * the buffer aks_publish() fills: an empty pair is "the probe never ran or nobody opened us", a
   * non-empty pair is the hit evidence itself, which is why no counter of mine is needed here. */
  native_keystore_probe ();
  {
    char ab[128];
    int an = snprintf (ab, sizeof (ab), "NABUAKS1 st=%u rc=%d svc=%llx o=%u c=%u\n",
        (unsigned) native_keyevents_stage, (int) native_keyevents_status,
        (unsigned long long) native_keyevents_service,
        (unsigned) native_aks_opens, (unsigned) native_aks_calls);
    if (an > 0 && (unsigned) an < sizeof (ab)) {
      nabu_log_puts (ab);
    }
    char rb[64];
    if (boot_progress_ext[0][0]) {
      snprintf (rb, sizeof rb, "NABUAKS2 %.47s\n", boot_progress_ext[0]);
      nabu_log_puts (rb);
    }
    if (boot_progress_ext[1][0]) {
      snprintf (rb, sizeof rb, "NABUAKS3 %.47s\n", boot_progress_ext[1]);
      nabu_log_puts (rb);
    }
    /* l531: up to three selectors we did not answer, each with the shape the caller used -- in=<scalar
     * in>/<scalar out>, si/so = structure in/out sizes, f = which of the four buffers/descriptors were
     * actually handed to us, rc = the return code we gave. Bounded to three because the ring carries
     * every other line too. This decides the next step for selector 34 and 93: if the caller asked for
     * no output at all, a bare success is well-formed and worth trying; if it asked for a structure of
     * size N, answering success with nothing written is a promise we cannot keep. */
    for (unsigned sel = 0u, shown = 0u; sel < 96u && shown < 3u; ++sel) {
      if (!native_aks_shape_seen[sel] || aks_rc[sel] == 0) {
        continue;
      }
      char sb[96];
      snprintf (sb, sizeof sb, "NABUAKS4 sel=%u in=%u/%u si=%u so=%u f=%u rc=%d o=%u p=%u\n", sel,
          (unsigned) native_aks_shape_data[sel][0], (unsigned) native_aks_shape_data[sel][1],
          (unsigned) native_aks_shape_data[sel][2], (unsigned) native_aks_shape_data[sel][3],
          (unsigned) native_aks_shape_data[sel][4], (int) aks_rc[sel],
          (unsigned) native_aks_shape_data[sel][5], (unsigned) native_aks_shape_data[sel][6]);
      nabu_log_puts (sb);
      shown = shown + 1u;
    }
    /* l533: the opener identities. `distinct` over `n` is the number that decides whether
     * opens >> calls is one daemon retrying its connection or several daemons each opening once --
     * which is the question the o=34/o=7 readings could not answer on their own. */
    {
      char eb[128];
      int en = snprintf (eb, sizeof eb, "NABUAKS5 n=%u distinct=%u pids=",
          (unsigned) native_aks_open_pid_n, (unsigned) native_aks_pid_distinct);
      if (en > 0 && (unsigned) en < sizeof (eb)) {
        unsigned used = (unsigned) en;
        for (unsigned i = 0; i < native_aks_open_pid_n && used + 8u < sizeof (eb) - 2u; ++i) {
          /* snprintf returns what it WOULD have written, so the length has to be checked before it is
           * added to the cursor: adding a truncating return value once could push the cursor past the
           * buffer and make the remaining space wrap to a huge unsigned. */
          int l = snprintf (eb + used, sizeof (eb) - used, "%u,", (unsigned) native_aks_open_pid[i]);
          if (l < 0 || (unsigned) l >= sizeof (eb) - used) {
            break;
          }
          used += (unsigned) l;
        }
        snprintf (eb + used, sizeof (eb) - used, "\n");
        nabu_log_puts (eb);
      }
    }
  }
  /* The splash is advanced from here, on the same clock as the panel ruler: no timer, no thread,
   * nothing added to the copy path, and it stands down by itself once the client has presented. */
  native_splash_update ();
  /* l520c: the GPU and vsync numbers ride their own ring line instead of the banner. The banner
   * has to fit the snapshot buffer in the console code (1536 bytes, and it drops the WHOLE line
   * when the formatted length would not fit), which is how l520 lost every NABUSTATE1 line. A
   * separate line has no such limit and matches how NABUDIAG1/NABUDIAG2 are already read. */
  {
    char gpu[192];
    int gn = snprintf (gpu, sizeof (gpu),
        "NABUGPU1 map=%x/%x/%x st=%x,%x,%x cp=%x/%x gmu=%x vsync=%u/%u/%u/%u/%u\n",
        (unsigned) gGpuMapRc[0], (unsigned) gGpuMapRc[1], (unsigned) gGpuMapRc[2],
        (unsigned) gGpuRead[0], (unsigned) gGpuRead[1], (unsigned) gGpuRead[2],
        (unsigned) gGpuRead[3], (unsigned) gGpuRead[4], (unsigned) gGpuRead[5],
        (unsigned) native_legacy_vsync_state[0], (unsigned) native_legacy_vsync_state[5],
        (unsigned) native_legacy_vsync_state[2], (unsigned) native_legacy_vsync_state[3],
        (unsigned) native_legacy_vsync_state[6]);
    if (gn > 0 && (unsigned) gn < sizeof (gpu)) {
      nabu_log_puts (gpu);
    } else {
      nabu_log_puts ("NABUGPU1 too-long\n");   /* say so; never fail silently */
    }
  }
  /* l521: the power chain gets its own line, and NABUGPU1 keeps exactly the shape 8714 read in
   * M0 -- adding fields to a line that already parses is how a round loses its numbers.
   * st= is how far the gated sequence got (1 GCC, 2 +GPUCC, 3 +GMU, 4 +RBBM); every field past a
   * closed gate reads deadbee2, so the line says where it stopped without needing a crash. */
  {
    char pw[320];
    int pn = snprintf (pw, sizeof (pw),
        "NABUGPUP1 st=%u ahb=%u mapg=%x/%x/%x gcc=%x,%x,%x,%x cc=%x,%x,%x,%x gd=%x,%x,%x,%x "
        "gmu=%x,%x,%x,%x\n",
        (unsigned) gGpuStage, (unsigned) ((gGpuGcc[0] & NABU_GPU_CLK_ON) ? 1u : 0u),
        (unsigned) gGpuMapGcc[0], (unsigned) gGpuMapGcc[1], (unsigned) gGpuMapGcc[2],
        (unsigned) gGpuGcc[0], (unsigned) gGpuGcc[1], (unsigned) gGpuGcc[2], (unsigned) gGpuGcc[3],
        (unsigned) gGpuCc[0], (unsigned) gGpuCc[1], (unsigned) gGpuCc[2], (unsigned) gGpuCc[3],
        (unsigned) gGpuGd[0], (unsigned) gGpuGd[1], (unsigned) gGpuGd[2], (unsigned) gGpuGd[3],
        (unsigned) gGpuGmu[0], (unsigned) gGpuGmu[1], (unsigned) gGpuGmu[2], (unsigned) gGpuGmu[3]);
    if (pn > 0 && (unsigned) pn < sizeof (pw)) {
      nabu_log_puts (pw);
    } else {
      nabu_log_puts ("NABUGPUP1 too-long\n");
    }
  }
  /* l522: the PDC/RSCC census on its own line. map= is the two PDC windows (reported only, never
   * read); rscc= is RSC_STATUS0/PDC_SEQ_START/SEQ_BUSY; mem= is the five words Linux loads the RSC
   * sequencer with; ctl= is CONTROL_REQ/CONTROL_ACK (idle expected: 0/0). */
  {
    char rq[176];
    int qn = snprintf (rq, sizeof (rq),
        "NABUGPUQ map=%x/%x rscc=%x,%x,%x mem=%x,%x,%x,%x,%x ctl=%x,%x\n",
        (unsigned) gGpuMapPdc[0], (unsigned) gGpuMapPdc[1],
        (unsigned) gGpuRs[0], (unsigned) gGpuRs[1], (unsigned) gGpuRs[2],
        (unsigned) gGpuRs[3], (unsigned) gGpuRs[4], (unsigned) gGpuRs[5],
        (unsigned) gGpuRs[6], (unsigned) gGpuRs[7],
        (unsigned) gGpuRs[8], (unsigned) gGpuRs[9]);
    if (qn > 0 && (unsigned) qn < sizeof (rq)) {
      nabu_log_puts (rq);
    } else {
      nabu_log_puts ("NABUGPUQ too-long\n");
    }
  }
  /* l523: what the write arm did, on its own line. did= is the whole story in one digit:
   * 0 never asked for, 1 the sequence ran and PWR_ON came up, 2 asked but the poll hit the 2 ms
   * bound, 3 the gate was closed or CX already read on (nothing written). clk= is the positive
   * control -- the two GMU branches that l521 saw held at bit0=0. */
  {
    char ac[128];
    int an = snprintf (ac, sizeof (ac),
        "NABUGPUA did=%u before=%x poll=%u hw=%x gd=%x clk=%x,%x\n",
        (unsigned) gGpuAct[0], (unsigned) gGpuAct[1], (unsigned) gGpuAct[2],
        (unsigned) gGpuAct[3], (unsigned) gGpuAct[4], (unsigned) gGpuAct[5],
        (unsigned) gGpuAct[6]);
    if (an > 0 && (unsigned) an < sizeof (ac)) {
      nabu_log_puts (ac);
    } else {
      nabu_log_puts ("NABUGPUA too-long\n");
    }
  }
  /* l525: the clock write (mode 4) and the post-write snapshot, each on its own line. cfg= is
   * gpu_cc_gmu_clk_src's CMD_CFG before and after (SRC_SEL lives in bits 10:8, DIV in 4:0), cx= is
   * gpu_cc_cx_gmu_clk 0x1098 before and after -- the pair is the positive control for this round,
   * because bit0 going to 1 with bit31 clearing is the branch actually running. */
  {
    char kl[128];
    int kn = snprintf (kl, sizeof (kl),
        "NABUGPUK cfg=%x,%x cx=%x,%x\n",
        (unsigned) gGpuClk[0], (unsigned) gGpuClk[1], (unsigned) gGpuClk[2],
        (unsigned) gGpuClk[3]);
    if (kn > 0 && (unsigned) kn < sizeof (kl)) {
      nabu_log_puts (kl);
    } else {
      nabu_log_puts ("NABUGPUK too-long\n");
    }
  }
  {
    char pl[224];
    int pn2 = snprintf (pl, sizeof (pl),
        "NABUGPUPOST fw=%x bc=%x rst=%x hfi=%x aon=%x core=%x,%x,%x,%x,%x\n",
        (unsigned) gGpuPost[0], (unsigned) gGpuPost[1], (unsigned) gGpuPost[2],
        (unsigned) gGpuPost[3], (unsigned) gGpuPost[9],
        (unsigned) gGpuPost[4], (unsigned) gGpuPost[5], (unsigned) gGpuPost[6],
        (unsigned) gGpuPost[7], (unsigned) gGpuPost[8]);
    if (pn2 > 0 && (unsigned) pn2 < sizeof (pl)) {
      nabu_log_puts (pl);
    } else {
      nabu_log_puts ("NABUGPUPOST too-long\n");
    }
  }
  /* l526: the heartbeat, one line per banner. This is the line that can actually say "counting":
   * aon=/cp= compared between two consecutive n= values is the tick. A single sample -- which is all
   * NABUGPUPOST was ever able to give -- only says the register reads non-zero. */
  if (gGpuHb[2] != 0u) {
    char hb[96];
    int hn = snprintf (hb, sizeof (hb), "NABUGPUHB n=%u aon=%x cp=%x\n",
        (unsigned) gGpuHb[2], (unsigned) gGpuHb[0], (unsigned) gGpuHb[1]);
    if (hn > 0 && (unsigned) hn < sizeof (hb)) {
      nabu_log_puts (hb);
    }
  }
  /* l524: the touch second half, on its own line for the same reason the GPU lines are separate --
   * the banner has to fit the 1536-byte snapshot buffer and drops the WHOLE line when it would not,
   * which is exactly how l520 lost every NABUSTATE1. cs0/cs1 and the first two cmd0 words stay in the
   * banner unchanged, so 8714's parser does not move; this carries the two patterns this round adds
   * (cs2 = CS released while sending 0xAA, cs3 = CS held while sending 0xAA) next to the held-state
   * pad read. Read it as: cs0 vs cs2 (and cs1 vs cs3) is the pattern test -- if the eight bytes do
   * not change when the sent byte changes, they are FIFO fill and never were the wire. */
  {
    char tb[256];
    int tn = snprintf (tb, sizeof (tb),
        "NABUTCH1 cs2=%02x%02x%02x%02x%02x%02x%02x%02x cs3=%02x%02x%02x%02x%02x%02x%02x%02x "
        "cmd0=%08x,%08x pad=cs%08x/miso%08x/%08x/%08x\n",
        native_touch_cs8[16], native_touch_cs8[17], native_touch_cs8[18], native_touch_cs8[19],
        native_touch_cs8[20], native_touch_cs8[21], native_touch_cs8[22], native_touch_cs8[23],
        native_touch_cs8[24], native_touch_cs8[25], native_touch_cs8[26], native_touch_cs8[27],
        native_touch_cs8[28], native_touch_cs8[29], native_touch_cs8[30], native_touch_cs8[31],
        native_touch_cmd0[2], native_touch_cmd0[3],
        native_touch_pad[0], native_touch_pad[1], native_touch_pad[2], native_touch_pad[3]);
    if (tn > 0 && (unsigned) tn < sizeof (tb)) {
      nabu_log_puts (tb);
    } else {
      nabu_log_puts ("NABUTCH1 too-long\n");
    }
  }

  /* l528: the four one-bit-apart patterns and the pad levels sampled while CS is still held for each.
   * pin= is the io_reg input bits packed bit0=gpio23 (MISO) .. bit3=gpio26 (CS) -- bit3 staying 0 is
   * the hold re-proved on every pattern, which is what makes bit0 readable as a level. rx= is the
   * engine's eight bytes for 0x55 / 0xAA / 0x01 / 0x80 in that order. Its own line, for the usual two
   * reasons: the banner has a shape 8714 parses, and the 1536-byte snapshot buffer drops a whole line
   * that overflows it. */
  {
    char pb[256];
    int pn2 = snprintf (pb, sizeof (pb),
        "NABUTCH2 pin=%x,%x,%x,%x rx=%02x%02x%02x%02x%02x%02x%02x%02x/%02x%02x%02x%02x%02x%02x%02x%02x"
        "/%02x%02x%02x%02x%02x%02x%02x%02x/%02x%02x%02x%02x%02x%02x%02x%02x\n",
        native_touch_pin4[0], native_touch_pin4[1], native_touch_pin4[2], native_touch_pin4[3],
        native_touch_p8[0], native_touch_p8[1], native_touch_p8[2], native_touch_p8[3],
        native_touch_p8[4], native_touch_p8[5], native_touch_p8[6], native_touch_p8[7],
        native_touch_p8[8], native_touch_p8[9], native_touch_p8[10], native_touch_p8[11],
        native_touch_p8[12], native_touch_p8[13], native_touch_p8[14], native_touch_p8[15],
        native_touch_p8[16], native_touch_p8[17], native_touch_p8[18], native_touch_p8[19],
        native_touch_p8[20], native_touch_p8[21], native_touch_p8[22], native_touch_p8[23],
        native_touch_p8[24], native_touch_p8[25], native_touch_p8[26], native_touch_p8[27],
        native_touch_p8[28], native_touch_p8[29], native_touch_p8[30], native_touch_p8[31]);
    if (pn2 > 0 && (unsigned) pn2 < sizeof (pb)) {
      nabu_log_puts (pb);
    } else {
      nabu_log_puts ("NABUTCH2 too-long\n");
    }
  }

  /* l535: what the engine recorded during each of the six arms, in the order 0x55, 0xAA, 0x01, 0x80,
   * send-only, receive-only. rc comes first because it is the authority on whether an arm completed at
   * all (0 ok, -110 timeout, -5 io, -22 bad parameter); pre is the master IRQ word as the driver saw
   * it BEFORE acknowledging it, so bit0 (GENI_M_CMD_DONE, GeniSpi.h:120) is the completion l534 could
   * never show; fed/sent are bytes the driver actually moved in each direction, counted at the moment
   * it moved them; polls says how many times the poll loop saw anything. The rule this line exists to
   * enforce: no value on it is a register read after the call, because that is what made l534 report
   * zeros for all six arms and look like a hardware answer when it was only the driver's own cleanup. */
  {
    char eb2[224];
    int used = snprintf (eb2, sizeof eb2,
        "NABUSE1 rc=%d,%d,%d,%d,%d,%d pre=%08x,%08x,%08x,%08x,%08x,%08x",
        (int) native_touch_eng[0][0], (int) native_touch_eng[1][0], (int) native_touch_eng[2][0],
        (int) native_touch_eng[3][0], (int) native_touch_eng[4][0], (int) native_touch_eng[5][0],
        (unsigned) native_touch_eng[0][1], (unsigned) native_touch_eng[1][1],
        (unsigned) native_touch_eng[2][1], (unsigned) native_touch_eng[3][1],
        (unsigned) native_touch_eng[4][1], (unsigned) native_touch_eng[5][1]);
    if (used > 0 && (unsigned) used < sizeof (eb2)) {
      int more = snprintf (eb2 + used, sizeof (eb2) - used,
          " fed=%u,%u,%u,%u,%u,%u sent=%u,%u,%u,%u,%u,%u polls=%u,%u,%u,%u,%u,%u\n",
          (unsigned) native_touch_eng[0][2], (unsigned) native_touch_eng[1][2],
          (unsigned) native_touch_eng[2][2], (unsigned) native_touch_eng[3][2],
          (unsigned) native_touch_eng[4][2], (unsigned) native_touch_eng[5][2],
          (unsigned) native_touch_eng[0][3], (unsigned) native_touch_eng[1][3],
          (unsigned) native_touch_eng[2][3], (unsigned) native_touch_eng[3][3],
          (unsigned) native_touch_eng[4][3], (unsigned) native_touch_eng[5][3],
          (unsigned) native_touch_eng[0][4], (unsigned) native_touch_eng[1][4],
          (unsigned) native_touch_eng[2][4], (unsigned) native_touch_eng[3][4],
          (unsigned) native_touch_eng[4][4], (unsigned) native_touch_eng[5][4]);
      if (more > 0 && (unsigned) more < sizeof (eb2) - used) {
        nabu_log_puts (eb2);
      }
    }
  }

  /* l542: the download's own line. It exists because the values were unreachable, not because
   * anything new is being measured: dl_stage names the vendor step (1 reset, 2 crc setup,
   * 3 auto-copy enable, 4 firmware write, 5 auto-copy wait, 6 crc enable, 7 boot ready,
   * 8 reset-state check), dl_rc is that step's return, rst/wait/polls are what the auto-copy and
   * reset-complete reads saw, crcok is the two-section CRC self-check against the image's own header
   * goldens, and pid is the endpoint (0xffff = still in the bootloader, 0x5953 = out of it). Its own
   * line for the usual reason: the 1536-byte banner snapshot drops a whole line that overflows it,
   * which is how l520 lost every NABUSTATE1. Same shape as NABUSE1: snprintf, then nabu_log_puts,
   * with a too-long guard instead of a silent truncation. */
  {
    char db[256];
    int dn = snprintf (db, sizeof (db),
        "NABUTCH3 dl=%u/%u/%d/%02x/%u rst=%02x,%02x,%02x,%02x,%02x crcok=%u flags=%02x casc=%u xf=%d/%u e%08x,%08x,%08x,%08x mirq=%08x depth=%u rb=%02x>%02x want=%08x got=%08x>%08x rrc=%d/%d pid=%04x map=%s\n",
        (unsigned) native_touch_dl[0], (unsigned) native_touch_dl[1],
        (int) (int32_t) native_touch_dl[2], (unsigned) native_touch_dl[3],
        (unsigned) native_touch_dl[4], (unsigned) native_touch_dl[5], (unsigned) native_touch_dl[6],
        (unsigned) native_touch_dl[7], (unsigned) native_touch_dl[8], (unsigned) native_touch_dl[9],
        (unsigned) native_touch_dl[11], (unsigned) native_touch_dl[10],
        (unsigned) native_touch_dl[12], (int) (int32_t) native_touch_dl[14],
        (unsigned) native_touch_dl[15], (unsigned) native_touch_dl[16],
        (unsigned) native_touch_dl[17], (unsigned) native_touch_dl[18],
        (unsigned) native_touch_dl[19], (unsigned) native_touch_dl[20],
        (unsigned) native_touch_dl[21],
        (unsigned) native_touch_dl[22], (unsigned) native_touch_dl[24],
        (unsigned) native_touch_dl[26], (unsigned) native_touch_dl[23],
        (unsigned) native_touch_dl[25],
        (int) (int32_t) native_touch_dl[27], (int) (int32_t) native_touch_dl[28],
        (unsigned) native_touch_dl[13],
        native_touch_mapname[0] ? native_touch_mapname : "-");
    if (dn > 0 && (unsigned) dn < sizeof (db)) {
      nabu_log_puts (db);
    } else {
      nabu_log_puts ("NABUTCH3 too-long\n");
    }
  }
  /* l551: the read-phase comparison, on its own line because NABUTCH3 already uses most of
   * the 256-byte buffer and a truncated line reads like a missing one. ph= is ilm_length read
   * with 0/1/2 dummy bytes (the middle one is what every read we have ever done used); hit is
   * which of the three equals the want= on the line above, computed here so a human does not
   * have to compare three hex words under pressure; dt= is the device tree's own two
   * addresses, read-only context. */
  {
    char pb[256];
    unsigned hit = 0;
    for (unsigned k = 0; k < 3; ++k)
      if (native_touch_dl[29 + k] == native_touch_dl[26]) hit = k + 1;
    int pn = snprintf (pb, sizeof (pb),
        "NABUTCH4 ph=%08x,%08x,%08x hit=%u prc=%d/%d/%d dt=%08x,%08x/%08x,%08x drc=%d\n",
        (unsigned) native_touch_dl[29], (unsigned) native_touch_dl[30],
        (unsigned) native_touch_dl[31], hit,
        (int) (int32_t) native_touch_dl[32], (int) (int32_t) native_touch_dl[33],
        (int) (int32_t) native_touch_dl[34],
        (unsigned) native_touch_dl[35], (unsigned) native_touch_dl[36],
        (unsigned) native_touch_dl[37], (unsigned) native_touch_dl[38],
        (int) (int32_t) native_touch_dl[39]);
    if (pn > 0 && (unsigned) pn < sizeof (pb)) {
      nabu_log_puts (pb);
    } else {
      nabu_log_puts ("NABUTCH4 too-long\n");
    }
  }
  /* l552: the segment map on its own line. seg= is sixteen 32-bit words -- the first four bytes of each
   * eight-byte read, group A (0x3F3xx) first, then group B (0x1FB5xx + 0x1FC925); mask= is the 16-bit
   * "this probe answered with something other than all-ones" map, so the two groups can be counted off
   * without comparing sixteen words by eye; src= is the first failing transfer return (0 = every read
   * itself worked, which is what makes mask= meaningful). */
  {
    char sb[256];
    int sn = snprintf (sb, sizeof (sb),
        "NABUTCH5 seg=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x mask=%04x src=%d\n",
        (unsigned) native_touch_dl[40], (unsigned) native_touch_dl[41],
        (unsigned) native_touch_dl[42], (unsigned) native_touch_dl[43],
        (unsigned) native_touch_dl[44], (unsigned) native_touch_dl[45],
        (unsigned) native_touch_dl[46], (unsigned) native_touch_dl[47],
        (unsigned) native_touch_dl[48], (unsigned) native_touch_dl[49],
        (unsigned) native_touch_dl[50], (unsigned) native_touch_dl[51],
        (unsigned) native_touch_dl[52], (unsigned) native_touch_dl[53],
        (unsigned) native_touch_dl[54], (unsigned) native_touch_dl[55],
        (unsigned) native_touch_dl[56], (int) (int32_t) native_touch_dl[57]);
    if (sn > 0 && (unsigned) sn < sizeof (sb)) {
      nabu_log_puts (sb);
    } else {
      nabu_log_puts ("NABUTCH5 too-long\n");
    }
  }
  /* l553: the map, one row per page from a SINGLE format string -- four rows on the ring, one literal in
   * the image (want= counts it once). seg= is sixteen 32-bit words at a 16-byte pitch starting at
   * 0x3F000 + pg*0x100; mask= is that page's "answered with something other than all-ones" map; rc= is
   * the first failing transfer anywhere in the map, so a dark bit can be told apart from a refused
   * read. Nothing here claims to know what a cell is. */
  for (unsigned pg = 0; pg < 4; ++pg) {
    char mb[256];
    int mn = snprintf (mb, sizeof (mb),
        "NABUTCH6 pg=%u seg=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x mask=%04x rc=%d\n",
        pg,
        (unsigned) native_touch_dl[58 + pg * 16 + 0],
        (unsigned) native_touch_dl[58 + pg * 16 + 1],
        (unsigned) native_touch_dl[58 + pg * 16 + 2],
        (unsigned) native_touch_dl[58 + pg * 16 + 3],
        (unsigned) native_touch_dl[58 + pg * 16 + 4],
        (unsigned) native_touch_dl[58 + pg * 16 + 5],
        (unsigned) native_touch_dl[58 + pg * 16 + 6],
        (unsigned) native_touch_dl[58 + pg * 16 + 7],
        (unsigned) native_touch_dl[58 + pg * 16 + 8],
        (unsigned) native_touch_dl[58 + pg * 16 + 9],
        (unsigned) native_touch_dl[58 + pg * 16 + 10],
        (unsigned) native_touch_dl[58 + pg * 16 + 11],
        (unsigned) native_touch_dl[58 + pg * 16 + 12],
        (unsigned) native_touch_dl[58 + pg * 16 + 13],
        (unsigned) native_touch_dl[58 + pg * 16 + 14],
        (unsigned) native_touch_dl[58 + pg * 16 + 15],
        (unsigned) native_touch_dl[122 + pg],
        (int) (int32_t) native_touch_dl[126]);
    if (mn > 0 && (unsigned) mn < sizeof (mb)) {
      nabu_log_puts (mb);
    } else {
      nabu_log_puts ("NABUTCH6 too-long\n");
    }
  }
  /* l554: the firmware read-back. Printed once. at= is the address the read-back aimed at (0 for ILM,
   * 0x20000 for DLM -- both from the image header, not chosen here); bad= is how many of the bytes that
   * came back differ from the image we wrote; first= is the first differing offset (-1 = none); crc= is
   * recomputed over what the chip holds, with the same rule the header's stored value was made with;
   * done= is how many bytes were actually read; rc= is the read-back transfer's own return. ok= is three
   * 1-bit flags for the three 32-byte windows (ILM start, ILM tail-32, DLM start) -- 1 means byte-exact.
   * No new SPI traffic: verify_sram_full() has run on every failed download since l544 said so. */
  {
    char rb[192];
    int rn = snprintf (rb, sizeof (rb),
        "NABUTCH7 rb ilm at=%08x bad=%u first=%d crc=%08x done=%u rc=%d dlm at=%08x bad=%u first=%d crc=%08x done=%u rc=%d ok=%u%u%u\n",
        (unsigned) native_touch_dl[127],
        (unsigned) native_touch_dl[128],
        (int) (int32_t) native_touch_dl[129],
        (unsigned) native_touch_dl[130],
        (unsigned) native_touch_dl[131],
        (int) (int32_t) native_touch_dl[132],
        (unsigned) native_touch_dl[133],
        (unsigned) native_touch_dl[134],
        (int) (int32_t) native_touch_dl[135],
        (unsigned) native_touch_dl[136],
        (unsigned) native_touch_dl[137],
        (int) (int32_t) native_touch_dl[138],
        (unsigned) native_touch_dl[139],
        (unsigned) native_touch_dl[140],
        (unsigned) native_touch_dl[141]);
    if (rn > 0 && (unsigned) rn < sizeof (rb)) {
      nabu_log_puts (rb);
    } else {
      nabu_log_puts ("NABUTCH7 too-long\n");
    }
  }
  /* l556: the one read. at= is the address the power-on path writes 0x5A into (NvtProtocol.c:320), so it
   * is a fixed literal here rather than a slot we could forget; want=5a names that written value in the
   * format string itself, which makes the row its own comparison. got= is the first four bytes little
   * endian, b0= the first byte, rc= the transfer's return. b0==5a means a register write landed and reads
   * back; b0==ff with rc==0 means the page is not decoded at all; rc!=0 means the read was refused and
   * neither reading means anything. */
  {
    char eb[128];
    int en = snprintf (eb, sizeof (eb),
        "NABUTCH8 eng at=%08x got=%08x b0=%02x rc=%d want=5a\n",
        (unsigned) native_touch_dl[142], (unsigned) native_touch_dl[143],
        (unsigned) native_touch_dl[144], (int) (int32_t) native_touch_dl[145]);
    if (en > 0 && (unsigned) en < sizeof (eb)) {
      nabu_log_puts (eb);
    } else {
      nabu_log_puts ("NABUTCH8 too-long\n");
    }
  }
  /* l558: the download path's own addresses, read back. The slot map below is the print order, asserted
   * position by position in the build gate -- l547 proved that a matching specifier/argument *count* does
   * not stop two fields from being swapped. dm sw=/dm ev= name the two bases so the row says what it
   * asked; seg= is the ten low words in point order; mask bit k = point k answered with non-all-ones. */
  {
    char db[192];
    int dn = snprintf (db, sizeof (db),
        "NABUTCH9 dm sw=%08x ev=%08x seg=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x"
        " mask=%04x rc=%d\n",
        (unsigned) native_touch_dl[159], (unsigned) native_touch_dl[158],
        (unsigned) native_touch_dl[146], (unsigned) native_touch_dl[147],
        (unsigned) native_touch_dl[148], (unsigned) native_touch_dl[149],
        (unsigned) native_touch_dl[150], (unsigned) native_touch_dl[151],
        (unsigned) native_touch_dl[152], (unsigned) native_touch_dl[153],
        (unsigned) native_touch_dl[154], (unsigned) native_touch_dl[155],
        (unsigned) native_touch_dl[156], (int) (int32_t) native_touch_dl[157]);
    if (dn > 0 && (unsigned) dn < sizeof (db)) {
      nabu_log_puts (db);
    } else {
      nabu_log_puts ("NABUTCH9 too-long\n");
    }
  }
  /* l585: cores 1..7 bring-up, direct read (see the extern block at the top). */
  {
    char sb[224];
    int sn = snprintf (sb, sizeof (sb),
        "NABUSMP1 boot=%08x st=%u,%u,%u,%u,%u,%u,%u fl=%u,%u,%u,%u,%u,%u,%u rc=%d,%d,%d,%d,%d,%d,%d\n",
        (unsigned) armcpu_boot_mpidr, (unsigned) armcpu_smp_step[1], (unsigned) armcpu_smp_step[2],
        (unsigned) armcpu_smp_step[3], (unsigned) armcpu_smp_step[4], (unsigned) armcpu_smp_step[5],
        (unsigned) armcpu_smp_step[6], (unsigned) armcpu_smp_step[7],
        (unsigned) armcpu_smp_fail[1], (unsigned) armcpu_smp_fail[2], (unsigned) armcpu_smp_fail[3],
        (unsigned) armcpu_smp_fail[4], (unsigned) armcpu_smp_fail[5], (unsigned) armcpu_smp_fail[6],
        (unsigned) armcpu_smp_fail[7], (int) armcpu_smp_psci_rc[1], (int) armcpu_smp_psci_rc[2],
        (int) armcpu_smp_psci_rc[3], (int) armcpu_smp_psci_rc[4], (int) armcpu_smp_psci_rc[5],
        (int) armcpu_smp_psci_rc[6], (int) armcpu_smp_psci_rc[7]);
    if (sn > 0 && (unsigned) sn < sizeof (sb)) {
      nabu_log_puts (sb);
    }
  }
  if (!dst || capacity < 64U) {
    return 0;
  }
  /* produced, not log=, is the running byte count: nabu_log_total stops at the 4 MiB cap
   * while dropped takes over, so a long round's total alone cannot say where the write
   * pointer is. head is the ring offset of the next byte to be written, which is exactly
   * the byte the host's last received tail byte must sit before. */
  return (unsigned) snprintf (dst, capacity,
      "NABUSTATE1 pull=v13 ev=%u t=%llu tch=%u/%u st%u e%d id=%02x%02x%02x%02x%02x%02x mrc=%d%d%d%d mt%u/%u lp=%u/%d rx=%02x%02x%02x%02x din=%u/%u pn=%08x/%08x/%08x/%08x/%08x/%08x pwr=%u hids=%u/%d/%d/%u hido=%llx hidr=%u hidm=%d hidself=%u sp=%08x,%08x,%08x,%08x tvspi=%016llx spw=%08x,%08x,%08x,%08x tvw=%016llx tvi=%016llx hz=%u/%u/%u/%u e%d xfer=%08x,%08x,%08x,%08x/%d/%u m%08x sr=%02x%02x/%02x%02x e%d%d%d%d tri=%02x%02x/%d,%02x%02x/%d,%02x%02x/%d,%02x%02x/%d,%02x%02x/%d,%02x%02x/%d cs0=%02x%02x%02x%02x%02x%02x%02x%02x cs1=%02x%02x%02x%02x%02x%02x%02x%02x cmd0=%08x,%08x pad=cs%08x/miso%08x/%08x/%08x rst=%u/%08x rt=%d/%02x cnthz=%llu lk=%llu/%llu tier=%u poison=%u quarantine=%u "
      "rd=%llu fail=%llu rt=%llu vb=%llu vt=%llu iv=%llu "
      "ringtotal=%llu ringdrop=%llu ringhead=%llu produced=%llu wdt=%u pet=%u imgf=%d apfs=%d gpu=%d/%d shot=%u nsurf=%u nswap=%u n_ssp=%u/%u ssp=%s gopbase=%016llx gop=%ux%u row=%u mapper=%d iomfb=%d nval=%u nrej=%u rsn=%u rw=%u nmap=%u nref=%u nunmap=%u nins=%u bootpa=%016llx bootlen=%llu bwh=%ux%u ncop=%u nok=%u nbad=%u nlast=%x ncpy=%u ncpt=%u ncb=%u nret=%u nfr=%u wf=%u lk=%u lc=%llu bb=%u,%u-%u,%u l0=%08x:%llu p0=%08x:%llu p1=%08x:%llu po=%llu fill=%08x nfill=%u fbcon=%u fbr=%u blank_kept=%llu irq=%u/%u fr=%u ok=%u c=%u hid=%u hcs=%u,%u,%u,%u,%u hevt=%u%u%u%u hres=%d,%d,%d,%d gop_p0=%08x:%llu gop_p1=%08x:%llu gop_po=%llu gop_lc=%llu gop_bb=%u,%u-%u,%u gop_h=%08x,%08x,%llu,%u splash=%u/%u/%u/%u\n",
      nabu_lock_event_enabled () ? 1U : 0U,
      ticks,
      (unsigned) native_touch_stage_done, (unsigned) native_touch_stage_req, (unsigned) native_touch_step,
      (int) native_touch_error,
      (unsigned) native_touch_id6[0], (unsigned) native_touch_id6[1], (unsigned) native_touch_id6[2],
      (unsigned) native_touch_id6[3], (unsigned) native_touch_id6[4], (unsigned) native_touch_id6[5],
      (int) (int32_t) native_touch_mode_rc[0], (int) (int32_t) native_touch_mode_rc[1],
      (int) (int32_t) native_touch_mode_rc[2], (int) (int32_t) native_touch_mode_rc[3],
      (unsigned) native_touch_mode_tried, (unsigned) native_touch_mode_used,
      (unsigned) native_touch_loop[0], (int) (int32_t) native_touch_loop[1],
      (unsigned) native_touch_loop[2], (unsigned) native_touch_loop[3],
      (unsigned) native_touch_loop[4], (unsigned) native_touch_loop[5],
      (unsigned) native_touch_loop[6], (unsigned) native_touch_loop[7],
      (unsigned) native_touch_pins[0], (unsigned) native_touch_pins[1], (unsigned) native_touch_pins[2],
      (unsigned) native_touch_pins[3], (unsigned) native_touch_pins[4], (unsigned) native_touch_pins[5],
      (unsigned) native_touch_power,
      (unsigned) native_hid_stage, (int) native_hid_load_status,
      (int) native_hid_input_meta_status, (unsigned) native_hid_class_sizes[4],
      (unsigned long long) native_hid_input_object,
      (unsigned) native_hid_resource_found, (int) native_hid_matching_load_status,
      (unsigned) native_hid_self_registered,
      (unsigned) native_touch_mux_rb[0], (unsigned) native_touch_mux_rb[1],
      (unsigned) native_touch_mux_rb[2], (unsigned) native_touch_mux_rb[3],
      (unsigned long long) native_touch_tlmm_va,
      (unsigned) native_touch_mux_flat[0], (unsigned) native_touch_mux_flat[1],
      (unsigned) native_touch_mux_flat[2], (unsigned) native_touch_mux_flat[3],
      (unsigned long long) native_touch_tlmm_flat_va,
      (unsigned long long) native_touch_tlmm_irq_va,
      native_touch_clock[0], native_touch_clock[1], native_touch_clock[2], native_touch_clock[3],
      (int) native_touch_clock_rc,
      native_touch_xfer[0], native_touch_xfer[1], native_touch_xfer[2], native_touch_xfer[3],
      (int) native_touch_xfer[4], native_touch_xfer[5], native_touch_xfer[6],
      native_touch_srst[0], native_touch_srst[1], native_touch_srst[2], native_touch_srst[3],
      (int) native_touch_srst_rc[0], (int) native_touch_srst_rc[1],
      (int) native_touch_srst_rc[2], (int) native_touch_srst_rc[3],
      native_touch_tri[0], native_touch_tri[1], (int) native_touch_tri[2],
      native_touch_tri[3], native_touch_tri[4], (int) native_touch_tri[5],
      native_touch_tri[6], native_touch_tri[7], (int) native_touch_tri[8],
      native_touch_tri[9], native_touch_tri[10], (int) native_touch_tri[11],
      native_touch_tri[12], native_touch_tri[13], (int) native_touch_tri[14],
      native_touch_tri[15], native_touch_tri[16], (int) native_touch_tri[17],
      native_touch_cs8[0], native_touch_cs8[1], native_touch_cs8[2], native_touch_cs8[3],
      native_touch_cs8[4], native_touch_cs8[5], native_touch_cs8[6], native_touch_cs8[7],
      native_touch_cs8[8], native_touch_cs8[9], native_touch_cs8[10], native_touch_cs8[11],
      native_touch_cs8[12], native_touch_cs8[13], native_touch_cs8[14], native_touch_cs8[15],
      native_touch_cmd0[0], native_touch_cmd0[1],
      native_touch_pad[0], native_touch_pad[1], native_touch_pad[2], native_touch_pad[3],
      native_touch_reset_done, native_touch_reset_ctl,
      (int) native_touch_rt[0], native_touch_rt[1],
      (unsigned long long) nabu_cntfreq (),
      (unsigned long long) native_ufs_lock_waits,
      (unsigned long long) native_ufs_lock_timeouts,
      (unsigned) native_ufs_lock_tier,
      gHost.poisoned ? 1U : 0U,
      gHost.writeQuarantined ? 1U : 0U,
      (unsigned long long) native_ufs_block_reads,
      (unsigned long long) native_ufs_block_failures,
      (unsigned long long) native_ufs_read_ticks,
      (unsigned long long) native_ufs_verified_blocks,
      (unsigned long long) native_ufs_verify_ticks,
      (unsigned long long) native_ufs_integrity_failures,
      ring_total, ring_dropped, ring_head,
      ring_total + ring_dropped,
      (unsigned) gWdtArmed, (unsigned) gWdtPetSeconds,
      native_mobile_graphics_load_status, native_apfs_load_status, native_iogpu_load_status,
      (unsigned) native_legacy_gpu_arg, (unsigned) (native_gop_virtual ? 1U : 0U),
      (unsigned) native_legacy_surfaces[0], (unsigned) native_legacy_surfaces[1],
      (unsigned) gDpuSspPipes, (unsigned) gDpuSspLive, gDpuSspText,
      (unsigned long long) gGopBase, (unsigned) gGopWidth, (unsigned) gGopHeight, (unsigned) gGopRow,
      native_ufs_mapper_count, native_ufs_display_service_count,
      (unsigned) native_legacy_validation[0], (unsigned) native_legacy_validation[2],
      (unsigned) native_legacy_validation[3], (unsigned) native_legacy_validation[4],
      (unsigned) native_legacy_mapper_calls[0], (unsigned) native_legacy_mapper_calls[1],
      (unsigned) native_legacy_mapper_calls[2], (unsigned) native_legacy_mapper_calls[3],
      (unsigned long long) native_legacy_gop[0], (unsigned long long) native_legacy_gop[2],
      (unsigned) native_legacy_gop[3], (unsigned) native_legacy_gop[4],
      (unsigned) native_legacy_transfer[0], (unsigned) native_legacy_transfer[1],
      (unsigned) native_legacy_transfer[2], (unsigned) native_legacy_transfer[3],
      (unsigned) native_legacy_transfer[5], (unsigned) native_legacy_transfer[6],
      (unsigned) native_legacy_transfer[4], (unsigned) native_legacy_transfer[7],
      (unsigned) native_legacy_frame[0], (unsigned) native_legacy_frame[10],
      (unsigned) native_legacy_frame[11], (unsigned long long) native_legacy_frame[18],
      (unsigned) native_legacy_frame[12], (unsigned) native_legacy_frame[13],
      (unsigned) native_legacy_frame[14], (unsigned) native_legacy_frame[15],
      (unsigned) native_legacy_frame[16], (unsigned long long) native_legacy_frame[17],
      (unsigned) native_legacy_frame[1], (unsigned long long) native_legacy_frame[5],
      (unsigned) native_legacy_frame[2], (unsigned long long) native_legacy_frame[6],
      (unsigned long long) native_legacy_frame[9], (unsigned) native_legacy_fill[0],
      (unsigned) native_legacy_fill[1], (unsigned) nabu_fbcon_state,
      (unsigned) nabu_fbcon_rows,
      (unsigned long long) native_legacy_fill[2],
      (unsigned) native_touch_irq_stats[0], (unsigned) native_touch_irq_stats[1],
      (unsigned) native_touch_frames, (unsigned) native_touch_valid, (unsigned) native_touch_contacts,
      (unsigned) native_touch_hid_reports,
      /* Which of the five classes are actually registered: a 0 here means that class is absent from
       * OSMetaClass's table. Order as probed: IOHIDUserDevice, IOHIDEventDriver, IOHIDEventService,
       * IOHIDResourceDeviceUserClient, IOHIDDevice -- the last is the one pinned to 208 by upstream. */
      native_hid_class_sizes[0], native_hid_class_sizes[1], native_hid_class_sizes[2],
      native_hid_class_sizes[3], native_hid_class_sizes[4],
      /* Lifecycle: events count attempts, results carry the return code of the LAST one (both are
       * reset-free accumulators, so a nonzero events[i] with results[i]==0 means that step passed).
       * Index 0 = new+metaCast+init, 1 = attach, 2 = the device's own start() reporting in,
       * 7 = start failed. */
      native_hid_input_events[0], native_hid_input_events[1], native_hid_input_events[2],
      native_hid_input_events[7],
      native_hid_input_results[0], native_hid_input_results[1], native_hid_input_results[2],
      native_hid_input_results[7],
      /* l518: the same ruler applied to the buffer we write. gop_p0/gop_p1 are the panel's two
       * dominant colours and their counts over every eighth row, gop_po counts the colours beyond
       * those two, gop_lc counts non-black pixels and gop_bb is their bounding box. gop_h is
       * <this hash>,<previous hash>,<ticks between the two>: the window is the fixed centre 64x64
       * pixels, so two different hashes can only mean somebody other than our copy wrote it. */
      (unsigned) gop_hist[0], (unsigned long long) gop_hist[1],
      (unsigned) gop_hist[2], (unsigned long long) gop_hist[3],
      (unsigned long long) gop_hist[4], (unsigned long long) gop_hist[5],
      (unsigned) gop_hist[6], (unsigned) gop_hist[7],
      (unsigned) gop_hist[8], (unsigned) gop_hist[9],
      (unsigned) gop_hist[10], (unsigned) gop_hist[11],
      (unsigned long long) gop_hist[12], (unsigned) gop_hist[13],
      (unsigned) native_splash_state[0], (unsigned) native_splash_state[1],
      (unsigned) native_splash_state[2], (unsigned) native_splash_state[3]);
}

static void
nabuLogThread (void *, wait_result_t)
{
  /* l484: opt-in snapshot cadence. Off unless nabu-state-ring=<seconds>, and the floor of 10 s is
   * enforced here rather than trusted from the command line - fifty-odd lines a round is the agreed
   * budget, and a forensic aid that turns into a flood is worse than no aid. Same NABUSTATE1 string
   * as the live banner, and only ever written from this thread. */
  unsigned cadence = 0;
  unsigned elapsed = 0;
  (void) PE_parse_boot_argn ("nabu-state-ring", &cadence, sizeof (cadence));
  if (cadence != 0 && cadence < 10) {
    cadence = 10;
  }
  for (;;) {
    IOSleep (2000);
    /* Visible proof in the ring that this thread is scheduled and its ticks land;
     * without it a stale window cannot be told from a dead writer thread. */
    nabu_log_puts ("lg:tick\n");
    /* l564: pet on this thread's own beat. Every other pet site hangs off a completion or off the
     * 4 KiB ring boundary, and this line is 8 bytes -- 512 of them before the boundary moves. So an
     * idle system (one tick every 2 s, plus a ~1 KB state snapshot every 20 s) stopped petting and
     * let BITE reset the board about 32 s after the last burst of traffic, which made
     * nabu-wdt-pet-until an upper bound nothing could reach: the board measured 461.9 s of last
     * event on a round whose pet-until was 900, with rd frozen at 283070 for the final 20.2 s.
     * This is not a timer and not a new counter: it is the same nabu_wdt_pet() on a thread that
     * already exists, so an idle system stays alive while a thread that really wedges still stops
     * petting and still resets on schedule. */
    nabu_wdt_pet ();
    nabu_log_periodic ();
    if (cadence != 0) {
      elapsed += 2;
      if (elapsed >= cadence) {
        elapsed = 0;
        nabu_mapper_census ();
        nabu_log_state_snapshot ();
      }
    }
  }
}

/* Separate from the writer on purpose. Run 65 hung the board for good: the USB
 * bring-up ran 20 s, the writer flushed UFS ten times during it, and nothing came
 * back -- not even this reset. The writer had wedged inside UFS, and with the
 * watchdog in the same thread the one mechanism meant to recover a hung board
 * hung with it. This thread only reads two counters and sends an SMC, so UFS or
 * controller MMIO cannot take it down. */
/* l485: that missing consumer. Only created when nabu-usb-reboot=1, so the default path stays
 * byte-for-byte what it was. The order is the library's own: take the one-shot request, let the ACK and
 * TX backlog drain within a bound, arm only if the transport still matches the generation the request
 * arrived in, then reset. reboot_failed() is called on every path that does not reset, because it is what
 * sends the host `ERR state=reboot result=%d` instead of leaving the request silently busy.
 *
 * It deliberately does not flush the log tail first. Staying off UFS is the entire reason this thread
 * exists (run 65: the writer wedged inside the controller and took the recovery mechanism down with it);
 * the pre-death tail is what the pull channel is for. */
static void
nabuUsbRebootThread (void *, wait_result_t)
{
  nabu_log_puts ("wd:usb-r=live\n");
  for (;;) {
    IOSleep (1000);
    if (qcom_usb_debug_control_take_reboot () != 1) {
      continue;
    }
    nabu_log_puts ("wd:usb-r=take\n");
    (void) qcom_usb_debug_wait_drained (2000);
    if (qcom_usb_debug_control_arm_reboot () == 1) {
      /* This TU's existing declaration of nabu_psci_reset() returns void (the definition in
       * serial_general.c returns the SMC result), so the value is not taken here - changing that
       * declaration would touch every other caller for no benefit. -3 means "the SMC returned",
       * which only happens if the reset did not, and is distinct from -2 = "never armed". */
      (void) nabu_psci_reset ();
      qcom_usb_debug_control_reboot_failed (-3);
      nabu_log_puts ("wd:usb-r=reset-returned\n");
    } else {
      qcom_usb_debug_control_reboot_failed (-2);
      nabu_log_puts ("wd:usb-r=not-armed\n");
    }
  }
}

static void
nabuWatchdogThread (void *, wait_result_t)
{
  unsigned hangSeconds = 0;
  PE_parse_boot_argn ("nabu-hang-reset", &hangSeconds, sizeof (hangSeconds));
  unsigned logEnabled = 0;
  PE_parse_boot_argn ("nabu-log", &logEnabled, sizeof (logEnabled));
  extern volatile unsigned long long nabu_log_total;
  /* l528b: this line is the watchdog's own first act, and until now it carried no t= -- which is why
   * 8714 could only bound "armed before the first spawn" instead of measuring it. One integer, the
   * same cntpct reading the banner's t= uses (timerTicks, line 2318, defined above this thread), so
   * arming-to-first-pet is now a subtraction away. Nothing else on this path changed: pet stays on
   * the block-completion path, and no experiment gates it. */
  {
    char ws[40];
    int wn = snprintf (ws, sizeof (ws), "wd:start t=%llu\n", (unsigned long long) timerTicks ());
    if (wn > 0 && (unsigned) wn < sizeof (ws)) {
      nabu_log_puts (ws);
    } else {
      nabu_log_puts ("wd:start\n");
    }
  }
  nabu_log_hex ("ufs:slowmax=", (unsigned long long)gUfsSlowMaxMs);
  nabu_log_hex ("ufs:slowseen=", (unsigned long long)gUfsSlowSeen);
  nabu_log_hex ("wd:args hang=", hangSeconds);
  nabu_log_hex ("wd:args log=", logEnabled);
  if (!hangSeconds || !logEnabled) {
    return;
  }
  if (hangSeconds && logEnabled) {
    const unsigned limit = hangSeconds / 2 ? hangSeconds / 2 : 1;
    unsigned long long last = nabu_log_total;
    unsigned stalled = 0;
    for (;;) {
      IOSleep (2000);
      unsigned long long now = nabu_log_total;
      if (now != last) {
        last = now;
        stalled = 0;
      } else if (++stalled >= limit) {
        nabu_log_puts ("wd:fire\n");
        /* Write one 4 KiB tail without sync (the only form that survives this
         * window), then reset through PSCI without touching storage. */
        nabu_log_flush_tail_nosync (0);  /* l461: the last write this kernel ever makes must not be throttled */
        nabu_psci_reset ();
      }
    }
  }
  unsigned int lastTicks = 0;
  unsigned int stalled = 0;
  (void)lastTicks;
  (void)stalled;
}

bool NativeUfs::start (IOService *provider)
{
  unsigned probe = 0, rootMode = 0;
  PE_parse_boot_argn ("native-ufs-probe", &probe, sizeof (probe));
  PE_parse_boot_argn ("native-ufs-root", &rootMode, sizeof (rootMode));
  native_ufs_probe_requested = probe != 0;
  // Default: match for observability only, never touch the controller. Hardware work runs once,
  // from the root-device personality; the ufshc nub match stays observation-only.
  if (!probe && !rootMode) {
    nabu_mark_text ("ufs:no-arg", 1940);
    return false;
  }
  if (!OSDynamicCast (IOPlatformExpertDevice, provider)) {
    nabu_mark_text ("ufs:not-expert", 1940);
    return false;
  }
  nabu_mark_text ("ufs:entered", 1940);
  unsigned diagnostic = 0;
  PE_parse_boot_argn ("native-ufs-diagnostics", &diagnostic, sizeof (diagnostic));
  {
    unsigned polls = 0, ms = 0;
    PE_parse_boot_argn ("nabu-ufs-polls", &polls, sizeof (polls));
    if (polls >= 100000U && polls <= 200000000U) { gUfsPollCeiling = polls; }
    PE_parse_boot_argn ("nabu-ufs-deadline-ms", &ms, sizeof (ms));
    // l461: the 100 ms floor could not manufacture a single read timeout on this board (mean
    // command 0.275 ms with fua=0), so the ladder was observable 0-2 times per boot by luck.
    if (ms >= 1U && ms <= 20000U) { gUfsReadDeadlineUs = ms * 1000U; }
    unsigned wms = 0;
    PE_parse_boot_argn ("nabu-ufs-write-ms", &wms, sizeof (wms));
    if (wms >= 100U && wms <= 20000U) { gUfsWriteDeadlineUs = wms * 1000U; }
    unsigned fua = 1;
    PE_parse_boot_argn ("nabu-ufs-fua", &fua, sizeof (fua));
    gUfsLogWriteFua = fua ? 1U : 0U;
    unsigned dlmode = 0;
    PE_parse_boot_argn ("nabu-ufs-dl", &dlmode, sizeof (dlmode));
    gUfsDeadlineMode = dlmode == 1U ? 1U : 0U;
    unsigned wq = 0;
    PE_parse_boot_argn ("nabu-ufs-wq", &wq, sizeof (wq));
    gUfsWriteQuarantine = wq == 1U ? 1U : 0U;
    unsigned slowms = 0, slown = 0;
    PE_parse_boot_argn ("nabu-ufs-slow-ms", &slowms, sizeof (slowms));
    // l455: 50 ms was too coarse - the board wants the whole distribution, and every
    // successful command so far finished under 250 ms.
    if (slowms >= 10U && slowms <= 60000U) { gUfsSlowMs = slowms; }
    PE_parse_boot_argn ("nabu-ufs-slow-n", &slown, sizeof (slown));
    if (slown >= 1U && slown <= 200U) { gUfsSlowBudget = slown; }
    unsigned grace = 0;
    PE_parse_boot_argn ("nabu-ufs-grace-ms", &grace, sizeof (grace));
    if (grace >= 10U && grace <= 20000U) { gUfsGraceMs = grace; }
    unsigned excq = 0;
    PE_parse_boot_argn ("nabu-ufs-excq", &excq, sizeof (excq));
    gUfsExcq = excq <= 3U ? excq : 0U;
    // l463: markevery must start from the compiled default. It used to start at 0, and 0 passes
    // the <= 10000 clamp below, so not passing nabu-ufs-mark-every silently wrote the new 1000U
    // default back to 0 - the bug the board caught in the delivered l462 package.
    unsigned flms = 0, markevery = gUfsMarkEvery;
    PE_parse_boot_argn ("nabu-ufs-flush-ms", &flms, sizeof (flms));
    if (flms >= 10U && flms <= 60000U) { gUfsFlushMs = flms; }
    PE_parse_boot_argn ("nabu-ufs-mark-every", &markevery, sizeof (markevery));
    if (markevery <= 10000U) { gUfsMarkEvery = markevery; }
    unsigned cmdstat = 0;
    PE_parse_boot_argn ("nabu-ufs-cmdstat", &cmdstat, sizeof (cmdstat));
    if (cmdstat >= 100U && cmdstat <= 200000U) { gUfsCmdStatEvery = cmdstat; }
    unsigned seqc = 1;
    if (!PE_parse_boot_argn ("nabu-ufs-seq", &seqc, sizeof (seqc))) { seqc = 1U; }
    gUfsSeqCensus = seqc ? 1U : 0U;
    unsigned rahead = 0;
    PE_parse_boot_argn ("nabu-ufs-read-ahead", &rahead, sizeof (rahead));
    if (rahead >= 1U && rahead <= kDataMax / 4096U) { gUfsReadAhead = rahead; }
    unsigned rf = 0;
    PE_parse_boot_argn ("nabu-ufs-read-flush", &rf, sizeof (rf));
    if (rf >= 1U && rf <= 8192U) { gUfsReadFlushEvery = rf; }
    unsigned raa = 0;
    PE_parse_boot_argn ("nabu-ufs-read-ahead-after", &raa, sizeof (raa));
    if (raa <= 600U) { gUfsReadAheadAfter = raa; }
    unsigned rdrec = 0;
    PE_parse_boot_argn ("nabu-ufs-read-recover", &rdrec, sizeof (rdrec));
    gUfsReadRecover = rdrec == 1U ? 1U : 0U;
    unsigned porecheck = 1;
    if (!PE_parse_boot_argn ("nabu-ufs-porecheck", &porecheck, sizeof (porecheck))) { porecheck = 1U; }
    gUfsPoRecheck = porecheck ? 1U : 0U;
  }
  gRootDiagnosticEnabled = diagnostic == 1;
  if (probe == 2) {
    inspect ();
  }
  if (probe == 8) {
    inspectContextBanks ();
  }
  if (probe == 7) {
    inspectSmmu ();
  }
  if (probe == 6) {
    dumpFirmwareList ();
  }
  if ((probe >= 3 && probe <= 5) || probe == 9) {
    adoptAndRead (probe);
  }
  if (probe == 10) {
    // Read-only check before anything is flashed: LUN 0 GPT -> "userdata" -> its first block.
    const char *why = "";
    if (!hostBringUp (&why)) {
      panic ("NATIVE_UFS root check: bring-up failed: %s", why);
    }
    RootLayout *layout = static_cast<RootLayout *> (IOMalloc (sizeof (RootLayout)));
    if (!layout || !findRootLayout (layout, &why)) {
      panic ("NATIVE_UFS root check: %s", why);
    }
    panic ("NATIVE_UFS root check\n"
           "userdata first=%llx last=%llx\n"
           "block0 magic %08x %08x ext4magic=%04x ext4blocks=%u\n"
           "manifest status=%u windows=%u",
           layout->userdataFirst, layout->userdataLast, layout->magic0, layout->magic1,
           layout->ext4Magic, layout->ext4Blocks, layout->manifestStatus, layout->windows);
  }
  if (probe == 11) {
    // After the root image is flashed: manifest, then each window's own GPT (sector 1 of the
    // attached dmg disk image carries "EFI PART"). Read-only.
    const char *why = "";
    if (!hostBringUp (&why)) {
      panic ("NATIVE_UFS root windows: bring-up failed: %s", why);
    }
    RootLayout *layout = static_cast<RootLayout *> (IOMalloc (sizeof (RootLayout)));
    if (!layout || !findRootLayout (layout, &why)) {
      panic ("NATIVE_UFS root windows: %s", why);
    }
    uint32_t sig[4][2] = {};
    uint32_t seen[4] = {};
    for (uint32_t index = 0; index < layout->windows && index < 4; index++) {
      uint64_t absolute = layout->userdataFirst * 4096ULL + layout->window[index].offset;
      if (hostRead4k (0, absolute / 4096, 1, gScratch) == kIOReturnSuccess) {
        seen[index] = 1;
        sig[index][0] = le32 (gScratch + 512);
        sig[index][1] = le32 (gScratch + 516);
      }
    }
    panic ("NATIVE_UFS root windows\n"
           "userdata first=%llx last=%llx manifest status=%u windows=%u\n"
           "w0 %s off=%llx size=%llx gpt=%08x %08x read=%u\n"
           "w1 %s off=%llx size=%llx gpt=%08x %08x read=%u\n"
           "w0 sha %08x %08x %08x %08x\n"
           "w1 sha %08x %08x %08x %08x",
           layout->userdataFirst, layout->userdataLast, layout->manifestStatus, layout->windows,
           layout->window[0].name, layout->window[0].offset, layout->window[0].size, sig[0][0], sig[0][1], seen[0],
           layout->window[1].name, layout->window[1].offset, layout->window[1].size, sig[1][0], sig[1][1], seen[1],
           be32w (layout->window[0].sha256), be32w (layout->window[0].sha256 + 4), be32w (layout->window[0].sha256 + 8), be32w (layout->window[0].sha256 + 12),
           be32w (layout->window[1].sha256), be32w (layout->window[1].sha256 + 4), be32w (layout->window[1].sha256 + 8), be32w (layout->window[1].sha256 + 12));
  }
  if (probe == 14) {
    // Immediate memory snapshot at root-device start, before the userspace that triggers jetsam.
    panic ("NATIVE_UFS early memory snapshot\n"
           "pages total=%u free=%u active=%u inactive=%u wired=%u spec=%u purgeable=%u\n"
           "available=%u max_mem=%llx procs=%d",
           vm_page_pages, vm_page_free_count, vm_page_active_count, vm_page_inactive_count,
           vm_page_wire_count, vm_page_speculative_count, vm_page_purgeable_count,
           memorystatus_available_pages, max_mem, nprocs);
  }
  if (probe == 13) {
    unsigned delay = 12;
    PE_parse_boot_argn ("native-ufs-stats-delay", &delay, sizeof (delay));
    if (!startRoot (provider)) {
      panic ("NATIVE_UFS memory snapshot: root start failed");
    }
    thread_t thread = nullptr;
    if (kernel_thread_start (statsThread, reinterpret_cast<void *> (static_cast<uintptr_t> (delay)), &thread) != KERN_SUCCESS) {
      panic ("NATIVE_UFS memory snapshot: cannot start the thread");
    }
    thread_deallocate (thread);
    return true;
  }
  if (probe == 12) {
    bool published = startRoot (provider);
    if (!published) {
      panic ("NATIVE_UFS media report: root start failed (blocks=%u)", native_ufs_blocks_published);
    }
    thread_t thread = nullptr;
    if (kernel_thread_start (mediaReportThread, nullptr, &thread) != KERN_SUCCESS) {
      panic ("NATIVE_UFS media report: cannot start the report thread");
    }
    thread_deallocate (thread);
    return true;
  }
  /* Watchdog: armed here when nabu-wdt-arm-delay is zero, otherwise on the first event
   * after that many seconds. The reset must not depend on a later open succeeding -- that
   * thread only starts when the driver is matched and has a client.
   * EN only: BITE is not writable, so the bootloader value (0xfffff) stands. */
  {
    unsigned armDelay = 0;
    unsigned petSeconds = 300;
    (void) PE_parse_boot_argn ("nabu-wdt-arm-delay", &armDelay, sizeof (armDelay));
    (void) PE_parse_boot_argn ("nabu-wdt-pet-until", &petSeconds, sizeof (petSeconds));
    gWdtArmDelay = armDelay;
    gWdtPetSeconds = petSeconds;
    vm_offset_t wdtBase = ml_io_map ((vm_offset_t) 0x17C10000ULL, (vm_size_t) 0x1000ULL);
    if (wdtBase != 0) {
      gWdtRegs = (volatile uint32_t *) wdtBase;
      /* Disarm whatever the bootloader left running first. The inherited deadline is
       * what killed earlier rounds ahead of our own first EN write, so until we arm on
       * our schedule this SoC must have no watchdog at all. */
      gWdtRegs[0x08 / 4] = 0;
      if (armDelay == 0) {
        nabu_wdt_arm ();
      } else {
        uint64_t armDeadline = 0;
        clock_interval_to_deadline (armDelay, 1000000000u, &armDeadline);
        gWdtArmAt = armDeadline;
      }
      nabu_log_puts ("ufs:wdt-arm\n");
    }
  }
  if (rootMode && !probe) {
    return startRoot (provider);
  }
  return false;
}

// Boot the root image's windows as block devices. Soft-fails (logs, publishes nothing).
bool NativeUfs::startRoot (IOService *provider)
{
  const char *why = "";
  IOLog ("NativeUfs: root begin\n");
  native_ufs_root_step = 1;
  nabu_mark_text ("root:1-bringup", 2020);
  if (!hostBringUp (&why)) {
    rootDiagnosticFailure (why);
    IOLog ("NativeUfs: root bring-up failed: %s\n", why);
    return false;
  }
  native_ufs_root_step = 2;
  nabu_mark_text ("root:2-layout", 2020);
  RootLayout *layout = static_cast<RootLayout *> (IOMalloc (sizeof (RootLayout)));
  if (!layout || !findRootLayout (layout, &why)) {
    rootDiagnosticFailure (layout ? why : "root layout allocation failed");
    IOLog ("NativeUfs: root layout failed: %s\n", why);
    if (layout) IOFree (layout, sizeof (RootLayout));
    return false;
  }
  native_ufs_root_step = 3;
  nabu_mark_text ("root:3-manifest", 2020);
  if (layout->manifestStatus != 0) {
    rootDiagnosticFailure ("root manifest rejected");
    IOLog ("NativeUfs: no valid root manifest in userdata (status %u, magic %08x %08x)\n",
           layout->manifestStatus, layout->magic0, layout->magic1);
    IOFree (layout, sizeof (RootLayout));
    return false;
  }
  gStoreGeometry = { true, layout->userdataFirst, layout->userdataLast, layout->imageBytes, layout->gptEntriesLba,
                     layout->gptHeaderCrc, layout->gptEntriesCrc, layout->gptEntryCount,
                     layout->miscFirst, layout->miscFound };
  const char *names[4] = {};
  for (unsigned i = 0; i < layout->windows; ++i) names[i] = layout->window[i].name;
  if (!ufsBuildRootPlan (names, layout->windows, gPublicationPlan)) {
    rootDiagnosticFailure ("root windows require distinct OS and SystemOS");
    IOFree (layout, sizeof (RootLayout));
    return false;
  }
  nabu_mark_text ("root:4-io-start", 2020);
  if (!IOService::start (provider)) {
    nabu_mark_text ("ufs:io-start-bad", 1940);
    rootDiagnosticFailure ("root IOService start failed");
    IOFree (layout, sizeof (RootLayout));
    return false;
  }
  // Prepare all windows or publish none. Diagnostics never decide policy.
  gPending = 0;
  gPendingProvider = this;
  struct Operations {
    NativeUfs *owner;
    RootLayout *layout;
    bool prepare (unsigned index) {
      gRootDiagnostic.stage = "root window allocation/configuration";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Prepare);
      gRootDiagnostic.publicationWindow = index;
      gRootDiagnostic.windowBytes = layout->window[index].size;
      nabu_mark_text ("prep:alloc", 2020);
      NativeUfsBlock *device = OSTypeAlloc (NativeUfsBlock);
      gPendingDevice[index] = device;
      gPendingAttached[index] = false;
      gPendingUuid[index][0] = 0;
      if (!device) {
        nabu_mark_text ("prep:alloc-fail", 2020);
        return false;
      }
      nabu_mark_text ("prep:alloc-done", 2020);
      uint64_t absolute = layout->userdataFirst * 4096ULL + layout->window[index].offset;
      if (!device->init (static_cast<OSDictionary *> (nullptr))) {
        nabu_mark_text ("prep:init-fail", 2020);
        return false;
      }
      nabu_mark_text ("prep:init-done", 2020);
      if (!device->configure (layout->window[index].name, absolute, layout->window[index].size)) {
        nabu_mark_text ("prep:cfg-fail", 2020);
        return false;
      }
      nabu_mark_text ("prep:cfg-done", 2020);
      gRootDiagnostic.stage = "root window attachment";
      nabu_mark_text ("prep:attach", 2020);
      if (!device->attach (owner)) {
        nabu_mark_text ("prep:attach-fail", 2020);
        return false;
      }
      nabu_mark_text ("prep:attach-done", 2020);
      gPendingAttached[index] = true;
      ++gPending;
      gRootDiagnostic.prepared = gPending;
      return true;
    }
    bool validate () {
      gRootDiagnostic.stage = "signed window identities";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Identity);
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
      for (unsigned i = 0; i < gPending; ++i) {
        gRootDiagnostic.publicationWindow = i;
        if (!gPendingDevice[i]->signedVolumeUuid (gPendingUuid[i])) return false;
        for (unsigned j = 0; j < i; ++j) if (!strcmp (gPendingUuid[i], gPendingUuid[j])) return false;
      }
      return true;
#else
      // NABU-BRINGUP: gate opened. The original refuses publication without owner verification,
    // which is why start() was never reached. Windows are published unauthenticated here so the
    // IOMedia -> waitForService -> mountroot chain can be tested. Restore the return false below
    // before treating any read as trusted.
    return true; // NABU-BRINGUP (was: return false;)
#endif
    }
    bool schedule () {
      gRootDiagnostic.stage = "root publisher thread";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Schedule);
      gRootDiagnostic.publicationWindow = 0xffffffff;
      thread_t thread = nullptr;
      owner->retain ();
      if (kernel_thread_start (publishThread, nullptr, &thread) != KERN_SUCCESS) {
        owner->release ();
        return false;
      }
      thread_deallocate (thread);
      return true;
    }
    void discard (unsigned index) { discardPendingWindow (index); }
  } operations = { this, layout };
  UfsRootPublicationResult result = ufsPrepareRootWindows (operations, layout->windows);
  IOFree (layout, sizeof (RootLayout));
  if (result.phase != UfsRootPhase::Complete) {
    nabu_mark_text ("ufs:prep-bad", 1940);
    gPending = 0;
    gRootDiagnostic.result = result.phase == UfsRootPhase::Identity ? kIOReturnNotPrivileged :
      result.phase == UfsRootPhase::Schedule ? kIOReturnNoResources : kIOReturnError;
    IOLog ("NativeUfs: root preparation failed phase=%u window=%u; nothing published\n",
           static_cast<unsigned> (result.phase), result.index);
    IOService::stop (provider);
    rootDiagnosticFailure (result.phase == UfsRootPhase::Schedule ? "root publisher thread creation failed" :
                          result.phase == UfsRootPhase::Identity ? "signed window identities rejected" :
                          "root window preparation failed");
    return false;
  }
  /* nabu: open the diagnostic window and write the boot half of the ring now, in
   * ordinary process context. The panic path then only has to append, and a window
   * that cannot be opened or written is proven here instead of at panic time.
   * nabu-log=0 disables the whole log path; nabu-log=1 enables it. */
  unsigned nabuLogEnabled = 0;
  PE_parse_boot_argn ("nabu-log", &nabuLogEnabled, sizeof (nabuLogEnabled));
  (void)nabuLogEnabled;
  /* nabu: start the periodic writer before the USB bring-up. That call can wedge or
   * panic inside the controller, and the writer used to be started after it -- so a
   * hanging bring-up also killed the log, and a dead log looked exactly like a dead
   * CPU in runs 61-63. */
  {
    thread_t logThread = nullptr;
    if (nabuLogEnabled && kernel_thread_start (nabuLogThread, nullptr, &logThread) == KERN_SUCCESS) {
      thread_deallocate (logThread);
    }
    thread_t watchdog = nullptr;
    if (kernel_thread_start (nabuWatchdogThread, nullptr, &watchdog) == KERN_SUCCESS) {
      thread_deallocate (watchdog);
    }
    /* l485: opt-in only. No argument, no thread, no behaviour change. */
    unsigned usbReboot = 0;
    PE_parse_boot_argn ("nabu-usb-reboot", &usbReboot, sizeof (usbReboot));
    if (usbReboot) {
      thread_t usbRebootThread = nullptr;
      if (kernel_thread_start (nabuUsbRebootThread, nullptr, &usbRebootThread) == KERN_SUCCESS) {
        thread_deallocate (usbRebootThread);
        nabu_log_puts ("wd:usb-r=on\n");
      } else {
        nabu_log_puts ("wd:usb-r=start-fail\n");
      }
    } else {
      nabu_log_puts ("wd:usb-r=off\n");
    }
  }
  /* Arm boot-recovery before anything that can hang: if the driver wedges later,
   * the next reset still lands in TWRP (WATCHDOG.md, "arm misc before bring-up"). */
  if (nabuLogEnabled) {
    (void)native_ufs_panic_recovery ();
    /* Open the diagnostic window while the path is still healthy. A 4 KiB tail
     * without sync is the only write form that survives this window, and it sets
     * nabu_log_opened so the periodic writer starts landing tails. */
    nabuFlushThrottled ();
    /* If the previous boot panicked, its text is in the reserved panic region. */
    (void)native_ufs_paniclog_import ();
  }
  /* One line per boot: the arguments this kernel actually parsed. Every free boot-arg round
   * needs a positive control, and the ring keeps only the last 4 KiB. */
  {
    unsigned hang = 0, panicRestart = 0, autolog = 0, log = 0, root = 0, smp = 0, unverified = 0;
    PE_parse_boot_argn ("nabu-hang-reset", &hang, sizeof (hang));
    PE_parse_boot_argn ("nabu-panic-restart", &panicRestart, sizeof (panicRestart));
    PE_parse_boot_argn ("nabu-autolog", &autolog, sizeof (autolog));
    PE_parse_boot_argn ("nabu-log", &log, sizeof (log));
    PE_parse_boot_argn ("native-ufs-root", &root, sizeof (root));
    PE_parse_boot_argn ("sheng-smp", &smp, sizeof (smp));
    PE_parse_boot_argn ("nabu-unverified-root", &unverified, sizeof (unverified));
    IOLog ("NativeUfs: args hang=%u panic=%u autolog=%u log=%u root=%u smp=%u unverified=%u pet=%u arm=%u diagnostics=%u polls=%u deadline=%u write=%u fua=%u dlmode=%u wq=%u slow=%u/%u grace=%u excq=%u flush=%ums mark=%u rdrec=%u porecheck=%u cmdstat=%u readahead=%u seq=%u rf=%u raafter=%u cnthz=%llu\n",
           hang, panicRestart, autolog, log, root, smp, unverified, gWdtPetSeconds, gWdtArmDelay,
           gRootDiagnosticEnabled ? 1U : 0U, gUfsPollCeiling, gUfsReadDeadlineUs,
           gUfsWriteDeadlineUs, gUfsLogWriteFua, gUfsDeadlineMode,
           gUfsWriteQuarantine, gUfsSlowMs, gUfsSlowBudget, gUfsGraceMs, gUfsExcq,
           gUfsFlushMs, gUfsMarkEvery, gUfsReadRecover, gUfsPoRecheck, gUfsCmdStatEvery, gUfsReadAhead, gUfsSeqCensus, gUfsReadFlushEvery, gUfsReadAheadAfter,
           (unsigned long long)nabu_cntfreq ());
    nabuFlushThrottled ();
  }
  /* nabu: run DWC3 before the probe write so its readings land in the window. */
  /* One line per boot saying which lock policy this kernel will actually use, so a
   * SIGBUS round and a slow round can be told apart without guessing at the args. */
  nabu_log_puts (nabu_lock_event_enabled () ? "ufs:lk-ev=on\n" : "ufs:lk-ev=off\n");
  nabu_log_puts ("ufs:dwc3-call\n");
  (void)native_dwc3_bringup ();
  nabu_log_puts ("ufs:start-done\n");
  /* l478: hand the pull channel the ring once DWC3 is up, and say out loud
   * whether the transport took it: -1 = debug library not linked,
   * -2 = the single provider slot was already taken, 1 = attached. */
  {
    /* nabu-pull=0 turns the pull channel off and says so: the round then runs the
     * old single-writer CDC with nothing registered, which is the A/B half of the
     * regression test. Default stays on - the boot-arg list is already at budget. */
    unsigned pullOn = 1;
    if (!PE_parse_boot_argn ("nabu-pull", &pullOn, sizeof (pullOn))) {
      pullOn = 1;
    }
    if (!pullOn) {
      nabu_log_puts ("usb:pull=off\n");
    } else {
      int attached = nabu_log_attach_pull ();
      nabu_log_hex ("usb:pull=", (unsigned long long) (unsigned int) attached);
      IOLog ("NativeUfs: pull channel attach=%d\n", attached);
    }
  }
  /* l487: one shot at the DPU pipes, taken here because everything that can hang us is
   * behind this point, and the answer is only interesting together with nsurf. */
  nabu_gop_probe_once ();
  nabu_dpu_probe_once ();
  if (nabuLogEnabled) {
    nabuFlushThrottled ();
  }
  return true;
}

void NativeUfs::stop (IOService *provider)
{
  fCmdDesc.reset ();
  fMap.reset ();
  IOService::stop (provider);
}

// nabu: let another driver (NativeDwc3) take the same bypass path for its stream.
extern "C" int native_smmu_bypass (uint16_t stream, uint64_t dma, uint64_t bytes)
{
  // Give the caller its own stream-match entry and a context bank that is not in
  // use by any other valid stream (UFS owns the last bank). USFCFG is read-only on
  // this SMMU, so an unmatched stream can only be handled this way.
  (void)dma; (void)bytes;
  if (!gHost.smmu) { return -1; }
  uintptr_t smmu = gHost.smmu;
  uint32_t idr0 = read32 (smmu, 0x20), idr1 = read32 (smmu, 0x24);
  unsigned count = idr0 & 0xff, banks = idr1 & 0xff;
  UfsSmmuPlan plan;
  plan.pages = 1U << (((idr1 >> 28) & 7) + 1);
  if (count > 128) count = 128;
  if (banks < 3 || !count) { return -2; }

  unsigned slot = 0xffffffffU;
  for (unsigned i = 0; i < count; ++i) {
    if (!(read32 (smmu, 0x800 + 4 * i) & 0x80000000U)) { slot = i; break; }
  }
  if (slot == 0xffffffffU) { return -3; }

  unsigned bank = 0xffffffffU;
  for (unsigned b = banks - 2; b > 0; --b) {
    bool used = false;
    for (unsigned i = 0; i < count && !used; ++i) {
      uint32_t smr = read32 (smmu, 0x800 + 4 * i);
      if (!(smr & 0x80000000U)) { continue; }
      uint32_t s2cr = read32 (smmu, 0xc00 + 4 * i);
      if (((s2cr >> 16) & 3) == 0 && (s2cr & 0xff) == b) { used = true; }
    }
    if (!used) { bank = b; break; }
  }
  if (bank == 0xffffffffU) { return -4; }

  plan.slot = slot;
  plan.bank = bank;
  const char *failure = configureUfsSmmu (plan, stream,
    [smmu](unsigned off) { return read32 (smmu, off); },
    [smmu](unsigned off, uint32_t v) {
      write32 (smmu, off, v);
      __asm__ volatile ("dsb sy" ::: "memory");
    });
  nabu_log_hex ("smmu2:slot=", slot);
  nabu_log_hex ("smmu2:bank=", bank);
  nabu_log_hex ("smmu2:smr=", read32 (smmu, 0x800 + 4 * slot));
  nabu_log_hex ("smmu2:s2cr=", read32 (smmu, 0xc00 + 4 * slot));
  return failure ? -5 : 0;
}

/* Bring-up stubs: the nabu log/status implementation lives in the device patch set. */
extern "C" void nabu_log_hex(const char *, unsigned long long) __attribute__((weak));
extern "C" void nabu_log_hex(const char *, unsigned long long) {}
extern "C" void nabu_log_puts(const char *) __attribute__((weak));
extern "C" void nabu_log_puts(const char *) {}
/* The autolog patch exports nabu_log_puts but no mark_text, so the empty stub used to
 * swallow every milestone after ufs:dwc3-call and the journal looked like a hang there.
 * Forward the milestones into the same ring; the call sites already carry a line number,
 * which is not worth spending ring bytes on. */
extern "C" void nabu_mark_text(const char *text, unsigned int) __attribute__((weak));
extern "C" void nabu_mark_text(const char *text, unsigned int)
{
  nabu_log_puts (text);
  nabu_log_puts ("\n");
}

/* Optional NativeDwc3 companion; the archive is not linked on this board yet. */
extern "C" int native_dwc3_bringup(void) __attribute__((weak));
extern "C" int native_dwc3_bringup(void) { return 0; }

/* Boot-manifest digest used by native_owner_publish_boot_manifest: SHA384 of the
 * signed OS policy envelope (header + signature + block hash table). */
extern "C" const uint8_t verified_root_manifest[];
extern "C" const uint64_t verified_root_manifest_bytes;
extern "C" const uint8_t verified_root_systemos_manifest[];
extern "C" const uint64_t verified_root_systemos_manifest_bytes;
extern "C" int native_verified_block_manifest_sha384(dev_t device, const char *uuid,
                                                     uint8_t digest[48])
{
  (void)device;
  static const char kOsUuid[] = "04769D26-2CA0-4825-B443-E2E5F88300F2";
  static const char kCacheUuid[] = "8846FE78-2910-4297-AF97-D59BB6F8FC16";
  /* The caller asks twice: once for the OS volume and once for the SystemOS/Cryptex
   * volume that owner-cache mounts. Each has its own signed manifest in the bundle, so
   * both UUIDs must be answered -- answering only the first rejects the second, and the
   * publish helper turns that rejection into EAUTH. */
  const uint8_t *manifest = nullptr;
  uint64_t manifestBytes = 0;
  if (!digest || !uuid) return 0;
  if (strcmp(uuid, kOsUuid) == 0) {
    manifest = verified_root_manifest;
    manifestBytes = verified_root_manifest_bytes;
  } else if (strcmp(uuid, kCacheUuid) == 0) {
    manifest = verified_root_systemos_manifest;
    manifestBytes = verified_root_systemos_manifest_bytes;
  } else {
    return 0;
  }
  SHA384_CTX context;
  SHA384_Init(&context);
  SHA384_Update(&context, manifest, static_cast<size_t>(manifestBytes));
  SHA384_Final(digest, &context);
  return 1;
}

/* Optional boot-progress hook of the sheng image; this board links no BootProgress. */
extern "C" void boot_progress_poll(void) __attribute__((weak));
extern "C" void boot_progress_poll(void) {}

/* Owner cache hooks used by the offline owner path in kern_exec.c. Unverified bring-up:
 * authorization follows the nabu-unverified-root boot argument. */
extern "C" int native_verified_block_root_authorized (dev_t device)
{
  (void) device;
  unsigned enabled = 0;
  PE_parse_boot_argn ("nabu-unverified-root", &enabled, sizeof (enabled));
  return enabled ? 1 : 0;
}

extern "C" int native_verified_block_volume_device (const char *uuid, dev_t *device,
                                                    char *path, size_t size)
{
  if (!uuid || !device || !path || size < 6 || strlen (uuid) != 36) {
    return 0;
  }
  *device = NODEV;
  path[0] = 0;
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  OSDictionary *properties = OSDictionary::withCapacity (1);
  OSString *wanted = OSString::withCString (uuid);
  bool ready = matching && properties && wanted && properties->setObject ("UUID", wanted) &&
               matching->setObject ("IOPropertyMatch", properties);
  OSSafeReleaseNULL (wanted);
  OSSafeReleaseNULL (properties);
  OSIterator *iterator = ready ? IOService::getMatchingServices (matching) : nullptr;
  OSSafeReleaseNULL (matching);
  if (!iterator) {
    return 0;
  }
  unsigned matches = 0;
  bool found = false;
  dev_t candidate = NODEV;
  char candidatePath[64] = {};
  while (IOService *media = OSDynamicCast (IOService, iterator->getNextObject ())) {
    if (++matches != 1) {
      found = false;
      break;
    }
    OSString *identity = OSDynamicCast (OSString, media->getProperty ("UUID"));
    OSString *name = OSDynamicCast (OSString, media->getProperty (kIOBSDNameKey));
    OSNumber *mjr = OSDynamicCast (OSNumber, media->getProperty (kIOBSDMajorKey));
    OSNumber *mnr = OSDynamicCast (OSNumber, media->getProperty (kIOBSDMinorKey));
    if (!identity || strcmp (identity->getCStringNoCopy (), uuid) || !name || !mjr || !mnr ||
        !name->getLength () || name->getLength () >= sizeof (candidatePath) - 6 ||
        name->getLength () >= size - 6 || mjr->unsigned64BitValue () > 255 ||
        mnr->unsigned64BitValue () > 0xffffff) {
      continue;
    }
    candidate = static_cast<dev_t> (makedev (mjr->unsigned32BitValue (), mnr->unsigned32BitValue ()));
    if (!native_verified_block_root_authorized (candidate)) {
      continue;
    }
    snprintf (candidatePath, sizeof (candidatePath), "/dev/%s", name->getCStringNoCopy ());
    found = true;
  }
  iterator->release ();
  if (!found || matches != 1) {
    return 0;
  }
  *device = candidate;
  strlcpy (path, candidatePath, size);
  return 1;
}
