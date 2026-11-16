/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoA6xxZap.h"
#include "NativeAdrenoKernelTime.h"
#include "NabuScmCall.h"
#include <IOKit/IOLib.h>
#include <string.h>

namespace {
uint32_t word(const uint8_t *p)
{
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}

void scmInvoke(void *,NabuScmRegisters *regs)
{
    register uint64_t x0 __asm__("x0")=regs->x[0];
    register uint64_t x1 __asm__("x1")=regs->x[1];
    register uint64_t x2 __asm__("x2")=regs->x[2];
    register uint64_t x3 __asm__("x3")=regs->x[3];
    register uint64_t x4 __asm__("x4")=regs->x[4];
    register uint64_t x5 __asm__("x5")=regs->x[5];
    register uint64_t x6 __asm__("x6")=regs->x[6];
    register uint64_t x7 __asm__("x7")=regs->x[7];
    __asm__ volatile("dsb sy\n\tsmc #0\n\tisb" :
        "+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),"+r"(x4),
        "+r"(x5),"+r"(x6),"+r"(x7) : :
        "x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","memory","cc");
    regs->x[0]=x0;regs->x[1]=x1;regs->x[6]=x6;
}

IOReturn scm(uint32_t initialFunction,uint32_t info,uint64_t arg0,uint64_t arg1,
    uint64_t arg2,int64_t *transport,int64_t *result)
{
    IOLog("NativeAdrenoZap: SCM begin function=%x info=%x arg=%llx/%llx/%llx limit_us=%llu\n",
        initialFunction,info,arg0,arg1,arg2,NabuScmTimeoutUs);
    const NabuScmIo io{nullptr,scmInvoke,NativeAdrenoKernelTime::now_us,NativeAdrenoKernelTime::wait_us};
    const NabuScmReply reply=nabuScmCall(io,initialFunction,info,arg0,arg1,arg2);
    *transport=reply.transport;*result=reply.result;
    IOLog("NativeAdrenoZap: SCM end function=%x stop=%s calls=%u interrupted=%u busy=%u elapsed_us=%llu transport=%lld result=%lld continuation=%llx\n",
        initialFunction,nabuScmStopName(reply.stop),reply.calls,reply.interrupted,reply.busy,
        reply.elapsed_us,reply.transport,reply.result,reply.continuation);
    switch(reply.stop) {
    case NabuScmStop::Complete:return kIOReturnSuccess;
    case NabuScmStop::Deadline:
    case NabuScmStop::CallBudget:return kIOReturnTimeout;
    case NabuScmStop::BusyBudget:return kIOReturnBusy;
    default:return kIOReturnIOError;
    }
}
}

IOReturn NativeAdrenoA6xxZap::load(const NativeAdrenoA6xxResources &resources,void *reservedCpu)
{
    const NativeAdrenoA6xxWindow &window=resources.zap;
    const uint8_t *image=resources.zapMetadata;
    const uint64_t native_adreno_zap_metadata_image_size=resources.zapMetadataBytes;
    const uint64_t native_adreno_zap_shader_image_size=resources.zapShaderBytes;
    const uint32_t pasId=resources.zapPasId;
    if(phase_ || !reservedCpu || !window.physical || window.bytes<0x1000 ||
        !image || !resources.zapShader || !pasId ||
        native_adreno_zap_metadata_image_size<148 ||
        native_adreno_zap_shader_image_size>window.bytes) {
        return kIOReturnBadArgument;
    }
    phase_=1;
    if(word(image)!=0x464c457f || image[4]!=1 || image[5]!=1 ||
        word(image+28)!=52 || image[44]!=3 || image[45]!=0) {
        return kIOReturnBadArgument;
    }
    const uint8_t *shader=image+52+2*32;
    const uint32_t fileBytes=word(shader+16),memoryBytes=word(shader+20);
    if(word(shader)!=1 || !(word(shader+24)&(1u<<27)) ||
        fileBytes!=native_adreno_zap_shader_image_size || memoryBytes<fileBytes ||
        memoryBytes>window.bytes) {
        return kIOReturnBadArgument;
    }
    IOReturn status=scm(0xc2000601,1,0x02000601,0,0,&transport_,&result_);
    if(status)return status;
    uint32_t pasBase=0x42000200;
    if(!transport_ && result_==1) {
        convention_=64;
    } else {
        status=scm(0x82000601,1,0x02000601,0,0,&transport_,&result_);
        if(status)return status;
        if(transport_ || result_!=1) {
            IOLog("NativeAdrenoZap: ARM SCM convention unavailable transport=%lld result=%lld\n",
                transport_,result_);
            return kIOReturnUnsupported;
        }
        convention_=32;
        pasBase=0x02000200;
        if(window.physical>UINT32_MAX || window.bytes>UINT32_MAX-window.physical) {
            return kIOReturnUnsupported;
        }
    }
    IOLog("NativeAdrenoZap: SCM convention=%u\n",convention_);

    const size_t metadataBytes=size_t((native_adreno_zap_metadata_image_size+4095)&~UINT64_C(4095));
    status=metadata_.allocate(metadataBytes,convention_==64?40:32,true,metadataBytes,false,false,true);
    if(status) {
        return status;
    }
    memcpy(metadata_.mapping(),image,size_t(native_adreno_zap_metadata_image_size));
    IOLog("NativeAdrenoZap: metadata publication begin physical=%llx bytes=%lx\n",metadata_.spans()[0].physical,metadataBytes);
    status=metadata_.publishForDevice(0,metadataBytes);
    IOLog("NativeAdrenoZap: metadata publication completed status=%08x\n",status);
    if(status) {
        return status;
    }

    phase_=2;
    status=scm(pasBase|1,0x82,pasId,metadata_.spans()[0].physical,0,&transport_,&result_);
    if(status)return status;
    if(transport_ || result_) {
        return kIOReturnNotPermitted;
    }
    phase_=3;
    status=scm(pasBase|2,3,pasId,window.physical,
        (uint64_t(memoryBytes)+4095)&~UINT64_C(4095),&transport_,&result_);
    if(status)return status;
    if(transport_ || result_) {
        return kIOReturnNotPermitted;
    }

    phase_=4;
    IOLog("NativeAdrenoZap: reserved shader copy begin physical=%llx bytes=%u\n",window.physical,fileBytes);
    memcpy(reservedCpu,resources.zapShader,fileBytes);
    if(memoryBytes>fileBytes) {
        memset(static_cast<uint8_t *>(reservedCpu)+fileBytes,0,memoryBytes-fileBytes);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    IOLog("NativeAdrenoZap: reserved shader copy completed\n");

    phase_=5;
    status=scm(pasBase|5,1,pasId,0,0,&transport_,&result_);
    if(status)return status;
    if(transport_ || result_) {
        return kIOReturnNotPermitted;
    }
    phase_=6;
    return kIOReturnSuccess;
}
