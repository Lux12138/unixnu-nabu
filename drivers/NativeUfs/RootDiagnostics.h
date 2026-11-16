// SPDX-License-Identifier: BSD-2-Clause-Patent
// Passive snapshot formatter; caller supplies integer types and snprintf.
// No hardware access or decision about trust, transport or publication here.
/* l448: this board's mach_absolute_time() timebase is NOT the 19.2 MHz CNTPCT that the
 * launchd-side timestamps are in. Two rounds with configured deadlines of 1 s and 10 s
 * both reported delta ~120M ticks, so kernel ticks cannot be turned into seconds from
 * that assumption. Take the architectural counter and its advertised frequency beside
 * mach_absolute_time(); the ratio of the two deltas is the calibration. */
static inline uint64_t nabu_cntpct (void)
{
  uint64_t v = 0;
  __asm__ volatile ("mrs %0, cntpct_el0" : "=r" (v));
  return v;
}
static inline uint64_t nabu_cntfreq (void)
{
  uint64_t v = 0;
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (v));
  return v;
}

struct UfsRootDiagnostics {
  const char *stage = "not started";
  uint64_t dma = 0, list = 0, lba = 0, first = 0, last = 0;
  uint64_t windowBytes = 0, policyCount = 0, reads = 0, failures = 0;
  uint32_t cap = 0, version = 0, hcs = 0, hce = 0, doorbell = 0, interrupt = 0;
  uint32_t smr = 0, s2cr = 0, cbar = 0, sctlr = 0;
  uint32_t type = 0, opcode = 0, lun = 0, command = 0, ocs = 0xff, scsi = 0xffff;
  uint32_t response = 0, result = 0, power = 0, manifest = 0xffffffff;
  uint32_t magic0 = 0, magic1 = 0, windows = 0, prepared = 0, published = 0;
  uint32_t verify = 0xffffffff;
  uint32_t protocol = 0, residual = 0, sense = 0;
  uint64_t far = 0, contiguous = 0;
  uint32_t runStop = 0, ahit = 0, polls = 0, dbSeen = 0, isSeen = 0, isBefore = 0;
  uint64_t submitTicks = 0, faultTicks = 0;
  // l446c: lba above is shared by both directions, so a fault line could never say
  // which command failed. These are written by the submitter about to ring the doorbell.
  uint64_t cmdLba = 0;
  uint64_t cntfreq = 0, submitCnt = 0, faultCnt = 0;
  // l451: the deadline we asked for, the deadline clock_interval_to_deadline() actually gave us and
  // the return code we used to throw away. A round that asked for 15 s and timed out in 0.2175 s can
  // only mean the deadline value was not what we asked for; without these three numbers that stays a
  // guess. The descriptor dumps then say whether the device wrote anything back at all.
  uint64_t dlWant = 0, dlGot = 0;
  uint32_t dlMode = 0;
  uint32_t utrd[8] = {}, cmdU[8] = {}, rspU[8] = {};
  uint32_t cmdBytes = 0, cmdDir = 0, cmdFua = 0;
  /* l455: one fault line carried dir=2 (READ) next to op=2a (WRITE(10)), which is impossible
   * for one command, so at least one half of it belonged to a different one. Every field that
   * the submitter knows is now stamped with the command number it was written for. */
  uint32_t cmdSeq = 0;
  uint32_t gracePolls = 0;
  uint64_t lastPollCnt = 0;   // CNTPCT at the last OCS sample inside the poll loop
  // l446d: 'db' alone was being read as 'the slot is occupied'. Read the siblings at
  // the same instant so the fault line says which register that claim came from.
  uint32_t utmDoorbell = 0, ctrl = 0, listHigh = 0, tmList = 0;
  uint32_t gfsrBefore = 0, gfsr = 0, gsyn = 0, fsrBefore = 0, fsr = 0, fsyn = 0;
  uint32_t smmuId2 = 0, cba2r = 0, uic[5] = {};
  uint32_t smmuId1 = 0, smmuSlot = 0xffffffff;
  uint32_t publicationPhase = 0, publicationWindow = 0xffffffff;
};

