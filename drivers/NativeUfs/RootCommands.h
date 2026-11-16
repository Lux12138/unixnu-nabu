// SPDX-License-Identifier: BSD-2-Clause-Patent
// Shared production command path. Included inside the root transport namespace.
// The host harness replaces only MMIO, DMA synchronization, clock and lock primitives.

// nabu: the mount path can reach this driver with preemption already disabled, so neither
// lck_mtx (IOLock) nor lck_spin (IOSimpleLock) is safe here: an IOLock panics once preemption
// is disabled, and an IOSimpleLock keeps it disabled across the transfer, which trips the same
// assertion in whichever mutex the caller takes next. A raw atomic test-and-set gives exclusion
// without touching preemption at all.
static inline int nabu_busy_try (volatile int *flag, unsigned spins)
{
  for (unsigned index = 0; index < spins; ++index) {
    if (!__atomic_test_and_set (flag, __ATOMIC_ACQUIRE)) return 1;
    IODelay (1);
  }
  return 0;
}
static inline void nabu_busy_release (volatile int *flag)
{
  __atomic_clear (flag, __ATOMIC_RELEASE);
}

// l441-ufswait: bounded waiting acquisition. nabu_busy_try() above is a fast-fail: a
// 2000-spin budget is ~2 ms of IODelay(1), while the holder of gHost.lock runs a SCSI
// command whose own deadline is 1 s (READ) or 2 s (WRITE). With no timer ticks on this
// board the reads were effectively serialized, so the fast-fail never fired; once
// INTID 19 delivers ticks (l438, 523 Hz in l440) launchd runs jobs in parallel and every
// second reader answered kIOReturnNotReady -- 'device is not ready' plus EBUSY (16) at
// nx_buf_bread:628, which is what turned into 'Missing executable' and the
// 'load code signature error 4' kills for logd/configd/rapportd/backboardd.
// IOSleep is deliberately NOT used here: the mount path can reach this with preemption
// disabled (see the note above nabu_busy_try), and IODelay stays legal in both cases.
static volatile uint64_t native_ufs_lock_waits = 0;
static volatile uint64_t native_ufs_lock_timeouts = 0;
static volatile uint32_t native_ufs_lock_tier = 0;
static unsigned nabu_lock_wait_spins = 0;
static bool nabu_lock_arg_set = false;
static bool nabu_lock_arg_parsed = false;

/*
 * l442-ufswait2. Two fixes over the l441 helper, both driven by measurement rather than taste:
 *
 *  (1) Argument parsing. l441 used 'arg ? arg : default', so 'nabu-ufswait=0' was read as
 *      'not passed' and the planned A/B control silently kept the 4 s budget. A value that is
 *      really present is now honoured verbatim; nabu-ufswait=2000 reproduces the original
 *      ~2 ms fast-fail exactly, which makes the A/B free.
 *
 *  (2) Context-aware budget. One long budget was wrong for every caller: the note at the top
 *      of this file says the mount path can arrive here with preemption disabled, and 4 s of
 *      IODelay(1) cannot be preempted on this single-core boot. l441 took that path and never
 *      reached userland - 'disk4s1: device is not readable.' then 'mockfs_mountroot failed: 13'
 *      at 102 s, while the fast-fail build got to 242 s and 512 spawn lines. So: thread
 *      context (interrupts enabled, timer ticks can preempt and let the holder finish) gets
 *      the seconds budget; a no-preemption context keeps a short fast-fail.
 *
 * Budgets are IODelay(1) spins: 4,000,000 is ~4 s (above the 2 s WRITE deadline), 5,000 is
 * ~5 ms. IOSleep is still not used because of the preemption note above.
 */
static inline unsigned nabu_lock_budget (void)
{
  if (!nabu_lock_arg_parsed) {
    unsigned arg = 0;
    nabu_lock_arg_parsed = true;
    if (PE_parse_boot_argn ("nabu-ufswait", &arg, sizeof (arg))) {
      nabu_lock_arg_set = true;
      nabu_lock_wait_spins = arg;
    }
  }
  if (nabu_lock_arg_set) {
    native_ufs_lock_tier = 3;
    return nabu_lock_wait_spins;
  }
  if (ml_get_interrupts_enabled ()) {
    native_ufs_lock_tier = 1;
    return 4000000u;
  }
  native_ufs_lock_tier = 2;
  return 5000u;
}
/* l479: is blocking legal right here? The answer is not "interrupts are on".
 * RootCommands.h's own header note says the mount path can reach this driver with
 * preemption already disabled, which is why lck_mtx, lck_spin and an unconditional
 * IOSleep are all excluded. clock.c makes the same distinction for IODelay(): it only
 * sleeps when the caller is preemptible, otherwise it busy-spins. So the one predicate
 * that decides both "can I sleep" and "will I pin this core" is the preemption level.
 * Declared here rather than by including sched_prim.h: this file is included inside a
 * namespace, and extern "C" still binds the global kernel symbol. */
extern "C" int get_preemption_level (void);

static inline bool nabu_lock_may_block (void)
{
  return get_preemption_level () == 0 && ml_get_interrupts_enabled () != 0;
}

/* nabu-ufs-lock-wait=1 selects this policy; without it the loop below is exactly what
 * has been running all along. The idea is not a better magic number - it is to stop
 * trading one pathology for the other. A 4,000,000-spin budget lets one contender busy
 * the only core for seconds while the holder it is waiting for needs that core; giving up
 * after 2,000 tries instead makes the read fail, and a failed page-in reaches the process
 * as SIGBUS (measured: 18 SIGBUS, 111 exited, only with nabu-ufswait=2000). So: spin a
 * short bounded round, and where sleeping is legal, sleep and try again within a real
 * budget that covers a legitimate SCSI command (READ deadline 1 s, WRITE 2 s). */
#define NABU_LOCK_EVENT_SPINS 2000u
#define NABU_LOCK_EVENT_SLEEP_MS 3000u

static bool nabu_lock_event_wait = false;
static bool nabu_lock_event_parsed = false;

static inline bool nabu_lock_event_enabled (void)
{
  if (!nabu_lock_event_parsed) {
    unsigned arg = 0;
    nabu_lock_event_parsed = true;
    if (PE_parse_boot_argn ("nabu-ufs-lock-wait", &arg, sizeof (arg))) {
      nabu_lock_event_wait = arg != 0U;
    }
  }
  return nabu_lock_event_wait;
}

static inline int nabu_busy_wait_spin (volatile int *flag, unsigned budget)
{
  for (unsigned index = 0; index < budget; ++index) {
    if (!__atomic_test_and_set (flag, __ATOMIC_ACQUIRE)) return 1;
    __atomic_fetch_add (&native_ufs_lock_waits, 1, __ATOMIC_RELAXED);
    IODelay (1);
  }
  return 0;
}

