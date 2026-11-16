// SPDX-License-Identifier: BSD-2-Clause-Patent
// Transfer completed GPU pixels to actual firmware scanout. No CPU rendering,
// fabricated refresh event, Metal discovery or original display publication.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <kern/task.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include "NativeGOPWire.h"
extern "C" {
volatile uint32_t native_gop_ready,native_gop_failures;
volatile uint64_t native_gop_transfers,native_gop_bytes,native_gop_physical,native_gop_virtual;
volatile uint64_t native_gop_gpu_offers,native_gop_gpu_completions,native_gop_gpu_failures,native_gop_gpu_abandoned;
/* waitSwap: [0] calls, [1] timeouts, [2] the requested timeout of the last call (ms), [3] time the last timed-out call really waited (ns), [4] the shortest
 * and [5] the longest such wait (ns), [6] total ns waited in timed-out calls. A timed wait that returns long before its deadline shows here. */
volatile uint64_t native_gop_wait_stats[7];
IOService *native_gop_presenter_create(void);
bool native_gop_gpu_offer(IOService *,void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height);
void native_gop_presenter_set_source(IOService *presenter,IOService *nub);
// Provided by the display driver: finishes (status 0) or fails the swap that was offered.
void native_legacy_gpu_complete(void *display,int status);
}
class NativeGOPClient;
class NativeGOPPresenter;
static NativeGOPPresenter *gHandoff;   // the presenter in GPU mode: the hand-off entry points below reach it without the registry
class NativeGOPPresenter : public IOService {
    OSDeclareDefaultStructors(NativeGOPPresenter)
    friend class NativeGOPClient;
    IOMemoryMap *mapping=nullptr;
    IOLock *gate=nullptr;
    IOService *owner=nullptr;
    NGPMode mode={};
    bool online=false;
    IOService *memorySource=nullptr;
    // GPU hand-off (sheng-fb-gpu=1): one offered frame at a time, from the display driver to the privileged client.
    bool gpuMode=false,gpuClient=false;
    struct {bool offered,delivered;uint64_t sequence;void *display;IOMemoryDescriptor *memory;IOMemoryMap *map;
            uint64_t stride;uint32_t width,height;} swap={};
    uint64_t swapSequence=0;
    IOReturn waitSwap(task_t,uint64_t timeoutMs,NGPSwap *);
    IOReturn completeSwap(uint64_t sequence,int status);
    void abandonSwap();
public:
    void setMemorySource(IOService *source) {memorySource=source;}
    static IOReturn handoffCall(uint32_t op,task_t task,const void *in,size_t inSize,void *out,size_t outSize);
    static IOMemoryDescriptor *handoffMemory();
    bool offer(void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height);
private:
    IOReturn transfer(task_t,const NGPPresent &,NGPTransfer *);
public:
    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;
    bool handleOpen(IOService *client,IOOptionBits,void *) override {if(owner)return false;owner=client;return true;}
    void handleClose(IOService *client,IOOptionBits) override {if(owner==client)owner=nullptr;}
    bool handleIsOpen(const IOService *client) const override {return client?owner==client:owner!=nullptr;}
    IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
};
class NativeGOPClient : public IOUserClient {
    OSDeclareDefaultStructors(NativeGOPClient)
    NativeGOPPresenter *controller=nullptr;
    task_t task=TASK_NULL;
    bool opened=false;
    static IOReturn modeAction(OSObject *,void *,IOExternalMethodArguments *);
    static IOReturn presentAction(OSObject *,void *,IOExternalMethodArguments *);
    static IOReturn waitAction(OSObject *,void *,IOExternalMethodArguments *);
    static IOReturn completeAction(OSObject *,void *,IOExternalMethodArguments *);
public:
    bool initWithTask(task_t,void *,UInt32,OSDictionary *) override;
    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;
    IOReturn clientClose() override {terminate();return kIOReturnSuccess;}
    IOReturn externalMethod(uint32_t,IOExternalMethodArguments *,IOExternalMethodDispatch *,OSObject *,void *) override;
    IOReturn clientMemoryForType(UInt32,IOOptionBits *,IOMemoryDescriptor **) override;
};
OSDefineMetaClassAndStructors(NativeGOPPresenter,IOService)
OSDefineMetaClassAndStructors(NativeGOPClient,IOUserClient)
IOService *native_gop_presenter_create(void) {return new NativeGOPPresenter;}
bool NativeGOPPresenter::start(IOService *provider) {
    if(!IOService::start(provider))return false;
    PE_Video video={};
    if(PE_current_console(&video) || !video.v_baseAddr || !video.v_width || !video.v_height ||
       video.v_width>16384 || video.v_height>16384 || video.v_depth!=32 || video.v_rotate ||
       video.v_rowBytes<video.v_width*4 || video.v_rowBytes>UINT64_MAX/video.v_height)return false;
    // The hand-off presenter is attached away from the display subsystem's node (the original display code enumerates that node's children),
    // so the framebuffer memory comes from the node it names instead of its provider.
    mapping=(memorySource?memorySource:provider)->mapDeviceMemoryWithIndex(0,kIOMapInhibitCache);
    if(!mapping || !mapping->getVirtualAddress() || mapping->getPhysicalAddress()!=video.v_baseAddr ||
       mapping->getLength()!=video.v_rowBytes*video.v_height)return false;
    gate=IOLockAlloc();if(!gate)return false;
    {unsigned v=0;PE_parse_boot_argn("sheng-fb-gpu",&v,sizeof v);gpuMode=(v==1);}
    mode={NGP_VERSION,NGP_BGRX8888,video.v_width,video.v_height,video.v_rowBytes,mapping->getLength()};
    native_gop_physical=mapping->getPhysicalAddress();native_gop_virtual=mapping->getVirtualAddress();
    online=true;native_gop_ready=1;if(gpuMode)gHandoff=this;registerService();return true;
}
void NativeGOPPresenter::stop(IOService *provider) {
    if(gHandoff==this)gHandoff=nullptr;
    if(gate)IOLockLock(gate);online=false;native_gop_ready=0;if(gate){IOLockWakeup(gate,&swap,false);IOLockUnlock(gate);}
    abandonSwap();
    IOService::stop(provider);
}
void NativeGOPPresenter::free() {
    OSSafeReleaseNULL(mapping);if(gate){IOLockFree(gate);gate=nullptr;}IOService::free();
}
IOReturn NativeGOPPresenter::transfer(task_t task,const NGPPresent &q,NGPTransfer *result) {
    if(gpuMode)return kIOReturnUnsupported;   // in GPU mode no CPU copy exists, not even on request
    if(!q.address || q.bytes!=mode.bytes || q.width!=mode.width || q.height!=mode.height ||
       q.stride!=mode.stride || q.format!=mode.format || q.address>UINT64_MAX-q.bytes)return kIOReturnBadArgument;
    IOMemoryDescriptor *input=IOMemoryDescriptor::withAddressRange(q.address,q.bytes,kIODirectionIn,task);
    if(!input)return kIOReturnNoMemory;
    IOReturn status=input->prepare(kIODirectionIn);
    if(status==kIOReturnSuccess) {
        IOLockLock(gate);
        if(!online)status=kIOReturnNotReady;
        else if(input->readBytes(0,reinterpret_cast<void *>(mapping->getVirtualAddress()),q.bytes)!=q.bytes)status=kIOReturnUnderrun;
        else {
            __asm__ volatile("dsb oshst" ::: "memory");
            native_gop_transfers=native_gop_transfers+1;result->sequence=native_gop_transfers;
            result->bytes=q.bytes;native_gop_bytes=native_gop_bytes+q.bytes;
        }
        IOLockUnlock(gate);
        IOReturn completion=input->complete(kIODirectionIn);if(status==kIOReturnSuccess)status=completion;
    }
    input->release();if(status!=kIOReturnSuccess)__atomic_fetch_add(&native_gop_failures,1,__ATOMIC_RELAXED);return status;
}
// The display driver offers a finished frame. Accepted only while the privileged GPU client is alive and waiting for work; otherwise the
// display driver keeps its own path (counted by it as a fallback).
bool NativeGOPPresenter::offer(void *display,IOMemoryDescriptor *memory,uint64_t stride,uint32_t width,uint32_t height) {
    if(!gpuMode || !memory || !display || !gate)return false;
    IOLockLock(gate);
    bool accepted=online && gpuClient && !swap.offered;
    if(accepted) {
        memory->retain();
        swap={true,false,++swapSequence,display,memory,nullptr,stride,width,height};
        native_gop_gpu_offers=native_gop_gpu_offers+1;
        IOLockWakeup(gate,&swap,false);
    }
    IOLockUnlock(gate);return accepted;
}
IOReturn NativeGOPPresenter::waitSwap(task_t task,uint64_t timeoutMs,NGPSwap *out) {
    if(!gpuMode)return kIOReturnUnsupported;
    if(timeoutMs>1000)timeoutMs=1000;
    uint64_t deadline;clock_interval_to_deadline((uint32_t)timeoutMs,kMillisecondScale,&deadline);
    const uint64_t began=mach_absolute_time();
    native_gop_wait_stats[0]=native_gop_wait_stats[0]+1;native_gop_wait_stats[2]=timeoutMs;
    IOLockLock(gate);
    gpuClient=true;
    while(online && (!swap.offered || swap.delivered)) {
        if(IOLockSleepDeadline(gate,&swap,deadline,THREAD_ABORTSAFE)!=THREAD_AWAKENED) {
            IOLockUnlock(gate);
            uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-began,&ns);
            native_gop_wait_stats[1]=native_gop_wait_stats[1]+1;native_gop_wait_stats[3]=ns;
            if(!native_gop_wait_stats[4] || ns<native_gop_wait_stats[4])native_gop_wait_stats[4]=ns;
            if(ns>native_gop_wait_stats[5])native_gop_wait_stats[5]=ns;
            native_gop_wait_stats[6]=native_gop_wait_stats[6]+ns;
            return kIOReturnTimeout;
        }
    }
    if(!online) {IOLockUnlock(gate);return kIOReturnNotReady;}
    swap.delivered=true;
    IOMemoryDescriptor *memory=swap.memory;memory->retain();
    const uint64_t sequence=swap.sequence,stride=swap.stride;const uint32_t width=swap.width,height=swap.height;
    IOLockUnlock(gate);
    // The frame's pages go into the client's task here, writable like any import (the GPU maps them for reading; the kernel pins and maintains them).
    IOMemoryMap *map=memory->createMappingInTask(task,0,kIOMapAnywhere);
    memory->release();
    IOLockLock(gate);
    if(!map || !map->getVirtualAddress() || !swap.offered || swap.sequence!=sequence) {
        IOLockUnlock(gate);OSSafeReleaseNULL(map);__atomic_fetch_add(&native_gop_gpu_failures,1,__ATOMIC_RELAXED);return kIOReturnNoMemory;
    }
    swap.map=map;
    out->sequence=sequence;out->address=map->getVirtualAddress();out->bytes=map->getLength();out->stride=stride;
    out->width=width;out->height=height;out->format=NGP_BGRX8888;out->reserved=0;
    IOLockUnlock(gate);return kIOReturnSuccess;
}
IOReturn NativeGOPPresenter::completeSwap(uint64_t sequence,int status) {
    IOLockLock(gate);
    if(!swap.offered || !swap.delivered || swap.sequence!=sequence) {IOLockUnlock(gate);return kIOReturnBadArgument;}
    auto done=swap;swap={};
    IOLockUnlock(gate);
    OSSafeReleaseNULL(done.map);done.memory->release();
    if(status)__atomic_fetch_add(&native_gop_gpu_failures,1,__ATOMIC_RELAXED);
    else native_gop_gpu_completions=native_gop_gpu_completions+1;
    native_legacy_gpu_complete(done.display,status);return kIOReturnSuccess;
}
// The client went away with a frame outstanding: the display driver is told it failed, so it can use its own path instead of waiting forever.
void NativeGOPPresenter::abandonSwap() {
    if(!gate)return;
    IOLockLock(gate);
    gpuClient=false;
    auto done=swap;swap={};
    IOLockUnlock(gate);
    if(!done.offered)return;
    OSSafeReleaseNULL(done.map);if(done.memory)done.memory->release();
    __atomic_fetch_add(&native_gop_gpu_abandoned,1,__ATOMIC_RELAXED);
    native_legacy_gpu_complete(done.display,kIOReturnAborted);
}
extern "C" void native_gop_presenter_set_source(IOService *presenter,IOService *nub) {
    if(NativeGOPPresenter *p=OSDynamicCast(NativeGOPPresenter,presenter))p->setMemorySource(nub);
}
extern "C" bool native_gop_gpu_offer(IOService *presenter,void *display,IOMemoryDescriptor *memory,uint64_t stride,uint32_t width,uint32_t height) {
    NativeGOPPresenter *p=OSDynamicCast(NativeGOPPresenter,presenter);
    return p && p->offer(display,memory,stride,width,height);
}
// Entry points for the GPU client that reaches the hand-off through an already admitted connection (the Adreno accelerator's user client, which
// the original sandbox profiles allow where they do not allow this service): op 0 mode, 2 wait for a frame, 3 complete it (the NGP_METHOD_*
// numbers). Same checks as the service's own methods; sizes are the wire records'.
IOReturn NativeGOPPresenter::handoffCall(uint32_t op,task_t task,const void *in,size_t inSize,void *out,size_t outSize) {
    NativeGOPPresenter *p=gHandoff;
    if(!p || !p->gpuMode || !p->online)return kIOReturnNotReady;
    switch(op) {
    case NGP_METHOD_MODE:
        if(inSize || !out || outSize!=sizeof(NGPMode))return kIOReturnBadArgument;
        memcpy(out,&p->mode,sizeof(NGPMode));return kIOReturnSuccess;
    case NGP_METHOD_WAIT_SWAP: {
        if(!in || inSize!=sizeof(NGPWait) || !out || outSize!=sizeof(NGPSwap))return kIOReturnBadArgument;
        NGPWait q={};NGPSwap swap={};memcpy(&q,in,sizeof(q));
        IOReturn status=p->waitSwap(task,q.timeout_ms,&swap);
        if(status==kIOReturnSuccess)memcpy(out,&swap,sizeof(swap));return status;
    }
    case NGP_METHOD_COMPLETE_SWAP: {
        if(!in || inSize!=sizeof(NGPComplete) || outSize)return kIOReturnBadArgument;
        NGPComplete q={};memcpy(&q,in,sizeof(q));
        return p->completeSwap(q.sequence,(int)q.status);
    }
    default:return kIOReturnUnsupported;
    }
}
// The scanout as a writable, retained descriptor (GPU mode only); the caller maps it into its task.
IOMemoryDescriptor *NativeGOPPresenter::handoffMemory() {
    NativeGOPPresenter *p=gHandoff;
    if(!p || !p->gpuMode || !p->online || !p->mapping)return nullptr;
    IOMemoryDescriptor *memory=p->mapping->getMemoryDescriptor();
    if(memory)memory->retain();
    return memory;
}
extern "C" IOReturn native_gop_handoff_call(uint32_t op,task_t task,const void *in,size_t inSize,void *out,size_t outSize) {
    return NativeGOPPresenter::handoffCall(op,task,in,inSize,out,outSize);
}
extern "C" IOMemoryDescriptor *native_gop_handoff_memory(void) {return NativeGOPPresenter::handoffMemory();}
IOReturn NativeGOPPresenter::newUserClient(task_t task,void *security,UInt32 type,OSDictionary *properties,IOUserClient **out) {
    if(!out || type)return kIOReturnBadArgument;
    *out=nullptr;NativeGOPClient *client=new NativeGOPClient;if(!client)return kIOReturnNoMemory;
    bool attached=false;
    if(!client->initWithTask(task,security,type,properties) || !(attached=client->attach(this)) || !client->start(this)) {
        if(attached)client->detach(this);client->release();return kIOReturnNotOpen;
    }
    *out=client;return kIOReturnSuccess;
}
bool NativeGOPClient::initWithTask(task_t ownerTask,void *security,UInt32 type,OSDictionary *properties) {
    if(!ownerTask || type || clientHasPrivilege(security,kIOClientPrivilegeAdministrator)!=kIOReturnSuccess ||
       !IOUserClient::initWithTask(ownerTask,security,type,properties))return false;
    task=ownerTask;task_reference(task);return true;
}
bool NativeGOPClient::start(IOService *provider) {
    controller=OSDynamicCast(NativeGOPPresenter,provider);
    if(!controller || !IOUserClient::start(provider))return false;
    opened=controller->open(this);return opened;
}
void NativeGOPClient::stop(IOService *provider) {
    if(opened){if(controller)controller->abandonSwap();controller->close(this);opened=false;}controller=nullptr;IOUserClient::stop(provider);
}
void NativeGOPClient::free() {if(task){task_deallocate(task);task=TASK_NULL;}IOUserClient::free();}
IOReturn NativeGOPClient::modeAction(OSObject *target,void *,IOExternalMethodArguments *a) {
    NativeGOPClient *client=OSDynamicCast(NativeGOPClient,target);
    if(!client || !client->opened || !client->controller)return kIOReturnNotOpen;
    if(a->structureOutputDescriptor || !a->structureOutput)return kIOReturnUnsupported;
    memcpy(a->structureOutput,&client->controller->mode,sizeof(NGPMode));return kIOReturnSuccess;
}
IOReturn NativeGOPClient::presentAction(OSObject *target,void *,IOExternalMethodArguments *a) {
    NativeGOPClient *client=OSDynamicCast(NativeGOPClient,target);
    if(!client || !client->opened || !client->controller)return kIOReturnNotOpen;
    if(a->structureInputDescriptor || a->structureOutputDescriptor || !a->structureInput || !a->structureOutput)return kIOReturnUnsupported;
    NGPPresent q={};NGPTransfer result={};memcpy(&q,a->structureInput,sizeof(q));
    IOReturn status=client->controller->transfer(client->task,q,&result);
    if(status==kIOReturnSuccess)memcpy(a->structureOutput,&result,sizeof(result));return status;
}
IOReturn NativeGOPClient::waitAction(OSObject *target,void *,IOExternalMethodArguments *a) {
    NativeGOPClient *client=OSDynamicCast(NativeGOPClient,target);
    if(!client || !client->opened || !client->controller)return kIOReturnNotOpen;
    if(a->structureInputDescriptor || a->structureOutputDescriptor || !a->structureInput || !a->structureOutput)return kIOReturnUnsupported;
    NGPWait q={};NGPSwap swap={};memcpy(&q,a->structureInput,sizeof(q));
    IOReturn status=client->controller->waitSwap(client->task,q.timeout_ms,&swap);
    if(status==kIOReturnSuccess)memcpy(a->structureOutput,&swap,sizeof(swap));return status;
}
IOReturn NativeGOPClient::completeAction(OSObject *target,void *,IOExternalMethodArguments *a) {
    NativeGOPClient *client=OSDynamicCast(NativeGOPClient,target);
    if(!client || !client->opened || !client->controller)return kIOReturnNotOpen;
    if(a->structureInputDescriptor || a->structureOutputDescriptor || !a->structureInput)return kIOReturnUnsupported;
    NGPComplete q={};memcpy(&q,a->structureInput,sizeof(q));
    return client->controller->completeSwap(q.sequence,(int)q.status);
}
IOReturn NativeGOPClient::externalMethod(uint32_t selector,IOExternalMethodArguments *a,IOExternalMethodDispatch *,OSObject *,void *) {
    static const IOExternalMethodDispatch methods[]={{modeAction,0,0,0,sizeof(NGPMode)},
                                                    {presentAction,0,sizeof(NGPPresent),0,sizeof(NGPTransfer)},
                                                    {waitAction,0,sizeof(NGPWait),0,sizeof(NGPSwap)},
                                                    {completeAction,0,sizeof(NGPComplete),0,0}};
    if(selector>=4)return kIOReturnUnsupported;
    return IOUserClient::externalMethod(selector,a,const_cast<IOExternalMethodDispatch *>(methods+selector),this,nullptr);
}
IOReturn NativeGOPClient::clientMemoryForType(UInt32 type,IOOptionBits *options,IOMemoryDescriptor **memory) {
    if(type>NGP_MEMORY_SCANOUT_RW || !options || !memory)return kIOReturnBadArgument;
    *memory=nullptr;if(!opened || !controller || !controller->online)return kIOReturnNotReady;
    // Type 1: the scanout itself, writable, for the GPU to write; only in GPU mode, where no CPU copy exists.
    if(type==NGP_MEMORY_SCANOUT_RW && !controller->gpuMode)return kIOReturnUnsupported;
    *memory=controller->mapping->getMemoryDescriptor();if(!*memory)return kIOReturnNoMemory;
    (*memory)->retain();if(type!=NGP_MEMORY_SCANOUT_RW)*options|=kIOMapReadOnly;return kIOReturnSuccess;
}
