/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_STATUS_H
#define NATIVE_ADRENO_STATUS_H
#include <IOKit/IOTypes.h>
#include <stdint.h>
/* Monotonic fault and suspend counters for Turnip's device_check_status and
 * device_get_suspend_count. Turnip compares the fault count against the value
 * it saw last; a change means the context is lost. This object only counts
 * what the driver tells it: the IRQ path records a fatal decode, the reset or
 * power path records a suspend, and submission poisoning must be recorded by
 * the caller that observed it. It performs no detection, reset or recovery and
 * never clears a count, so a client cannot hide a fault by re-reading. */
struct NativeAdrenoStatusCounts {
    uint64_t faults;
    uint64_t suspends;
};
class NativeAdrenoStatus final {
    uint64_t faults_=0,suspends_=0;
public:
    void noteFault() {if(faults_!=UINT64_MAX)++faults_;}
    void noteSuspend() {if(suspends_!=UINT64_MAX)++suspends_;}
    uint64_t faults() const {return faults_;}
    uint64_t suspends() const {return suspends_;}
    NativeAdrenoStatusCounts counts() const {return {faults_,suspends_};}
};
#endif