static inline int nabu_busy_wait (volatile int *flag)
{
  const unsigned budget = nabu_lock_budget ();

  if (!nabu_lock_event_enabled ()) {
    if (!nabu_busy_wait_spin (flag, budget)) {
      __atomic_fetch_add (&native_ufs_lock_timeouts, 1, __ATOMIC_RELAXED);
      return 0;
    }
    return 1;
  }

  /* One acquisition at a time is still the invariant: nothing is held between rounds,
   * and a caller that cannot block gets the same fast honest failure as before. */
  /* 19200 ticks per ms at cnthz=19.2 MHz - the same conversion already used by the
   * flush gate below (gUfsFlushMs * 19200ULL), and it avoids depending on a helper
   * that may not be declared yet at this point in the included file. */
  const uint64_t until = mach_absolute_time () +
      (uint64_t) NABU_LOCK_EVENT_SLEEP_MS * 19200ULL;
  for (;;) {
    if (nabu_busy_wait_spin (flag, NABU_LOCK_EVENT_SPINS)) return 1;
    __atomic_fetch_add (&native_ufs_lock_timeouts, 1, __ATOMIC_RELAXED);
    if (!nabu_lock_may_block ()) return 0;
    if (mach_absolute_time () >= until) return 0;
    IOSleep (1);
  }
}
// One-shot ring notice, following this driver's existing reportedNotReady idiom. The tier
// travels with it so a timeout can never again be read without knowing which budget fired.
static void nabu_note_lock_timeout (const char *who)
{
  static int reported = 0;
  if (reported) return;
  reported = 1;
  gRootDiagnostic.stage = who;
  nabu_log_puts ("ufs:lk-timeout\n");
  nabu_log_hex ("ufs:lk-tier=", (unsigned long long) native_ufs_lock_tier);
  nabu_log_flush_tail_nosync (0);
}
struct HardwareTransferPort {
  uint64_t deadline;
  uint32_t doorbell () { return read32 (gHost.hc, 0x58); }
  uint32_t interrupt () { return read32 (gHost.hc, 0x20); }
  uint32_t completedOcs () {
    cacheInvalidate (gHost.virt, 64);
    // l455: when the poll loop and the fault record disagree about the same word, the only
    // way to price that disagreement is the counter reading each side took.
    gRootDiagnostic.lastPollCnt = nabu_cntpct ();
    return *reinterpret_cast<volatile uint32_t *> (gHost.virt + 8) & 0xff;
  }
  bool expired () { return mach_absolute_time () >= deadline; }
  void delay () {
    // NABU-BRINGUP: do not use IODelay here. It spins on mach_absolute_time(), which does not
    // appear to advance this early on this board, so the first call never returns and neither
    // the deadline check nor the poll ceiling above ever executes. A fixed CPU spin always ends.
    for (unsigned spin = 0; spin < 200; ++spin) { __asm__ volatile ("yield" ::: "memory"); }
  }
};

static void putBe32 (uint8_t *at, uint32_t value)
{
  at[0] = static_cast<uint8_t> (value >> 24);
  at[1] = static_cast<uint8_t> (value >> 16);
  at[2] = static_cast<uint8_t> (value >> 8);
  at[3] = static_cast<uint8_t> (value);
}

static uint32_t loadBe32 (const uint8_t *at)
{
  return (uint32_t (at[0]) << 24) | (uint32_t (at[1]) << 16) | (uint32_t (at[2]) << 8) | at[3];
}

// Issue the UPIU already placed at gHost.virt + kUcdOffset through request slot 0. `direction` is
// the UTRD data direction (0 none, 1 from host, 2 to host). Direction 1 is reachable only through hostWrite4k, which
// confines it to the persistent store window. Returns the overall command status
// (0 = completed) in *ocs. Timeout/fatal status quarantines the allocation.
/* l446: deadline for one 4 KiB-class read, in microseconds, overridable per boot with
 * nabu-ufs-deadline-ms. Paired with gUfsPollCeiling: raising only one of the two leaves the
 * other in charge, which is what made the effective window ~70 ms instead of 1 s. */
static volatile uint32_t gUfsReadDeadlineUs = 1000000U;
/* Same reasoning as the read deadline: the first fault of the boot is a FUA
 * WRITE(10) from the log window, so the knob that matters for it is this one. */
static volatile uint32_t gUfsWriteDeadlineUs = 2000000U;
/* l450: FUA is our own choice, not a hardware requirement. `hostWrite4k` sets CDB byte 1 bit 3 so
 * the medium takes the data before the command completes, which is exactly the kind of request an
 * eMMC/UFS device may hold off while it runs background operations. Make it a boot switch so a
 * round can ask 'is it the FUA write that goes unanswered?' without touching the log format. */
static volatile uint32_t gUfsLogWriteFua = 1U;
/* l451: 0 = keep whatever clock_interval_to_deadline() produced (history).
 * 1 = use our own now + microseconds * measured counter frequency. Both values are
 * printed in the fault line, so one package answers is the conversion right and
 * does an explicit window change anything, without a second build. */
static volatile uint32_t gUfsDeadlineMode = 0U;
/* l452: 0 = today's behaviour (any transport failure quarantines the host, which is
 * what turned one slow log flush into permanent storage death); 1 = a failed WRITE
 * retires only the writers, reads continue. */
static volatile uint32_t gUfsWriteQuarantine = 0U;
/* l454: the write window is currently a guess. The first fault is a 4 KiB FUA WRITE(10) that does not
 * finish inside 2 s, and a 15 s window hides it, so the answer is somewhere between and we have never
 * measured it: a command that succeeds after two seconds looks perfectly healthy in a log that only
 * reports failures. Report slow completions, off unless asked for, one line per command up to a budget.
 * The switch has to be a boot argument rather than the diagnostic switch because these are successes. */
static volatile uint32_t gUfsSlowMs = 0U;
static volatile uint32_t gUfsSlowBudget = 8U;
static volatile uint64_t gUfsSlowSeen = 0;
static volatile uint64_t gUfsSlowMaxMs = 0;
/* l463: positive control for the slow sampler. 'slow=20 produced zero hits' cannot be read as
 * 'there are no slow commands' while the only evidence of the sampler running is a hit, and with
 * fua=0 the mean command is 0.275 ms so a 20 ms gate SHOULD be silent. This prints, every Nth
 * completed command, the worst completion seen so far and the timing of this one - no threshold
 * involved, so a silent sampler and a healthy boot can no longer be confused. 0 = off, so the
 * rounds that do not ask for it are byte-for-byte the old behaviour. */
static volatile uint32_t gUfsCmdStatEvery = 0U;
/* l464: the read verdict must not sentence a command the device already finished. Two provoked
 * rounds (l462, deadline-ms=5) recorded `res=e00002d6 ocs=0f` and, milliseconds later, read
 * `dw2=00000000` with the RESPONSE UPIU (0x21) already sitting in the UCD: the completion landed
 * after our last sample. `gUfsLateReads` counts every such conversion so the fix reports its own
 * hits; `gUfsRecoverDw2` carries the descriptor word out of hostRecoverReadList() (unset =
 * 0xFFFFFFFF sentinel, so 'the ladder never ran' can never look like 'it ran and saw success').
 * The two seq counters only advance when nabu-ufs-cmdstat is on: they measure whether userland's
 * 1.27-blocks-per-command reads are sequential, which is the only thing that would justify
 * read-ahead in this driver (the pre-userland phase already averages 25.4 blocks per command). */
static volatile uint32_t gUfsLateReads = 0U;
static volatile uint32_t gUfsRecoverDw2 = 0xFFFFFFFFU;
static volatile uint64_t gUfsSeqNextLba = 0;
/* l466b: the seq accounting is now switchable separately from the record it feeds. The board's
 * pair (l463-cmdon: CMDSTAT without seq= -> first spawn 99.0 s; l464c-cmdon: with seq= ->
 * 151.2 s, same rd/vb/rt/vt) points at the per-command work rather than the periodic line,
 * and my 'printing inside the transport lock costs ~1 s per line' story does not fit either
 * (50 lines, a handful of instructions per command). So: nabu-ufs-seq=0 keeps the record and
 * drops the per-command compare, which separates 'the census work is expensive' from 'that
 * round was just slow'. Default 1 preserves the l464c/l466 behaviour for the A/B.
 */
static volatile uint32_t gUfsSeqCensus = 1U;
/* l469: the divisor the mount-phase tail landing uses to fire off the read stream. It was a
 * literal every eighth read; with ~63k reads per boot that is ~8k landings, each of them 25
 * single-block write commands. Default 8 keeps today's behaviour for rounds that do not ask. */
