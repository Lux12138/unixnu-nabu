// SPDX-License-Identifier: MIT
// Board glue for BootResetCore.h: PSCI feature probe, a read-only look at the PMIC arbiter and the reboot-reason cell, and the guarded
// "restart into fastboot" (boot argument sheng-autoreset=<seconds>, see BootLog.c). Nothing here runs without BootLog's boot arguments.
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <string.h>
#include <IOKit/IOLib.h>
/* Integration note (nabu build, 2026-11-01): this file is included into NativeUfs.cpp, where
 * <kern/sched.h> clashes with headers already in the translation unit (MAXPRI redefined,
 * sched_group_t unknown). MAXPRI_KERNEL is 95 on this kernel -- osfmk/kern/sched.h:146,
 * BASEPRI_REALTIME - 1. Nothing else in this file was changed. */
#if defined(NABU_BOOTRESET_IN_TU)
#define NABU_MAXPRI_KERNEL 95
#else
#include <kern/sched.h>
#define NABU_MAXPRI_KERNEL MAXPRI_KERNEL
#endif
#include <pexpert/pexpert.h>
#include "BootResetCore.h"

// Physical windows from the sheng devicetree (spmi@c400000 reg, reg-names core/chnls/obsrvr/cnfg).
enum {SpmiCore = 0xc400000, SpmiCoreSize = 0x3000, SpmiObsrvr = 0xc440000, SpmiChnls = 0xc500000, SpmiCnfg = 0xc42d000, SpmiCnfgSize = 0x4000};
enum {PsciVersion = 0x84000000u, PsciFeatures = 0x8400000au, PsciSystemReset = 0x84000009u, PsciSystemReset2_32 = 0x84000012u,
      PsciSystemReset2_64 = 0xc4000012u};

// SMCCC call with two arguments; x0 and x2 come back (the Gunyah watchdog's status reports the time since the last ping in x2).
static uint64_t smc2(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t *r2) {
  register uint64_t x0 __asm__("x0") = fid, x1 __asm__("x1") = a1, x2 __asm__("x2") = a2, x3 __asm__("x3") = 0;
  __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
                   "x14", "x15", "x16", "x17");
  if (r2) *r2 = x2;
  return x0;
}

static uint64_t psci(uint64_t fid, uint64_t a1) {
  register uint64_t x0 __asm__("x0") = fid, x1 __asm__("x1") = a1, x2 __asm__("x2") = 0, x3 __asm__("x3") = 0;
  __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
                   "x14", "x15", "x16", "x17");
  return x0;
}

// ml_io_map hands out permanent mappings (io_submap, KMA_PERMANENT) that ml_io_unmap cannot free: its kmem_free on kernel_map panics with the
// map lock held (int-105 hung launchd that way). Mappings that are unmapped again must be unmappable ones (flags 7 = VM_WIMG_IO).
/* Integration note (nabu build): this file is compiled inside NativeUfs.cpp, i.e. as C++,
 * while these machine-routine and thread entry points are C symbols in this kernel. The
 * unmappable mapping call is the one this file already relied on; the other three are the
 * ids the unmapping path and the watchdog thread need in this header set. */
#ifdef __cplusplus
extern "C" {
#endif
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, uint32_t flags);
/* Integration note (nabu build): this kernel header set declares ml_io_map_unmappable but not
 * ml_io_unmap, which the unmapping path below needs. Same signature as osfmk/arm/machine_routines.h. */
extern void ml_io_unmap(vm_offset_t addr, vm_size_t size);
/* Integration note (nabu build): the same header does not expose ml_io_map here, and
 * <kern/thread.h> reaches this translation unit without kernel_thread_start_priority. */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter,
                                                   integer_t priority, thread_t *new_thread);
#ifdef __cplusplus
}
#endif
static volatile uint32_t *map(uint64_t phys, uint64_t size) {return (volatile uint32_t *)ml_io_map_unmappable((vm_offset_t)phys, (vm_size_t)size, 7);}