static inline int formatUfsRootDiagnostics(char *buffer, size_t capacity,
                                          const UfsRootDiagnostics &d, const char *reason,
                                          const char *title = "UFS ROOT FAILURE")
{
  return snprintf(buffer, capacity,
    "%s: %s\nstage=%s result=%08x\n"
    "HC cap=%08x ver=%08x hcs=%08x hce=%08x db=%08x is=%08x\n"
    "HCSIBLING ctrl=%08x listClear=%08x listBaseHi=%08x cnr=%08x\n"
    "DMA=%llx UTRL=%llx SMMU smr=%08x s2cr=%08x cbar=%08x sctlr=%08x\n"
    "CMD n=%u type=%02x op=%02x lun=%02x ocs=%02x rsp=%08x scsi=%04x power=%02x\n"
    "PROTOCOL reject=%u residual=%u sense=%06x SMMU slot=%u id1=%x\n"
    "QUEUE rsr=%x ahit=%x polls=%u dbSeen=%x isSeen=%x isBefore=%x contiguous=%llx "
    "RANGE submit=%llu fault=%llu delta=%llu\n"
    "TIMEBASE cntfreq=%llu cnt=%llu..%llu cntdelta=%llu\n"
    "FAULT global=%x->%x syn=%x cb=%x->%x syn=%x far=%llx id2=%x cba2r=%x\n"
    "UIC pa=%08x dl=%08x net=%08x tr=%08x dme=%08x\n"
    "CMDCUR seq=%u dir=%u lba=%llx bytes=%u fua=%u grace=%u pollgap=%llu\n"
    "GPT/read lba=%llx userdata=%llx..%llx magic=%08x %08x manifest=%u\n"
    "WINDOWS found=%u prepared=%u published=%u bytes=%llx policies=%llu verify=%08x\n"
    "IO reads=%llu failures=%llu pubPhase=%u window=%u\n"
    "DEADLINE want=%llu got=%llu diff=%lld mode=%u\n"
    "UTRD %08x %08x %08x %08x %08x %08x %08x %08x\n"
    "CMDU %08x %08x %08x %08x %08x %08x %08x %08x\n"
    "RSPU %08x %08x %08x %08x %08x %08x %08x %08x\n",
    title, reason, d.stage, d.result, d.cap, d.version, d.hcs, d.hce, d.doorbell, d.interrupt,
    d.ctrl, d.utmDoorbell, d.listHigh, d.tmList,
    (unsigned long long)d.dma, (unsigned long long)d.list, d.smr, d.s2cr, d.cbar, d.sctlr,
    d.command, d.type, d.opcode, d.lun, d.ocs, d.response, d.scsi, d.power,
    d.protocol, d.residual, d.sense, d.smmuSlot, d.smmuId1,
    d.runStop, d.ahit, d.polls, d.dbSeen, d.isSeen, d.isBefore, (unsigned long long)d.contiguous,
    (unsigned long long)d.submitTicks, (unsigned long long)d.faultTicks,
    (unsigned long long)(d.faultTicks - d.submitTicks),
    (unsigned long long)d.cntfreq, (unsigned long long)d.submitCnt,
    (unsigned long long)d.faultCnt, (unsigned long long)(d.faultCnt - d.submitCnt),
    d.gfsrBefore, d.gfsr, d.gsyn, d.fsrBefore, d.fsr, d.fsyn, (unsigned long long)d.far,
    d.smmuId2, d.cba2r, d.uic[0], d.uic[1], d.uic[2], d.uic[3], d.uic[4],
    d.cmdSeq, d.cmdDir, (unsigned long long)d.cmdLba, d.cmdBytes, d.cmdFua,
    d.gracePolls, (unsigned long long)(d.faultCnt - d.lastPollCnt),
    (unsigned long long)d.lba, (unsigned long long)d.first, (unsigned long long)d.last,
    d.magic0, d.magic1, d.manifest, d.windows, d.prepared, d.published,
    (unsigned long long)d.windowBytes, (unsigned long long)d.policyCount, d.verify,
    (unsigned long long)d.reads, (unsigned long long)d.failures, d.publicationPhase, d.publicationWindow,
    (unsigned long long)d.dlWant, (unsigned long long)d.dlGot,
    (long long)((int64_t)d.dlGot - (int64_t)d.dlWant), d.dlMode,
    d.utrd[0], d.utrd[1], d.utrd[2], d.utrd[3], d.utrd[4], d.utrd[5], d.utrd[6], d.utrd[7],
    d.cmdU[0], d.cmdU[1], d.cmdU[2], d.cmdU[3], d.cmdU[4], d.cmdU[5], d.cmdU[6], d.cmdU[7],
    d.rspU[0], d.rspU[1], d.rspU[2], d.rspU[3], d.rspU[4], d.rspU[5], d.rspU[6], d.rspU[7]);
}