static volatile uint32_t gUfsReadFlushEvery = 8U;
/* l475: phase gate for the read-ahead buffer, in seconds of uptime. Measured reason for it:
 * the pre-userland bulk read already runs at 25.4 blocks per device command (215 MB/s), and the
 * 49 s window that follows it does ZERO storage reads - so prefetch there can only compete for
 * the single slot, which is exactly the +2.6 s window cost the board measured with ra=8. The
 * place where requests really are 1.3 blocks per command is userland. 0 keeps today's
 * 'prefetch whenever asked' behaviour byte-for-byte. */
static volatile uint32_t gUfsReadAheadAfter = 0U;
static volatile uint64_t gUfsSeqHits = 0;
static volatile uint64_t gUfsSeqCmds = 0;
/* l465: optional read-ahead inside this driver, not inside the VM. The measurement that
 * justifies it: userland issues 1.27 blocks per command while the same hardware moved 25.4
 * blocks per command before userland, and the board's census says the sequential share grows
 * from 67% early in the boot to 86% by n=67,144. XNU itself could widen this - the pager
 * attribute `cluster_size` is what controls it - but every pager in this baseline pins it to
 * one page (bsd_vm.c:376, vm_shared_region_pager.c:394, vm_apple_protect.c:256,
 * vm_fourk_pager.c:238, vm_dyld_pager.c:220, device_vm.c:287) and that is Apple's own text,
 * not one of our patches. So the width is added here instead, behind a switch that is off by
 * default: nabu-ufs-read-ahead=<pages>, 1..32 (32 x 4 KiB = kDataMax, one command).
 * 0 keeps every code path byte-for-byte the behaviour of l464c. */
static volatile uint32_t gUfsReadAhead = 0U;
static volatile uint64_t gUfsRaLimitLba = 0;      // exclusive, set per request from the partition
static volatile uint64_t gUfsRaLba = 0;
static volatile uint32_t gUfsRaBlocks = 0;        // valid blocks cached from gUfsRaLba
static volatile uint64_t gUfsRaHits = 0;
static volatile uint64_t gUfsRaMisses = 0;
static volatile uint64_t gUfsRaFetched = 0;       // blocks actually read from the device
/* l468: `windowLock` is one per partition instance while `gUfsRaBuf` is a single buffer for all
 * of them, so two instances can interleave inside a cache copy and hand their caller bytes from two
 * generations - the exact shape of the corruption we have been chasing (ENOEXEC, unverifiable
 * signatures). The cache therefore gets a classic sequence counter: odd while a refill owns it,
 * even otherwise. A reader proves the generation was stable across its copy or refuses the cache;
 * a writer proves its own key survived its copy or re-reads honestly. Both count what they threw
 * away, because a race nobody counts is a race nobody finds. */
static volatile uint32_t gUfsRaSeq = 0U;
static volatile uint64_t gUfsRaStolen = 0;
/* The cache itself lives here rather than in NativeUfs.cpp because `configure()` allocates it
 * ~340 lines before the read path that uses it, and C++ needs the declaration first. Same
 * static-storage pattern as every other knob in this file. */
static uint8_t *gUfsRaBuf = nullptr;
/* l466: what the other 68% of the commands are. `n=` counts every hostSubmit and the read census
 * counts reads, so the split was only an inference; these make it measured. */
static volatile uint64_t gUfsCmdWrites = 0;
static volatile uint64_t gUfsCmdNone = 0;
/* l470: accumulated completion time of our own writes, in ms. Until now every 'write cost'
 * number in this project was (wall - rt)/writes, which is circular: it hands whatever the read
 * calls did not explain to the writes and then reports that as the price of writing. This is a
 * unit-bearing measurement taken on the same clock as rt, and it only runs when the census or
 * the slow sampler is asked for, so a default round pays nothing for it. */
static volatile uint64_t gUfsWriteMs = 0;
/* l455: the fault dump proved the point. Seven of the eight descriptor words were still
 * exactly what hostSubmit wrote, and only the OCS word had changed - to 0, success - after
 * the poll loop's last sample. A deadline that expires inside the gap between the controller
 * clearing the doorbell and it writing the status word condemns a command that worked. This
 * is a bounded second look before any verdict; 0 keeps the old behaviour, so the rounds that
 * do not ask for it are untouched. */
static volatile uint32_t gUfsGraceMs = 0U;
/* l456: the grace re-read proves the command answered late; it does not say why. UFS keeps
 * the reason inside the device - dBkopsStatus, dExceptionEventsStatus, power mode, and the
 * fDeviceInit flag - and this driver has never asked for any of them. nabu-ufs-excq selects
 * when to ask: 1 = once, the first time a command completes late or slowly; 2 = every 64th
 * window write; 3 = every 8th, so a boot leaves a time series instead of one sample. Each
 * sample costs five real QUERY commands on the same single slot, and a query that fails can
 * fault the host, so the return codes travel with the values and 0 stays off. */
static volatile uint32_t gUfsExcq = 0U;
static volatile uint32_t gUfsExcqDone = 0U;
/* l457: measured on the board, three fua=1 rounds all cap at 1,363..1,370 commands/s and fua=0 at
 * 3,634/s, and reads are whatever the cap does not spend on writes. The forensic flush is therefore
 * not a reporting detail: every unneeded flush taxes a read. Two knobs, both off by default so the
 * rounds that do not ask for them are untouched: flush-ms (skip flushes closer together than this)
 * and mark-every-N (my own fl-arm/fl-done pair, which alone filled the 4 MiB ring at fua=0). */
static volatile uint32_t gUfsFlushMs = 0U;
/* l462: measured - with a mark pair on every write, the last 96 KiB of the flushed tail held
 * 3,440/3,589 fl lines, so a panic text sitting in the 1.5 MB ring could never reach disk.
 * Default to one pair per 1000 commands; nabu-ufs-mark-every=0 still restores the old
 * mark-every-write behaviour for anyone who wants the raw sequence. */
static volatile uint32_t gUfsMarkEvery = 1000U;
static volatile uint64_t gUfsLastFlush = 0;
/* l458: excq=2 poisoned the whole host: the device answered every query (rc=0 on all five, the first
 * real attribute reads on this board) and then hostResponseHeader() rejected the QUERY RESPONSE as
 * malformed, which routes through hostProtocolError() -> gHost.poisoned -> EINVAL on every later
 * read -> `exec of /sbin/launchd failed, errno 22`. A diagnostic that cannot interpret a response
 * must retire ITSELF, not the storage. */
static volatile uint32_t gQueriesRetired = 0U;
/* l459: the two read-side hypotheses are now separated by measurement, not by argument. dl5000 and
 * dl20s both failed to raise rd(t) (57,031 at t=117 against 62,601 with a 1 s deadline, and launchd
 * reached 'running' on 25 services there but only 1 with the 20 s deadline), while grace2k's fault
 * record shows a READ(10) whose OCS never left the sentinel after 2.91 s and 81 polls. So a stuck
 * read is NOT the write-side false timeout: the device stopped serving that request. Waiting longer
 * hands the single slot to a request that will not answer. This resets the request list instead and
 * returns the error, so the filesystem can re-issue the read - without poisoning every other reader.
 * Off unless asked for: nabu-ufs-read-recover=1. */
static volatile uint32_t gUfsReadRecover = 0U;

