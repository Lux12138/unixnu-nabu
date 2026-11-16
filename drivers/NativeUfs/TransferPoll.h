// SPDX-License-Identifier: BSD-2-Clause-Patent
// Single-slot completion policy. Port supplies ordered MMIO, invalidate-only
// descriptor reads, a monotonic deadline and bounded pacing. No resubmission.
// l460: one counter for one mechanism. The pre-Timeout re-read turned a 2.93 s write, which
// should have hit the 2 s deadline, into a Completed with a zero fault count for the whole boot.
// That was a single case, so the mechanism now reports itself every time it fires.
static volatile uint32_t gUfsFalseTimeouts = 0U;
/* l461: the same-kernel scatter measured on the board (err0@117 swings 85..107 on one identical
// kernel by changing only extra arguments) retired the idea that this re-read costs throughput.
// It stays switchable anyway so the mechanism has an off state to prove itself against: with it
// off, rescues= must read 0. Default 1 keeps the behaviour of every kernel since l458. */
static volatile uint32_t gUfsPoRecheck = 1U;

enum class UfsTransferEnd { Completed, Fatal, Timeout };
struct UfsTransferPoll {
  uint32_t polls = 0, doorbellSeen = 0, interruptSeen = 0, ocs = 15;
};
static constexpr uint32_t kUfsFatalStatus = 0x71880U;
/* l446: the iteration ceiling IS the effective timeout on this board. The clock deadline
 * below 1 s never bounds the loop on its own when a poll iteration costs ~200 yields plus
 * two MMIO reads: at 1,000,000 iterations the command is abandoned after roughly 70 ms,
 * twenty times sooner than the deadline that is configured. Make it tunable so a round can
 * tell 'the device answers late' apart from 'the controller is wedged'. */
static volatile uint32_t gUfsPollCeiling = 1000000U;

template <class Port>
static UfsTransferEnd pollUfsTransfer(Port &port, UfsTransferPoll &state)
{
  for (;;) {
    ++state.polls;
    uint32_t doorbell = port.doorbell();
    state.doorbellSeen |= doorbell;
    state.interruptSeen |= port.interrupt();
    if (state.interruptSeen & kUfsFatalStatus) return UfsTransferEnd::Fatal;
    if (!(doorbell & 1)) {
      state.ocs = port.completedOcs();
      // A zero doorbell alone does not prove that this command was accepted.
      // Also allow a completion so fast that software never observes DB=1.
      if (state.ocs != 15) return UfsTransferEnd::Completed;
    }
    if (port.expired ()) {
      // l458: measured, not assumed. `UFS GRACE RESCUED seq=189244 ocs=00 polls=0` proved the status word
      // was already good on the FIRST re-read after the deadline, and l453b showed the same shape (the OCS
      // word held 0x00=Success where the sentinel had been). So before declaring a timeout, take one more
      // sample of the same word the loop was already reading. This costs one cache invalidate and one load,
      // not the 2 s spin that made nabu-ufs-grace-ms a regression, and it applies to reads as well as writes.
      if (gUfsPoRecheck && !(port.doorbell () & 1U)) {
        state.ocs = port.completedOcs ();
        if (state.ocs != 15U) {
          __atomic_fetch_add (&gUfsFalseTimeouts, 1, __ATOMIC_RELAXED);
          return UfsTransferEnd::Completed;
        }
      }
      return UfsTransferEnd::Timeout;
    }
    // NABU-BRINGUP: iteration ceiling as well as the clock. On this board mach_absolute_time() does
    // not appear to advance this early, so expired() never fires and the loop spins forever -- the
    // caller's timeout never returns a verdict. Count alone terminates regardless of the clock.
    if (state.polls >= gUfsPollCeiling) return UfsTransferEnd::Timeout;
    port.delay();
  }
}
