#include "NativeLegacyRequest.h"
#include "NabuSmmuMapper.h"
// SPDX-License-Identifier: BSD-2-Clause-Patent
// Reuse the unchanged original generic framebuffer implementation. Unknown
// backend leaves stop; this class is not yet a published system display.
#include "OriginalLegacyFramebufferABI.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
/* l635: maintained by NativeLegacyProbe (nabu_volume_ready); -1 not called, 0 failed, 1 ok */
extern volatile int g_nabu_legacy_start_result;
/* l688: the kernel's GPU addon stages, printed from logFrames so the line is
 * periodic and therefore survives in the tail of the log ring (a one-shot
 * printf during exec gets overwritten long before the window is read). */
extern volatile int native_gpu_addon_stage, native_gpu_addon_status;
extern volatile int native_gpu_addon2_stage, native_gpu_addon2_status;
extern volatile int native_gpu_addon3_stage, native_gpu_addon3_status;
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOMapper.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
extern "C" __attribute__((noreturn)) void native_legacy_backend_guard(unsigned,void *,void *);
extern "C" IOService *native_legacy_display_allocate(void);
extern "C" bool native_legacy_gop_mapping_observed(IOService *,IOMemoryMap *,uint32_t,uint32_t);
extern "C" void native_legacy_registration_observed(IOService *);
extern "C" void native_legacy_swap_stage(unsigned);
extern "C" void native_legacy_client_enter(task_t,uint32_t);
extern "C" void native_legacy_client_observed(IOReturn,IOUserClient *);
extern "C" void native_legacy_pm_observed(unsigned,IOReturn,unsigned long);
extern "C" IOMemoryDescriptor *native_legacy_surface_descriptor(OSObject *);
extern "C" uint64_t native_legacy_surface_width(OSObject *),native_legacy_surface_height(OSObject *);
extern "C" uint64_t native_legacy_surface_stride(OSObject *),native_legacy_surface_allocation(OSObject *);
extern "C" uint32_t native_legacy_surface_format(OSObject *),native_legacy_surface_planes(OSObject *);
extern "C" void native_legacy_fill_observed(uint32_t colour);
extern "C" void native_legacy_blank_kept(void);
extern "C" void native_legacy_frame_observed(unsigned kind,uint64_t count,uint64_t minx,uint64_t miny,uint64_t maxx,uint64_t maxy);
extern "C" void native_legacy_frame_colors_observed(const uint32_t *colors,const uint64_t *counts,uint64_t other);
extern "C" void native_legacy_swap_validation_observed(bool,unsigned,const void *,const void *);
// The surface of a rejected swap, as the validator read it: width, height, format, planes, stride, allocation, descriptor length.
extern "C" void native_legacy_surface_rejected(uint64_t,uint64_t,uint32_t,uint32_t,uint64_t,uint64_t,uint64_t);
extern "C" bool native_legacy_prepare_scanout(void);
extern "C" void native_legacy_transfer_observed(IOReturn,const void *,uint64_t);
extern "C" void native_legacy_copy_timing_observed(uint64_t ticks,uint64_t bytes);
extern "C" void native_legacy_retirement_observed(IOService *);
// GPU hand-off (boot-arg sheng-fb-gpu=1): the frame is offered to the privileged GPU client instead of being copied; the swap retires when the
// client reports the GPU copy done (native_legacy_gpu_complete). A refused offer or a failed copy uses the CPU copy below and is counted.
extern "C" bool native_legacy_gpu_offer(void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height);
// The compositor's swapped surfaces, for a display that fetches them itself: [0] distinct surfaces seen, [1] swaps checked; for the first four, at
// [2 + 3 * k]: allocation bytes, physical segments, largest segment (KiB). The pages are wired for the count and released.
/* l501: the two full-frame content scans at :561-569 read the whole source twice per frame. They were unconditional (a fixed 32-frame budget). Boot arg sheng-fb-scan=<N> sets that budget, and =0 turns the scans off, so one package can run both arms and tell a stall inside the scans apart from a stall in memory->map() (:551) or in noteSurface's segment walk (:63-69). Absent => 32 => exactly the behaviour measured so far. */
/* l510: nabu-fb-blank=1 changes what a swap with no IOSurface does to the panel. Until now that
 * branch filled the whole GOP with opaque black. The board proved the pixels reach the panel through
 * this copy (removing offline-legacy-transfer left the screen completely black), and l506b0's ring
 * counted two such frames out of eleven (nfill=2) -- enough to wipe a visible Setup page to black.
 * With the switch on the frame is retired exactly like the other rejected swaps: nothing is written,
 * the last good frame stays on screen, and the counter in native_legacy_fill[2] proves the branch ran.
 * Default off: the old behaviour, byte for byte. */
static bool native_legacy_blank_keep(void)
{
	static int cached=-1;
	if(cached<0){unsigned v=0;PE_parse_boot_argn("nabu-fb-blank",&v,sizeof(v));cached=v?1:0;}
	return cached!=0;
}

/* l504: sheng-fb-fill is a WRITE-PATH TEST, not a feature. A photograph of the panel proved the
 * console text this kernel kprintfs lands on the memory the panel scans, yet six full-screen copies
 * of a mostly-opaque-black surface left the firmware logo untouched. So the open question is no
 * longer "what is in the frame" but "does a store through our mapping become a pixel". One solid
 * colour, written every frame, answers that in a single round. Small values are colour NUMBERS so
 * the boot-arg parser cannot be fooled by its numeric base; 0x100000 and above is taken as a
 * literal ARGB word. Absent or 0 keeps the old behaviour byte-for-byte (opaque black, and only on
 * the blank-swap branch). */