// UFSHCI 13.1.2.1 / 13.2.2 sequence, bounded and sleepless. Two things here are learned from our own
// tree, not assumed: the vendor header (SamsungPkg/Drivers/UfsDxe/UfsDxe.h:171-176) has no name for 0x68,
// and its own driver writes RUN_STOP = 1 (UfsDxe.c:867-868) while every fault dump of ours reads rsr=1 -
// so '0 = enabled' is NOT safe to assume for this controller. The sequence therefore saves whatever
// RUN_STOP and the base addresses already are and restores them, instead of forcing spec-typical values.
static bool hostRecoverReadList (void)
{
  uint32_t lo = read32 (gHost.hc, 0x50), hi = read32 (gHost.hc, 0x54);
  gUfsRecoverDw2 = 0xFFFFFFFFU;   // l464: re-arm the sentinel on every attempt
  uint32_t rsr = read32 (gHost.hc, 0x60);
  if (!lo && !hi) {
    // l464: this used to leave no trace at all, which made 'not attempted because the list base
    // reads as zero' indistinguishable from 'the ladder never fired'. Same reason the NOCS branch
    // is printed: a silent early return on a recovery path is worse than a wrong one.
    IOLog ("UFS READ RECOVER NOBASE lo=%08x hi=%08x\n", lo, hi);
    return false;
  }
  write32 (gHost.hc, 0x60, 2U);                       // UTRLRL = Halt
  bool halted = false;
  for (unsigned spin = 0; spin < 20000U && !halted; spin++) {
    halted = (read32 (gHost.hc, 0x68) & 3U) == 2U;    // UTRLCS == Halted
    for (unsigned y = 0; y < 200U; ++y) { __asm__ volatile ("yield" ::: "memory"); }
  }
  if (!halted) {
    write32 (gHost.hc, 0x60, rsr);
    // l461: this branch used to be silent, which collapsed 'not attempted', 'halt refused' and
    // 'squeezed out of the channel' into one unreadable outcome. Report it, then fall back to the
    // old verdict. cs is the value of UTRLCS (0x68), which our vendor header never names - that
    // number is the direct evidence of whether this controller reports list status at all.
    // l462: measured on the board - UTRLCS(0x68) reads 00000000 on this controller, so waiting for it
    // to report "halted" can never succeed and the ladder never even tried the reset. Report the
    // missing acknowledgement, then continue anyway: the only completion evidence we have is the
    // CNR bit clearing itself.
    IOLog ("UFS READ RECOVER NOCS cs=%08x rsr=%08x db=%08x\n",
           read32 (gHost.hc, 0x68), rsr, read32 (gHost.hc, 0x58));
  }
  write32 (gHost.hc, 0x5C, 1U);                       // clear our slot's doorbell bit
  write32 (gHost.hc, 0x64, 1U);                       // UTRLCNR bit0 = reset the request list
  bool cleared = false;
  for (unsigned spin = 0; spin < 20000U && !cleared; spin++) {
    cleared = (read32 (gHost.hc, 0x64) & 1U) == 0U;   // self-clearing
    for (unsigned y = 0; y < 200U; ++y) { __asm__ volatile ("yield" ::: "memory"); }
  }
  write32 (gHost.hc, 0x50, lo);
  write32 (gHost.hc, 0x54, hi);
  write32 (gHost.hc, 0x60, rsr);                      // restore what we found, not an assumed 0
  // l462: the A/B arms differed only in this word - one timed-out command had already been
  // answered (dw2=00000000 = OCS Success), the other had not (dw2=0000000f = our sentinel).
  // Without it, "we reset a stuck list" and "it was finished anyway" read the same.
  cacheInvalidate (gHost.virt, 64);
  uint32_t dw2 = *reinterpret_cast<volatile uint32_t *> (gHost.virt + 8);
  gUfsRecoverDw2 = dw2;   // l464: the caller converts a read that the reset proves was answered
  IOLog ("UFS READ RECOVER halted=%u cnrCleared=%u base=%08x:%08x rsr=%08x cs=%08x db=%08x dw2=%08x\n",
         halted ? 1U : 0U, cleared ? 1U : 0U, hi, lo, rsr, read32 (gHost.hc, 0x68),
         read32 (gHost.hc, 0x58), dw2);
  return cleared;
}

// l456: defined below, after hostQueryRead(), but called from hostSubmit()'s success path.
static void hostReadExceptionAttrs (uint32_t every);

