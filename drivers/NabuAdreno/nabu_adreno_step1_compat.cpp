/* Step 3 compat: symbols whose only providers are out of this build.
 *
 * 1) NativeAdrenoAccelerator.cpp stays out: it derives from IOAccelerator, and this kernel has no
 *    IOAccelerator family at all (llvm-nm kernel.macho | grep -c IOAcceleratorES = 0).
 *
 * 2) The nabu ring-log backend is header-only and only emits its implementation when
 *    NABU_LOG_IMPLEMENTATION is defined (NabuLogCore.h:23), which nothing here does. Resources
 *    includes the header for its logging calls, so the symbols are referenced but never defined.
 *    Weak, so a real implementation would silently win instead of colliding.
 *
 * Nothing here changes control flow: the log calls become no-ops and the counters stay zero.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>

struct nb_log_snapshot;   /* only ever passed by pointer here */

/* l626: the accelerator publication is real now (NativeAdrenoAcceleratorPublish.cpp). The
 * old stub here returned kIOReturnUnsupported unconditionally, which is why every round
 * printed "Metal accelerator publication failed e00002c7" — that was the stub's value, not a
 * failure of the publication itself. l623/l625 established that IOAcceleratorES exists in the
 * prelinked IOGPUFamily component (classSize 136, superclass IOService) and is only missing
 * because nothing loads that kext; the publisher supplies the base definition instead. */

extern "C" __attribute__((weak)) void nabu_capture_append(const char *p, unsigned n)
{
    (void) p;
    (void) n;
}

/* MUST return non-zero. `native_adreno_checkpoint()` (the real one in NabuGpuResources.cpp:162)
 * does `captured = nabu_capture_snapshot(&snapshot) != 0;` and, if that is false twenty times, it
 * returns 1 -- which makes Service.cpp:1179 `fail(kIOReturnNotReady)` before mapWindows() is ever
 * called. Returning 0 here therefore blocked step 4 at the first gate (measured: the board showed
 * step=1 status=e00002d8 and never printed "mapping SM8150 board resources").
 *
 * Reporting success with end=0 is honest for this kernel: there is no NABU ring here, so there is
 * nothing to persist, and committed_end (also 0) satisfies the `>= snapshot.end` test on the first
 * iteration. The checkpoint's real meaning -- "the log is on the medium" -- has no object in a
 * build that does not carry that ring. */
extern "C" __attribute__((weak)) int nabu_capture_snapshot(struct nb_log_snapshot *s)
{
    if (s) {
        unsigned char *b = reinterpret_cast<unsigned char *>(s);
        for (unsigned i = 0; i < 28; ++i) b[i] = 0;   /* end=first=lost=0, length=0 */
    }
    return 1;
}

extern "C" {
__attribute__((weak)) volatile unsigned nabu_log_writer_state = 0;
/* NABU-A640-HANDOFF-L575: the two optional owner-probe callbacks. Declared in NabuLogCore.h:18-19,
 * defined there only under NABU_LOG_IMPLEMENTATION (serial_console.c, not this build). The driver
 * stores its sampler into the first one, so the object must exist; no caller exists here, which is
 * that header's "disabled builds supply no GPU dependency" case. */
__attribute__((weak)) void (*nabu_log_probe_poll)(void) = 0;
__attribute__((weak)) void (*nabu_log_probe_process_snapshot)(uint32_t *) = 0;
__attribute__((weak)) volatile unsigned nabu_log_writer_rounds = 0;
__attribute__((weak)) volatile unsigned nabu_log_writer_failures = 0;
__attribute__((weak)) uint64_t nabu_log_committed_end = 0;
}
