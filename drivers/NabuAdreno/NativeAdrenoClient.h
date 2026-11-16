/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_CLIENT_H
#define NATIVE_ADRENO_CLIENT_H
#include "NativeAdrenoABI.h"
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>

/* Userspace side of the wire ABI. The transport is injected so the same client
 * can drive an IOKit connection later and the kernel dispatch core during host
 * qualification. Records are built and checked here at their exact fixed size;
 * malformed counts are refused before any transport call. The client stores
 * only the last reported parameters and never caches object identity. */
struct NativeAdrenoTransport {
    void *cookie;
    IOReturn (*call)(void *cookie,uint32_t method,const void *input,size_t input_size,
        void *output,size_t output_size);
};
class NativeAdrenoClient final {
    NativeAdrenoTransport transport_{};
    NativeAdrenoAbiParams params_{};
    bool open_=false;
public:
    NativeAdrenoClient()=default;
    ~NativeAdrenoClient()=default;
    NativeAdrenoClient(const NativeAdrenoClient &)=delete;
    NativeAdrenoClient &operator=(const NativeAdrenoClient &)=delete;
    /* Queries the kernel and refuses a version or capability mismatch. */
    IOReturn open(const NativeAdrenoTransport *);
    IOReturn close();
    IOReturn query(NativeAdrenoAbiParams *);
    IOReturn allocBo(uint64_t bytes,uint32_t align_log2,bool contiguous,uint64_t *handle);
    IOReturn mapBo(uint64_t handle,uint64_t iova,uint32_t permissions,uint32_t writeback,
        uint32_t shareability,uint64_t *mapped);
    IOReturn freeBo(uint64_t handle);
    /* buffers are the caller's own array; the fixed record is assembled here. */
    IOReturn submit(const NativeAdrenoAbiSubmitBuffer *buffers,size_t count,
        uint64_t wait_fence,uint64_t wait_timeout_ns,uint64_t *fence,uint64_t queue=0);
    IOReturn waitFence(uint64_t fence,uint64_t timeout_ns);
    IOReturn timestamp(uint64_t *ticks);
    IOReturn status(uint64_t *faults,uint64_t *suspends);
    IOReturn deviceInfo(NativeAdrenoAbiDeviceInfo *);
    IOReturn createQueue(uint64_t *queue);
    IOReturn cpuMap(uint64_t handle,uint32_t *token,uint64_t *bytes);
    IOReturn cpuUnmap(uint64_t handle,uint32_t token);
    IOReturn closeQueue(uint64_t queue);
    IOReturn setMetadata(uint64_t handle,const void *,uint32_t bytes);
    IOReturn getMetadata(uint64_t handle,void *out,uint32_t capacity,uint32_t *length);
    bool isOpen() const {return open_;}
    const NativeAdrenoAbiParams &info() const {return params_;}
};
#endif