static IOReturn hostSubmit (uint32_t direction, uint32_t dataBytes, uint32_t timeoutUs, uint32_t *ocs)
{
  *ocs = 15;
  if (!timeoutUs || dataBytes > kDataMax || (dataBytes & 3) ||
      (dataBytes ? (direction != 1 && direction != 2) : direction != 0)) return kIOReturnBadArgument;
  if (gHost.poisoned) return kIOReturnNotReady;
  if (read32 (gHost.hc, 0x58) != 0) {
    gHost.poisoned = true;
    gHost.ready = false;
    return recordTransport (kIOReturnBusy, *ocs);
  }
  if (read32 (gHost.hc, 0x20) & kUfsFatalStatus) {
    gHost.poisoned = true;
    gHost.ready = false;
    return recordTransport (kIOReturnIOError, *ocs);
  }
  gRootDiagnostic.command++;
  if (direction == 1U) { __atomic_fetch_add (&gUfsCmdWrites, 1, __ATOMIC_RELAXED); }
  else if (!direction) { __atomic_fetch_add (&gUfsCmdNone, 1, __ATOMIC_RELAXED); }
  gRootDiagnostic.cmdDir = direction; gRootDiagnostic.cmdBytes = dataBytes;
  gRootDiagnostic.cmdSeq = gRootDiagnostic.command;
  gRootDiagnostic.type = gHost.virt[kUcdOffset];
  gRootDiagnostic.lun = gHost.virt[kUcdOffset + 2];
  gRootDiagnostic.opcode = gHost.virt[kUcdOffset + (gRootDiagnostic.type == 1 ? 16 : 12)];
  gRootDiagnostic.scsi = 0xffff;
  gRootDiagnostic.response = 0;
  gRootDiagnostic.protocol = 0;
  gRootDiagnostic.residual = 0;
  gRootDiagnostic.sense = 0;
  uint8_t *utrd = gHost.virt;
  uint64_t ucdPhys = gHost.phys + kUcdOffset;
  uint32_t *words = reinterpret_cast<uint32_t *> (utrd);
  for (unsigned index = 0; index < 8; index++) {
    words[index] = 0;
  }
  words[0] = 0x10000000U | (direction << 25);
  words[2] = 0x0fU;
  words[4] = static_cast<uint32_t> (ucdPhys);
  words[5] = static_cast<uint32_t> (ucdPhys >> 32);
  words[6] = (512U >> 2) | ((kRspOffset >> 2) << 16);
  words[7] = (dataBytes ? 1U : 0U) | ((kPrdtOffset >> 2) << 16);
  if (dataBytes) {
    uint64_t dataPhys = gHost.phys + kDataOffset;
    uint32_t *prd = reinterpret_cast<uint32_t *> (gHost.virt + kUcdOffset + kPrdtOffset);
    prd[0] = static_cast<uint32_t> (dataPhys);
    prd[1] = static_cast<uint32_t> (dataPhys >> 32);
    prd[2] = 0;
    prd[3] = dataBytes - 1;
  }
  cacheCivac (gHost.virt, kDataOffset + dataBytes);
  hostWrite (0x20, 1);  // Acknowledge completion only; preserve error evidence.
  HardwareTransferPort port;
  // clock_interval_to_deadline() returns void in this kernel, so there is no refusal to read:
  // the only way to check the conversion is to do it ourselves from the measured counter
  // frequency and print both. want - got is then the scaling error, in ticks.
  uint64_t nowTicks = mach_absolute_time ();
  uint64_t hz = nabu_cntfreq ();
  if (hz < 1000000ULL || hz > 100000000ULL) { hz = 19200000ULL; }
  uint64_t wantTicks = nowTicks + (uint64_t) timeoutUs * hz / 1000000ULL;
  clock_interval_to_deadline (timeoutUs, kMicrosecondScale, &port.deadline);
  gRootDiagnostic.dlWant = wantTicks;
  gRootDiagnostic.dlGot = port.deadline;
  gRootDiagnostic.dlMode = gUfsDeadlineMode;
  if (gUfsDeadlineMode == 1U) { port.deadline = wantTicks; }
  if (gRootDiagnosticEnabled && direction == 1) {
    // l455: the board's ask, in the form that survives their measurement. The ring and the
    // window share one thread, so a mark written after the hang is lost with it - a mark
    // written before the ring bell is already in the ring when the hang happens, and any
    // round where reads stay alive flushes it. Writes only: the flush cadence, not 43k reads.
    if (gUfsMarkEvery == 0U || (gRootDiagnostic.command % gUfsMarkEvery) == 0U) {
      nabu_log_hex ("ufs:fl-arm=", (unsigned long long) gRootDiagnostic.command);
    }
  }
  hostWrite (0x58, 1);
  // l446: price the poll loop. 7 polls against a 2 s deadline means the loop was
  // off-CPU, not slow: this is the only way to tell 'device answers late' from
  // 'our polling thread is starved now that ticks exist'.
  gRootDiagnostic.submitTicks = mach_absolute_time ();
  gRootDiagnostic.submitCnt = nabu_cntpct ();
  if (!gRootDiagnostic.cntfreq) { gRootDiagnostic.cntfreq = nabu_cntfreq (); }
  UfsTransferPoll state;
  UfsTransferEnd end = pollUfsTransfer (port, state);
  gRootDiagnostic.polls = state.polls;
  gRootDiagnostic.dbSeen = state.doorbellSeen;
  gRootDiagnostic.isSeen = state.interruptSeen;
  *ocs = state.ocs;
  // l457: restricted to direction==1. As written in l455 the re-look applied to reads too, and a read
  // timeout then spun up to 2 s inside the mount path with the host lock held - that is why grace=2000
  // died 50 s earlier than the default round instead of surviving longer.
  if (direction == 1U && end == UfsTransferEnd::Timeout && gUfsGraceMs && !(read32 (gHost.hc, 0x58) & 1U)) {
    // Same read the poll loop was already making, for a bounded while longer. Nothing is
    // re-submitted, so the slot the device may still own is left alone.
    uint64_t stop = mach_absolute_time () + (uint64_t) gUfsGraceMs * 19200ULL;
    uint32_t seen = port.completedOcs ();
    while (seen == 15U && mach_absolute_time () < stop) {
      seen = port.completedOcs ();
      gRootDiagnostic.gracePolls++;
    }
    if (seen != 15U) {
      end = UfsTransferEnd::Completed; *ocs = seen;
      // l457: the resolution has to be countable on its own. In l455 gracePolls only reached the
      // fault dump, so 'it ran and helped' and 'the branch never fired' were indistinguishable.
      IOLog ("UFS GRACE RESCUED seq=%u ocs=%02x polls=%u\n", gRootDiagnostic.cmdSeq, seen,
             gRootDiagnostic.gracePolls);
    }
  }
  if (gRootDiagnosticEnabled && direction == 1) {
    uint64_t doneMs = (mach_absolute_time () - gRootDiagnostic.submitTicks) / 19200ULL;
    if (gUfsMarkEvery == 0U || (gRootDiagnostic.command % gUfsMarkEvery) == 0U) {
      nabu_log_hex ("ufs:fl-done=", (unsigned long long) doneMs);
    }
    if (doneMs >= 1000U) {
      // the slowest command in the boot is the one that decides whether a deadline was sane
      IOLog ("UFS WRITE SLOW1S seq=%u dir=%u ms=%llu total=%u rescues=%u late=%u\n", gRootDiagnostic.cmdSeq, direction,
             (unsigned long long) doneMs, gRootDiagnostic.command, gUfsFalseTimeouts, gUfsLateReads);
    }
  }
  /* l464: one last look at the status word before a read is condemned. `UFS GRACE RESCUED` is the
   * same idea for writes and is bounded by a spin; here the cost is deliberately one cache
   * invalidate and one load - no loop, no MMIO - because this branch runs inside the mount path
   * with preemption off, and a read that is genuinely stuck must still fail fast. */
  if (direction == 2U && end == UfsTransferEnd::Timeout && !(port.doorbell () & 1U)) {
    /* l464c: the doorbell test is NOT cosmetic. A non-sentinel OCS while the doorbell is still set
     * means the device still owns this slot; declaring the command finished and returning lets the
     * NEXT submission hit `read32 (0x58) != 0` at the top of hostSubmit() and poison the whole host -
     * the exact failure mode we spent a day removing. With DB clear the transfer is over, the
     * status word is final, and completing it is safe. The two provoked l462 rounds had db=0 and
     * OCS 0, so this is the same condition they satisfied, only checked by us. */
    uint32_t late = port.completedOcs ();
    if (late != 15U) {
      __atomic_fetch_add (&gUfsLateReads, 1, __ATOMIC_RELAXED);
      IOLog ("UFS LATE READ seq=%u ocs=%02x polls=%u late=%u via=%c\n", gRootDiagnostic.cmdSeq,
             late, gRootDiagnostic.polls, gUfsLateReads, 'p');
      nabu_log_hex ("ufs:late=", (unsigned long long) gUfsLateReads);
      end = UfsTransferEnd::Completed; *ocs = late;
    }
  }
  if (end != UfsTransferEnd::Completed) {
    // No cancel/reset/retry: the controller may still own the buffer, even
    // with DB=0. Keep it allocated and prohibit all subsequent CPU writes.
    IOReturn code = end == UfsTransferEnd::Timeout ? kIOReturnTimeout : kIOReturnIOError;
    if (direction == 2U && end == UfsTransferEnd::Timeout && gUfsReadRecover == 1U &&
        hostRecoverReadList ()) {
      // The list came back and the controller is enabled again. Report the fault (recordTransport still
      // prints its once-per-boot snapshot) but leave gHost.poisoned alone: this read failed, the device
      // is not dead, and sentencing every other reader is what turned one slow command into a wall.
      nabu_log_hex ("ufs:rdrec=", (unsigned long long) gRootDiagnostic.command);
      if (gUfsRecoverDw2 != 0U) {
        return recordTransport (code, *ocs);   // truly unanswered: report it, do not poison
      }
      // l464: the reset proved the descriptor was already answered (OCS 0, response UPIU in the
      // UCD). Returning Timeout here hands APFS an error for a command the device completed, and
      // that is what surfaced as failed execs and unverifiable binaries. Fall through to the
      // normal completion path: the data is in the bounce buffer. The poison tail below is
      // guarded by the same test, so this must not `return`.
      __atomic_fetch_add (&gUfsLateReads, 1, __ATOMIC_RELAXED);
      IOLog ("UFS LATE READ seq=%u ocs=%02x polls=%u late=%u via=%c\n", gRootDiagnostic.cmdSeq,
             0U, gRootDiagnostic.polls, gUfsLateReads, 'l');
      nabu_log_hex ("ufs:late=", (unsigned long long) gUfsLateReads);
      end = UfsTransferEnd::Completed; *ocs = 0;
    }
    if (direction == 1 && gUfsWriteQuarantine == 1U) {
      // Our own forensic write went unanswered. Retire the writers only: reads from the same
      // single-slot queue keep flowing, and no further CPU write can reach the device. The
      // buffer stays allocated, so nothing is re-issued into a slot the device may still own.
      gHost.writeQuarantined = true;
      return recordTransport (code, *ocs);
    }
    if (end == UfsTransferEnd::Completed) {
      // l464: rescued above (the read landed after the deadline, or the ladder proved it was
      // answered). The outer verdict no longer applies, so this command must neither poison the
      // transport nor be reported as failed - it falls through to the completion path.
    } else {
      gHost.poisoned = true;
      gHost.ready = false;
      return recordTransport (code, *ocs);
    }
  }
  // l454: this command completed. Time it anyway - the window we chose is only defensible
  // if it clears the real worst case, and the worst case is what nobody has looked at.
  if (gUfsSlowMs || gUfsCmdStatEvery) {
    uint64_t ms = (mach_absolute_time () - gRootDiagnostic.submitTicks) / 19200ULL;
    if (ms > gUfsSlowMaxMs) { gUfsSlowMaxMs = ms; }
    if (direction == 1U) {
      __atomic_fetch_add (&gUfsWriteMs, ms, __ATOMIC_RELAXED);
    }
    if (gUfsSlowMs && ms >= gUfsSlowMs && gUfsSlowSeen < gUfsSlowBudget) {
      __atomic_fetch_add (&gUfsSlowSeen, 1, __ATOMIC_RELAXED);   // volatile++ is deprecated
      IOLog ("UFS SLOW CMD dir=%u lba=%llx bytes=%u fua=%u polls=%u ms=%llu stage=%s\n",
             gRootDiagnostic.cmdDir, (unsigned long long)gRootDiagnostic.cmdLba,
             gRootDiagnostic.cmdBytes, gRootDiagnostic.cmdFua, state.polls,
             (unsigned long long)ms, gRootDiagnostic.stage);
    }
    if (gUfsCmdStatEvery && gUfsSeqCensus && direction == 2U) {
      // Sequentiality census: was this command's first LBA exactly where the previous one ended?
      // This is the measurement that decides whether read-ahead is worth building at all.
      uint64_t blocks = gRootDiagnostic.cmdBytes / 4096U;
      if (gUfsSeqCmds && gRootDiagnostic.cmdLba == gUfsSeqNextLba) {
        __atomic_fetch_add (&gUfsSeqHits, 1, __ATOMIC_RELAXED);
      }
      gUfsSeqNextLba = gRootDiagnostic.cmdLba + blocks;
      __atomic_fetch_add (&gUfsSeqCmds, 1, __ATOMIC_RELAXED);
    }
    if (gUfsCmdStatEvery && (gRootDiagnostic.command % gUfsCmdStatEvery) == 0U) {
      // max is the running worst case (never reset), so each step in the series marks the moment
      // the worst command of the boot happened; ms is this command, which says whether the boot
      // is currently healthy or only remembers one bad event.
      /* l465b: the periodic census must not print from here. Measured on the board with the same
       * kernel: that 52 s did NOT reproduce. Same package, same arguments -> 46.54 / 98.91 s, and
       * across the 90 archived CDC rounds the pre-launchd phase doubles in exactly 2 of them (2.2%),
       * with the same tick count and verbatim-identical rd/vb/rt at the checkpoint. So the census
       * never cost 52 s and nothing here needed repairing. The outlet still becomes the ring, for
       * two reasons that survive the lost attribution: it is free, and printing from inside
       * hostSubmit() while it holds the transport lock remains the oldest rule on this driver. Eight marks per snapshot, every Nth command:
       * at N=4000 and 200k commands that is ~12 KB of a 4 MiB ring, read back from the disk pull
       * after the round instead of from CDC. Rare event lines (UFS LATE READ, WRITE SLOW1S, FAULT1)
       * still print, because those fire a handful of times per boot. */
      nabu_log_hex ("ufs:cs-n=", (unsigned long long) gRootDiagnostic.command);
      nabu_log_hex ("ufs:cs-max=", gUfsSlowMaxMs);
      nabu_log_hex ("ufs:cs-sh=", (unsigned long long) gUfsSeqHits);
      nabu_log_hex ("ufs:cs-sc=", (unsigned long long) gUfsSeqCmds);
      nabu_log_hex ("ufs:cs-ra=", (unsigned long long) gUfsRaHits);
      nabu_log_hex ("ufs:cs-rm=", (unsigned long long) gUfsRaMisses);
      nabu_log_hex ("ufs:cs-w=", gUfsCmdWrites);
      nabu_log_hex ("ufs:cs-x=", gUfsCmdNone);
      /* l472: read-ahead seqlock steals, moved here from the flush path (see NativeUfs.cpp). Hits and misses were already in this snapshot as cs-ra / cs-rm; this is the third of the three,
       * and with it the whole read-ahead audit lives on the census outlet instead of on the
       * path that writes to the medium. */
      nabu_log_hex ("ufs:cs-rs=", (unsigned long long) gUfsRaStolen);
      /* l470: write ms, the number that replaces (wall - rt)/writes. Compare against the `rt=`
       * field of the same checkpoint: rt is read-call time, this is write-call time, and the two
       * together say what share of the boot the transport was even asked about. */
      nabu_log_hex ("ufs:wt=", (unsigned long long) gUfsWriteMs);
    }
  }
  cacheInvalidate (gHost.virt, kDataOffset + dataBytes);
  gRootDiagnostic.response = loadBe32 (gHost.virt + kUcdOffset + kRspOffset);
  IOReturn verdict = recordTransport (*ocs == 0 ? kIOReturnSuccess : kIOReturnIOError, *ocs);
  // Last, after this command's own data and response have been read out: the queries rebuild
  // the same shared UCD buffer and overwrite the response area.
  if (gUfsExcq == 1U && (gRootDiagnostic.gracePolls || gUfsSlowSeen)) hostReadExceptionAttrs (0U);
  if (direction == 1U && gUfsExcq >= 2U) hostReadExceptionAttrs (gUfsExcq == 3U ? 8U : 64U);
  return verdict;
}

