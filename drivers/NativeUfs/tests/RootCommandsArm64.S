// SPDX-License-Identifier: BSD-2-Clause-Patent
// Offline QEMU virt fixture only; these are not sheng boot tables.
.section .text.entry,"ax"
.global _start
_start:
  msr daifset, #15
  ldr x0, =stack_end
  mov sp, x0
  ldr x0, =vectors
  msr vbar_el1, x0
  ldr x0, =__bss_start
  ldr x1, =__bss_end
1:
  cmp x0, x1
  b.hs 2f
  str xzr, [x0], #8
  b 1b
2:
  bl guest_main
  b .

.balign 2048
vectors:
.rept 16
  bl guest_exception
  b .
  .balign 128
.endr

.global enable_translation
enable_translation:
  msr ttbr0_el1, x0
  msr ttbr1_el1, xzr
  // Attr0: normal WBWA; Attr1: device nGnRnE. 4 KiB TG0, 38-bit
  // VA, caller-selected shareability, WBWA walks, 48-bit PA, TTBR1 disabled.
  mov x0, #0xff
  msr mair_el1, x0
  msr tcr_el1, x1
  dsb sy
  isb
  tlbi vmalle1
  dsb sy
  isb
  mrs x0, sctlr_el1
  orr x0, x0, #1
  orr x0, x0, #4
  orr x0, x0, #0x1000
  msr sctlr_el1, x0
  isb
  ret

.section .bss,"aw",%nobits
.balign 16
stack:
  .skip 65536
stack_end:
