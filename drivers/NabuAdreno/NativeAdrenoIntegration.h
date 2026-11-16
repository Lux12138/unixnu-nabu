/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_INTEGRATION_H
#define NATIVE_ADRENO_INTEGRATION_H
#include <IOKit/IOMemoryDescriptor.h>
#include <mach/mach_types.h>

/* NativeAdrenoIntegration holds optional callbacks supplied by the display
 * presenter and persistent-log owner. The board installs this table before
 * publishing the GPU; the callback owners retain their backing objects.
 * Display callbacks synchronize their own state and may run concurrently.
 * Export fragment writes are serialized by the GPU service's export lock. */
struct NativeAdrenoIntegration {
    IOReturn (*displayCall)(uint32_t, task_t, const void *, const void *, size_t, void *, size_t);
    IOMemoryDescriptor *(*displayMemory)(task_t, const void *);
    int (*exportPut)(uint32_t, uint32_t, uint32_t, const void *, uint32_t);
    void (*displayClose)(const void *);
};

extern "C" void native_adreno_set_integration(const NativeAdrenoIntegration *);
#endif