static bool hostClearUcd (void)
{
  if (gHost.poisoned) return false;
  // Check before touching the command/response area, not just before ringing.
  if (read32 (gHost.hc, 0x58) != 0) {
    gHost.poisoned = true;
    gHost.ready = false;
    recordTransport (kIOReturnBusy, 15);
    return false;
  }
  if (read32 (gHost.hc, 0x20) & kUfsFatalStatus) {
    gHost.poisoned = true;
    gHost.ready = false;
    recordTransport (kIOReturnIOError, 15);
    return false;
  }
  uint32_t *words = reinterpret_cast<uint32_t *> (gHost.virt + kUcdOffset);
  for (unsigned index = 0; index < (kPrdtOffset + 16) / 4; index++) {
    words[index] = 0;
  }
  return true;
}

static IOReturn hostProtocolError (uint32_t reason)
{
  gRootDiagnostic.protocol = reason;
  gHost.ready = false;
  gHost.poisoned = true;
  return recordTransport (kIOReturnIOError, gRootDiagnostic.ocs);
}

// Validate a completed response before interpreting zero-filled status bytes.
// Protocol codes: 1 type, 2 task tag, 3 LUN, 4 length/EHS, 5 query echo,
// 6 short/overrun transfer, 7 nonzero NOP result.
static IOReturn hostResponseHeader (uint8_t type, uint8_t lun, uint8_t tag)
{
  const uint8_t *rsp = gHost.virt + kUcdOffset + kRspOffset;
  if (rsp[0] != type) return hostProtocolError (1);
  if (rsp[3] != tag) return hostProtocolError (2);
  if (rsp[2] != lun) return hostProtocolError (3);
  unsigned length = (unsigned (rsp[10]) << 8) | rsp[11];
  if (rsp[8] || length > 512 - 32) return hostProtocolError (4);
  return kIOReturnSuccess;
}

static IOReturn hostNop (void)
{
  if (!hostClearUcd ()) return kIOReturnNotReady;
  gHost.virt[kUcdOffset + 3] = 1;
  uint32_t ocs = 15;
  IOReturn result = hostSubmit (0, 0, 200000, &ocs);
  if (result != kIOReturnSuccess) return result;
  result = hostResponseHeader (0x20, 0, 1);
  if (result != kIOReturnSuccess) return result;
  const uint8_t *rsp = gHost.virt + kUcdOffset + kRspOffset;
  if (rsp[10] || rsp[11]) return hostProtocolError (4);
  if (rsp[6] || rsp[7]) return hostProtocolError (7);
  return kIOReturnSuccess;
}

