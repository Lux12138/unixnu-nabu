/* SPDX-License-Identifier: MIT */
#ifndef NABU_GPU_MEMORY_POLICY_H
#define NABU_GPU_MEMORY_POLICY_H
#include <stddef.h>

/* A640 has only qualified Normal-NC GPU mappings, not A642L cached coherence.
 * Owned user BOs must therefore bypass the WB pool and keep CPU/GPU aliases NC.
 * Explicit cache modes and imported memory are not changed by this policy. */
constexpr bool NabuGpuOwnedBoWriteCombined(bool user_owned)
{
#if defined(NABU_GPU_METAL) && NABU_GPU_METAL
    return user_owned;
#else
    (void)user_owned;
    return false;
#endif
}

constexpr size_t NabuGpuVmObjectCapacity(size_t maximum)
{
#if defined(NABU_GPU_METAL) && NABU_GPU_METAL
    return maximum;
#else
    (void)maximum;
    return 16;
#endif
}
#endif