static uint32_t native_legacy_fill_argb(void)
{
	unsigned v=0;
	if(!PE_parse_boot_argn("sheng-fb-fill",&v,sizeof(v)) || !v)return 0;
	switch(v) {
		case 1: return 0xffff0000U;
		case 2: return 0xff00ff00U;
		case 3: return 0xff0000ffU;
		case 4: return 0xffffffffU;
		case 5: return 0xffff00ffU;
		case 6: return 0xffffff00U;
		default: return (v>=0x00100000U) ? static_cast<uint32_t>(v) : 0xffff0000U;
	}
}
static uint32_t native_legacy_scan_limit(void)
{
    unsigned v=32;
    PE_parse_boot_argn("sheng-fb-scan",&v,sizeof(v));
    return (uint32_t)v;
}
extern "C" {volatile uint32_t native_legacy_surfaces[14];}
// Direct scanout (sheng-dpu-scanout=2): the display's two pipes fetch the compositor's surface through the display bank's window
// (NativeUfs native_ufs_dpu_map). [0] flips, [1] surfaces mapped, [2] map failures, [3] latches that took over 50 ms, [4] longest latch (us),
// [5] latch total (us, for the mean), [6] surfaces let go, [7] swaps that fell back to the copy, [8] flips back to the boot memory.
extern "C" {volatile uint32_t native_legacy_direct[9];}
extern "C" uint64_t native_ufs_dpu_map(unsigned slot,const uint64_t *pages,unsigned count) __attribute__((weak_import));
extern "C" volatile uint32_t native_ufs_dpu_state __attribute__((weak_import));
static void noteSurface(OSObject *surface,IOMemoryDescriptor *memory)
{
    static OSObject *seen[4];
    native_legacy_surfaces[1]=native_legacy_surfaces[1]+1;
    for(OSObject *known:seen)if(known==surface)return;
    const unsigned k=native_legacy_surfaces[0];
    native_legacy_surfaces[0]=k+1;
    if(k>=4)return;
    seen[k]=surface;
    if(memory->prepare()!=kIOReturnSuccess)return;
    uint32_t segments=0;uint64_t largest=0;
    for(IOByteCount offset=0,length=0;offset<memory->getLength() && segments<100000;offset+=length,++segments) {
        if(!memory->getPhysicalSegment(offset,&length,kIOMemoryMapperNone) || !length)break;
        if(length>largest)largest=length;
    }
    memory->complete();
    native_legacy_surfaces[2+3*k]=uint32_t(memory->getLength());native_legacy_surfaces[3+3*k]=segments;native_legacy_surfaces[4+3*k]=uint32_t(largest>>10);
}
extern "C" void native_legacy_gpu_fallback_observed(int status);
// Swaps retired when the GPU client took the frame (before its copy finished), and copies that then failed after such a retirement.
extern "C" {volatile uint64_t native_legacy_early_retires,native_legacy_early_failures;}
// Display refresh tick (boot-arg sheng-vsync=<Hz>, 0 or absent: off). The original family sends the compositor's vsync message (type 5)
// through native_legacy_vsync_notify, the unchanged original sender, but only a platform subclass calls it from the panel's vblank and
// that subclass is not part of this port: without it the render server never saw a vsync. The GOP scanout has no vblank interrupt, so a
// work-loop timer stands in for it while at least one client has vsync notifications enabled. Events: 0 armed, 1 sent, 2 idle (no client),
// 3 late (deadline passed, cadence restarted), 4 started, 5 refresh properties reported at that rate.
extern "C" void native_legacy_vsync_notify(void *display,uint64_t timestamp,uint64_t continuousTime,uint64_t interval);
// The unchanged original setDisplayRefreshProperties: publishes IOMFBDisplayRefresh from slot 301 (getRefreshAttributes below).
extern "C" bool native_legacy_set_refresh_properties(void *display);
extern "C" void native_legacy_vsync_observed(unsigned event,unsigned hz);
extern "C" uint64_t mach_continuous_time(void);
// 32-bit BGRA, plain ('BGRA') or in Apple's lossless-compressed spelling ('&BGA', one plane): the GPU that renders the surface here writes plain rows
// either way (no compression is implemented), so the bytes are the same.
static inline bool nativeBgra(uint32_t format,uint32_t planes) {
    return (format==0x42475241 && !planes) || (format==0x26424741 && planes<=1);
}
/* Identity IOMapper (boot-arg nabu-mapper=1; with the switch absent no object is ever created).
 *
 * The prelinked IOMobileFramebuffer family builds its default framebuffer surface - the blob in
 * __PRELINK_TEXT carries create_default_fb_surface, default_fb_surface, "%s: IODMACommand::
 * setMemoryDescriptor failed" and map_memory_dart(IOMapper *, IOMemoryDescriptor *, IODirection,
 * IODMACommand **, dva_t *) - and reaches the mapper through IOMapper::copyMapperForDeviceWithIndex(),
 * which returns NULL the moment the device has no "iommu-parent" property. l490b measured mapper=0
 * with the positive control iomfb=1: the registry holds no IOMapper instance at all, so the family
 * cannot map a surface, the surface is never created, and the compositor has no default target to
 * draw into. Nothing in either tree has ever implemented an IOMapper subclass.
 *
 * Two deliberate limits. fIsSystem stays false: IOMapper::start() only publishes gSystem when
 * fIsSystem is set, so the platform-wide IOMemoryDescriptor::dmaMap/IODMACommand path (UFS first among
 * them) is untouched - this mapper is reachable only through the property lookup above. And identity
 * is only honest when the requested range really is one contiguous physical segment; anything else is
 * refused rather than answered with a fabricated address. */
/* l493: did the framebuffer family ever ask this mapper for anything? Registering it (l491c)
 * did not clear "No Default FB Surface", and only a count can separate "never asked" from
 * "asked, and our identity answer was refused". [0] map calls, [1] refusals (range is not one
 * contiguous physical segment), [2] unmap calls, [3] insert calls. Written on whatever thread
 * the caller uses; read from the state banner (pull=v11). */
extern "C" {volatile uint32_t native_legacy_mapper_calls[4] = {0, 0, 0, 0};}
extern "C" {extern volatile uint32_t native_legacy_transfer[8];}   /* l660b */
extern "C" {extern volatile uint32_t native_legacy_validation[6];} /* l739 tmp: nval/nrej/rsn on frames line */
extern "C" {extern volatile uint64_t native_adreno_import_diag[10];} /* l745 tmp: import counts on frames line */
extern "C" {extern volatile uint32_t nabu_touch_stats[10];}       /* l661 */
class NabuIdentityMapper : public IOMapper {
    OSDeclareDefaultStructors(NabuIdentityMapper)
public:
    bool init(OSDictionary *parameters) override {
        if(!IOMapper::init(parameters))return false;
        fIsSystem=false;                     // never advertise as the system mapper
        return true;
    }
    bool initHardware(IOService *provider) override {return true;}
    uint64_t getPageSize() const override {return 4096;}
    uint64_t mapToPhysicalAddress(uint64_t address) override {return address;}
    IOReturn iovmMapMemory(IOMemoryDescriptor *memory,uint64_t offset,uint64_t length,uint32_t,
        const IODMAMapSpecification *,IODMACommand *,const IODMAMapPageList *pageList,
        uint64_t *mapAddress,uint64_t *mapLength) override {
        volatile uint32_t *c = native_legacy_mapper_calls;
        __atomic_fetch_add (&c[0], 1U, __ATOMIC_RELAXED);
        if(!memory||!mapAddress||!mapLength)return kIOReturnBadArgument;
        /* l641: getPhysicalSegment returns only ONE contiguous run. The framebuffer range is
         * usually several runs, so the old "remaining < length => refuse" rejected every call
         * (l640: map=64 refuse=64). Walk the runs instead and accept only when the whole
         * [offset, offset+length) window is physically contiguous. */
        /* l650 (nabu-mapper=2): return the FIRST physical run without any contiguity check.
         * The SMMU path (nabu-mapper=1) is blocked because every one of the 56 banks has a
         * non-zero CBAR (firmware pre-populated all of them, l649). */
        IOByteCount remaining=0;
        IOByteCount physical=memory->getPhysicalSegment(offset,&remaining,kIOMemoryMapperNone);
        if(!physical) {
            __atomic_fetch_add (&native_legacy_mapper_calls[1], 1U, __ATOMIC_RELAXED);
            return kIOReturnUnsupported;
        }
        *mapAddress=uint64_t(physical);
        *mapLength=length;
        return kIOReturnSuccess;
    }
    IOReturn iovmUnmapMemory(IOMemoryDescriptor *,IODMACommand *,uint64_t,uint64_t) override {
        __atomic_fetch_add (&native_legacy_mapper_calls[2], 1U, __ATOMIC_RELAXED);
        return kIOReturnSuccess;
    }
    IOReturn iovmInsert(uint32_t,uint64_t mapAddress,uint64_t,uint64_t physicalAddress,uint64_t) override {
        __atomic_fetch_add (&native_legacy_mapper_calls[3], 1U, __ATOMIC_RELAXED);
        return mapAddress==physicalAddress?kIOReturnSuccess:kIOReturnUnsupported;
    }
};
OSDefineMetaClassAndStructors(NabuIdentityMapper,IOMapper)