// Run a SCSI command. direction 0: no data, 1: host -> device from the DMA data area (already filled by the caller), 2: device
// -> host into the DMA data area. Callers other than hostWrite4k never pass direction 1.
static IOReturn hostScsiDir (uint8_t lun, const uint8_t *cdb, unsigned cdbLength, unsigned direction, uint32_t dataBytes,
                             uint32_t timeoutUs, uint32_t *scsiStatus)
{
  *scsiStatus = 0xffff;
  if (!cdb || !cdbLength || cdbLength > 16 || dataBytes > kDataMax || direction > 2 ||
      (dataBytes & 3) || (dataBytes ? !direction : direction)) return kIOReturnBadArgument;
  if (!hostClearUcd ()) return kIOReturnNotReady;
  uint8_t *ucd = gHost.virt + kUcdOffset;
  ucd[0] = 0x01;                      // COMMAND UPIU
  ucd[1] = direction == 2 ? 0x40 : direction == 1 ? 0x20 : 0x00;   // R: data in, W: data out
  ucd[2] = lun;
  putBe32 (ucd + 12, dataBytes);
  for (unsigned index = 0; index < cdbLength; index++) {
    ucd[16 + index] = cdb[index];
  }
  uint32_t ocs = 0xf;
  IOReturn status = hostSubmit (direction ? direction : 0U, dataBytes, timeoutUs, &ocs);
  if (status != kIOReturnSuccess) return status;
  status = hostResponseHeader (0x21, lun, 0);
  if (status != kIOReturnSuccess) return status;
  const uint8_t *rsp = ucd + kRspOffset;
  *scsiStatus = (uint32_t (rsp[6]) << 8) | rsp[7];
  gRootDiagnostic.scsi = *scsiStatus;
  gRootDiagnostic.residual = loadBe32 (rsp + 12);
  unsigned length = (unsigned (rsp[10]) << 8) | rsp[11];
  if (length) {
    unsigned senseLength = (unsigned (rsp[32]) << 8) | rsp[33];
    if (length < 2 || senseLength > length - 2) return hostProtocolError (4);
    const uint8_t *sense = rsp + 34;
    if (senseLength >= 14 && ((sense[0] & 0x7f) == 0x70 || (sense[0] & 0x7f) == 0x71))
      gRootDiagnostic.sense = (uint32_t (sense[2] & 15) << 16) | (uint32_t (sense[12]) << 8) | sense[13];
    else if (senseLength >= 4 && ((sense[0] & 0x7f) == 0x72 || (sense[0] & 0x7f) == 0x73))
      gRootDiagnostic.sense = (uint32_t (sense[1] & 15) << 16) | (uint32_t (sense[2]) << 8) | sense[3];
  }
  if (!*scsiStatus && ((rsp[1] & 0x60) || gRootDiagnostic.residual)) return hostProtocolError (6);
  IOReturn result = *scsiStatus == 0 ? kIOReturnSuccess : kIOReturnNotReady;
  gRootDiagnostic.result = static_cast<uint32_t> (result);
  return result;
}

// The original read-only entry point: data, when present, always flows device -> host.
static IOReturn hostScsi (uint8_t lun, const uint8_t *cdb, unsigned cdbLength, bool toHost, uint32_t dataBytes,
                          uint32_t timeoutUs, uint32_t *scsiStatus)
{
  if (dataBytes && !toHost) { *scsiStatus = 0xffff; return kIOReturnBadArgument; }
  return hostScsiDir (lun, cdb, cdbLength, dataBytes ? 2U : 0U, dataBytes, timeoutUs, scsiStatus);
}

// Query read request (opcode 0x03 READ ATTRIBUTE / 0x05 READ FLAG). Returns the value field.
static IOReturn hostQueryRead (uint8_t opcode, uint8_t idn, uint32_t *value)
{
  if (!value || (opcode != 3 && opcode != 5)) return kIOReturnBadArgument;
  if (!hostClearUcd ()) return kIOReturnNotReady;
  uint8_t *ucd = gHost.virt + kUcdOffset;
  ucd[0] = 0x16;                      // QUERY REQUEST
  ucd[3] = 1;
  ucd[5] = 0x01;                      // standard read request (header dword 1, byte 1)
  ucd[12] = opcode;
  ucd[13] = idn;
  uint32_t ocs = 0xf;
  IOReturn status = hostSubmit (0, 0, 200000, &ocs);
  if (status != kIOReturnSuccess) {
    return status;
  }
  status = hostResponseHeader (0x36, 0, 1);
  if (status != kIOReturnSuccess) return status;
  const uint8_t *rsp = ucd + kRspOffset;
  if (rsp[10] || rsp[11]) return hostProtocolError (4);
  if (rsp[12] != opcode || rsp[13] != idn || rsp[14] || rsp[15]) return hostProtocolError (5);
  if (rsp[6] != 0) {
    gRootDiagnostic.result = static_cast<uint32_t> (kIOReturnError);
    return kIOReturnError;
  }
  *value = loadBe32 (rsp + 20);
  return kIOReturnSuccess;
}

// dExceptionEventsStatus is quoted as IDN 0x0F by one source and 0x10 by another, so both are
// read rather than one being guessed. 0x05 = dBkopsStatus (0 none, 1 idle, 2 paused, 3 bad),
// 0x02 = bCurrentPowerMode (the attribute the bring-up query already proves we can read),
// flag 0x01 = fDeviceInit, because a device that never finished initialisation looks exactly
// like a slow command. Queries are direction 0, so they can never re-enter this sampler.
// Soft variant: same request as hostQueryRead(), but a response we cannot interpret returns an
// error and retires the sampler without touching gHost.poisoned. hostQueryRead()'s own checks
// deliberately keep their power to quarantine, because those run against real boot traffic.
static IOReturn hostQueryReadSoft (uint8_t opcode, uint8_t idn, uint32_t *value)
{
  if (gQueriesRetired || !value || (opcode != 3 && opcode != 5)) return kIOReturnUnsupported;
  if (gHost.poisoned || !hostClearUcd ()) return kIOReturnNotReady;
  uint8_t *ucd = gHost.virt + kUcdOffset;
  ucd[0] = 0x16; ucd[3] = 1; ucd[5] = 0x01; ucd[12] = opcode; ucd[13] = idn;
  uint32_t ocs = 0xf;
  IOReturn status = hostSubmit (0, 0, 200000, &ocs);
  if (status != kIOReturnSuccess) { gQueriesRetired = 1U; return status; }
  const uint8_t *rsp = ucd + kRspOffset;
  if (rsp[0] != 0x36 || rsp[3] != 1 || rsp[12] != opcode || rsp[13] != idn || rsp[6] != 0) {
    gRootDiagnostic.protocol = 0;      // do not let our disagreement look like a transport fault
    gQueriesRetired = 1U;
    return kIOReturnError;
  }
  *value = loadBe32 (rsp + 20);
  return kIOReturnSuccess;
}

// l458: READ FLAG(fDeviceInit) is gone. Its response layout is not the attribute layout, that is what
// our header validation choked on; the four attributes all came back rc=0. EE bits are decoded with the
// single bit we care about (URGENT_BKOPS = BIT(2)), not a 0x204 mask that silently includes health.
static void hostReadExceptionAttrs (uint32_t every)
{
  if (!gUfsExcq || gQueriesRetired) return;
  if (every) { if (gRootDiagnostic.command % every) return; }
  else { if (gUfsExcqDone) return; gUfsExcqDone = 1U; }
  uint32_t pwr = 0, bkops = 0, ee_ctl = 0, ee_status = 0;
  IOReturn r1 = hostQueryReadSoft (3, 0x02, &pwr);
  IOReturn r2 = hostQueryReadSoft (3, 0x05, &bkops);
  IOReturn r3 = hostQueryReadSoft (3, 0x0D, &ee_ctl);      // EE_CONTROL, ufs_kernel.h:171
  IOReturn r4 = hostQueryReadSoft (3, 0x0E, &ee_status);   // EE_STATUS, ufs_kernel.h:172
  IOLog ("UFS EXCQ cmd=%u pwr=%u bkops=%u ee_ctl=%u ee_status=%u urgent_bkops=%u "
         "rc=%x/%x/%x/%x\n",
         gRootDiagnostic.command, pwr, bkops, ee_ctl, ee_status, (ee_status & 0x4U) ? 1U : 0U,
         (unsigned) r1, (unsigned) r2, (unsigned) r3, (unsigned) r4);
  nabu_log_hex ("ufs:excq-bk=", (unsigned long long) bkops);
  nabu_log_hex ("ufs:excq-ex=", (unsigned long long) ee_status);
}