struct reset_maps {
  struct br_arbiter a;
  struct br_found f;
  volatile uint32_t *obs, *chn;
  uint64_t obs_size, chn_size;
  int find;   // br_find's result, -1 before it ran
};
static void unmap_all(struct reset_maps *m) {
  if (m->a.core) ml_io_unmap((vm_offset_t)m->a.core, SpmiCoreSize);
  if (m->a.cnfg) ml_io_unmap((vm_offset_t)m->a.cnfg, SpmiCnfgSize);
  if (m->obs) ml_io_unmap((vm_offset_t)m->obs, (vm_size_t)m->obs_size);
  if (m->chn) ml_io_unmap((vm_offset_t)m->chn, (vm_size_t)m->chn_size);
  memset(m, 0, sizeof *m);
}
// Maps the arbiter, finds the SDAM's APID and maps its observer (and, when asked, read/write) channels. 0 = ready.
static int open_maps(struct reset_maps *m, int want_write) {
  memset(m, 0, sizeof *m);
  m->find = -1;
  m->a.ee = 0;   // qcom,ee = <0>
  m->a.core = map(SpmiCore, SpmiCoreSize);
  m->a.cnfg = map(SpmiCnfg, SpmiCnfgSize);
  if (!m->a.core || !m->a.cnfg) return 1;
  m->find = br_find(&m->a, &m->f);
  if (m->find) return 2;
  m->obs_size = ((uint64_t)BR_OBS_APID_STRIDE * ((uint64_t)m->f.apid + 1) + 0xfff) & ~0xfffull;
  m->obs = map(SpmiObsrvr, m->obs_size);
  if (!m->obs) return 3;
  if (want_write) {
    m->chn_size = (uint64_t)BR_CHNL_APID_STRIDE * ((uint64_t)m->f.apid + 1);
    m->chn = map(SpmiChnls, m->chn_size);
    if (!m->chn) return 4;
  }
  return 0;
}

// The arbiter and the SDAM cell, read only. Fills text; returns 0 when the cell can be written (version known, APID found, owned by this EE).
int boot_reset_probe(char *text, unsigned cap) {
  const uint64_t ver = psci(PsciVersion, 0);
  const int64_t f_reset = (int64_t)psci(PsciFeatures, PsciSystemReset), f2_32 = (int64_t)psci(PsciFeatures, PsciSystemReset2_32),
                f2_64 = (int64_t)psci(PsciFeatures, PsciSystemReset2_64);
  struct reset_maps m;
  uint8_t cell = 0xff;
  int open_rc = open_maps(&m, 0), read_rc = -1;
  if (!open_rc) read_rc = br_read8(m.obs, &m.a, m.f.apid, BR_REBOOT_CELL, &cell);
  const int writable = !open_rc && !read_rc && m.f.owner == m.a.ee;
  snprintf(text, cap, "psci v%x reset=%lld reset2_32=%lld reset2_64=%lld | spmi v%08x apids=%u open=%d find=%d apid=%d owner=%d sdam48=%02x read=%d ok=%d",
           (unsigned)ver, (long long)f_reset, (long long)f2_32, (long long)f2_64, m.a.core ? m.f.version : 0, m.a.core ? m.f.apids : 0, open_rc,
           m.find, m.find ? -1 : m.f.apid, m.find ? -1 : m.f.owner, cell, read_rc, writable);
  unmap_all(&m);
  return writable ? 0 : 1;
}

// Writes "bootloader" into the reboot-reason cell and verifies it, without restarting. Done once early in an unattended run, it sends every
// later restart to fastboot: the timed one below, a watchdog bite, or a long press of the power key.
int boot_reset_mark_bootloader(char *text, unsigned cap) {
  struct reset_maps m;
  uint8_t before = 0xff, after = 0xff;
  int rc = open_maps(&m, 1);
  if (rc) {snprintf(text, cap, "mark refused: open=%d find=%d", rc, m.find); unmap_all(&m); return rc;}
  if (m.f.owner != m.a.ee) {snprintf(text, cap, "mark refused: apid %d is owned by EE %d", m.f.apid, m.f.owner); unmap_all(&m); return 5;}
  rc = br_set_bootloader(m.obs, m.chn, &m.a, m.f.apid, &before, &after);
  snprintf(text, cap, "mark: cell %02x -> %02x rc=%d", before, after, rc);
  unmap_all(&m);
  return rc ? 10 + rc : 0;
}

