/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_SMMU_CONTEXT_H
#define NATIVE_ADRENO_SMMU_CONTEXT_H
#include "NativeAdrenoSmmuMemory.h"
#include "ArmSmmu4KStreams.h"

/* One single-region GPU SMMU context (lower/TTBR0, as the GMU's own 0..2 GiB
 * space, or upper/TTBR1), composed from the qualified kits:
 * prepare an inactive native4K table pool, program the context bank through the
 * real register sequence, then attach the caller's streams. Every step is
 * separate and can only run in order; a failure leaves the earlier state
 * retained and marks this object unusable for further programming. The bank
 * number, ASID and stream slots are required inputs and are never guessed.
 * No power, clock, reset, fault-interrupt or global SMMU setup is performed. */
class NativeAdrenoSmmuContext final {
    NativeAdrenoMemory *tables_=nullptr;
    ArmSmmu4KLiveArena arena_{};
    ArmSmmu4KLiveState state_{};
    ArmSmmu4KLiveVisibility visibility_{};
    ArmSmmu4KContextConfig config_{};
    ArmSmmu4KProgramResult program_{};
    ArmSmmu4KStreamState streams_{};
    ArmSmmu4KStreamResult routes_{};
    ArmSmmu4KIo io_{};
    unsigned bank_=0,poll_budget_=0;
    ArmSmmu4KStatus program_status_=ArmSmmu4KSuccess,attach_status_=ArmSmmu4KSuccess;
    bool prepared_=false,programmed_=false,attached_=false,poisoned_=false;
    uint32_t actlr_=0;
    ArmSmmu4KStatus actlr_status_=ArmSmmu4KSuccess;
public:
    NativeAdrenoSmmuContext()=default;
    ~NativeAdrenoSmmuContext()=default;
    NativeAdrenoSmmuContext(const NativeAdrenoSmmuContext &)=delete;
    NativeAdrenoSmmuContext &operator=(const NativeAdrenoSmmuContext &)=delete;
    /* A 2 GiB lower aperture, the GMU's, is 524288 leaves. */
    enum : size_t {MaxLeaves=size_t(1)<<19};
    /* iova/bytes describe the aperture in the chosen region; the pool must be contiguous. */
    /* output_bits: the context's output size, at most the SMMU's ID2.OAS (Linux uses OAS for stage 1). */
    IOReturn prepare(NativeAdrenoMemory *tables,unsigned upper,uint64_t iova,size_t bytes,
        size_t leaf_budget,unsigned bank,unsigned asid,unsigned poll_budget,unsigned output_bits=40,
        bool continueAfterFault=true);
    /* Reads actual IDs, programs and enables exactly this bank. */
    IOReturn program(const ArmSmmu4KIo *);
    /* Attach the caller's exact stream routes to the programmed bank. */
    IOReturn attach(const ArmSmmu4KStream *,size_t count);
    /* Read-only observation of the owned enabled context. */
    IOReturn observe() const;
    /* Only after attach; leaves streams faulted by the caller's own recovery. */
    IOReturn disable();
    bool prepared() const {return prepared_;}
    bool programmed() const {return programmed_;}
    bool attached() const {return attached_;}
    bool poisoned() const {return poisoned_;}
    const ArmSmmu4KContextConfig *configuration() const {return &config_;}
    const ArmSmmu4KLiveArena *arena() const {return &arena_;}
    /* Handles for a NativeAdrenoVm that will map buffers through this bank. */
    ArmSmmu4KLiveArena *arenaHandle() {return &arena_;}
    /* Scratch the kit's physical-alias checks sort in (ArmSmmu4KLiveArena::alias_scratch), owned by the caller for this context's
     * lifetime; without it those checks compare pairwise. Only after prepare. */
    void setAliasScratch(uint32_t *entries,size_t count) {arena_.alias_scratch=entries;arena_.alias_scratch_entries=entries?count:0;}
    ArmSmmu4KLiveState *stateHandle() {return &state_;}
    ArmSmmu4KContextConfig *configHandle() {return &config_;}
    ArmSmmu4KStreamState *streamsHandle() {return &streams_;}
    const ArmSmmu4KIo *ioHandle() const {return &io_;}
    const ArmSmmu4KProgramResult *programResult() const {return &program_; }
    const ArmSmmu4KStreamResult *streamResult() const {return &routes_;}
    /* The kit's own status of the last program/attach, which IOReturn folds into one error. */
    ArmSmmu4KStatus programStatus() const {return program_status_;}
    ArmSmmu4KStatus attachStatus() const {return attach_status_;}
    uint32_t actlr() const {return actlr_;}
    ArmSmmu4KStatus actlrStatus() const {return actlr_status_;}
};
#endif
