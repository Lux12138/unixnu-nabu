/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoClient.h"
#include <IOKit/IOReturn.h>
#include <string.h>
IOReturn NativeAdrenoClient::open(const NativeAdrenoTransport *transport)
{
    if(open_ || transport_.call)return kIOReturnBusy;
    if(!transport || !transport->call)return kIOReturnBadArgument;
    transport_=*transport;
    NativeAdrenoAbiParams reply{};
    const IOReturn status=transport_.call(transport_.cookie,NativeAdrenoMethodQuery,
        nullptr,0,&reply,sizeof(reply));
    if(status) {transport_={};return status;}
    if(reply.abi_version!=NativeAdrenoAbiVersion || reply.input_bits<32 ||
        reply.input_bits>48 || reply.output_bits<32 || reply.output_bits>48 ||
        !reply.max_bo_bytes || (reply.flags&~uint32_t(NativeAdrenoParamsHostImport))) {transport_={};return kIOReturnUnsupported;}
    params_=reply;open_=true;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoClient::close()
{
    if(!open_)return kIOReturnNotReady;
    open_=false;transport_={};params_={};
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoClient::query(NativeAdrenoAbiParams *params)
{
    if(!params)return kIOReturnBadArgument;*params={};
    if(!open_)return kIOReturnNotReady;
    *params=params_;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoClient::allocBo(uint64_t bytes,uint32_t align_log2,bool contiguous,
    uint64_t *handle)
{
    if(!handle)return kIOReturnBadArgument;*handle=0;
    if(!open_)return kIOReturnNotReady;
    if(!bytes || (bytes&4095) || bytes>params_.max_bo_bytes || align_log2<12 ||
        align_log2>24)return kIOReturnBadArgument;
    NativeAdrenoAbiBoAlloc request{bytes,align_log2,contiguous?1u:0u};
    NativeAdrenoAbiBoAllocReply reply{};
    const IOReturn status=transport_.call(transport_.cookie,NativeAdrenoMethodBoAlloc,
        &request,sizeof(request),&reply,sizeof(reply));
    if(!status)*handle=reply.handle;
    return status;
}
IOReturn NativeAdrenoClient::mapBo(uint64_t handle,uint64_t iova,uint32_t permissions,
    uint32_t writeback,uint32_t shareability,uint64_t *mapped)
{
    if(!mapped)return kIOReturnBadArgument;*mapped=0;
    if(!open_)return kIOReturnNotReady;
    if(!handle || iova&4095)return kIOReturnBadArgument;
    NativeAdrenoAbiBoMap request{handle,iova,permissions,writeback,shareability,0};
    NativeAdrenoAbiBoMapReply reply{};
    const IOReturn status=transport_.call(transport_.cookie,NativeAdrenoMethodBoMap,
        &request,sizeof(request),&reply,sizeof(reply));
    if(!status)*mapped=reply.iova;
    return status;
}
IOReturn NativeAdrenoClient::freeBo(uint64_t handle)
{
    if(!open_)return kIOReturnNotReady;
    if(!handle)return kIOReturnBadArgument;
    NativeAdrenoAbiBoFree request{handle};
    return transport_.call(transport_.cookie,NativeAdrenoMethodBoFree,&request,
        sizeof(request),nullptr,0);
}
IOReturn NativeAdrenoClient::submit(const NativeAdrenoAbiSubmitBuffer *buffers,size_t count,
    uint64_t wait_fence,uint64_t wait_timeout_ns,uint64_t *fence,uint64_t queue)
{
    if(!fence)return kIOReturnBadArgument;*fence=0;
    if(!open_)return kIOReturnNotReady;
    if(!buffers || !count || count>NativeAdrenoAbiMaxBuffers)return kIOReturnBadArgument;
    NativeAdrenoAbiSubmit request{};
    request.buffer_count=(uint32_t)count;
    request.queue=queue;
    request.wait_fence=wait_fence;
    request.wait_timeout_ns=wait_timeout_ns;
    for(size_t i=0;i<count;++i)request.buffers[i]=buffers[i];
    NativeAdrenoAbiSubmitReply reply{};
    const IOReturn status=transport_.call(transport_.cookie,NativeAdrenoMethodSubmit,
        &request,sizeof(request),&reply,sizeof(reply));
    if(!status)*fence=reply.fence;
    return status;
}
IOReturn NativeAdrenoClient::waitFence(uint64_t fence,uint64_t timeout_ns)
{
    if(!open_)return kIOReturnNotReady;
    NativeAdrenoAbiWaitFence request{fence,timeout_ns};
    return transport_.call(transport_.cookie,NativeAdrenoMethodWaitFence,&request,
        sizeof(request),nullptr,0);
}
IOReturn NativeAdrenoClient::deviceInfo(NativeAdrenoAbiDeviceInfo *info)
{
    if(!info)return kIOReturnBadArgument;*info={};
    if(!open_)return kIOReturnNotReady;
    NativeAdrenoAbiDeviceInfo reply{};
    const IOReturn status=transport_.call(transport_.cookie,NativeAdrenoMethodGetDeviceInfo,
        nullptr,0,&reply,sizeof(reply));
    if(!status)*info=reply;
    return status;
}
IOReturn NativeAdrenoClient::timestamp(uint64_t *ticks)
{
    if(!ticks)return kIOReturnBadArgument;*ticks=0;
    if(!open_)return kIOReturnNotReady;
    NativeAdrenoAbiTimestamp reply{};
    const IOReturn status=transport_.call(transport_.cookie,NativeAdrenoMethodGetTimestamp,
        nullptr,0,&reply,sizeof(reply));
    if(!status)*ticks=reply.ticks;
    return status;
}
IOReturn NativeAdrenoClient::status(uint64_t *faults,uint64_t *suspends)
{
    if(!faults || !suspends)return kIOReturnBadArgument;
    *faults=0;*suspends=0;
    if(!open_)return kIOReturnNotReady;
    NativeAdrenoAbiStatus reply{};
    const IOReturn status_value=transport_.call(transport_.cookie,NativeAdrenoMethodGetStatus,
        nullptr,0,&reply,sizeof(reply));
    if(!status_value) {*faults=reply.faults;*suspends=reply.suspends;}
    return status_value;
}
IOReturn NativeAdrenoClient::cpuMap(uint64_t handle,uint32_t *token,uint64_t *bytes)
{
    if(!token || !bytes)return kIOReturnBadArgument;*token=0;*bytes=0;
    if(!open_)return kIOReturnNotReady;
    if(!handle)return kIOReturnBadArgument;
    NativeAdrenoAbiBoCpuMap request{handle};
    NativeAdrenoAbiBoCpuMapReply reply{};
    const IOReturn status=transport_.call(transport_.cookie,NativeAdrenoMethodBoCpuMap,
        &request,sizeof(request),&reply,sizeof(reply));
    if(!status) {*token=reply.token;*bytes=reply.bytes;}
    return status;
}
IOReturn NativeAdrenoClient::cpuUnmap(uint64_t handle,uint32_t token)
{
    if(!open_)return kIOReturnNotReady;
    if(!handle || !token)return kIOReturnBadArgument;
    NativeAdrenoAbiBoCpuUnmap request{handle,token,0};
    return transport_.call(transport_.cookie,NativeAdrenoMethodBoCpuUnmap,&request,
        sizeof(request),nullptr,0);
}
IOReturn NativeAdrenoClient::createQueue(uint64_t *queue)
{
    if(!queue)return kIOReturnBadArgument;*queue=0;
    if(!open_)return kIOReturnNotReady;
    NativeAdrenoAbiQueue request{0};
    NativeAdrenoAbiQueueReply reply{};
    const IOReturn status=transport_.call(transport_.cookie,NativeAdrenoMethodCreateQueue,
        &request,sizeof(request),&reply,sizeof(reply));
    if(!status)*queue=reply.queue;
    return status;
}
IOReturn NativeAdrenoClient::closeQueue(uint64_t queue)
{
    if(!open_)return kIOReturnNotReady;
    if(!queue)return kIOReturnBadArgument;
    NativeAdrenoAbiQueue request{queue};
    return transport_.call(transport_.cookie,NativeAdrenoMethodCloseQueue,&request,
        sizeof(request),nullptr,0);
}
IOReturn NativeAdrenoClient::setMetadata(uint64_t handle,const void *bytes,uint32_t length)
{
    if(!open_)return kIOReturnNotReady;
    if(!handle || length>NativeAdrenoAbiMetadataBytes || (length && !bytes))
        return kIOReturnBadArgument;
    NativeAdrenoAbiMetadata request{};
    request.handle=handle;request.bytes=length;
    if(length)memcpy(request.text,bytes,length);
    return transport_.call(transport_.cookie,NativeAdrenoMethodSetMetadata,&request,
        sizeof(request),nullptr,0);
}
IOReturn NativeAdrenoClient::getMetadata(uint64_t handle,void *out,uint32_t capacity,
    uint32_t *length)
{
    if(!length)return kIOReturnBadArgument;*length=0;
    if(!open_)return kIOReturnNotReady;
    if(!handle || capacity>NativeAdrenoAbiMetadataBytes || (capacity && !out))
        return kIOReturnBadArgument;
    NativeAdrenoAbiMetadata request{};
    request.handle=handle;request.bytes=capacity;
    NativeAdrenoAbiMetadata reply{};
    const IOReturn status=transport_.call(transport_.cookie,NativeAdrenoMethodGetMetadata,
        &request,sizeof(request),&reply,sizeof(reply));
    if(status)return status;
    if(reply.bytes>capacity)return kIOReturnBadArgument;
    if(reply.bytes && out)memcpy(out,reply.text,reply.bytes);
    *length=reply.bytes;
    return kIOReturnSuccess;
}