class NativeLegacyDisplay : public IOMobileFramebufferLegacy {
    OSDeclareDefaultStructors(NativeLegacyDisplay)
    IOMemoryMap *bootMapping=nullptr;
    IOMapper *identityMapper=nullptr;      // set only under nabu-mapper=1
    uint32_t scanBudget=native_legacy_scan_limit();
    void *shadow=nullptr;uint64_t shadowBytes=0;bool shadowValid=false;
    bool primary=false;
    bool logicalPM=false;
    bool transferred=false;
    // Every request the validator accepted and the family has not retired yet. A client may submit swap N+1 while swap N is still on the GPU
    // (int-131: swap 3 was validated during swap 2's GPU copy and one remembered pointer lost swap 2, which then never retired and left
    // every wait on it busy), so more than one can be pending.
    const OriginalLegacySwapRequest *validated[8]={};
    bool isValidated(const OriginalLegacySwapRequest *request) const {
        if(!request)return false;
        for(auto entry: validated)if(entry==request)return true;
        return false;
    }
    void removeValidated(const OriginalLegacySwapRequest *request) {for(auto &entry: validated)if(entry==request)entry=nullptr;}
    void addValidated(const OriginalLegacySwapRequest *request) {
        if(isValidated(request))return;
        for(auto &entry: validated)if(!entry){entry=request;return;}
        validated[0]=request;
    }
    const OriginalLegacySwapRequest *gpuRequest=nullptr;
    uint64_t gpuLength=0;
    bool gpuBypass=false;
    // The swap of the frame the GPU client is copying already retired (boot-arg sheng-early-retire, default 1).
    bool gpuRetired=false;
    // The refresh tick runs on the family's work loop, the loop of the command gate that registers and removes the vsync clients, so the
    // client queue at +5592 never changes while the sender walks it.
    IOTimerEventSource *vsyncTimer=nullptr;
    uint64_t vsyncInterval=0,vsyncNext=0;
    unsigned vsyncHz=0;
    bool vsyncArmed=false;
    // Direct scanout state, used on the work loop only. DPU 9.2 inside the MDP block (0xae01000): VIG 0 and 1 at 0x4000 and 0x6000 (source
    // address 0 at +0x14, stride 0 at +0x24; VIG 1 takes the right half through its own source x), CTL 0 at 0x15000 (flush at +0x18, VIG 0
    // and 1 are bits 0 and 1). i562: a flush of the two pipes latched within 5.4 ms with no start (the panel refreshes by itself).
    struct DirectSlot {OSObject *surface;IOMemoryDescriptor *memory;uint64_t iova;uint64_t used;};
    static constexpr unsigned kDirectSlots=4;
    DirectSlot directSlots[kDirectSlots]={};
    IOMemoryMap *mdpMap=nullptr;
    volatile uint32_t *mdp=nullptr;
    uint64_t *directPages=nullptr;
    IOTimerEventSource *latchTimer=nullptr;
    IOTimerEventSource *framesTimer=nullptr;   /* l660b: 60 s observation */
    bool framesFast_=true;                    /* l661: 15 s for the first 5 min */
    unsigned framesTicks_=0;
    const OriginalLegacySwapRequest *latchRequest=nullptr;
    uint64_t latchStart=0,directClock=0;
    int shownSlot=-1,pendingSlot=-1;uint32_t pendingStride=0;
    bool directOn=false,directBoot=true;
    static constexpr uint32_t kVig[2]={0x4000,0x6000},kCtlFlush=0x15018,kBootIova=0xb8000000U,kBootStride=0x2fa0;
    void startDirect() {
        unsigned mode=2;PE_parse_boot_argn("sheng-dpu-scanout",&mode,sizeof(mode));
        if(mode<2 || !&native_ufs_dpu_map || !&native_ufs_dpu_state || !(native_ufs_dpu_state>>31) || !primary)return;
        IODeviceMemory *memory=IODeviceMemory::withRange(0xae01000,0x8f000);
        mdpMap=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
        if(memory)memory->release();
        directPages=static_cast<uint64_t *>(IOMallocData(8192*sizeof(uint64_t)));
        IOWorkLoop *loop=getWorkLoop();
        latchTimer=loop?IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::latchFired):nullptr;
        if(!mdpMap || !directPages || !latchTimer || loop->addEventSource(latchTimer)!=kIOReturnSuccess) {
            OSSafeReleaseNULL(mdpMap);OSSafeReleaseNULL(latchTimer);
            if(directPages){IOFreeData(directPages,8192*sizeof(uint64_t));directPages=nullptr;}
            return;
        }
        mdp=reinterpret_cast<volatile uint32_t *>(mdpMap->getVirtualAddress());
        directOn=true;
    }
    void flip(uint64_t iova,uint32_t stride) {
        for(uint32_t vig:kVig){mdp[(vig+0x14)/4]=(uint32_t)(iova&0xFFFFFFFFu);mdp[(vig+0x24)/4]=stride&0xffff;}
        __asm__ volatile("dsb sy" ::: "memory");
        mdp[kCtlFlush/4]=0x3;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    // The slot that holds this surface, mapping it into a free one (never the one on screen or the one waiting to latch) the first time.
    int directSlot(OSObject *surface,IOMemoryDescriptor *memory) {
        ++directClock;
        int pick=-1;
        for(unsigned k=0;k<kDirectSlots;++k)if(directSlots[k].surface==surface && directSlots[k].memory==memory){directSlots[k].used=directClock;return int(k);}
        for(unsigned k=0;k<kDirectSlots;++k) {
            if(int(k)==shownSlot || int(k)==pendingSlot)continue;
            if(pick<0 || !directSlots[k].surface || (directSlots[pick].surface && directSlots[k].used<directSlots[pick].used))pick=int(k);
        }
        if(pick<0)return -1;
        DirectSlot &slot=directSlots[pick];
        if(slot.surface) {
            slot.memory->complete();OSSafeReleaseNULL(slot.memory);OSSafeReleaseNULL(slot.surface);
            native_legacy_direct[6]=native_legacy_direct[6]+1;
        }
        if(memory->getLength()>8192ull*4096 || memory->prepare()!=kIOReturnSuccess){native_legacy_direct[2]=native_legacy_direct[2]+1;return -1;}
        unsigned count=0;bool ok=true;
        for(IOByteCount offset=0;offset<memory->getLength() && ok;) {
            IOByteCount length=0;
            const addr64_t pa=memory->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
            if(!pa || (pa&0xfff) || !length){ok=false;break;}
            for(IOByteCount at=0;at<length && offset+at<memory->getLength();at+=4096)directPages[count++]=pa+at;
            offset+=length;
        }
        const uint64_t iova=ok?native_ufs_dpu_map(unsigned(pick),directPages,count):0;
        if(!iova){memory->complete();native_legacy_direct[2]=native_legacy_direct[2]+1;return -1;}
        surface->retain();memory->retain();
        slot={surface,memory,iova,directClock};
        native_legacy_direct[1]=native_legacy_direct[1]+1;
        return pick;
    }
    static void latchFired(OSObject *owner,IOTimerEventSource *) {static_cast<NativeLegacyDisplay *>(owner)->latchTick();}
    /* l660b: print the frame counters every 60 s, no new probes */
    static void framesFired(OSObject *owner,IOTimerEventSource *sender) {
        static_cast<NativeLegacyDisplay *>(owner)->logFrames(sender);
    }
    void logFrames(IOTimerEventSource *sender) {
        /* l661: nsurf = flips (surfaces[0]); the old "nodef" slot was surfaces[2],
         * which is the whole-screen BYTE COUNT (1600*2560*4 = 16,384,000), not a
         * count. Print it as sbytes=. fingers comes from nabu_touch_stats[2]. */
        IOLog("nabu-display: frames xfer=%u nswap=%u nsurf=%u sbytes=%u nok=%u ncpy=%u nret=%u fingers=%u"
              " | addon=%d/%d dmn=%d/%d wlp=%d/%d nval=%u nrej=%u rsn=%u don=%d tr=%d d0=%u d7=%u"
              " idg=%llu/%llu/%llu\n",
              (unsigned)native_legacy_transfer[0], (unsigned)native_legacy_surfaces[1],
              (unsigned)native_legacy_surfaces[0], (unsigned)native_legacy_surfaces[2],
              (unsigned)native_legacy_transfer[1], (unsigned)native_legacy_transfer[5],
              (unsigned)native_legacy_transfer[7], (unsigned)nabu_touch_stats[2],
              (int)native_gpu_addon_stage, (int)native_gpu_addon_status,
              (int)native_gpu_addon2_stage, (int)native_gpu_addon2_status,
              (int)native_gpu_addon3_stage, (int)native_gpu_addon3_status,
              (unsigned)native_legacy_validation[0], (unsigned)native_legacy_validation[2],
              (unsigned)native_legacy_validation[3], (int)directOn, (int)transferred,
              (unsigned)native_legacy_direct[0], (unsigned)native_legacy_direct[7],
              (unsigned long long)native_adreno_import_diag[0],
              (unsigned long long)native_adreno_import_diag[1],
              (unsigned long long)native_adreno_import_diag[3]);
        if (sender) sender->setTimeoutMS(framesFast_ ? 15000 : 60000);
        if (framesFast_ && ++framesTicks_ >= 20) framesFast_ = false;   /* 20 x 15 s = 5 min */
    }
    void latchTick() {
        if(!latchRequest)return;
        uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-latchStart,&ns);
        if((mdp[kCtlFlush/4]&0x3) && ns<50000000ull){latchTimer->setTimeoutUS(500);return;}
        if(ns>=50000000ull)native_legacy_direct[3]=native_legacy_direct[3]+1;
        const uint32_t us=uint32_t(ns/1000);
        if(us>native_legacy_direct[4])native_legacy_direct[4]=us;
        native_legacy_direct[5]=native_legacy_direct[5]+us;
        shownSlot=pendingSlot;pendingSlot=-1;
        const OriginalLegacySwapRequest *request=latchRequest;latchRequest=nullptr;
        finishTransfer(request,true);
    }
    // Back to the boot loader's memory (the blank frame and the copy are drawn there).
    void directToBoot() {
        if(!directOn || directBoot)return;
        flip(kBootIova,kBootStride);directBoot=true;shownSlot=-1;
        native_legacy_direct[8]=native_legacy_direct[8]+1;
    }
    bool vsyncWanted() const {
        const void *head=nullptr;memcpy(&head,reinterpret_cast<const unsigned char *>(this)+5592,sizeof(head));return head;
    }
    // Selector 72's registration of a vsync client ends in kickSwapQueue (slot 290), which arms the tick; the tick stops by itself once
    // the last client is removed.
    void armVsync() {
        if(!vsyncTimer || vsyncArmed || !vsyncWanted())return;
        vsyncArmed=true;vsyncNext=mach_absolute_time()+vsyncInterval;
        vsyncTimer->wakeAtTime(vsyncNext);native_legacy_vsync_observed(0,vsyncHz);
    }
    static void vsyncFired(OSObject *owner,IOTimerEventSource *) {static_cast<NativeLegacyDisplay *>(owner)->vsyncTick();}
    void vsyncTick() {
        vsyncArmed=false;
        if(!vsyncTimer)return;
        if(!vsyncWanted()){native_legacy_vsync_observed(2,vsyncHz);return;}
        const uint64_t now=mach_absolute_time();
        native_legacy_vsync_notify(this,now,mach_continuous_time(),vsyncInterval);
        native_legacy_vsync_observed(1,vsyncHz);
        vsyncNext+=vsyncInterval;
        if(vsyncNext<=now){vsyncNext=now+vsyncInterval;native_legacy_vsync_observed(3,vsyncHz);}
        vsyncArmed=true;vsyncTimer->wakeAtTime(vsyncNext);
    }
    void startVsync() {
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(!hz || hz>240 || !primary || !bootMapping || vsyncTimer)return;
        IOWorkLoop *loop=getWorkLoop();
        if(!loop)return;
        IOTimerEventSource *timer=IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::vsyncFired);
        if(!timer)return;
        if(loop->addEventSource(timer)!=kIOReturnSuccess){timer->release();return;}
        nanoseconds_to_absolutetime(1000000000ull/hz,&vsyncInterval);
        vsyncHz=hz;vsyncTimer=timer;native_legacy_vsync_observed(4,hz);
        // A client may already have registered: arm under the gate, like the registration does.
        loop->runAction([](OSObject *owner,void *,void *,void *,void *)->IOReturn {
            static_cast<NativeLegacyDisplay *>(owner)->armVsync();return kIOReturnSuccess;
        },this);
    }
    void stopVsync() {
        if(!vsyncTimer)return;
        vsyncTimer->cancelTimeout();
        if(IOWorkLoop *loop=getWorkLoop())loop->removeEventSource(vsyncTimer);
        OSSafeReleaseNULL(vsyncTimer);vsyncArmed=false;
    }