// Read `blocks` (<= kDataMax / 4096) 4096-byte blocks of LUN `lun` starting at `lba` into dst.
static IOReturn hostRead4k (uint8_t lun, uint64_t lba, uint32_t blocks, void *dst)
{
  if (!gHost.ready || !dst || !blocks || blocks > kDataMax / 4096 || lba > 0xffffffffULL - (blocks - 1)) {
    return kIOReturnBadArgument;
  }
  if (!nabu_busy_wait (&gHost.lock)) { nabu_note_lock_timeout ("host lock wait expired"); return kIOReturnNotReady; }
  gRootDiagnostic.lba = lba;
  gRootDiagnostic.cmdLba = lba; gRootDiagnostic.cmdBytes = blocks * 4096U;
  gRootDiagnostic.cmdDir = 2; gRootDiagnostic.cmdFua = 0;
  uint8_t cdb[10] = { 0x28, 0, uint8_t (lba >> 24), uint8_t (lba >> 16), uint8_t (lba >> 8), uint8_t (lba),
                      0, uint8_t (blocks >> 8), uint8_t (blocks), 0 };
  uint32_t scsi = 0;
  IOReturn status = hostScsi (lun, cdb, 10, true, blocks * 4096U, gUfsReadDeadlineUs, &scsi);
  if (status == kIOReturnSuccess) {
    memcpy (dst, gHost.virt + kDataOffset, blocks * 4096U);
  }
  nabu_busy_release (&gHost.lock);
  return status;
}

// ---- Persistent store and log windows ------------------------------------------------------------------------------
// The only place this driver writes. At most two windows of 4096-byte LBAs on LUN 0 (the Data store and the diagnostic log),
// each opened once by its integration after it has proved that the window lies inside the userdata partition and clear of the
// root image; every write is checked against them again here, so nothing outside them can be written even if a caller is wrong.
static bool gStoreOpen = false, gLogOpen = false, gMiscOpen = false;
static uint64_t gStoreFirst = 0, gStoreBlocks = 0, gLogFirst = 0, gLogBlocks = 0, gMiscFirst = 0, gMiscBlocks = 0;

static bool hostLogOpen (uint64_t first, uint64_t blocks)
{
  if (gLogOpen || !blocks || first > 0xffffffffULL || blocks > 0xffffffffULL - first) return false;
  if (gStoreOpen && first < gStoreFirst + gStoreBlocks && gStoreFirst < first + blocks) return false;
  gLogFirst = first;
  gLogBlocks = blocks;
  gLogOpen = true;
  return true;
}

static bool hostInWindow (uint64_t lba, uint32_t blocks)
{
  if (gStoreOpen && lba >= gStoreFirst && lba - gStoreFirst < gStoreBlocks && blocks <= gStoreBlocks - (lba - gStoreFirst)) return true;
  if (gLogOpen && lba >= gLogFirst && lba - gLogFirst < gLogBlocks && blocks <= gLogBlocks - (lba - gLogFirst)) return true;
  /* nabu: the misc window holds the bootloader_message (block 0) and one diagnostic
   * block right behind it; writes are still range-checked. */
  return gMiscOpen && lba >= gMiscFirst && lba - gMiscFirst < gMiscBlocks &&
    blocks <= gMiscBlocks - (lba - gMiscFirst);
}

// One-block write window on the misc partition (the Android bootloader_message block),
// optionally extended by a diagnostic block that the bootloader never looks at.
static bool hostMiscOpen (uint64_t first, uint64_t blocks = 1)
{
  if (gMiscOpen || !first || !blocks || blocks > 16 || first > 0xfffffffeULL - blocks) return false;
  if (gStoreOpen && first < gStoreFirst + gStoreBlocks && gStoreFirst < first + blocks) return false;
  if (gLogOpen && first < gLogFirst + gLogBlocks && gLogFirst < first + blocks) return false;
  gMiscFirst = first;
  gMiscBlocks = blocks;
  gMiscOpen = true;
  return true;
}

static bool hostStoreOpen (uint64_t first, uint64_t blocks)
{
  if (gStoreOpen || !blocks || first > 0xffffffffULL || blocks > 0xffffffffULL - first) return false;
  if (gLogOpen && first < gLogFirst + gLogBlocks && gLogFirst < first + blocks) return false;
  gStoreFirst = first;
  gStoreBlocks = blocks;
  gStoreOpen = true;
  return true;
}

// WRITE(10) of `blocks` 4096-byte blocks from src to lba on LUN 0; the range must lie inside one open window.
static IOReturn hostWrite4k (uint8_t lun, uint64_t lba, uint32_t blocks, const void *src,
                             bool fua = false)
{
  if (!gHost.ready || !src || !blocks || blocks > kDataMax / 4096 || lun != 0 || !hostInWindow (lba, blocks)) {
    return kIOReturnBadArgument;
  }
  if (gHost.writeQuarantined) return kIOReturnNotReady;  // l452: writers retired
  if (!nabu_busy_wait (&gHost.lock)) { nabu_note_lock_timeout ("host lock wait expired"); return kIOReturnNotReady; }
  gRootDiagnostic.lba = lba;
  gRootDiagnostic.cmdLba = lba; gRootDiagnostic.cmdBytes = blocks * 4096U;
  gRootDiagnostic.cmdDir = 1; gRootDiagnostic.cmdFua = fua ? 1U : 0U;
  memcpy (gHost.virt + kDataOffset, src, blocks * 4096U);
  /* FUA (CDB byte 1, bit 3) forces the medium to take the data before the command
   * completes. The log window needs it: plain write + SYNCHRONIZE CACHE left the
   * old bytes on NAND while the driver's own readback hit its cache (2026-10-26). */
  uint8_t cdb[10] = { 0x2a, uint8_t (fua ? 0x08 : 0), uint8_t (lba >> 24), uint8_t (lba >> 16),
                      uint8_t (lba >> 8), uint8_t (lba),
                      0, uint8_t (blocks >> 8), uint8_t (blocks), 0 };
  uint32_t scsi = 0;
  IOReturn status = hostScsiDir (0, cdb, 10, 1, blocks * 4096U, gUfsWriteDeadlineUs, &scsi);
  nabu_busy_release (&gHost.lock);
  return status;
}

// SYNCHRONIZE CACHE(10) for the whole LUN: everything written so far is on the medium when this returns.
static IOReturn hostSyncCache (void)
{
  if (!gHost.ready || (!gStoreOpen && !gLogOpen && !gMiscOpen)) return kIOReturnNotReady;
  if (!nabu_busy_wait (&gHost.lock)) { nabu_note_lock_timeout ("host lock wait expired"); return kIOReturnNotReady; }
  if (gHost.writeQuarantined) return kIOReturnNotReady;  // l452: writers retired
  uint8_t cdb[10] = { 0x35 };
  uint32_t scsi = 0;
  IOReturn status = hostScsiDir (0, cdb, 10, 0, 0, 10000000, &scsi);
  nabu_busy_release (&gHost.lock);
  return status;
}

