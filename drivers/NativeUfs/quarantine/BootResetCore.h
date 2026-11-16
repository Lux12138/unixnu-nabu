// SPDX-License-Identifier: MIT
// Reboot-into-fastboot control for the sheng board: one byte of the PMIC's SDAM scratch memory (the cell the stock bootloader reads as the
// reboot reason) written through the SPMI PMIC arbiter, then a PSCI SYSTEM_RESET. The register facts are the SM8550 devicetree's
// (arbiter "core" 0xc400000, "chnls" 0xc500000, "obsrvr" 0xc440000, "cnfg" 0xc42d000, EE 0; SDAM at PMIC SID 0 register 0x7100, reboot-reason
// cell at byte 0x48 bits 7:1, bootloader = 2) and the arbiter's version-7 layout as the mainline driver documents it.
// The core is pure register-pointer code so a host test can drive it against a software model of the arbiter.
#ifndef BOOT_RESET_CORE_H
#define BOOT_RESET_CORE_H
#include <stdint.h>

enum {
  BR_VERSION_V7_MIN = 0x70000000u, BR_VERSION_V8_MIN = 0x80000000u,
  BR_FEATURES = 0x04, BR_FEATURES_APIDS_MASK = 0x7ffu,
  BR_APID_MAP = 0x2000,                     // core + 0x2000 + 4*apid: bits 19:8 PPID, bit 24 IRQ owner
  BR_CNFG_OWNER = 0x00,                     // cnfg + 4*apid: bits 2:0 the EE allowed to write the APID
  BR_OBS_EE_STRIDE = 0x8000, BR_OBS_APID_STRIDE = 0x20,   // observer (read) channels: 0x8000*EE + 0x20*APID
  BR_CHNL_APID_STRIDE = 0x1000,             // read/write channels: 0x1000*APID
  BR_CMD = 0x00, BR_STATUS = 0x08, BR_WDATA0 = 0x10, BR_RDATA0 = 0x18,
  BR_OP_EXT_WRITEL = 0, BR_OP_EXT_READL = 1,
  BR_STATUS_DONE = 1u << 0, BR_STATUS_ERRORS = 0xeu,      // bits 1..3: failed, denied, dropped
  BR_SDAM_PPID = 0x071, BR_REBOOT_CELL = 0x48,
  BR_REBOOT_MASK = 0xfe, BR_REBOOT_SHIFT = 1, BR_REBOOT_BOOTLOADER = 2,
  BR_POLLS = 100000
};
struct br_arbiter {
  volatile uint32_t *core;   // 0x3000 bytes
  volatile uint32_t *cnfg;   // 0x4000 bytes
  uint8_t ee;
};
struct br_found {uint32_t version, apids; int apid, owner;};

static inline uint32_t br_cmd(uint8_t op, uint8_t byte_addr, uint8_t bc) {return ((uint32_t)op << 27) | ((uint32_t)byte_addr << 4) | (bc & 7u);}

// Version, APID count and the APID that serves the SDAM peripheral for this EE. 0 on success; every refusal has its own code.
static inline int br_find(const struct br_arbiter *a, struct br_found *f) {
  f->version = a->core[0]; f->apids = a->core[BR_FEATURES / 4] & BR_FEATURES_APIDS_MASK; f->apid = -1; f->owner = -1;
  if (f->version < BR_VERSION_V7_MIN || f->version >= BR_VERSION_V8_MIN) return 1;   // not the layout this code knows
  if (!f->apids || f->apids > 1024) return 2;
  for (uint32_t i = 0; i < f->apids; ++i) {
    uint32_t map = a->core[(BR_APID_MAP + 4 * i) / 4];
    if (!map || ((map >> 8) & 0xfff) != BR_SDAM_PPID) continue;
    int owner = (int)(a->cnfg[(BR_CNFG_OWNER + 4 * i) / 4] & 7u);
    // As the mainline driver picks it: the first mapping, replaced by one this EE owns.
    if (f->apid < 0 || owner == a->ee) {f->apid = (int)i; f->owner = owner;}
  }
  return f->apid < 0 ? 3 : 0;
}
static inline int br_wait(volatile uint32_t *base) {
  for (unsigned n = 0; n < BR_POLLS; ++n) {
    uint32_t s = base[BR_STATUS / 4];
    if (s & BR_STATUS_DONE) return (s & BR_STATUS_ERRORS) ? 2 : 0;
  }
  return 1;
}
// One byte of the peripheral through the observer channel of this EE. obs/chn point at the channel bases (not the apid's channel).
static inline int br_read8(volatile uint32_t *obs, const struct br_arbiter *a, int apid, uint8_t byte_addr, uint8_t *value) {
  volatile uint32_t *ch = obs + (BR_OBS_EE_STRIDE * a->ee + BR_OBS_APID_STRIDE * (uint32_t)apid) / 4;
  ch[BR_CMD / 4] = br_cmd(BR_OP_EXT_READL, byte_addr, 0);
  int rc = br_wait(ch);
  if (rc) return rc;
  *value = (uint8_t)ch[BR_RDATA0 / 4];
  return 0;
}
static inline int br_write8(volatile uint32_t *chn, int apid, uint8_t byte_addr, uint8_t value) {
  volatile uint32_t *ch = chn + (BR_CHNL_APID_STRIDE * (uint32_t)apid) / 4;
  ch[BR_WDATA0 / 4] = value;
  ch[BR_CMD / 4] = br_cmd(BR_OP_EXT_WRITEL, byte_addr, 0);
  return br_wait(ch);
}
// Sets the reboot-reason cell to "bootloader", leaving bit 0, and reads it back. 0 = written and verified.
static inline int br_set_bootloader(volatile uint32_t *obs, volatile uint32_t *chn, const struct br_arbiter *a, int apid, uint8_t *before,
                                    uint8_t *after) {
  int rc = br_read8(obs, a, apid, BR_REBOOT_CELL, before);
  if (rc) return 10 + rc;
  uint8_t next = (uint8_t)((*before & ~BR_REBOOT_MASK) | (BR_REBOOT_BOOTLOADER << BR_REBOOT_SHIFT));
  rc = br_write8(chn, apid, BR_REBOOT_CELL, next);
  if (rc) return 20 + rc;
  rc = br_read8(obs, a, apid, BR_REBOOT_CELL, after);
  if (rc) return 30 + rc;
  return *after == next ? 0 : 40;
}
#endif