// The hypervisor's watchdog (Gunyah, SMCCC vendor hypervisor calls 5 control, 6 status, 7 ping, 8 set time in ms, as Linux gunyah_wdt
// uses them). An unattended run arms it with a 60 s timeout and pings it from its own thread until <deadline> seconds after the start, then
// stops pinging: the hypervisor restarts the tablet even when the kernel hangs or panics, and the cell above makes that restart fastboot.
enum {GunyahWdtControl = 0x86000005u, GunyahWdtStatus = 0x86000006u, GunyahWdtPing = 0x86000007u, GunyahWdtSetTime = 0x86000008u,
      GunyahWdtEnable = 3, GunyahWdtDisable = 2, GunyahWdtTimeoutMs = 60000};
volatile int32_t boot_reset_wdt_stats[4];   // [0] status call result, [1] set-time result, [2] enable result, [3] pings sent
static uint32_t wdt_deadline;
static void wdt_thread(void *arg, wait_result_t wr) {
  (void)arg; (void)wr;
  uint64_t f = 0; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); if (!f) f = 19200000;
  const uint64_t start = mach_absolute_time();
  while ((mach_absolute_time() - start) / f < wdt_deadline) {
    smc2(GunyahWdtPing, 0, 0, NULL);
    boot_reset_wdt_stats[3] = boot_reset_wdt_stats[3] + 1;
    IOSleep(10000);
  }
}
int boot_reset_watchdog(uint32_t deadline, char *text, unsigned cap) {
  uint64_t since = 0;
  const int64_t status = (int64_t)smc2(GunyahWdtStatus, 0, 0, &since);
  boot_reset_wdt_stats[0] = (int32_t)status;
  if (status != 0) {snprintf(text, cap, "watchdog: status %lld, not armed", (long long)status); return 1;}
  smc2(GunyahWdtControl, GunyahWdtDisable, 0, NULL);
  boot_reset_wdt_stats[1] = (int32_t)smc2(GunyahWdtSetTime, GunyahWdtTimeoutMs, GunyahWdtTimeoutMs, NULL);
  if (boot_reset_wdt_stats[1]) {snprintf(text, cap, "watchdog: set time %d, not armed", boot_reset_wdt_stats[1]); return 2;}
  wdt_deadline = deadline;
  thread_t thread;
  if (kernel_thread_start_priority(wdt_thread, NULL, NABU_MAXPRI_KERNEL, &thread) != KERN_SUCCESS) {snprintf(text, cap, "watchdog: no thread"); return 3;}
  thread_deallocate(thread);
  boot_reset_wdt_stats[2] = (int32_t)smc2(GunyahWdtControl, GunyahWdtEnable, 0, NULL);
  snprintf(text, cap, "watchdog: armed %d, pings until %us, bite 60 s later (last ping %llu ms ago)", boot_reset_wdt_stats[2], deadline,
           (unsigned long long)since);
  return boot_reset_wdt_stats[2] ? 4 : 0;
}

// Writes "bootloader" into the reboot-reason cell, verifies it, then PSCI SYSTEM_RESET. Returns only when something refused (text says what);
// the cell is left as it was in every refusal before the write.
int boot_reset_bootloader(char *text, unsigned cap) {
  struct reset_maps m;
  uint8_t before = 0xff, after = 0xff;
  int rc = open_maps(&m, 1);
  if (rc) {
    snprintf(text, cap, "reset refused: open=%d find=%d", rc, m.find);
    unmap_all(&m);
    return rc;
  }
  if (m.f.owner != m.a.ee) {
    snprintf(text, cap, "reset refused: apid %d is owned by EE %d", m.f.apid, m.f.owner);
    unmap_all(&m);
    return 5;
  }
  rc = br_set_bootloader(m.obs, m.chn, &m.a, m.f.apid, &before, &after);
  snprintf(text, cap, "reset: cell %02x -> %02x rc=%d", before, after, rc);
  unmap_all(&m);
  if (rc) return 10 + rc;
  psci(PsciSystemReset, 0);
  return 99;   // SYSTEM_RESET returned
}
