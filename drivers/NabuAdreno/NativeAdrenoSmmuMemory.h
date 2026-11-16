/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_SMMU_MEMORY_H
#define NATIVE_ADRENO_SMMU_MEMORY_H
#include "NativeAdrenoMemory.h"
#include "ArmSmmu4KLive.h"
/* Bind the actual contiguous raw-PA backing allocation to an INACTIVE native
 * arena, publish its used table pages and take a device-lifetime reference.
 * Caller retains memory/arena/state/visibility under an exclusive resource
 * gate; all outputs and the backing pool are disjoint. State starts zeroed.
 * Never invoke on an active or foreign table pool. This does not program a
 * context, route streams, allocate MMIO/IRQs or establish power/security.
 * On success the memory reference remains held until real masters are stopped,
 * streams faulted, context disabled and native TLB synchronization completes.
 * On prepare/publication failure no context has been exposed; caller retains
 * allocation and poisoned state and handles cleanup. No automatic replay. */
IOReturn NativeAdrenoPrepareSmmuMemory(NativeAdrenoMemory *,unsigned input_bits,
    unsigned output_bits,uint64_t iova,uint64_t bytes,unsigned upper_region,
    size_t leaf_budget,ArmSmmu4KLiveArena *,ArmSmmu4KLiveState *,ArmSmmu4KLiveVisibility *);
/* The visibility callback orders stores to the retained WB table allocation.
 * GPU and GMU SMMU contexts walk these tables coherently with Outer Shareable
 * WB attributes. */
IOReturn NativeAdrenoBindSmmuVisibility(NativeAdrenoMemory *,ArmSmmu4KLiveVisibility *);
#endif