public:
    // Called by the GPU client's completion (any thread): the swap is finished on the framebuffer's work loop.
    void gpuComplete(int status) {
        struct Args {NativeLegacyDisplay *display;int status;} args={this,status};
        IOWorkLoop *loop=getWorkLoop();
        auto action=[](OSObject *,void *a0,void *,void *,void *)->IOReturn {
            Args *a=static_cast<Args *>(a0);a->display->finishGpuSwap(a->status);return kIOReturnSuccess;
        };
        if(loop)loop->runAction(action,this,&args);else finishGpuSwap(status);
    }
    void free() override {stopVsync();native_legacy_gop_mapping_observed(this,nullptr,0,0);if(logicalPM){PMstop();logicalPM=false;}primary=false;OSSafeReleaseNULL(bootMapping);IOMobileFramebufferLegacy::free();}
    void stop(IOService *provider) override {
        stopVsync();native_legacy_gop_mapping_observed(this,nullptr,0,0);
        transferred=false;for(auto &entry: validated)entry=nullptr;gpuRequest=nullptr;gpuRetired=false;
        directToBoot();latchRequest=nullptr;pendingSlot=-1;
        if(latchTimer){latchTimer->cancelTimeout();if(IOWorkLoop *loop=getWorkLoop())loop->removeEventSource(latchTimer);OSSafeReleaseNULL(latchTimer);}
        directOn=false;
        if(logicalPM){PMstop();logicalPM=false;}
        primary=false;IOMobileFramebufferLegacy::stop(provider);OSSafeReleaseNULL(bootMapping);
    }
    // The panel is already lit by the firmware's GOP framebuffer, which is the one thing the original code's power-state word (display+5840,
    // the "powered" half of the state pair next to +5836) is meant to say; the platform subclass that normally writes it is not part of this
    // port, so without it the user client's wait (selector 6) answers kIOReturnBusy forever and the compositor never presents. This is the
    // original state setter (virtual 354, the one the original code calls with this same request shape) with only the powered word named.
    void announcePoweredByFirmware() {
        struct StateRequest {uint32_t skipRequested,requested,skipPowered,powered;uint8_t changed;uint8_t pad[11];} request={1,0,0,1,1,{}};
        typedef int (*Setter)(void *,StateRequest *,bool);
        Setter setter=reinterpret_cast<Setter *>(*reinterpret_cast<void **>(this))[354];
        setter(this,&request,false);
    }
    bool start(IOService *provider) override {
        /* l639: the mapper must exist before IOMobileFramebufferLegacy::start builds the default
         surface. copyMapperForDeviceWithIndex() reads "iommu-parent" first and returns NULL
         immediately when it is absent, so registering the mapper after startDisplay() is too
         late (l638 measured map=0 with the mapper registered in startHardware()). */
        startIdentityMapper();
        IOLog("nabu-display: before-startDisplay t=%llu\n",(unsigned long long)mach_absolute_time());
        if(!startDisplay(provider))return false;
        if (!framesTimer) {
            IOWorkLoop *wl = getWorkLoop();
            framesTimer = IOTimerEventSource::timerEventSource(this, &NativeLegacyDisplay::framesFired);
            if (framesTimer && wl && wl->addEventSource(framesTimer) == kIOReturnSuccess) {
                framesTimer->setTimeoutMS(60000);
            }
        }
        IOLog("nabu-display: after-startDisplay t=%llu\n",(unsigned long long)mach_absolute_time());
        startVsync();startDirect();return true;
    }
    // Register the identity mapper and point this display at it. Both halves share one switch on
    // purpose: an "iommu-parent" property with no matching IOMapper would leave
    // copyMapperForDeviceWithIndex() waiting on a service that never arrives.
    void startIdentityMapper() {
        unsigned enabled=0;PE_parse_boot_argn("nabu-mapper",&enabled,sizeof(enabled));
        /* l650: 1 = SMMU mapper, 2 = identity-no-check (all 56 banks are pre-populated). */
        if((enabled!=1 && enabled!=2)||identityMapper)return;
        /* l650m2: read-only probe, mapper-agnostic. Maps the apps_smmu window once at boot
         * and reports whether firmware already routes SID 0x800 (the display identity map,
         * kBootIova=0xb8000000). Runs for both nabu-mapper=1 and =2. */
        {
            IODeviceMemory *smrMem=IODeviceMemory::withRange(0x15000000,0x100000);
            IOMemoryMap *smrMap=smrMem?smrMem->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
            if(smrMap) {
                volatile unsigned char *base=(volatile unsigned char *)smrMap->getVirtualAddress();
                uint32_t id1p=*(volatile uint32_t *)(base+0x24);
                unsigned shift=(id1p>>31)?16:12;
                size_t pages=(size_t)1<<(((id1p>>28)&7)+1), stride=(size_t)1<<shift;
                for(unsigned i=0;i<94;++i) {
                    uint32_t smr=*(volatile uint32_t *)(base+0x800+4*i);
                    if(!(smr&0x80000000u)) continue;
                    if((smr&0xffffu)!=0x800u) continue;
                    uint32_t s2cr=*(volatile uint32_t *)(base+0xc00+4*i);
                    unsigned b=(s2cr>>7)&0x1f;
                    uint32_t cbar=*(volatile uint32_t *)(base+(pages+b)*stride+0x4);
                    uint32_t sctlr=*(volatile uint32_t *)(base+(pages+b)*stride+0x0);
                    uint32_t cba2r=*(volatile uint32_t *)(base+(pages+b)*stride+0x8);
                    IOLog("nabu-smmu: sid800 slot=%u smr=%08x s2cr=%08x bank=%u cbar=%08x sctlr=%08x cba2r=%08x\n",
                          i,(unsigned)smr,(unsigned)s2cr,b,(unsigned)cbar,(unsigned)sctlr,(unsigned)cba2r);
                }
                smrMap->release();
            }
            if(smrMem) smrMem->release();
        }
        /* l650: nabu-mapper=2 selects the no-check identity mapper (the SMMU path needs a
         * free bank and all 56 are pre-populated). Both branches register the same way. */
        bool useIdentity=(enabled==2);
        IOMapper *mapper=useIdentity?(IOMapper*)new NabuIdentityMapper:(IOMapper*)new NabuSmmuMapper;
        if(!mapper->init(nullptr)||!mapper->attach(this)){
            IOLog("nabu-display: smmu-mapper attach-fail\n");mapper->release();return;
        }
        // IOMapper::start() copies the provider's "iommu-id" (else "AAPL,phandle") into the mapper's
        // gIOMapperIDKey property, and we attach the display as the provider. Both sides of the lookup
        // must therefore carry the SAME object, or copyMapperForDeviceWithIndex() waits for a match
        // that can never arrive.
        OSObject *source=copyProperty("iommu-id");
        if(!source)source=copyProperty("AAPL,phandle");
        OSData *id=nullptr;
        if(source) {
            id=OSDynamicCast(OSData,source);
            if(!id){
                IOLog("nabu-display: smmu-mapper id-not-data\n");
                source->release();mapper->detach(this);mapper->release();return;
            }
            id->retain();source->release();
        } else {
            static const uint32_t kIdentityMapperID=1;
            id=OSData::withBytes(&kIdentityMapperID,sizeof(kIdentityMapperID));
        }
        if(id) {
            mapper->setProperty(gIOMapperIDKey,id);
            setProperty("iommu-parent",id);
            /* l640: IODMACommand::Create_Impl's device is the PROVIDER (nub), not the display.
            * copyMapperForDeviceWithIndex(provider) reads "iommu-parent" on the provider. */
            IOService *prov = getProvider();
            if (prov) { prov->setProperty("iommu-parent",id); IOLog("nabu-display: iommu-parent set on provider\n"); }
            id->release();
            mapper->registerService();
            IOLog("nabu-display: mapper-ready t=%llu\n",(unsigned long long)mach_absolute_time());
            /* l643: registerService() only calls startMatching() (IOService.cpp:1031-1085,
             * last line is startMatching(options)) -- it never calls this->start(), so
             * prepareMapper() never ran and iovmMapMemory answered kIOReturnNotReady. */
            bool startOK=mapper->start(this);
            IOLog("nabu-display: smmu-mapper start=%d\n",(int)startOK);
            /* l644: keep the service registered even when start fails. The provider still
             * carries "iommu-parent", so copyMapperForDeviceWithIndex() reaches
             * waitForMatchingService(); detaching here would remove the only match and block
             * IOMFB forever (UINT64_MAX timeout). A registered-but-unstarted mapper answers
             * kIOReturnNotReady, which is the l642 behaviour we can diagnose. */
            if(startOK)identityMapper=mapper;    // kept for the life of the display
            IOLog("nabu-display: smmu-mapper attached\n");
        } else {
            IOLog("nabu-display: mapper no-id\n");
            mapper->detach(this);mapper->release();
        }
    }
    bool startDisplay(IOService *provider) {
        bool startOK=IOMobileFramebufferLegacy::start(provider);
        /* l635: if this is false every later line (starthw / mapper / pm) is skipped, so the
         * round would show only their absence. This is the same class of record as the
         * starthw reject lines, not a probe. */
        IOLog("nabu-display: startDisplay ok=%d\n",(int)startOK);
        if(!startOK)return false;
        if(primary && bootMapping)announcePoweredByFirmware();
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-pm",&enabled,sizeof(enabled));
        if(!enabled)return true;
        unsigned phase=0;PE_parse_boot_argn("offline-legacy-phase",&phase,sizeof(phase));
        if(enabled!=1 || phase!=4 || !provider || !bootMapping || !primary) {
            native_legacy_pm_observed(1,kIOReturnNotReady,0);IOMobileFramebufferLegacy::stop(provider);return false;
        }
        // Actual IOKit PM lifecycle for logical scanout scheduling only. GOP
        // provides no panel power switch: neither state advertises physical
        // power output, power control or a hardware power transition.
        static IOPMPowerState states[2]={};
        states[0].version=states[1].version=1;
        states[1].capabilityFlags=kIOPMDeviceUsable;
        PMinit();logicalPM=initialized;
        if(!logicalPM){native_legacy_pm_observed(1,kIOReturnNotReady,0);IOMobileFramebufferLegacy::stop(provider);return false;}
        provider->joinPMtree(this);
        IOReturn status=registerPowerDriver(this,states,2);native_legacy_pm_observed(1,status,0);
        if(status==kIOReturnSuccess){status=changePowerStateToPriv(1);native_legacy_pm_observed(2,status,1);}
        if(status!=kIOReturnSuccess){PMstop();logicalPM=false;IOMobileFramebufferLegacy::stop(provider);return false;}
        return true;
    }
    IOReturn setPowerState(unsigned long ordinal,IOService *device) override {
        if(!logicalPM)return IOMobileFramebufferLegacy::setPowerState(ordinal,device);
        if(ordinal>1 || !bootMapping){native_legacy_pm_observed(3,kIOReturnUnsupported,ordinal);return kIOReturnUnsupported;}
        native_legacy_pm_observed(3,kIOReturnSuccess,ordinal);
        IOReturn status=IOMobileFramebufferLegacy::setPowerState(ordinal,device);
        native_legacy_pm_observed(4,status,ordinal);return status;
    }
    // The explicit offline system-client experiment publishes only the real
    // GOP mode. Unknown backend calls retain their stop-with-caller guards.
    void registerService(IOOptionBits options=0) override {
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-register",&enabled,sizeof(enabled));
        if(enabled!=1)native_legacy_backend_guard(84,this,__builtin_return_address(0));
        // Nothing in the family publishes IOMFBDisplayRefresh (its platform subclass does), and without it the render server keeps its own
        // 1/60 s heartbeat (int-210: refresh=0). Publish it before any client can find the display, only when sheng-vsync asks for more.
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(primary && hz>60 && hz<=240)native_legacy_set_refresh_properties(this);
        IOService::registerService(options);native_legacy_registration_observed(this);
    }
    bool isMainDisplay() override {return primary;}
    // The render server paces its display timer by the minimum refresh interval in IOMFBDisplayRefresh (int-209: no vsync client ever
    // registered, frames came every 16.7 ms), and the generic slot 301 fixes it at 1/60 s. With sheng-vsync=<Hz> it reports the panel's
    // own rate instead; the maximum interval and the step keep the original values. Words in 32.32 seconds, then their mach times.
    // The original maximum interval is 1/24 s (int-240), which offers the render server a 24 to sheng-vsync Hz range. The panel has no
    // variable refresh, so sheng-vsync-fixed=1 reports the maximum interval equal to the minimum: one fixed rate.
    bool getRefreshAttributes(uint64_t *words,bool fixed) override {
        if(!IOMobileFramebufferLegacy::getRefreshAttributes(words,fixed) || !words)return false;
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(hz<=60 || hz>240)return true;
        unsigned locked=0;PE_parse_boot_argn("sheng-vsync-fixed",&locked,sizeof(locked));
        const uint64_t interval=(1ull<<32)/hz;
        uint64_t machTime=0;nanoseconds_to_absolutetime(interval*1000000000ull>>32,&machTime);
        words[0]=interval;words[1]=machTime;
        if(fixed || locked==1){words[2]=interval;words[3]=machTime;}
        native_legacy_vsync_observed(5,hz);
        return true;
    }
    // A swap the client submitted while the previous one was still on its way (transferred) is not kicked by the family again: without the
    // re-kick after each retirement it waited for the next submission (int-143: every offer came one frame late, a still screen showed the
    // previous frame until the user touched it again). A retirement inside a kick only asks the running kick to loop.
    bool kicking=false,rekick=false;
    void kickSwapQueue() override {
        armVsync();
        if(kicking){rekick=true;return;}
        kicking=true;
        do {
            rekick=false;
            kickOnce();
            gpuBypass=false;
        } while(rekick && !transferred);
        kicking=false;
    }
    void kickOnce() {
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-transfer",&enabled,sizeof(enabled));
        if(enabled!=1 || transferred || !logicalPM || !bootMapping || !primary)return;
        const OriginalLegacySwapRequest *request=pendingSwapRequest();
        if(!request || !isValidated(request))return;
        // This bounded first-frame transfer preserves the original queue's
        // resource ownership. Separately gated retirement follows the copy;
        // neither path signals a hardware fence, announces a vblank or
        // claims general producer synchronization.
        // The owned GPU instrument completes its work before submission.
        // The separate original software-renderer experiment uses completed
        // synchronous CPU surfaces, with no GPU or display fence fabricated.
        OSObject *surface=nullptr;
        memcpy(&surface,reinterpret_cast<const unsigned char *>(request)+2624,sizeof(surface));
        uint32_t forcedFill=native_legacy_fill_argb();
        if(!surface && !forcedFill && native_legacy_blank_keep()) {
            // A frame with no surface: leave the panel alone and retire the request like any other
            // rejected swap (the paths below answer kIOReturnNotReady without completing it either).
            native_legacy_blank_kept();
            native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
        }
        if(!surface || forcedFill) {
            // Validated blank swap (all layers disabled over black): solid-fill the actual GOP.
            PE_Video blank={};
            uint32_t bw=0,bh=0;getDisplayDimensions(&bw,&bh);
            uint64_t blankLength=(uint64_t)bw*bh*4;
            if(PE_current_console(&blank) || blank.v_baseAddr!=bootMapping->getPhysicalAddress() ||
               blank.v_width!=bw || blank.v_height!=bh || blank.v_depth!=32 || blank.v_rotate ||
               !native_legacy_surface_bounds(bw,bh,blank.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {
                native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
            }
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
            directToBoot();
            if(!native_legacy_fill_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                bootMapping->getLength(),blank.v_rowBytes,bw,bh,forcedFill?forcedFill:0xff000000U)) {
                native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
            }
            __asm__ volatile("dsb sy" ::: "memory");
            shadowValid=false;
            transferred=true;
            native_legacy_frame_observed(forcedFill?2U:1U,0,0,0,0,0);
            {const uint32_t colors[4]={forcedFill?forcedFill:0xff000000U,0,0,0};const uint64_t counts[4]={(uint64_t)bw*bh,0,0,0};native_legacy_frame_colors_observed(colors,counts,0);}
            native_legacy_fill_observed(forcedFill?forcedFill:0xff000000U);
            native_legacy_transfer_observed(kIOReturnSuccess,request,blankLength);
            finishTransfer(request);
            return;
        }
        if(!surface->metaCast("IOSurface")){native_legacy_transfer_observed(kIOReturnUnsupported,request,0);return;}
        IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
        PE_Video video={};
        uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
        uint64_t length=(uint64_t)width*height*4;
        uint64_t sourceStride=native_legacy_surface_stride(surface);
        uint64_t allocation=native_legacy_surface_allocation(surface);
        if(!memory || !memory->metaCast("IOMemoryDescriptor") || !native_legacy_surface_bounds(width,height,sourceStride,allocation,memory->getLength()) ||
           native_legacy_surface_width(surface)<width || native_legacy_surface_height(surface)<height ||
           !nativeBgra(native_legacy_surface_format(surface),native_legacy_surface_planes(surface)) ||
           PE_current_console(&video) || video.v_baseAddr!=bootMapping->getPhysicalAddress() ||
           video.v_width!=width || video.v_height!=height || video.v_depth!=32 || video.v_rotate ||
           !native_legacy_surface_bounds(width,height,video.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {
            native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
        }
        // The diagnostic renderer repaints the page over the scanout until it is told the system owns the display; the GPU path needs that as much
        // as the CPU copy (int-093: accepted swaps were GPU-copied under a page that kept drawing).
        if(!gpuBypass) {
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
        }
        noteSurface(surface,memory);
        if(directOn && !gpuBypass && !latchRequest) {
            const int slot=directSlot(surface,memory);
            if(slot>=0) {
                native_legacy_swap_stage(1);
                flip(directSlots[slot].iova,uint32_t(sourceStride));directBoot=false;
                native_legacy_direct[0]=native_legacy_direct[0]+1;
                shadowValid=false;pendingSlot=slot;latchRequest=request;transferred=true;pendingStride=uint32_t(sourceStride);
                latchStart=mach_absolute_time();latchTimer->setTimeoutUS(500);
                return;
            }
            native_legacy_direct[7]=native_legacy_direct[7]+1;
        }
        directToBoot();
        if(!gpuBypass && native_legacy_gpu_offer(this,memory,sourceStride,width,height)) {
            // The GPU writes the scanout memory behind the CPU's back: the differential copy's picture of it is stale from now on.
            native_legacy_swap_stage(1);
            shadowValid=false;gpuRequest=request;gpuLength=length;transferred=true;
            // The compositor renders the next frame only after this swap retires, so retiring at the end of the copy serialized render and
            // copy (int-243: about 4.4 ms render then about 3 ms copy per frame). Retire now: the presenter holds its own reference to this
            // frame's memory, the previous frame's copy has finished (transferred held this one back), and the next swap still waits for
            // the copy because transferred stays set until finishGpuSwap.
            return;
        }
        IOMemoryMap *source=memory->map(kIOMapAnywhere|kIOMapReadOnly);
        if(!source || !source->getVirtualAddress() || source->getLength()<allocation) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnNoMemory,request,0);return;
        }
        if(!native_legacy_copy_layout(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
            bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
            source->getLength(),sourceStride,width,height)) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
        }
        // The two content scans read every pixel again; they only describe the first frames (the probe keeps 32).
        if(scanBudget) {
            --scanBudget;
            uint64_t scan[5];
            if(native_legacy_frame_scan(reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride,width,height,scan))
                native_legacy_frame_observed(0,scan[0],scan[1],scan[2],scan[3],scan[4]);
            uint32_t colors[4];uint64_t counts[4],other=0;
            if(native_legacy_frame_colors(reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride,width,height,colors,counts,&other))
                native_legacy_frame_colors_observed(colors,counts,other);
        }
        if(!native_legacy_prepare_scanout()) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
        }
        uint64_t copyStart;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(copyStart));
        uint64_t stored=UINT64_MAX;
        {
            // Store only what changed since the previous frame (the scanout memory is slow device memory).
            uint64_t need=(uint64_t)width*height*4;
            // Differential copy is opt-in (sheng-fb-diff=1): on hardware it reads the whole source every frame and was not faster
            // than the plain copy (143-285 ms per swap either way), and the first boots with it were followed by an unresponsive
            // lock screen that disappeared with the plain copy (int-005).
            static int diffOff=-1;
            if(diffOff<0){unsigned v=1;PE_parse_boot_argn("sheng-fb-diff",&v,sizeof v);diffOff=(v!=1);}
            if(!diffOff && !shadow && need<=(64ull<<20)){shadow=IOMallocAligned((vm_size_t)need,16384);if(shadow){shadowBytes=need;shadowValid=false;}}
            if(!diffOff && shadow && shadowBytes>=need)
                stored=native_legacy_copy_diff(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                    bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
                    source->getLength(),sourceStride,width,height,shadow,shadowValid);
            if(stored!=UINT64_MAX)shadowValid=true;
        }
        if(stored==UINT64_MAX) {
            shadowValid=false;
            if(!native_legacy_copy_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
                source->getLength(),sourceStride,width,height)) {
                OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
            }
            stored=length;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        {uint64_t copyEnd;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(copyEnd));native_legacy_copy_timing_observed(copyEnd-copyStart,stored);}
        OSSafeReleaseNULL(source);transferred=true;
        native_legacy_transfer_observed(kIOReturnSuccess,request,length);
        finishTransfer(request);
    }
    // The GPU client reported its copy (status 0) or lost it. Success retires the swap like a finished copy; anything else uses the CPU copy.
    void finishGpuSwap(int status) {
        const OriginalLegacySwapRequest *request=gpuRequest;
        if(!request)return;
        gpuRequest=nullptr;
        if(gpuRetired) {
            // The swap retired when the client took it; its copy ending only releases the next frame. A failed copy loses this frame
            // (the next one takes the CPU copy).
            gpuRetired=false;
            if(status==0)native_legacy_transfer_observed(kIOReturnSuccess,request,gpuLength);
            else {native_legacy_early_failures=native_legacy_early_failures+1;native_legacy_gpu_fallback_observed(status);gpuBypass=true;}
            transferred=false;kickSwapQueue();return;
        }
        if(status==0) {
            native_legacy_transfer_observed(kIOReturnSuccess,request,gpuLength);
            finishTransfer(request,true);return;
        }
        native_legacy_gpu_fallback_observed(status);
        transferred=false;gpuBypass=true;kickSwapQueue();
    }
    // Hands the pending head to the unchanged family's completion. Leaves transferred alone: the callers decide when the next frame may go.
    bool retireSwap(const OriginalLegacySwapRequest *request,bool force) {
        unsigned retire=0,cpuPresent=0;
        PE_parse_boot_argn("offline-legacy-retire",&retire,sizeof(retire));
        PE_parse_boot_argn("offline-legacy-cpu-present",&cpuPresent,sizeof(cpuPresent));
        if(!(retire==1 || cpuPresent==1 || force))return false;
        if(!activateOnlyPendingSwap(request))return false;
        removeValidated(request);
        IOMobileFramebufferLegacy::completeCurrentSwap();
        native_legacy_retirement_observed(this);
        return true;
    }
    // Separately gated retirement after a real scanout store, shared by copied and blank frames.
    void finishTransfer(const OriginalLegacySwapRequest *request,bool force=false) {
        // Only after the real copy and store barrier: select the actual pending head and let the unchanged family release it and wake
        // waiters. This reports memory scanout completion, not a panel vblank or hardware fence.
        if(!retireSwap(request,force))return;
        // The unchanged family has retired this generation and released its current reference. Permit the next independently validated
        // single pending frame. Keep the latch set while completion can reenter this method, and whenever retirement remains disabled.
        transferred=false;
        kickSwapQueue();
    }
    IOReturn collectCompletionReports(uint32_t,void *,uint32_t *count,uint32_t) override {
        // The original completion helper passes null optional outputs and
        // ignores this IOReturn. GOP exposes no hardware completion telemetry.
        if(count)*count=0;
        return kIOReturnUnsupported;
    }
    bool validateSwap(const OriginalLegacySwapRequest *request) override {
        // Qualified original request fields are packed; use memcpy for each
        // access. Accept only one full-frame, untransformed, nonplanar BGRA
        // surface with real original backing memory. No generic capability
        // or success is supplied for other layer/format/geometry contracts.
        auto bytes=reinterpret_cast<const unsigned char *>(request);
        auto word=[&](unsigned offset){uint32_t value=0;memcpy(&value,bytes+offset,4);return value;};
        OSObject *surface=nullptr;unsigned reason=1;bool accepted=false;
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-validate",&enabled,sizeof(enabled));
        if(enabled==1 && logicalPM && primary && bootMapping && request) {
            reason=2;OSObject *surfaces[4]={};memcpy(surfaces,bytes+2624,sizeof(surfaces));surface=surfaces[0];
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            if(surface && surface->metaCast("IOSurface") && !surfaces[1] && !surfaces[2] && !surfaces[3] &&
               native_legacy_single_layer_state(word(792),word(796),word(800))) {
                reason=3;bool rectangles=true;
                const unsigned rectOffsets[]={17,81};
                for(unsigned offset: rectOffsets) {
                    uint32_t rect[4]={};memcpy(rect,bytes+offset,sizeof(rect));
                    rectangles=rectangles && rect[0]==0 && rect[1]==0 && rect[2]==width && rect[3]==height;
                }
                bool options=true;for(unsigned offset=804;offset<=812;offset+=4)options=options && word(offset)==0;
                if(width && height && rectangles && options && word(816)==width && word(820)==height) {
                    reason=4;
                    IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
                    // A surface larger than the display (rows or lines padded to the GPU's alignment) is accepted: only the visible rectangle,
                    // which the request's rectangles already name as the whole display, is copied.
                    if(native_legacy_surface_width(surface)>=width && native_legacy_surface_height(surface)>=height &&
                       nativeBgra(native_legacy_surface_format(surface),native_legacy_surface_planes(surface)) &&
                       memory && memory->metaCast("IOMemoryDescriptor") &&
                       native_legacy_surface_bounds(width,height,native_legacy_surface_stride(surface),
                           native_legacy_surface_allocation(surface),memory->getLength())) {
                        reason=0;accepted=true;
                    } else {
                        native_legacy_surface_rejected(native_legacy_surface_width(surface),native_legacy_surface_height(surface),
                            native_legacy_surface_format(surface),native_legacy_surface_planes(surface),native_legacy_surface_stride(surface),
                            native_legacy_surface_allocation(surface),memory?memory->getLength():0);
                    }
                }
            }
        }
        if(!accepted && reason==2 && !surface && enabled==1) {
            // No surface in any slot and every updated layer disabled over black: present a blank screen.
            OSObject *none[4]={};memcpy(none,bytes+2624,sizeof(none));
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            bool options=true;for(unsigned offset=804;offset<=812;offset+=4)options=options && word(offset)==0;
            if(!none[0] && !none[1] && !none[2] && !none[3] && width && height && options &&
               word(816)==width && word(820)==height && native_legacy_blank_state(word(792),word(796),word(800))) {
                reason=5;accepted=true;
            }
        }
        if(accepted)addValidated(request);else removeValidated(request);
        native_legacy_swap_validation_observed(accepted,reason,request,surface);return accepted;
    }
    // The original slot-188 caller passes a uint32_t output and consumes an
    // IOReturn. Its implementation assumes an AppleARMIODevice provider.
    // Query our actual generic provider instead; GOP supplies no dot pitch.
    IOReturn getDotPitch(uint32_t *pitch) override {
        if(!pitch)return kIOReturnBadArgument;
        *pitch=0;IOService *provider=getProvider();if(!provider)return kIOReturnNotReady;
        OSObject *property=provider->getProperty("dot-pitch");
        if(!property)return kIOReturnNotFound;
        OSData *data=OSDynamicCast(OSData,property);
        if(!data || data->getLength()!=sizeof(*pitch))return kIOReturnBadArgument;
        memcpy(pitch,data->getBytesNoCopy(),sizeof(*pitch));return kIOReturnSuccess;
    }
    // Selector 74 enumerates optional frame-info trace keys, not display
    // modes. This backend implements no trace-key subscription contract.
    IOReturn supportedFrameInfo(uint32_t,uint32_t *count,char *key,uint32_t *mask) override {
        if(!count || !key || !mask)return kIOReturnBadArgument;
        *count=0;*mask=0;bzero(key,64);return kIOReturnUnsupported;
    }
    IOReturn newUserClient(task_t task,void *security,UInt32 type,IOUserClient **client) override {
        native_legacy_client_enter(task,type);
        IOReturn status=IOMobileFramebufferLegacy::newUserClient(task,security,type,client);
        native_legacy_client_observed(status,status==kIOReturnSuccess && client?*client:nullptr);
        /* One line per real open: the client object already carries IOUserClientCreator, so the
         * caller identity is available here without polling anything. Cold path. */
        const char *creator="-";
        if(status==kIOReturnSuccess && client && *client && (*client)->getProperty("IOUserClientCreator")) {
            OSObject *property=(*client)->getProperty("IOUserClientCreator");
            OSString *string=OSDynamicCast(OSString,property);
            OSSymbol *symbol=OSDynamicCast(OSSymbol,property);
            if(string && string->getCStringNoCopy()) creator=string->getCStringNoCopy();
            else if(symbol && symbol->getCStringNoCopy()) creator=symbol->getCStringNoCopy();
        }
        const char *clientClass="-";
        if(status==kIOReturnSuccess && client && *client && (*client)->getMetaClass()) clientClass=(*client)->getMetaClass()->getClassName();
        IOLog("nabu-display: open type=%u status=%08x pid=%d creator=%s class=%s\n",(unsigned)type,(unsigned)status,
              task_pid(task),creator,clientClass);
        /* l635: the state at open time. The early startHardware/mapper lines land before the CDC
         * starts capturing (~93 s), so primary/identityMapper cannot be read from a round; the
         * first real open is always inside the captured window. Separates:
         *   primary=0            -> startHardware never ran (or v_display != 1) => mapper never made
         *   primary=1 mapper=0   -> startHardware ran, mapper still missing (timing/condition)
         *   primary=1 mapper=1   -> mapper exists; the failure is downstream (lookup/answer) */
        IOLog("nabu-display: state primary=%d mapper=%d pm=%d start=%d map=%d refuse=%d could=%u nodef=%u\n",
              (int)primary, identityMapper?1:0, (int)logicalPM, (int)g_nabu_legacy_start_result,
              (int)native_legacy_mapper_calls[0], (int)native_legacy_mapper_calls[1],
              (unsigned)native_legacy_transfer[2], (unsigned)native_legacy_surfaces[2]);
        return status;
    }
    bool startHardware() override {
        unsigned enabled=0;
        PE_parse_boot_argn("offline-legacy-gop",&enabled,sizeof(enabled));
        /* nabu: every rejection below used to be a bare `return false`, so a blank screen told us only
         * "the chain broke", never where. Each branch now prints the values it actually judged.
         * startHardware runs once per boot, so these are single-shot by construction - no counters,
         * no static flags, nothing on a hot path. Control flow is byte-for-byte the original. */
        if(enabled!=1){
            IOLog("nabu-display: starthw step=1 reject arg offline-legacy-gop=%u\n",enabled);
            return false;
        }
        PE_Video video={};IOService *provider=getProvider();
        if(!provider || PE_current_console(&video) || !video.v_baseAddr ||
           !video.v_width || !video.v_height || video.v_depth!=32 || video.v_rotate ||
           video.v_width>16384 || video.v_height>16384 || video.v_rowBytes<video.v_width*4 ||
           video.v_rowBytes>UINT64_MAX/video.v_height){
            /* PE_current_console's return code is not stored by the original; a failed query leaves
             * `video` zero-initialised, so base/width/height all being 0 is itself the signal. */
            IOLog("nabu-display: starthw step=2 reject console provider=%p base=%016llx w=%u h=%u depth=%u rotate=%u row=%u\n",
                  (void *)provider,(unsigned long long)video.v_baseAddr,(unsigned)video.v_width,(unsigned)video.v_height,
                  (unsigned)video.v_depth,(unsigned)video.v_rotate,(unsigned)video.v_rowBytes);
            return false;
        }
        IOMemoryMap *mapping=provider->mapDeviceMemoryWithIndex(0,kIOMapInhibitCache);
        if(!mapping){
            IOLog("nabu-display: starthw step=3 reject no mapping index=0 base=%016llx row=%u h=%u\n",
                  (unsigned long long)video.v_baseAddr,(unsigned)video.v_rowBytes,(unsigned)video.v_height);
            return false;
        }
        bool valid=mapping->getVirtualAddress() && mapping->getPhysicalAddress()==video.v_baseAddr &&
                   mapping->getLength()==video.v_rowBytes*video.v_height;
        if(!valid){
            IOLog("nabu-display: starthw step=4 reject map va=%016llx pa=%016llx len=%llu want pa=%016llx len=%llu\n",
                  (unsigned long long)(uintptr_t)mapping->getVirtualAddress(),
                  (unsigned long long)mapping->getPhysicalAddress(),(unsigned long long)mapping->getLength(),
                  (unsigned long long)video.v_baseAddr,
                  (unsigned long long)(video.v_rowBytes*video.v_height));
        }
        if(valid) {
            installBootDimensions((uint32_t)video.v_width,(uint32_t)video.v_height);
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            valid=width==video.v_width && height==video.v_height;
            if(!valid){
                IOLog("nabu-display: starthw step=5 reject readback w=%u h=%u gop w=%u h=%u\n",
                      width,height,(unsigned)video.v_width,(unsigned)video.v_height);
            }
            if(valid) {
                OSSafeReleaseNULL(bootMapping);bootMapping=mapping;mapping=nullptr;primary=video.v_display==1;
                valid=native_legacy_gop_mapping_observed(this,bootMapping,width,height);
                if(!valid){
                    IOLog("nabu-display: starthw step=6 reject guard boot pa=%016llx len=%llu w=%u h=%u primary=%u\n",
                          (unsigned long long)bootMapping->getPhysicalAddress(),
                          (unsigned long long)bootMapping->getLength(),width,height,(unsigned)primary);
                }
            }
        }
        if(valid){
            IOLog("nabu-display: starthw ok w=%u h=%u row=%u depth=%u primary=%u\n",
                  (unsigned)video.v_width,(unsigned)video.v_height,(unsigned)video.v_rowBytes,(unsigned)video.v_depth,(unsigned)primary);
        }
        /* l638: register the identity mapper HERE, before IOMobileFramebufferLegacy::start builds the
         * default surface. copyMapperForDeviceWithIndex() reads "iommu-parent" first and returns NULL
         * immediately when it is absent, so the mapper must exist before that lookup - registering it in
         * start() (after startDisplay) is too late. */
        startIdentityMapper();
        OSSafeReleaseNULL(mapping);return valid;
    }
#define LEAF(n) __attribute__((noreturn,noinline)) void opaque##n() override { native_legacy_backend_guard(n,this,__builtin_return_address(0)); }
    LEAF(211)
    LEAF(245)
    LEAF(246)
    LEAF(306)
    LEAF(307)
    LEAF(338)
    LEAF(345)
#undef LEAF
};
OSDefineMetaClassAndStructors(NativeLegacyDisplay,IOMobileFramebufferLegacy)
IOService *native_legacy_display_allocate(void) {return new NativeLegacyDisplay;}
extern "C" void native_legacy_gpu_complete(void *display,int status) {if(display)static_cast<NativeLegacyDisplay *>(display)->gpuComplete(status);}
