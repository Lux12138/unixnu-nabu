/* SPDX-License-Identifier: MIT */
/* IOKit surface for an already-running GPU context. The board code (kits/sheng) brings the GPU up, then hands this file
 * the live VM, submit object, ring, clock and optional timer; one IOService is published and each IOServiceOpen creates a
 * connection that mounts its own NativeAdrenoUserClient core. Up to NativeAdrenoMaxClients connections (one per Metal process) share
 * the VM, the submit object and the ring: each core has its own handles, its own queue range on the ring and its own window of the
 * one GPU address space (the render server the large one), and closing a connection waits for its own jobs only, then unmaps and
 * frees everything it created. Not yet done: per-process GPU page tables (the windows are enforced by the driver, not by the SMMU)
 * and a reset path. This is not the Metal or Turnip ABI; it carries NativeAdrenoABI.h unchanged.
 * Controlled BO CPU mappings share the allocation's persistent WC descriptor.
 * Legacy CPU tokens retain their physical-range views and cache-attribute repair.
 * AGXDeviceUserClient serves the Metal plugin's connections. The accelerator's
 * NativeSandboxGrantProcess property supplies the process authorization used by
 * the kernel's IOServiceOpen path; the client class name alone is insufficient. */
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBSD.h>
#include <string.h>
#include <kern/thread.h>
#include <kern/task.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include "NativeAdrenoUserClient.h"
#include "NativeAdrenoIntegration.h"
#include "NativeAdrenoA6xxService.h"
#include "NativeAdrenoDisplay.h"
#include "NativeAdrenoScheduler.h"
#include "NabuGpuProblemLog.h"

static NativeAdrenoIntegration integration = {};
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void native_adreno_publish_static_init(void)
    __asm__("__GLOBAL__sub_I_NativeAdrenoPublish.cpp");
extern "C" IOReturn native_adreno_accelerator_publish(IOService *,uint32_t);

extern "C" void native_adreno_set_integration(const NativeAdrenoIntegration *callbacks)
{
    integration = callbacks ? *callbacks : NativeAdrenoIntegration{};
}

extern "C" void proc_selfname(char *, int);
/* The plugin's telemetry table (rows of text the Metal bridge keeps), sent as markers: 0xC000|row starts a row, 0x8000|(a<<7)|b appends two characters. */
extern "C" {char native_adreno_metal_rows[48][96];volatile uint32_t native_adreno_metal_rows_seq;}

extern "C" {
/* What the processes that reached this driver did, for the boot diagnostic page. Plain counters and short strings. */
struct NativeAdrenoMetalDiag {
    uint32_t opens, busy, bad, closes, reports, property_reads, foreign_opens, foreign_type, name_reads, calls, last_selector, last_status, ext_entries, ext_selector, ext_status, ext_in, ext_out, tunnel_calls, probes, sel_bits[8], mb_maps, mb_calls, marks_total, marks[16], chk_macf, chk_res, chk_type, type_pass_lo, type_pass_hi, attach_status, inflight, inflight_since_calls, forced_unmaps;
    char opener[3][17];      /* process names of the latest connections, newest first */
    char reader[3][17];      /* process names that read a registry property of the accelerator, newest first */
    char name_reader[17];    /* latest process that read MetalPluginName specifically */
    char starter[3][17];     /* processes that sent plugin marker 1 (the plugin's initWithAcceleratorPort: started), newest first */
    char report[64];         /* the plugin's own result line (SetMetadata text), latest */
    char report2[64];        /* the plugin's Turnip-phase line (text starting "T " or "W "), latest */
    uint32_t grants;         /* open requests covered by the accelerator's process or entitlement grant after a sandbox refusal */
} native_adreno_metal_diag;
/* BO CPU views (clientMemoryForType for a BO token): [0] calls, [1] new descriptors, [2] served from the view cache, [3] refused (not a BO / out of
 * resources), [4] last refusal's IOReturn, [5] threads inside now. int-115 showed a backboardd thread in IOConnectMapMemory64 ~650 times a second. */
extern "C" void native_adreno_profile_lock_wait(uint32_t method,uint64_t us);
extern "C" void native_adreno_profile_set_client(const char *name);
extern "C" void native_adreno_profile_dump(void);
extern "C" void native_adreno_profile_set_continuous_clock(uint64_t (*)(void));
extern "C" {volatile uint32_t native_adreno_view_diag[6],native_adreno_view_kept;}   /* kept: BoCpuUnmap calls that left the view for another lease */
}

/* The open type the Adreno plugin passes to IOServiceOpen ('AG'); it lets the driver tell the plugin from the original IOGPU library
 * (type 1 plus an API value) and from any other client of the accelerator. */
enum : UInt32 {NativeAdrenoPluginOpenType=0x4147,NativeAdrenoTunnelMagic=0x4e544e4c /* 'NTNL' */};
/* The display hand-off (boot-arg sheng-fb-gpu=1, drivers/GopFramebuffer) reaches the plugin through this already admitted connection: the
 * original sandbox profiles admit this class but not the presenter's own service. Methods 0x7000 + the presenter's NGP_METHOD numbers (mode,
 * wait for a frame, complete it) are forwarded unchanged, without the GPU lock (a wait sleeps); memory type NativeAdrenoScanoutType is the
 * writable scanout. Nothing here touches the GPU. */
enum : UInt32 {NativeAdrenoHandoffBase=0x7000,NativeAdrenoHandoffLast=0x7003,NativeAdrenoScanoutType=0x5343,
               NativeAdrenoHandoffOpenType=0x4148};
/* Open type 0x4148 is a connection that serves only the hand-off: it does not mount a GPU core, so it does not need (or take) a
 * GPU connection slot (the Vulkan driver holds one for as long as the Metal device lives). Same class, so the same sandbox admission. */
/* Export (int-147): method NativeAdrenoExportMethod hands one in-order piece of a blob to the board's UFS log (kits/sheng/BootLog.c), which writes
 * the newest complete blob to its export area. The render server sends its pipeline caches this way, so a later image can be built with them.
 * Input: {generation, total, offset, bytes} then the bytes. No GPU state is touched; any connection of the render server may send it. */
enum : UInt32 {NativeAdrenoExportMethod=0x7100,NativeAdrenoExportMaxPiece=3968};
extern "C" {volatile uint32_t native_adreno_export_stats[4];}   /* [0] pieces taken, [1] refused, [2] last generation, [3] its total bytes */

/* Mailbox: a last-resort command channel for a sandbox that admits no external-method selector at all. It is one shared page (memory
 * type NativeAdrenoMailboxType through IOConnectMapMemory64, which the selector filter does not cover) and a kernel poller that runs
 * whatever the live connection writes into it. Layout (little endian dwords): +0 magic, +4 request seq, +8 response seq, +12 method,
 * +16 status, +20 input bytes, +24 output bytes, input at +0x40 (max 0x700), output at +0x800 (max 0x700). */
enum : UInt32 {NativeAdrenoMailboxType=0xC0DE,NativeAdrenoMailboxMagic=0x4d41494c /* 'MAIL' */};
namespace {
IOBufferMemoryDescriptor *gMailbox=nullptr;
volatile uint32_t *gMailboxWords=nullptr;
volatile bool gPollerStarted=false;
}
class AGXDeviceUserClient;
/* The render server's connection serves the mailbox; it is retained while set (under g.lock), so the poller never runs on a freed object. */
static AGXDeviceUserClient *gMailboxOwner=nullptr;
void nativeAdrenoMailboxPoller(void *,wait_result_t);
static IOReturn runCore(AGXDeviceUserClient *,uint32_t method,const void *input,size_t input_size,void *output,size_t output_size);
static uint32_t activeCoreCalls[NativeAdrenoMethodGetResourceBudget+1]{};

/* The two sandbox gates IOServiceOpen applies after newUserClient (XNU IOUserClient.cpp is_io_service_open_extended): the MACF open check on
 * the client object and the sandbox's filter resolver. They are run here for the caller too, only to record which one refuses it. */
extern "C" int mac_iokit_check_open(void *cred,void *client,unsigned type);
extern "C" void *kauth_cred_get(void);
extern const struct io_filter_callbacks *gIOUCFilterCallbacks;

extern "C" volatile uint32_t native_adreno_trace_stage;
extern "C" {
/* One entry per IOServiceOpen of the accelerator (not markers): who, which type, what the two sandbox gates said. */
struct NativeAdrenoOpenRecord {char name[20]; uint32_t type, macf, res, attach, foreign;};
struct NativeAdrenoOpenRecord native_adreno_open_log[8];
volatile uint32_t native_adreno_open_log_count;
/* One row per process name that opened the plugin type (first eight names): opens granted, refused (sandbox, cap or no free slot),
 * closes, the slot of its live connection (0xff none), the highest GPU address its mappings reached (MiB) and its last refusal. */
struct NativeAdrenoClientRecord {char name[20]; uint32_t opens, refused, closes, slot, peak_mib, last_refusal;};
struct NativeAdrenoClientRecord native_adreno_client_log[8];
/* The last text line (the plugin's failure reason) each process other than the render server streamed: process name and a 64-byte line. */
struct NativeAdrenoClientText {char name[20]; char text[64];};
struct NativeAdrenoClientText native_adreno_client_text[4];
/* Why the latest mount of a connection failed: [0] 1 no free slot, 2 the core's open, 3 the window scope; [1] its IOReturn; [2] the slot; [3] failures so far. */
volatile uint32_t native_adreno_mount_fail[4];
/* Processes that read MetalPluginName (the Metal discovery), first twelve names, with counts. */
struct NativeAdrenoAskerRecord {char name[20]; uint32_t reads;};
struct NativeAdrenoAskerRecord native_adreno_metal_askers[12];
}
/* GPU address windows inside the board's VA range (GpuPowerProbe gDeviceInfo: 0x20000000..0x80000000 of the 2 GiB aperture). Slot 0 is the
 * render server's (it held the whole 1.5 GiB alone before int-155 and used at most a few hundred MiB); slots 1 and 2 are for other Metal
 * processes. A kernel connection (the board's self-test) maps where it likes and is never scoped. */
/* Slot 0 is the render server's 1 GiB (it reached 934 MiB in int-196); the rest of the aperture is shared by three more processes (PosterBoard for the
 * wallpaper, iconservicesagent for the icons, WidgetRenderer_Default for the widgets: int-196's screen pictures showed a black wallpaper and white widget
 * boxes, none of which had a native device), 170 MiB each (page aligned). int-198: iconservicesagent reached 128 MiB into its window and then lost its
 * Metal device; whether the window is the cause is what the [gpu windows] log row decides before these sizes change. */
enum : unsigned {NativeAdrenoMaxClients=4};
static NativeAdrenoAbiResourceBudget resourceBudgets[NativeAdrenoMaxClients]{};
static const struct {uint64_t start,bytes;} kClientWindows[NativeAdrenoMaxClients]={
    {0x20000000ull,0x40000000ull},{0x60000000ull,0x0AAA9000ull},{0x6AAA9000ull,0x0AAA9000ull},{0x75552000ull,0x0AAA9000ull}};

namespace {
void shiftName(char names[3][17]) {
    memcpy(names[2], names[1], 17);
    memcpy(names[1], names[0], 17);
    proc_selfname(names[0], 17);
}
struct Published {
    bool published=false;
    bool failed=false;
    NativeAdrenoStatus status;
    NativeAdrenoVm *vm=nullptr;
    NativeAdrenoSubmit *submit=nullptr;
    NativeAdrenoRing *ring=nullptr;
    NativeAdrenoRingClock clock{};
    const NativeAdrenoTimer *timer=nullptr;
    const NativeAdrenoAbiDeviceInfo *info=nullptr;
    IOLock *lock=nullptr;
    IOLock *export_lock=nullptr; /* Serializes optional persistent-log fragment writes. */
    thread_call_t completionCall=nullptr;
    uint32_t completionEvent=0;
    uint32_t interruptFailure=0;
    IOService *service=nullptr;
    bool slots[NativeAdrenoMaxClients]{};
    char slot_names[NativeAdrenoMaxClients][20]{}; /* Owners of occupied slots, including failed teardown. */
    bool kernel_client=false;
    uint32_t input_bits=0,output_bits=0;
    uint32_t opened=0,closed=0;
} g;

void completionWorker(thread_call_param_t,thread_call_param_t)
{
    IOLockLock(g.lock);
    if(__atomic_exchange_n(&g.interruptFailure,0,__ATOMIC_ACQ_REL)) {
        g.failed=true;
        g.status.noteFault();
        g.ring->recordFault();
        g.submit->scheduler()->fault(kIOReturnIOError,false);
    } else if(!g.failed) {
        const IOReturn status=g.submit->scheduler()->advance();
        if(status) {
            g.failed=true;
            g.status.noteFault();
            g.ring->recordFault();
            g.submit->scheduler()->fault(status,false);
        }
    }
    ++g.completionEvent;
    IOLockWakeup(g.lock,&g.completionEvent,false);
    for(;;) {
        NativeAdrenoSurfaceRead *read=g.submit->scheduler()->takeCallback();
        if(!read) {
            break;
        }
        IOLockUnlock(g.lock);
        NativeAdrenoScheduler::invokeCallback(read);
        IOLockLock(g.lock);
        g.submit->scheduler()->finishCallback(read);
        IOLockWakeup(g.lock,read,false);
    }
    IOLockUnlock(g.lock);
}

/* BoProfileRecords retains one Vulkan device's counters, bins and slow events.
 * The connection generation and device ID identify the reporting owner. Closed
 * records remain available to USB until their connection slot or storage is reused. */
struct BoProfileRecords {
    uint64_t generation=0;
    uint64_t device_id=0;
    unsigned connection_slot=0;
    bool active=false;
    char name[20]{};
    NativeAdrenoAbiBoProfile counters{};
    NativeAdrenoAbiBoProfile budget{};
    NativeAdrenoAbiBoProfile sizes[NativeAdrenoBoProfileSizeCount]{};
    NativeAdrenoAbiBoProfile usages[NativeAdrenoBoProfileUsageCount]{};
    NativeAdrenoAbiBoProfile slow[NativeAdrenoBoProfileSlowCount]{};
};

enum : unsigned {BoProfileSources=8};
BoProfileRecords boProfiles[BoProfileSources];
BoProfileRecords boProfileSnapshot;
NativeAdrenoMemoryDiagnostics kernelBoProfileSnapshot;
uint64_t boProfileClientGenerations[NativeAdrenoMaxClients]{};
uint64_t boProfileGeneration=0;
uint64_t kernelBoProfileSequence=0;
uint32_t boProfileDumpBusy=0;

void sanitizeProfileName(char *name,size_t length)
{
    if(!length) {
        return;
    }
    for(size_t index=0;index+1<length && name[index];++index) {
        const unsigned char character=static_cast<unsigned char>(name[index]);
        if(character<32 || character>126 || character=='"' || character=='\\') {
            name[index]='_';
        }
    }
    name[length-1]=0;
}

bool validBoProfile(const NativeAdrenoAbiBoProfile &record)
{
    if(record.version!=NativeAdrenoBoProfileVersion || record.reserved ||
        !record.sequence || !record.device_id) {
        return false;
    }
    switch(record.kind) {
        case NativeAdrenoBoProfileCounters:
        case NativeAdrenoBoProfileBudget:
            return record.index==0;
        case NativeAdrenoBoProfileSize:
            return record.index<NativeAdrenoBoProfileSizeCount;
        case NativeAdrenoBoProfileUsage:
            return record.index<NativeAdrenoBoProfileUsageCount;
        case NativeAdrenoBoProfileSlow:
            return record.index<NativeAdrenoBoProfileSlowCount &&
                record.data.slow.operation>=NativeAdrenoBoSlowAllocate &&
                record.data.slow.operation<=NativeAdrenoBoSlowRetire &&
                record.data.slow.cache_reason<=NativeAdrenoBoCacheNotRequested &&
                record.data.slow.elapsed_us>=8000;
        default:
            return false;
    }
}

void saveBoProfile(BoProfileRecords &records,NativeAdrenoAbiBoProfile &record)
{
    NativeAdrenoAbiBoProfile *destination=nullptr;
    switch(record.kind) {
        case NativeAdrenoBoProfileCounters:
            destination=&records.counters;
            break;
        case NativeAdrenoBoProfileBudget:
            destination=&records.budget;
            break;
        case NativeAdrenoBoProfileSize:
            destination=&records.sizes[record.index];
            record.data.bin.name[0]=0;
            break;
        case NativeAdrenoBoProfileUsage:
            destination=&records.usages[record.index];
            sanitizeProfileName(record.data.bin.name,sizeof(record.data.bin.name));
            break;
        case NativeAdrenoBoProfileSlow:
            destination=&records.slow[record.index];
            sanitizeProfileName(record.data.slow.name,sizeof(record.data.slow.name));
            break;
    }
    if(destination && record.sequence>destination->sequence) {
        *destination=record;
    }
}

void printBoProfile(const BoProfileRecords &records)
{
    IOLog("NativeAdrenoBO: source name=\"%.*s\" s=%u g=%llu d=%llu active=%u\n",
        int(sizeof(records.name)-1),records.name,records.connection_slot,records.generation,
        records.device_id,unsigned(records.active));
    const NativeAdrenoAbiBoProfile &sample=records.counters;
    if(sample.sequence) {
        const NativeAdrenoAbiBoCacheCounters &counts=sample.data.counters;
        IOLog("NativeAdrenoBO: counter_time s=%u g=%llu d=%llu q=%llu sampled_us=%llu\n",
            records.connection_slot,records.generation,records.device_id,sample.sequence,sample.sampled_us);
        IOLog("NativeAdrenoBO: counter_counts s=%u g=%llu d=%llu q=%llu requests=%llu eligible=%llu hits=%llu\n",
            records.connection_slot,records.generation,records.device_id,sample.sequence,
            counts.requests,counts.eligible,counts.hits);
        IOLog("NativeAdrenoBO: counter_imports s=%u g=%llu d=%llu q=%llu misses=%llu imports=%llu\n",
            records.connection_slot,records.generation,records.device_id,sample.sequence,
            counts.misses,counts.imports);
        IOLog("NativeAdrenoBO: counter_bytes s=%u g=%llu d=%llu q=%llu requested_bytes=%llu import_bytes=%llu\n",
            records.connection_slot,records.generation,records.device_id,sample.sequence,
            counts.requested_bytes,counts.import_bytes);
        IOLog("NativeAdrenoBO: miss_shape s=%u g=%llu d=%llu q=%llu empty=%llu size=%llu alignment=%llu\n",
            records.connection_slot,records.generation,records.device_id,sample.sequence,
            counts.miss_empty,counts.miss_size,counts.miss_alignment);
        IOLog("NativeAdrenoBO: miss_attributes s=%u g=%llu d=%llu q=%llu flags=%llu memory_property=%llu pending=%llu\n",
            records.connection_slot,records.generation,records.device_id,sample.sequence,
            counts.miss_flags,counts.miss_memory_property,counts.miss_pending);
        IOLog("NativeAdrenoBO: bypass s=%u g=%llu d=%llu q=%llu fixed=%llu flags=%llu oversize=%llu\n",
            records.connection_slot,records.generation,records.device_id,sample.sequence,
            counts.bypass_fixed,counts.bypass_flags,counts.bypass_oversize);
        IOLog("NativeAdrenoBO: cache_evict s=%u g=%llu d=%llu q=%llu puts=%llu evict_objects=%llu evict_bytes=%llu\n",
            records.connection_slot,records.generation,records.device_id,sample.sequence,
            counts.puts,counts.evict_objects,counts.evict_bytes);
        IOLog("NativeAdrenoBO: cache_evict_reason s=%u g=%llu d=%llu q=%llu iova=%llu allocation=%llu close=%llu\n",
            records.connection_slot,records.generation,records.device_id,sample.sequence,
            counts.evict_iova,counts.evict_allocation,counts.evict_close);
        IOLog("NativeAdrenoBO: cache_objects s=%u g=%llu d=%llu q=%llu cached=%llu peak=%llu pending=%llu\n",
            records.connection_slot,records.generation,records.device_id,sample.sequence,
            counts.cached_objects,counts.peak_objects,counts.pending_objects);
        IOLog("NativeAdrenoBO: cache_bytes s=%u g=%llu d=%llu q=%llu cached=%llu peak=%llu pending=%llu\n",
            records.connection_slot,records.generation,records.device_id,sample.sequence,
            counts.cached_bytes,counts.peak_bytes,counts.pending_bytes);
        IOLog("NativeAdrenoBO: retirement s=%u g=%llu d=%llu q=%llu compatible_pending=%llu pressure_waits=%llu reaped=%llu\n",
            records.connection_slot,records.generation,records.device_id,sample.sequence,
            counts.compatible_pending_requests,counts.pressure_waits,counts.reaped);
        IOLog("NativeAdrenoBO: reporting s=%u g=%llu d=%llu q=%llu slow_total=%llu slow_overwritten=%llu report_failures=%llu\n",
            records.connection_slot,records.generation,records.device_id,sample.sequence,
            counts.slow_total,counts.slow_overwritten,counts.report_failures);
    }
    const NativeAdrenoAbiBoProfile &budgetRecord=records.budget;
    if(budgetRecord.sequence) {
        const NativeAdrenoAbiBoCacheBudget &budget=budgetRecord.data.budget;
        IOLog("NativeAdrenoBO: budget_time s=%u g=%llu d=%llu q=%llu sampled_us=%llu\n",
            records.connection_slot,records.generation,records.device_id,budgetRecord.sequence,budgetRecord.sampled_us);
        IOLog("NativeAdrenoBO: budget_nodes s=%u g=%llu d=%llu q=%llu objects=%llu limit=%llu metadata_bytes=%llu\n",
            records.connection_slot,records.generation,records.device_id,budgetRecord.sequence,
            budget.node_objects,budget.node_limit,budget.metadata_bytes);
        IOLog("NativeAdrenoBO: budget_bytes s=%u g=%llu d=%llu q=%llu cache=%llu reclaim=%llu\n",
            records.connection_slot,records.generation,records.device_id,budgetRecord.sequence,
            budget.cache_budget_bytes,budget.reclaim_bytes);
        IOLog("NativeAdrenoBO: budget_reclaim s=%u g=%llu d=%llu q=%llu high_bytes=%llu low_bytes=%llu\n",
            records.connection_slot,records.generation,records.device_id,budgetRecord.sequence,
            budget.reclaim_high_bytes,budget.reclaim_low_bytes);
    }
    for(unsigned kind=NativeAdrenoBoProfileSize;kind<=NativeAdrenoBoProfileUsage;++kind) {
        const unsigned count=kind==NativeAdrenoBoProfileSize?NativeAdrenoBoProfileSizeCount:NativeAdrenoBoProfileUsageCount;
        const NativeAdrenoAbiBoProfile *bins=kind==NativeAdrenoBoProfileSize?records.sizes:records.usages;
        for(unsigned index=0;index<count;++index) {
            const NativeAdrenoAbiBoProfile &entry=bins[index];
            const NativeAdrenoAbiBoCacheBin &bin=entry.data.bin;
            if(!entry.sequence || (!bin.requests && !bin.eligible && !bin.hits && !bin.misses &&
                !bin.compatible_pending_requests)) {
                continue;
            }
            const char *kindName=kind==NativeAdrenoBoProfileSize?"size":"usage";
            IOLog("NativeAdrenoBO: bin_name name=\"%.*s\" s=%u g=%llu d=%llu q=%llu kind=%s index=%u sampled_us=%llu\n",
                int(sizeof(bin.name)-1),bin.name,records.connection_slot,records.generation,
                records.device_id,entry.sequence,kindName,index,entry.sampled_us);
            IOLog("NativeAdrenoBO: bin_counts s=%u g=%llu d=%llu q=%llu kind=%s index=%u requests=%llu eligible=%llu hits=%llu\n",
                records.connection_slot,records.generation,records.device_id,entry.sequence,kindName,index,
                bin.requests,bin.eligible,bin.hits);
            IOLog("NativeAdrenoBO: bin_misses s=%u g=%llu d=%llu q=%llu kind=%s index=%u misses=%llu compatible_pending=%llu\n",
                records.connection_slot,records.generation,records.device_id,entry.sequence,kindName,index,
                bin.misses,bin.compatible_pending_requests);
            IOLog("NativeAdrenoBO: bin_bytes s=%u g=%llu d=%llu q=%llu kind=%s index=%u upper_bytes=%llu requested_bytes=%llu\n",
                records.connection_slot,records.generation,records.device_id,entry.sequence,kindName,index,
                bin.upper_bytes,bin.requested_bytes);
        }
    }
    for(unsigned index=0;index<NativeAdrenoBoProfileSlowCount;++index) {
        const NativeAdrenoAbiBoProfile &entry=records.slow[index];
        if(!entry.sequence) {
            continue;
        }
        const NativeAdrenoAbiBoSlowEvent &event=entry.data.slow;
        IOLog("NativeAdrenoBO: slow_name name=\"%.*s\" s=%u g=%llu d=%llu q=%llu index=%u operation=%u\n",
            int(sizeof(event.name)-1),event.name,records.connection_slot,records.generation,
            records.device_id,entry.sequence,index,event.operation);
        IOLog("NativeAdrenoBO: slow s=%u g=%llu d=%llu q=%llu sampled_us=%llu elapsed_us=%llu bytes=%llu\n",
            records.connection_slot,records.generation,records.device_id,entry.sequence,
            entry.sampled_us,event.elapsed_us,event.bytes);
        IOLog("NativeAdrenoBO: slow_object s=%u g=%llu d=%llu q=%llu alignment=%llu handle=%llu iova=%llx\n",
            records.connection_slot,records.generation,records.device_id,entry.sequence,
            event.alignment,event.handle,event.iova);
        IOLog("NativeAdrenoBO: slow_flags s=%u g=%llu d=%llu q=%llu reason=%u alloc_flags=%x memory_property=%x status=%08x\n",
            records.connection_slot,records.generation,records.device_id,entry.sequence,
            event.cache_reason,event.alloc_flags,event.memory_property,event.status);
        IOLog("NativeAdrenoBO: slow_queue s=%u g=%llu d=%llu q=%llu connection_fence=%u cached_objects=%u pending_objects=%u\n",
            records.connection_slot,records.generation,records.device_id,entry.sequence,
            event.connection_fence,event.cached_objects,event.pending_objects);
        IOLog("NativeAdrenoBO: slow_bytes s=%u g=%llu d=%llu q=%llu cached_bytes=%llu pending_bytes=%llu\n",
            records.connection_slot,records.generation,records.device_id,entry.sequence,
            event.cached_bytes,event.pending_bytes);
    }
}

void printKernelBoProfile(uint64_t sequence,uint64_t sampled_us)
{
    const NativeAdrenoMemoryDiagnostics &records=kernelBoProfileSnapshot;
    IOLog("NativeAdrenoBO: kernel sequence=%llu sampled_us=%llu insert_attempts=%llu insertions=%llu\n",
        sequence,sampled_us,records.insert_attempts,records.insertions);
    IOLog("NativeAdrenoBO: kernel_objects sequence=%llu cached_objects=%llu peak_objects=%llu\n",
        sequence,records.cached_objects,records.peak_objects);
    IOLog("NativeAdrenoBO: kernel_bytes sequence=%llu cached_bytes=%llu peak_bytes=%llu\n",
        sequence,records.cached_bytes,records.peak_bytes);
    IOLog("NativeAdrenoBO: kernel_events sequence=%llu slow_total=%llu slow_count=%u\n",
        sequence,records.slow_total,records.slow_count);
    for(unsigned index=0;index<NativeAdrenoMemorySizeBucketCount;++index) {
        const NativeAdrenoMemoryBucketStats &bin=records.buckets[index];
        if(!bin.requests && !bin.lookups && !bin.bypassed) {
            continue;
        }
        IOLog("NativeAdrenoBO: kernel_bin_bytes sequence=%llu index=%u upper_bytes=%llu requested_bytes=%llu\n",
            sequence,index,bin.upper_bound_bytes,bin.bytes);
        IOLog("NativeAdrenoBO: kernel_bin_counts sequence=%llu index=%u requests=%llu lookups=%llu hits=%llu\n",
            sequence,index,bin.requests,bin.lookups,bin.hits);
        IOLog("NativeAdrenoBO: kernel_bin_misses sequence=%llu index=%u misses=%llu bypassed=%llu\n",
            sequence,index,bin.misses,bin.bypassed);
        IOLog("NativeAdrenoBO: kernel_bin_shape sequence=%llu index=%u miss_empty=%llu miss_size=%llu\n",
            sequence,index,bin.miss_reason[0],bin.miss_reason[1]);
        IOLog("NativeAdrenoBO: kernel_bin_attributes sequence=%llu index=%u miss_attributes=%llu miss_quarantined=%llu\n",
            sequence,index,bin.miss_reason[2],bin.miss_reason[3]);
    }
    static const char *rejectNames[]={"not_owned","not_prepared","not_wc","quarantined",
        "no_spans","zero_size","oversized","no_evictable_entry","no_slot","imported",
        "pooled","memory_pressure","busy"};
    for(unsigned index=0;index<NativeAdrenoBackingRejectReasonCount;++index) {
        if(records.insert_reject[index]) {
            IOLog("NativeAdrenoBO: kernel_reject sequence=%llu reason=%s count=%llu\n",
                sequence,rejectNames[index],records.insert_reject[index]);
        }
    }
    static const char *evictionNames[]={"object_limit","byte_limit","client_close",
        "memory_pressure","allocation_retry"};
    for(unsigned index=0;index<NativeAdrenoBackingEvictReasonCount;++index) {
        if(records.eviction_attempts[index] || records.evictions[index] || records.eviction_failures[index]) {
            IOLog("NativeAdrenoBO: kernel_evict_counts sequence=%llu reason=%s attempts=%llu failures=%llu\n",
                sequence,evictionNames[index],records.eviction_attempts[index],records.eviction_failures[index]);
            IOLog("NativeAdrenoBO: kernel_evict_size sequence=%llu reason=%s objects=%llu bytes=%llu\n",
                sequence,evictionNames[index],records.evictions[index],records.evicted_bytes[index]);
        }
    }
    static const char *operationNames[]={"allocation","owned_descriptor_release",
        "imported_descriptor_release","import"};
    static const char *sourceNames[]={"unknown","kernel_owned","user_owned","imported"};
    static const char *cacheNames[]={"not_requested","hit","miss"};
    for(unsigned index=0;index<records.slow_count && index<NativeAdrenoMemorySlowEventCount;++index) {
        const NativeAdrenoMemorySlowEvent &event=records.slow[index];
        const unsigned operation=static_cast<unsigned>(event.operation);
        const unsigned source=static_cast<unsigned>(event.source);
        const unsigned cache=static_cast<unsigned>(event.cache_result);
        char process[sizeof(event.process)];
        memcpy(process,event.process,sizeof(process));
        sanitizeProfileName(process,sizeof(process));
        IOLog("NativeAdrenoBO: kernel_slow process=\"%.*s\" sequence=%llu sampled_us=%llu operation=%s source=%s cache=%s\n",
            int(sizeof(process)-1),process,event.sequence,event.sampled_us,
            operation<4?operationNames[operation]:"unknown",source<4?sourceNames[source]:"unknown",
            cache<3?cacheNames[cache]:"unknown");
        IOLog("NativeAdrenoBO: kernel_slow_object sequence=%llu elapsed_us=%llu bytes=%llu spans=%llu output_bits=%u flags=%x\n",
            event.sequence,event.elapsed_us,event.bytes,event.spans,event.output_bits,event.flags);
        IOLog("NativeAdrenoBO: kernel_slow_cache sequence=%llu cached_objects=%u cached_bytes=%llu available_pages=%llu\n",
            event.sequence,event.cached_objects,event.cached_bytes,event.available_pages);
    }
}

void dumpBoProfiles()
{
    if(!g.published || !g.lock) {
        return;
    }
    if(__atomic_exchange_n(&boProfileDumpBusy,1,__ATOMIC_ACQUIRE)) {
        IOLog("NativeAdrenoBO: sample=dump_busy\n");
        return;
    }

    if(IOLockTryLock(g.lock)) {
        NativeAdrenoMemory::copyDiagnostics(&kernelBoProfileSnapshot);
        const uint64_t sampled_us=g.clock.now_us?g.clock.now_us(g.clock.cookie):0;
        const uint64_t sequence=++kernelBoProfileSequence;
        IOLockUnlock(g.lock);
        printKernelBoProfile(sequence,sampled_us);
    } else {
        IOLog("NativeAdrenoBO: kernel_sample=lock_busy\n");
    }
    for(unsigned index=0;index<BoProfileSources;++index) {
        if(!IOLockTryLock(g.lock)) {
            IOLog("NativeAdrenoBO: sample=lock_busy source_index=%u\n",index);
            break;
        }
        memcpy(&boProfileSnapshot,&boProfiles[index],sizeof(boProfileSnapshot));
        IOLockUnlock(g.lock);

        if(boProfileSnapshot.generation) {
            printBoProfile(boProfileSnapshot);
        }
    }
    __atomic_store_n(&boProfileDumpBusy,0,__ATOMIC_RELEASE);
}
}

class NativeAdrenoGpuPublished : public IOService {
    OSDeclareDefaultStructors(NativeAdrenoGpuPublished)
public:
    IOReturn newUserClient(task_t owningTask,void *securityID,UInt32 type,
        OSDictionary *properties,IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(NativeAdrenoGpuPublished,IOService)

class AGXDeviceUserClient : public IOUserClient {
    OSDeclareDefaultStructors(AGXDeviceUserClient)
    bool live_=false;
    bool closing_=false;
    IOReturn viewTeardownStatus_=kIOReturnSuccess;
    bool handoffOnly_=false;
    bool kernel_=false;
    NativeAdrenoUserClient *core_=nullptr;   /* own core; deleted in free(), so a call that raced the close sees live_ false, never freed memory */
    unsigned slot_=NativeAdrenoMaxClients;
    uint64_t profileGeneration_=0;
    char name_[20]{};
    IOLock *legacyCpuLock_=nullptr; /* Serializes legacy lease changes and cached-view teardown. */
    /* One user-view descriptor per BO token, reused by every clientMemoryForType call. IOConnectUnmapMemory64 asks clientMemoryForType for
     * the descriptor again and unmaps only a mapping that belongs to THAT descriptor (kIOMapReference); a fresh descriptor per call made the
     * unmap a silent no-op, the user PTEs outlived BoCpuUnmap, and BoFree handed pages with a live user mapping to the VM:
     * pmap_assert_free panicked the freeing core and the panic screen parked it (the "BoFree hang", dead touch). */
    struct View {uint32_t token;IOMemoryDescriptor *descriptor;};
    /* One view per live BO token. The cache must hold every BO the client can have (NativeAdrenoClientMaxObjects): a BO whose view is not
     * cached makes IOConnectUnmapMemory64 (which asks clientMemoryForType again) a no-op, the user mapping outlives BoFree and the freed pages panic
     * pmap_assert_free. A cache of 64 held until the render server's hundreds of mapped BOs (int-087). */
    enum {MaxViews=NativeAdrenoClientMaxObjects};
    View *views_=nullptr;   /* on the heap: 1024 entries would make the client object too big for the kernel allocator's fixed-size classes */
    /* CpuMapping retains one completed map and the BO lease that prevents its
     * allocation from being released while the user address remains mapped. */
    struct CpuMapping {
        uint64_t handle=0,map_id=0;
        uint32_t token=0;
        IOMemoryMap *map=nullptr;
    };
    CpuMapping *cpuMappings_=nullptr;
    uint64_t nextCpuMapId_=0;
    IOReturn dropViews(uint32_t token);
    IOReturn legacyMemoryForType(UInt32,IOOptionBits *,IOMemoryDescriptor **);
    IOReturn runLegacyCpuMapping(uint32_t,const void *,size_t,void *,size_t);
    IOReturn mapCpu(const NativeAdrenoAbiBoCpuMapV2 &,NativeAdrenoAbiBoCpuMapV2Reply &);
    IOReturn unmapCpu(const NativeAdrenoAbiBoCpuUnmapV2 &);
    IOReturn dropCpuMappings();
    static IOReturn action(OSObject *target,void *reference,IOExternalMethodArguments *arguments);
    IOReturn run(uint32_t method,const void *input,size_t input_size,void *output,size_t output_size);
public:
    bool initializeMethodLocks();
    void setHandoffOnly() {handoffOnly_=true;}
    IOReturn mount(bool kernel,unsigned connection_limit);
    friend IOReturn runCore(AGXDeviceUserClient *,uint32_t,const void *,size_t,void *,size_t);
    friend void nativeAdrenoMailboxPoller(void *,wait_result_t);
    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn clientMemoryForType(UInt32 type,IOOptionBits *options,IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t selector,IOExternalMethodArguments *arguments,
        IOExternalMethodDispatch *dispatch,OSObject *target,void *reference) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(AGXDeviceUserClient,IOUserClient)

/* NativeAdrenoControl accepts the administrator's first hardware start
 * request after userspace has connected USB. Its properties report progress;
 * NativeAdrenoGpuPublished appears after the CP has completed GPU work. */
class NativeAdrenoControl : public IOService {
    OSDeclareDefaultStructors(NativeAdrenoControl)
public:
    IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(NativeAdrenoControl,IOService)

static NativeAdrenoControl *controlService=nullptr;

IOReturn NativeAdrenoControl::setProperties(OSObject *properties)
{
    const IOReturn privilege=IOUserClient::clientHasPrivilege(current_task(),kIOClientPrivilegeAdministrator);
    if(privilege) {
        return privilege;
    }
    OSDictionary *dictionary=OSDynamicCast(OSDictionary,properties);
    if(!dictionary || dictionary->getCount()!=1 || dictionary->getObject("Start")!=kOSBooleanTrue) {
        return kIOReturnBadArgument;
    }
    return native_adreno_a6xx_request_start();
}

extern "C" void native_adreno_control_update(uint32_t step,uint32_t status,const char *state)
{
    NativeAdrenoControl *control=controlService;
    if(!control) {
        return;
    }
    control->setProperty("Step",step,32);
    control->setProperty("Status",status,32);
    control->setProperty("State",state);
    control->setProperty("Attempted",state && strcmp(state,"waiting")?kOSBooleanTrue:kOSBooleanFalse);
}

extern "C" IOReturn native_adreno_control_publish(void)
{
    if(controlService) {
        return kIOReturnBusy;
    }
    NativeAdrenoControl *control=new NativeAdrenoControl;
    if(!control || !control->init() || !control->attach(IOService::getPlatform())) {
        OSSafeReleaseNULL(control);
        return kIOReturnNoResources;
    }
    controlService=control;
    control->setName("NativeAdrenoControl");
    control->setProperty("NativeSandboxGrant",kOSBooleanTrue);
    control->setProperty("ChipId",0x06030500u,32);
    native_adreno_control_update(0,0,"waiting");
    control->registerService();
    return kIOReturnSuccess;
}

/* NABU-A640-PUBLISH-L576: is this class already in sAllClassesDict? Same question l440 answered
 * for PMGR (package/ShengPlatformPower.cpp:83-93), same abort: this archive is force-loaded into a
 * prelinked kernel, so its static initializers ran at boot and the metaclasses are registered. The
 * kext handshake must not run twice: postModLoad's duplicate-class branch reads
 * ((OSKext *)orig->reserved->kext)->getIdentifierCString(), and `reserved` is NULL on this build --
 * that is the measured l575 panic, kernel data abort at postModLoad+0x448 (far=0, x0=0), with x21
 * holding NativeAdrenoGpuPublished::gMetaClass. */
static const OSMetaClass *native_adreno_class_known(const char *text)
{
    const OSSymbol *name=OSSymbol::withCString(text);
    const OSMetaClass *meta=name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    return meta;
}

extern "C" IOReturn native_adreno_initialize_classes(void)
{
    static bool initialized=false;
    if(initialized) {
        return kIOReturnBusy;
    }
    if(native_adreno_class_known("NativeAdrenoGpuPublished")) {
        IOLog("NativeAdrenoA6xx: classes registered at boot, skipping the kext modLoad handshake\n");
        initialized=true;
        return kIOReturnSuccess;
    }
    void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if(!handle) {
        return kIOReturnNoResources;
    }
    native_adreno_publish_static_init();
    const OSReturn status=OSMetaClass::postModLoad(handle);
    if(status!=kOSReturnSuccess) {
        return status;
    }
    initialized=true;
    return kIOReturnSuccess;
}

void nativeAdrenoMailboxPoller(void *,wait_result_t)
{
    thread_set_thread_name(current_thread(),"sheng-gpu-mailbox");
    uint32_t last=0;
    uint64_t lastGeneration=0;
    for(;;) {
        IOSleep(1);
        uint8_t input[0x700],output[0x700];
        IOLockLock(g.lock);
        AGXDeviceUserClient *owner=gMailboxOwner;
        if(!owner || !gMailboxWords) {
            IOLockUnlock(g.lock);
            continue;
        }
        const uint64_t generation=owner->profileGeneration_;
        const uint32_t request=gMailboxWords[1];
        if((generation==lastGeneration && request==last) || request==gMailboxWords[2]) {
            IOLockUnlock(g.lock);
            continue;
        }
        last=request;
        lastGeneration=generation;
        const uint32_t method=gMailboxWords[3],in_size=gMailboxWords[5],out_size=gMailboxWords[6];
        const bool validSizes=in_size<=sizeof(input) && out_size<=sizeof(output);
        if(validSizes) {
            memcpy(input,const_cast<uint32_t *>(gMailboxWords)+0x10,in_size);
        }
        owner->retain();
        IOLockUnlock(g.lock);

        IOReturn status=kIOReturnBadArgument;
        if(validSizes) {
            if(method>=NativeAdrenoMethodQuery && method<=NativeAdrenoMethodGetDeviceInfo) {
                /* The native dispatcher holds the same IPC read lock as a
                 * userspace call. Task-bound import and V2 map are unavailable
                 * to this kernel-thread transport. */
                IOExternalMethodArguments arguments{};
                arguments.selector=method;
                arguments.structureInput=input;
                arguments.structureInputSize=in_size;
                arguments.structureOutput=output;
                arguments.structureOutputSize=out_size;
                status=owner->callExternalMethod(method,&arguments);
            } else {
                status=kIOReturnUnsupported;
            }
        }

        IOLockLock(g.lock);
        if(gMailboxOwner==owner && owner->profileGeneration_==generation && gMailboxWords[1]==request) {
            if(!status) {
                memcpy(const_cast<uint32_t *>(gMailboxWords)+0x200,output,out_size);
            }
            ++native_adreno_metal_diag.mb_calls;
            gMailboxWords[4]=uint32_t(status);
            __asm__ volatile("dmb sy" ::: "memory");
            gMailboxWords[2]=request;
            __asm__ volatile("dmb sy" ::: "memory");
        }
        IOLockUnlock(g.lock);
        owner->release();
    }
}

/* The client log row of name (created for the first eight names); null when the table is full. Under g.lock. */
static NativeAdrenoClientRecord *clientRecord(const char *name)
{
    for(auto &record:native_adreno_client_log) {
        if(!record.name[0]) {strlcpy(record.name,name,sizeof record.name);record.slot=0xff;return &record;}
        if(!strncmp(record.name,name,sizeof record.name))return &record;
    }
    return nullptr;
}
IOReturn AGXDeviceUserClient::mount(bool kernel,unsigned connection_limit)
{
    proc_selfname(name_,sizeof name_);
    NativeAdrenoUserClient *core=new NativeAdrenoUserClient;
    if(!core) {
        return kIOReturnNoMemory;
    }
    IOLockLock(g.lock);
    bool ok=false;
    NativeAdrenoClientRecord *record=kernel?nullptr:clientRecord(name_);
    unsigned active=0;
    for(unsigned index=0;index<NativeAdrenoMaxClients;++index) {
        if(g.slots[index] && !strncmp(g.slot_names[index],name_,sizeof(name_))) {
            ++active;
        }
    }
    const bool capped=!kernel && connection_limit && active>=connection_limit;
    /* The render server takes slot 0 (the large window) when free; every other process, or a second render-server connection, the next free one. */
    unsigned slot=NativeAdrenoMaxClients;
    if(kernel)slot=g.kernel_client?NativeAdrenoMaxClients:0;
    else {
        if(!strncmp(name_,"backboardd",sizeof name_) && !g.slots[0])slot=0;
        for(unsigned i=1;slot==NativeAdrenoMaxClients && i<NativeAdrenoMaxClients;++i)if(!g.slots[i])slot=i;
    }
    const IOReturn opened=g.failed?kIOReturnNotReady:capped?kIOReturnBusy:
        slot<NativeAdrenoMaxClients?core->open(NativeAdrenoAbiVersion,g.vm,g.submit,g.ring,g.input_bits,g.output_bits):kIOReturnNoSpace;
    IOReturn result=opened;
    if(opened!=kIOReturnSuccess) {native_adreno_mount_fail[0]=slot<NativeAdrenoMaxClients?2:1;native_adreno_mount_fail[1]=uint32_t(opened);native_adreno_mount_fail[2]=slot;native_adreno_mount_fail[3]=native_adreno_mount_fail[3]+1;}
    if(opened==kIOReturnSuccess) {
        if(g.timer)core->attachTimer(g.timer);
        if(g.info)core->attachDeviceInfo(g.info);
        core->attachStatus(&g.status);
        /* Queue ranges: the kernel client keeps range 0 alone (no user slot is live while the board's self-test runs); users get their slot's. */
        const IOReturn scoped=kernel?core->setScope(0,0,0):core->setScope(slot*NativeAdrenoAbiMaxQueues,kClientWindows[slot].start,kClientWindows[slot].bytes);
        result=scoped;
        if(scoped==kIOReturnSuccess) {
            core_=core;core=nullptr;slot_=slot;kernel_=kernel;live_=true;ok=true;
            if(kernel) {
                g.kernel_client=true;
            } else {
                g.slots[slot]=true;
                strlcpy(g.slot_names[slot],name_,sizeof(g.slot_names[slot]));
            }
            if(!kernel) {
                profileGeneration_=++boProfileGeneration;
                boProfileClientGenerations[slot]=profileGeneration_;
                for(auto &profile:boProfiles) {
                    if(profile.generation && profile.connection_slot==slot) {
                        memset(&profile,0,sizeof(profile));
                    }
                }
            }
            ++g.opened;
            if(record) {
                ++record->opens;
                record->slot=slot;
            }
        } else {
            native_adreno_mount_fail[0]=3;native_adreno_mount_fail[1]=uint32_t(scoped);native_adreno_mount_fail[2]=slot;native_adreno_mount_fail[3]=native_adreno_mount_fail[3]+1;
            core->releaseAll(&g.clock,0);
        }
    }
    /* The mailbox page and its poller serve the render server's connection only. */
    if(ok && !kernel && slot==0 && !strncmp(name_,"backboardd",sizeof name_)) {
        if(!gMailbox) {
            gMailbox=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,kIODirectionInOut,4096,4096);
            if(gMailbox) {gMailboxWords=static_cast<volatile uint32_t *>(gMailbox->getBytesNoCopy());memset(const_cast<uint32_t *>(gMailboxWords),0,4096);}
        }
        if(gMailboxWords) {
            memset(const_cast<uint32_t *>(gMailboxWords),0,4096);
            gMailboxWords[0]=NativeAdrenoMailboxMagic;
            if(!gPollerStarted) {
                thread_t thread=nullptr;
                if(kernel_thread_start(nativeAdrenoMailboxPoller,nullptr,&thread)==KERN_SUCCESS) {thread_deallocate(thread);gPollerStarted=true;}
            }
        }
        if(gMailboxOwner)gMailboxOwner->release();
        retain();gMailboxOwner=this;
    }
    IOLockUnlock(g.lock);
    if(core)delete core;
    return result;
}
bool AGXDeviceUserClient::initializeMethodLocks()
{
    legacyCpuLock_=IOLockAlloc();
    return legacyCpuLock_!=nullptr;
}
/* Removes the cached view(s) of token (0 = all) and tears down every user mapping of them, so the BO's pages are never freed while a user
 * PTE still points at them. The caller holds legacyCpuLock_ through teardown
 * and the corresponding lease decrement, or has exclusive final ownership. */
IOReturn AGXDeviceUserClient::dropViews(uint32_t token)
{
    for(;;) {
        IOMemoryDescriptor *view=nullptr;
        size_t index=MaxViews;
        IOLockLock(g.lock);
        const IOReturn previous=viewTeardownStatus_;
        if(!previous && views_) {
            for(size_t i=0;i<MaxViews;++i) {
                if(views_[i].descriptor && (!token || views_[i].token==token)) {
                    view=views_[i].descriptor;
                    index=i;
                    break;
                }
            }
        }
        IOLockUnlock(g.lock);
        if(previous || !view) {
            return previous;
        }

        /* removeMappingForDescriptor transfers a retained map out of the client's
         * mapping set. Its user PTEs must be removed before the BO can release pages. */
        while(IOMemoryMap *map=removeMappingForDescriptor(view)) {
            const IOReturn status=map->unmap();
            map->release();
            if(status) {
                IOLockLock(g.lock);
                viewTeardownStatus_=status;
                IOLockUnlock(g.lock);
                return status;
            }
            __atomic_fetch_add(&native_adreno_metal_diag.forced_unmaps,1,__ATOMIC_RELAXED);
        }

        IOLockLock(g.lock);
        views_[index].descriptor=nullptr;
        views_[index].token=0;
        IOLockUnlock(g.lock);
        view->release();
    }
}
IOReturn AGXDeviceUserClient::mapCpu(const NativeAdrenoAbiBoCpuMapV2 &request,
    NativeAdrenoAbiBoCpuMapV2Reply &reply)
{
    if(request.version!=NativeAdrenoCpuMapVersion || request.flags || (request.address&4095)) {
        return kIOReturnBadArgument;
    }

    IOLockLock(g.lock);
    if(!live_ || closing_ || viewTeardownStatus_ || g.failed || kernel_) {
        IOLockUnlock(g.lock);
        return kIOReturnNotReady;
    }
    if(nextCpuMapId_==UINT64_MAX) {
        IOLockUnlock(g.lock);
        return kIOReturnNoResources;
    }
    if(!cpuMappings_) {
        cpuMappings_=IONewZero(CpuMapping,MaxViews);
    }
    if(!cpuMappings_) {
        IOLockUnlock(g.lock);
        return kIOReturnNoMemory;
    }
    CpuMapping *destination=nullptr;
    for(size_t index=0;index<MaxViews;++index) {
        if(!cpuMappings_[index].map_id) {
            destination=&cpuMappings_[index];
            break;
        }
    }
    if(!destination) {
        IOLockUnlock(g.lock);
        return kIOReturnNoResources;
    }

    NativeAdrenoAbiBoCpuMap leaseRequest{request.handle};
    NativeAdrenoAbiBoCpuMapReply lease{};
    IOReturn status=core_->cpuMap(&leaseRequest,&lease,true);
    if(status) {
        IOLockUnlock(g.lock);
        return status;
    }
    NativeAdrenoMemory *backing=core_->memoryOf(request.handle);
    IOMemoryDescriptor *descriptor=backing?backing->descriptor():nullptr;
    IOMemoryMap *map=nullptr;
    if(descriptor && request.address<=UINT64_MAX-lease.bytes) {
        /* Default cache flags preserve the allocation's mode in its memory entry.
         * A unique map gives each returned ID an independent unmap lifetime. */
        const IOOptionBits options=kIOMapUnique|(request.address?0:kIOMapAnywhere);
        map=descriptor->createMappingInTask(current_task(),request.address,options,0,lease.bytes);
    }
    if(map && map->getAddress() && map->getLength()==lease.bytes &&
        (!request.address || map->getAddress()==request.address)) {
        destination->handle=request.handle;
        destination->map_id=++nextCpuMapId_;
        destination->token=lease.token;
        destination->map=map;
        reply={destination->map_id,map->getAddress(),map->getLength()};
        IOLockUnlock(g.lock);
        return kIOReturnSuccess;
    }

    if(map) {
        status=map->unmap();
        if(status) {
            destination->handle=request.handle;
            destination->map_id=++nextCpuMapId_;
            destination->token=lease.token;
            destination->map=map;
            viewTeardownStatus_=status;
            backing->quarantine();
            IOLockUnlock(g.lock);
            return status;
        }
        map->release();
    }
    NativeAdrenoAbiBoCpuUnmap undo{request.handle,lease.token,0};
    status=core_->cpuUnmap(&undo,true);
    if(status) {
        viewTeardownStatus_=status;
        if(backing) {
            backing->quarantine();
        }
    }
    IOLockUnlock(g.lock);
    return status?status:kIOReturnNoMemory;
}

IOReturn AGXDeviceUserClient::unmapCpu(const NativeAdrenoAbiBoCpuUnmapV2 &request)
{
    if(!request.handle || !request.map_id) {
        return kIOReturnBadArgument;
    }

    IOLockLock(g.lock);
    if(!live_ || viewTeardownStatus_) {
        const IOReturn status=viewTeardownStatus_?viewTeardownStatus_:kIOReturnNotReady;
        IOLockUnlock(g.lock);
        return status;
    }
    CpuMapping *mapping=nullptr;
    for(size_t index=0;cpuMappings_ && index<MaxViews;++index) {
        if(cpuMappings_[index].map_id==request.map_id && cpuMappings_[index].handle==request.handle) {
            mapping=&cpuMappings_[index];
            break;
        }
    }
    if(!mapping) {
        IOLockUnlock(g.lock);
        return kIOReturnBadArgument;
    }

    IOReturn status=mapping->map->unmap();
    if(!status) {
        NativeAdrenoAbiBoCpuUnmap lease{mapping->handle,mapping->token,0};
        status=core_->cpuUnmap(&lease,true);
    }
    if(status) {
        viewTeardownStatus_=status;
        if(NativeAdrenoMemory *backing=core_->memoryOf(mapping->handle)) {
            backing->quarantine();
        }
        IOLockUnlock(g.lock);
        return status;
    }
    IOMemoryMap *map=mapping->map;
    *mapping={};
    IOLockUnlock(g.lock);
    map->release();
    return kIOReturnSuccess;
}

IOReturn AGXDeviceUserClient::dropCpuMappings()
{
    for(;;) {
        NativeAdrenoAbiBoCpuUnmapV2 request{};
        IOLockLock(g.lock);
        const IOReturn previous=viewTeardownStatus_;
        for(size_t index=0;!previous && cpuMappings_ && index<MaxViews;++index) {
            if(cpuMappings_[index].map_id) {
                request={cpuMappings_[index].handle,cpuMappings_[index].map_id};
                break;
            }
        }
        IOLockUnlock(g.lock);
        if(previous || !request.map_id) {
            return previous;
        }
        const IOReturn status=unmapCpu(request);
        if(status) {
            return status;
        }
    }
}

IOReturn AGXDeviceUserClient::clientClose()
{
    IOLockLock(g.lock);
    closing_=true;
    if(live_) {
        core_->beginClose();
        thread_call_enter(g.completionCall);
    }
    IOLockWakeup(g.lock,&g.completionEvent,false);
    if(!kernel_ && slot_<NativeAdrenoMaxClients) {
        for(auto &profile:boProfiles) {
            if(profile.generation==profileGeneration_ && profile.connection_slot==slot_) {
                profile.active=false;
            }
        }
    }
    AGXDeviceUserClient *mailbox=nullptr;
    if(gMailboxOwner==this) {mailbox=gMailboxOwner;gMailboxOwner=nullptr;}
    IOLockUnlock(g.lock);
    if(handoffOnly_ && integration.displayClose)integration.displayClose(this);

    IOLockLock(legacyCpuLock_);
    IOReturn status=dropCpuMappings();
    if(!status) {
        status=dropViews(0);
    }
    IOLockUnlock(legacyCpuLock_);
    IOLockLock(g.lock);
    if(!status && live_) {
        const uint64_t peak=core_->windowPeak();
        uint64_t deadline=0;
        clock_interval_to_deadline(2000,kMillisecondScale,&deadline);
        for(;;) {
            status=core_->releaseAll(&g.clock,0);
            if((status!=kIOReturnTimeout && status!=kIOReturnBusy) || g.failed) {
                break;
            }
            if(IOLockSleepDeadline(g.lock,&g.completionEvent,deadline,THREAD_ABORTSAFE)!=THREAD_AWAKENED) {
                status=kIOReturnTimeout;
                break;
            }
        }
        /* A failed teardown leaves the objects live and the slot taken: nothing is forced or reused. */
        if(!status) {
            live_=false;
            if(kernel_) {
                g.kernel_client=false;
            } else if(slot_<NativeAdrenoMaxClients) {
                g.slots[slot_]=false;
                memset(g.slot_names[slot_],0,sizeof(g.slot_names[slot_]));
            }
            ++g.closed;native_adreno_metal_diag.closes=native_adreno_metal_diag.closes+1;
            if(NativeAdrenoClientRecord *record=kernel_?nullptr:clientRecord(name_)) {
                record->closes=record->closes+1;record->slot=0xff;
                if(peak>>20>record->peak_mib)record->peak_mib=uint32_t(peak>>20);
            }
        }
    }
    IOLockUnlock(g.lock);
    if(mailbox)mailbox->release();
    if(!status)terminate();
    return status;
}
void AGXDeviceUserClient::free()
{
    IOReturn status=dropCpuMappings();
    if(!status) {
        status=dropViews(0);
    }
    if(!status && cpuMappings_) {
        IODelete(cpuMappings_,CpuMapping,MaxViews);
        cpuMappings_=nullptr;
    }
    if(!status && views_) {IODelete(views_,View,MaxViews);views_=nullptr;}
    /* Still live only when a teardown failed: its objects stay referenced by the VM and the ring, so the core is kept, not freed. */
    if(core_ && !live_) {delete core_;core_=nullptr;}
    if(legacyCpuLock_) {
        IOLockFree(legacyCpuLock_);
        legacyCpuLock_=nullptr;
    }
    if(handoffOnly_ && integration.displayClose)integration.displayClose(this);
    IOUserClient::free();
}
IOReturn AGXDeviceUserClient::clientMemoryForType(UInt32 type,IOOptionBits *options,IOMemoryDescriptor **memory)
{
    if(memory)*memory=nullptr;
    if(!memory || !options)return kIOReturnBadArgument;
    IOLockLock(g.lock);
    const bool ready=!closing_ && !viewTeardownStatus_;
    const bool scanoutReady=ready && handoffOnly_;
    IOLockUnlock(g.lock);
    if(!ready)return kIOReturnNotReady;
    if(type==NativeAdrenoScanoutType) {
        if(!scanoutReady)return kIOReturnNotReady;
        IOMemoryDescriptor *scanout=integration.displayMemory?integration.displayMemory(current_task(),this):nullptr;
        if(!scanout)return kIOReturnNotReady;
        *memory=scanout;                        /* already retained for the caller */
        *options=kIOMapAnywhere|kIOMapInhibitCache;   /* firmware framebuffer: device memory, never cached by the CPU */
        return kIOReturnSuccess;
    }
    if(type==NativeAdrenoMailboxType) {
        IOLockLock(g.lock);
        if(live_ && gMailbox && gMailboxOwner==this) {
            gMailbox->retain();
            *memory=gMailbox;
            *options=kIOMapAnywhere;   /* ordinary cached memory: both sides are CPUs, so it is coherent */
            native_adreno_metal_diag.mb_maps=native_adreno_metal_diag.mb_maps+1;
            IOLockUnlock(g.lock);
            return kIOReturnSuccess;
        }
        IOLockUnlock(g.lock);
        return kIOReturnNotReady;
    }
    if(!legacyCpuLock_)return kIOReturnNotReady;
    IOLockLock(legacyCpuLock_);
    const IOReturn status=legacyMemoryForType(type,options,memory);
    IOLockUnlock(legacyCpuLock_);
    return status;
}

IOReturn AGXDeviceUserClient::legacyMemoryForType(UInt32 type,IOOptionBits *options,IOMemoryDescriptor **memory)
{
    IOReturn status=kIOReturnNotFound;
    native_adreno_view_diag[0]=native_adreno_view_diag[0]+1;
    /* [5]: threads inside this function right now (a thread stuck here, on the gate or in a mapping, shows as a non-zero count that stays). */
    struct Inside {Inside() {__atomic_fetch_add(&native_adreno_view_diag[5],1,__ATOMIC_RELAXED);} ~Inside() {__atomic_fetch_sub(&native_adreno_view_diag[5],1,__ATOMIC_RELAXED);}} inside;
    IOLockLock(g.lock);
    if(!live_ || closing_ || viewTeardownStatus_) {
        IOLockUnlock(g.lock);
        return kIOReturnNotReady;
    }
    if(!views_)views_=IONewZero(View,MaxViews);
    if(!views_) {IOLockUnlock(g.lock);return kIOReturnNoMemory;}
    for(size_t i=0;i<MaxViews;++i)if(views_[i].descriptor && views_[i].token==type) {
        views_[i].descriptor->retain();*memory=views_[i].descriptor;*options=kIOMapAnywhere;
        native_adreno_view_diag[2]=native_adreno_view_diag[2]+1;
        IOLockUnlock(g.lock);
        return kIOReturnSuccess;
    }
    NativeAdrenoMemory *backing=live_?core_->memoryForToken(type):nullptr;
    if(backing) {
        /* The descriptor describes the BO's physical spans. The caller requests
         * the allocation's cache mode; dropViews removes each mapping before
         * the BO releases its pages. */
        /* A BO's pages can come scattered, one span per page (a 64 MiB BO is 16384 of them): the descriptor takes them all, as many as the GPU's
         * own table accepts. A fixed 64-entry array made every scattered BO above 256 KiB fail to map (VK_ERROR_MEMORY_MAP_FAILED, int-084),
         * only when memory happened to be fragmented. The descriptor copies the ranges. */
        enum {MaxSpans=65536};
        const size_t count=backing->spanCount();
        if(!count || count>MaxSpans)status=kIOReturnNoResources;
        else {
            IOAddressRange *ranges=IONewData(IOAddressRange,count);
            if(!ranges)status=kIOReturnNoMemory;
            else {
                for(size_t i=0;i<count;++i)ranges[i]={backing->spans()[i].physical,backing->spans()[i].bytes};
                IOMemoryDescriptor *descriptor=IOMemoryDescriptor::withAddressRanges(ranges,(UInt32)count,
                    static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryTypePhysical64),nullptr);
                IODeleteData(ranges,IOAddressRange,count);
                if(!descriptor)status=kIOReturnNoMemory;
                else {
                    bool cached=false;
                    for(size_t i=0;i<MaxViews;++i)if(!views_[i].descriptor) {views_[i].token=type;views_[i].descriptor=descriptor;descriptor->retain();cached=true;break;}
                    if(!cached) {
                        /* Never hand out a view that cannot be torn down again. */
                        descriptor->release();
                        status=kIOReturnNoResources;
                    } else {
                        *memory=descriptor;
                        *options=kIOMapAnywhere|(backing->writeCombined()?kIOMapWriteCombineCache:kIOMapCopybackCache);
                        native_adreno_view_diag[1]=native_adreno_view_diag[1]+1;
                        status=kIOReturnSuccess;
                    }
                }
            }
        }
    }
    IOLockUnlock(g.lock);
    if(status){native_adreno_view_diag[3]=native_adreno_view_diag[3]+1;native_adreno_view_diag[4]=(uint32_t)status;}
    return status;
}
/* Fence waits as the callers see them (int-147; method 6's own statistics count every poll): [0] waits, [1] us waited in total, [2] pauses
 * taken outside the connection lock, [3] the longest wait in us. */
extern "C" {volatile uint64_t native_adreno_wait_outer[4];}
static IOReturn runCore(AGXDeviceUserClient *connection,uint32_t method,const void *input,size_t input_size,void *output,size_t output_size)
{
    const uint64_t entered=g.clock.now_us?g.clock.now_us(g.clock.cookie):0;
    IOLockLock(g.lock);
    const uint64_t entryLockWait=entered?g.clock.now_us(g.clock.cookie)-entered:0;
    if(method==NativeAdrenoMethodSetMetadata && input && input_size==sizeof(NativeAdrenoAbiMetadata)) {
        const NativeAdrenoAbiMetadata *meta=static_cast<const NativeAdrenoAbiMetadata *>(input);
        /* The raw test's line and the Turnip phase's line ("T ..." result or "W ..." watchdog sample) are kept apart, so neither hides the other. */
        const bool turnip=meta->bytes>=2 && (meta->text[0]=='T' || meta->text[0]=='W') && meta->text[1]==' ';
        char *line=turnip?native_adreno_metal_diag.report2:native_adreno_metal_diag.report;
        uint32_t n=meta->bytes<sizeof(native_adreno_metal_diag.report)-1?meta->bytes:uint32_t(sizeof(native_adreno_metal_diag.report)-1);
        memset(line,0,sizeof(native_adreno_metal_diag.report));
        memcpy(line,meta->text,n);
        native_adreno_metal_diag.reports=native_adreno_metal_diag.reports+1;
    }
    /* Concurrent core calls retain one active count per method. The diagnostic
     * marker names a method that is still inside the driver. */
    if(method<=NativeAdrenoMethodGetResourceBudget) {
        ++activeCoreCalls[method];
    }
    native_adreno_metal_diag.inflight=method+1;
    native_adreno_metal_diag.inflight_since_calls=native_adreno_metal_diag.calls;
    IOLockUnlock(g.lock);
    IOReturn status;
    uint64_t timeoutNs=0;
    bool sleepingWait=false;
    NativeAdrenoAbiWaitFence legacyPoll{};
    const void *waitInput=input;
    if(method==NativeAdrenoMethodWaitFence && input && input_size==sizeof(legacyPoll)) {
        memcpy(&legacyPoll,input,sizeof(legacyPoll));
        timeoutNs=legacyPoll.timeout_ns;
        legacyPoll.timeout_ns=0;
        waitInput=&legacyPoll;
        sleepingWait=true;
    } else if(method==NativeAdrenoMethodSyncWait && input && input_size==sizeof(NativeAdrenoAbiSyncWait)) {
        timeoutNs=static_cast<const NativeAdrenoAbiSyncWait *>(input)->timeout_ns;
        sleepingWait=true;
    } else if(method==NativeAdrenoMethodWaitCompletion && input && input_size==sizeof(NativeAdrenoAbiCompletionWait)) {
        timeoutNs=static_cast<const NativeAdrenoAbiCompletionWait *>(input)->timeout_ns;
        sleepingWait=true;
    } else if(method==NativeAdrenoMethodWaitCapacity && input && input_size==sizeof(NativeAdrenoAbiCapacityWait)) {
        timeoutNs=static_cast<const NativeAdrenoAbiCapacityWait *>(input)->timeout_ns;
        sleepingWait=true;
    } else if(method==NativeAdrenoMethodWaitBoIdle && input && input_size==sizeof(NativeAdrenoAbiBoWait)) {
        timeoutNs=static_cast<const NativeAdrenoAbiBoWait *>(input)->timeout_ns;
        sleepingWait=true;
    }
    if(sleepingWait) {
        uint64_t interval=0,deadline=0,pauses=0;
        if(timeoutNs!=UINT64_MAX) {
            nanoseconds_to_absolutetime(timeoutNs,&interval);
            clock_absolutetime_interval_to_deadline(interval,&deadline);
        }
        IOLockLock(g.lock);
        for(;;) {
            status=connection->live_ && !connection->closing_ && !connection->viewTeardownStatus_ && !g.failed?
                connection->core_->dispatch(method,waitInput,input_size,output,output_size,&g.clock):kIOReturnNotReady;
            if(status!=kIOReturnTimeout || !timeoutNs) {
                break;
            }
            ++pauses;
            const int awakened=timeoutNs==UINT64_MAX?
                IOLockSleep(g.lock,&g.completionEvent,THREAD_ABORTSAFE):
                IOLockSleepDeadline(g.lock,&g.completionEvent,deadline,THREAD_ABORTSAFE);
            if(awakened!=THREAD_AWAKENED) {
                status=kIOReturnAborted;
                /* The deadline passed without a completion wakeup: poll once more so a
                 * finished fence is not reported as a timeout when the interrupt is missing. */
                if(awakened==THREAD_TIMED_OUT) {
                    status=connection->live_ && !connection->closing_ && !connection->viewTeardownStatus_ && !g.failed?
                        connection->core_->dispatch(method,waitInput,input_size,output,output_size,&g.clock):kIOReturnNotReady;
                }
                break;
            }
        }
        const uint64_t waited=g.clock.now_us(g.clock.cookie)-entered;
        native_adreno_wait_outer[0]=native_adreno_wait_outer[0]+1;
        native_adreno_wait_outer[1]=native_adreno_wait_outer[1]+waited;
        native_adreno_wait_outer[2]=native_adreno_wait_outer[2]+pauses;
        if(waited>native_adreno_wait_outer[3]) {
            native_adreno_wait_outer[3]=waited;
        }
        IOLockUnlock(g.lock);
    } else {
        const uint64_t lockBegan=g.clock.now_us?g.clock.now_us(g.clock.cookie):0;
        IOLockLock(g.lock);
        if(lockBegan && g.clock.now_us) {
            native_adreno_profile_lock_wait(method,entryLockWait+g.clock.now_us(g.clock.cookie)-lockBegan);
        }
        if(method==NativeAdrenoMethodSubmit || method==NativeAdrenoMethodSubmitV2 ||
            method==NativeAdrenoMethodSubmitAsync) {
            native_adreno_profile_set_client(connection->name_);
        }
        status=connection->live_ && !connection->closing_ && !connection->viewTeardownStatus_ && (!g.failed || method==NativeAdrenoMethodGetStatus)?
            connection->core_->dispatch(method,input,input_size,output,output_size,&g.clock):kIOReturnNotReady;
        if(!g.failed && (g.vm->poisoned() || g.submit->poisoned() || g.ring->poisoned())) {
            g.failed=true;
            g.status.noteFault();
            g.submit->scheduler()->fault(kIOReturnIOError,false);
            IOLockWakeup(g.lock,&g.completionEvent,false);
            thread_call_enter(g.completionCall);
        }
        if(method==NativeAdrenoMethodSubmitAsync || method==NativeAdrenoMethodSyncSignal) {
            IOLockWakeup(g.lock,&g.completionEvent,false);
        }
        if(connection->live_ && connection->slot_<NativeAdrenoMaxClients &&
            (method==NativeAdrenoMethodBoMap || method==NativeAdrenoMethodBoFree ||
             method==NativeAdrenoMethodGetResourceBudget)) {
            connection->core_->resourceBudget(&resourceBudgets[connection->slot_]);
        }
        if(method==NativeAdrenoMethodBoMap && !status && connection->live_ && !connection->kernel_)
            if(NativeAdrenoClientRecord *record=clientRecord(connection->name_))
                if(connection->core_->windowPeak()>>20>record->peak_mib)record->peak_mib=uint32_t(connection->core_->windowPeak()>>20);
        IOLockUnlock(g.lock);
    }
    IOLockLock(g.lock);
    if(method<=NativeAdrenoMethodGetResourceBudget) {
        --activeCoreCalls[method];
    }
    native_adreno_metal_diag.inflight=0;
    for(unsigned active=0;active<=NativeAdrenoMethodGetResourceBudget;++active) {
        if(activeCoreCalls[active]) {
            native_adreno_metal_diag.inflight=active+1;
            break;
        }
    }
    native_adreno_metal_diag.calls=native_adreno_metal_diag.calls+1;
    native_adreno_metal_diag.last_selector=method;
    native_adreno_metal_diag.last_status=uint32_t(status);
    IOLockUnlock(g.lock);
    return status;
}
IOReturn AGXDeviceUserClient::run(uint32_t method,const void *input,size_t input_size,void *output,size_t output_size)
{
    if(method==NativeAdrenoMethodBoCpuMapV2) {
        if(!input || input_size!=sizeof(NativeAdrenoAbiBoCpuMapV2) ||
            !output || output_size!=sizeof(NativeAdrenoAbiBoCpuMapV2Reply)) {
            return kIOReturnBadArgument;
        }
        NativeAdrenoAbiBoCpuMapV2 request{};
        NativeAdrenoAbiBoCpuMapV2Reply reply{};
        memcpy(&request,input,sizeof(request));
        const IOReturn status=mapCpu(request,reply);
        if(!status) {
            memcpy(output,&reply,sizeof(reply));
        }
        return status;
    }
    if(method==NativeAdrenoMethodBoCpuUnmapV2) {
        if(!input || input_size!=sizeof(NativeAdrenoAbiBoCpuUnmapV2) || output_size) {
            return kIOReturnBadArgument;
        }
        NativeAdrenoAbiBoCpuUnmapV2 request{};
        memcpy(&request,input,sizeof(request));
        return unmapCpu(request);
    }
    if(method==NativeAdrenoMethodReportBoProfile) {
        if(!input || input_size!=sizeof(NativeAdrenoAbiBoProfile) || output_size) {
            return kIOReturnBadArgument;
        }
        NativeAdrenoAbiBoProfile record{};
        memcpy(&record,input,sizeof(record));
        if(!validBoProfile(record)) {
            return kIOReturnBadArgument;
        }

        IOLockLock(g.lock);
        const bool current=live_ && !closing_ && !kernel_ && slot_<NativeAdrenoMaxClients &&
            g.slots[slot_] && boProfileClientGenerations[slot_]==profileGeneration_;
        IOReturn status=kIOReturnNotReady;
        if(current) {
            BoProfileRecords *destination=nullptr;
            BoProfileRecords *unused=nullptr;
            BoProfileRecords *closed=nullptr;
            for(auto &profile:boProfiles) {
                if(profile.generation==profileGeneration_ && profile.connection_slot==slot_ &&
                    profile.device_id==record.device_id) {
                    destination=&profile;
                    break;
                }
                if(!profile.generation && !unused) {
                    unused=&profile;
                } else if(!profile.active && !closed) {
                    closed=&profile;
                }
            }
            if(!destination) {
                destination=unused?unused:closed;
                if(destination) {
                    memset(destination,0,sizeof(*destination));
                    destination->generation=profileGeneration_;
                    destination->device_id=record.device_id;
                    destination->connection_slot=slot_;
                    destination->active=true;
                    memcpy(destination->name,name_,sizeof(destination->name));
                    sanitizeProfileName(destination->name,sizeof(destination->name));
                }
            }
            if(destination) {
                saveBoProfile(*destination,record);
                status=kIOReturnSuccess;
            } else {
                status=kIOReturnNoResources;
            }
        }
        IOLockUnlock(g.lock);
        return status;
    }
    if(method>=NativeAdrenoHandoffBase && method<=NativeAdrenoHandoffLast) {
        if(!handoffOnly_)return kIOReturnUnsupported;
        return integration.displayCall?
            integration.displayCall(method-NativeAdrenoHandoffBase,current_task(),this,input,input_size,output,output_size):
            kIOReturnUnsupported;
    }
    if(method==NativeAdrenoExportMethod) {
        struct Piece {uint32_t generation,total,offset,bytes;};
        char caller[20]={};proc_selfname(caller,sizeof caller);
        Piece piece{};
        if(input && input_size>=sizeof(piece))memcpy(&piece,input,sizeof(piece));
        const bool valid=!output_size && input && input_size>=sizeof(piece) && piece.bytes && piece.bytes<=NativeAdrenoExportMaxPiece &&
            input_size==sizeof(piece)+piece.bytes && !strncmp(caller,"backboardd",sizeof caller);
        IOLockLock(g.export_lock);
        const int stored=valid && integration.exportPut?integration.exportPut(piece.generation,piece.total,piece.offset,
            static_cast<const uint8_t *>(input)+sizeof(piece),piece.bytes):-1;
        if(stored) {
            native_adreno_export_stats[1]=native_adreno_export_stats[1]+1;
        } else {
            native_adreno_export_stats[0]=native_adreno_export_stats[0]+1;
            native_adreno_export_stats[2]=piece.generation;
            native_adreno_export_stats[3]=piece.total;
        }
        IOLockUnlock(g.export_lock);
        return stored?kIOReturnBadArgument:kIOReturnSuccess;
    }
    if(method==NativeAdrenoMethodBoCpuMap || method==NativeAdrenoMethodBoCpuUnmap) {
        if(!legacyCpuLock_) {
            return kIOReturnNotReady;
        }
        IOLockLock(legacyCpuLock_);
        const IOReturn status=runLegacyCpuMapping(method,input,input_size,output,output_size);
        IOLockUnlock(legacyCpuLock_);
        return status;
    }
    if(method==NativeAdrenoMethodBoImport) {
        /* The pages belong to the task making this call (an external method runs in the caller's context). */
        if(!input || input_size!=sizeof(NativeAdrenoAbiBoImport) || !output || output_size!=sizeof(NativeAdrenoAbiBoAllocReply))
            return kIOReturnBadArgument;
        NativeAdrenoAbiBoImport request{};
        memcpy(&request,input,sizeof(request));
        NativeAdrenoAbiBoAllocReply reply{};
        IOLockLock(g.lock);
        const uint32_t previousInflight=native_adreno_metal_diag.inflight;
        native_adreno_metal_diag.inflight=method+1;
        const IOReturn status=live_ && !closing_ && !viewTeardownStatus_?
            core_->importBo(&request,current_task(),&reply):kIOReturnNotReady;
        native_adreno_metal_diag.inflight=previousInflight;
        ++native_adreno_metal_diag.calls;
        native_adreno_metal_diag.last_selector=method;
        native_adreno_metal_diag.last_status=uint32_t(status);
        IOLockUnlock(g.lock);
        if(!status)memcpy(output,&reply,sizeof(reply));
        return status;
    }
    return runCore(this,method,input,input_size,output,output_size);
}

/* The legacy mapping operation holds legacyCpuLock_ across its lease count,
 * descriptor teardown and final core update. V2 mappings own separate IDs and
 * use g.lock for their complete state transition. */
IOReturn AGXDeviceUserClient::runLegacyCpuMapping(uint32_t method,const void *input,size_t input_size,
    void *output,size_t output_size)
{
    if(method==NativeAdrenoMethodBoCpuUnmap) {
        if(!input || input_size!=sizeof(NativeAdrenoAbiBoCpuUnmap)) {
            return kIOReturnBadArgument;
        }
        /* The view is shared by every CPU mapping of the BO, and dropViews removes all of them. A BO mapped twice (the view cache served 207
         * second requests in int-177) lost both mappings at the first unmap, and the other holder's next store hit an unmapped page: the
         * render server's crash in the command stream writes of vkCmdCopyBuffer. Only the last lease takes the view down; the earlier
         * ones leave their mapping until it, or the client unmaps it itself (IOConnectUnmapMemory64 works on the cached view). */
        const auto *request=static_cast<const NativeAdrenoAbiBoCpuUnmap *>(input);
        const uint32_t token=request->token;
        IOLockLock(g.lock);
        const bool current=live_ && !closing_ && !viewTeardownStatus_ &&
            core_->hasMappingLease(request->handle,token);
        const uint32_t leases=live_?core_->mappingLeases(token):0;
        IOLockUnlock(g.lock);
        if(!current) {
            return kIOReturnBadArgument;
        }
        if(leases>1) {
            __atomic_fetch_add(&native_adreno_view_kept,1,__ATOMIC_RELAXED);
        }
        else {
            const IOReturn status=dropViews(token);
            if(status) {
                return status;
            }
        }
    }
    return runCore(this,method,input,input_size,output,output_size);
}
IOReturn AGXDeviceUserClient::action(OSObject *target,void *,IOExternalMethodArguments *arguments)
{
    AGXDeviceUserClient *connection=OSDynamicCast(AGXDeviceUserClient,target);
    if(!connection || !arguments)return kIOReturnBadArgument;
    return connection->run(arguments->selector,arguments->structureInput,arguments->structureInputSize,
        arguments->structureOutput,arguments->structureOutputSize);
}
IOReturn AGXDeviceUserClient::externalMethod(uint32_t selector,IOExternalMethodArguments *arguments,
    IOExternalMethodDispatch *,OSObject *target,void *reference)
{
    struct InputCopy {
        uint8_t *bytes=nullptr;
        size_t count=0;
        ~InputCopy() {
            if(bytes) {
                IODeleteData(bytes,uint8_t,count);
            }
        }
    } owned;
    IOExternalMethodArguments copied{};
    if(arguments && arguments->structureInputDescriptor) {
        IOMemoryDescriptor *descriptor=arguments->structureInputDescriptor;
        const uint64_t length=descriptor->getLength();
        if(!length || length>NativeAdrenoSubmitMaxPacketBytes+8) {
            return kIOReturnBadArgument;
        }
        owned.count=size_t(length);
        owned.bytes=IONewData(uint8_t,owned.count);
        if(!owned.bytes) {
            return kIOReturnNoMemory;
        }

        /* The IOKit input descriptor owns pageable user memory. Its pages must
         * stay wired while readBytes copies them into the bounded packet. */
        const IOReturn prepare_status=descriptor->prepare(kIODirectionOut);
        if(prepare_status) {
            return prepare_status;
        }
        const IOByteCount copied_bytes=descriptor->readBytes(0,owned.bytes,owned.count);
        const IOReturn complete_status=descriptor->complete(kIODirectionOut);
        if(complete_status) {
            return complete_status;
        }
        if(copied_bytes!=owned.count) {
            return kIOReturnBadArgument;
        }

        copied=*arguments;
        copied.structureInput=owned.bytes;
        copied.structureInputSize=uint32_t(owned.count);
        copied.structureInputDescriptor=nullptr;
        arguments=&copied;
    }
    /* One entry per ABI method; sizes are the exact wire records. */
    static const IOExternalMethodDispatch table[]={
        {&action,0,0,0,sizeof(NativeAdrenoAbiParams)},
        {&action,0,sizeof(NativeAdrenoAbiBoAlloc),0,sizeof(NativeAdrenoAbiBoAllocReply)},
        {&action,0,sizeof(NativeAdrenoAbiBoMap),0,sizeof(NativeAdrenoAbiBoMapReply)},
        {&action,0,sizeof(NativeAdrenoAbiBoFree),0,0},
        {&action,0,sizeof(NativeAdrenoAbiSubmit),0,sizeof(NativeAdrenoAbiSubmitReply)},
        {&action,0,sizeof(NativeAdrenoAbiWaitFence),0,0},
        {&action,0,0,0,sizeof(NativeAdrenoAbiTimestamp)},
        {&action,0,0,0,sizeof(NativeAdrenoAbiStatus)},
        {&action,0,sizeof(NativeAdrenoAbiMetadata),0,0},
        {&action,0,sizeof(NativeAdrenoAbiMetadata),0,sizeof(NativeAdrenoAbiMetadata)},
        {&action,0,sizeof(NativeAdrenoAbiQueue),0,sizeof(NativeAdrenoAbiQueueReply)},
        {&action,0,sizeof(NativeAdrenoAbiQueue),0,0},
        {&action,0,sizeof(NativeAdrenoAbiBoCpuMap),0,sizeof(NativeAdrenoAbiBoCpuMapReply)},
        {&action,0,sizeof(NativeAdrenoAbiBoCpuUnmap),0,0},
        {&action,0,0,0,sizeof(NativeAdrenoAbiDeviceInfo)},
        {&action,0,sizeof(NativeAdrenoAbiBoImport),0,sizeof(NativeAdrenoAbiBoAllocReply)},
        {&action,0,sizeof(NativeAdrenoAbiBoProfile),0,0},
        {&action,0,sizeof(NativeAdrenoAbiBoCpuMapV2),0,sizeof(NativeAdrenoAbiBoCpuMapV2Reply)},
        {&action,0,sizeof(NativeAdrenoAbiBoCpuUnmapV2),0,0},
        {&action,0,0,0,sizeof(NativeAdrenoAbiSubmitCaps)},
        {&action,0,kIOUCVariableStructureSize,0,sizeof(NativeAdrenoAbiSubmitV2Reply)},
        {&action,0,0,0,sizeof(NativeAdrenoAbiResourceBudget)},
        {&action,0,0,0,sizeof(NativeAdrenoAbiAsyncCaps)},
        {&action,0,sizeof(NativeAdrenoAbiSyncCreate),0,sizeof(NativeAdrenoAbiSyncHandle)},
        {&action,0,sizeof(NativeAdrenoAbiSyncHandle),0,0},
        {&action,0,sizeof(NativeAdrenoAbiSyncPoint),0,0},
        {&action,0,sizeof(NativeAdrenoAbiSyncHandle),0,sizeof(NativeAdrenoAbiSyncValue)},
        {&action,0,sizeof(NativeAdrenoAbiSyncWait),0,0},
        {&action,0,kIOUCVariableStructureSize,0,sizeof(NativeAdrenoAbiAsyncReply)},
        {&action,0,sizeof(NativeAdrenoAbiCompletionWait),0,0},
        {&action,0,sizeof(NativeAdrenoAbiCapacityWait),0,0},
        {&action,0,sizeof(NativeAdrenoAbiBoWait),0,0}};
    __atomic_fetch_add(&native_adreno_metal_diag.ext_entries,1,__ATOMIC_RELAXED);
    if(selector<256) {
        __atomic_fetch_or(&native_adreno_metal_diag.sel_bits[selector>>5],1u<<(selector&31),__ATOMIC_RELAXED);
    }
    /* Tunnel. The sandbox resolves a filter policy for this class at open time and then admits only the selectors its profile lists,
     * so which selector numbers reach this object is decided outside the driver. A tunnel call carries the real ABI method in its
     * first eight bytes and may use ANY admitted selector: {magic, method, payload...} in, the method's exact output out. Method
     * 0xffffffff is a probe that only proves the selector was admitted. */
    if(arguments && arguments->structureInput && arguments->structureInputSize>=8 &&
        static_cast<const uint32_t *>(arguments->structureInput)[0]==NativeAdrenoTunnelMagic) {
        const uint32_t *header=static_cast<const uint32_t *>(arguments->structureInput);
        if(header[1]==0xffffffffu) {
            __atomic_fetch_add(&native_adreno_metal_diag.probes,1,__ATOMIC_RELAXED);
            return kIOReturnSuccess;
        }
        __atomic_fetch_add(&native_adreno_metal_diag.tunnel_calls,1,__ATOMIC_RELAXED);
        const IOReturn tunneled=run(header[1],reinterpret_cast<const uint8_t *>(arguments->structureInput)+8,
            arguments->structureInputSize-8,arguments->structureOutput,arguments->structureOutputSize);
        __atomic_store_n(&native_adreno_metal_diag.ext_status,uint32_t(tunneled),__ATOMIC_RELAXED);
        return tunneled;
    }
    __atomic_store_n(&native_adreno_metal_diag.ext_selector,selector,__ATOMIC_RELAXED);
    __atomic_store_n(&native_adreno_metal_diag.ext_in,arguments?arguments->structureInputSize:0xffff,__ATOMIC_RELAXED);
    __atomic_store_n(&native_adreno_metal_diag.ext_out,arguments?arguments->structureOutputSize:0xffff,__ATOMIC_RELAXED);
    if(selector<1 || selector>sizeof(table)/sizeof(table[0])) {
        __atomic_store_n(&native_adreno_metal_diag.ext_status,uint32_t(kIOReturnUnsupported),__ATOMIC_RELAXED);
        return kIOReturnUnsupported;
    }
    const IOReturn result=IOUserClient::externalMethod(selector,arguments,const_cast<IOExternalMethodDispatch *>(&table[selector-1]),
        target?target:this,reference);
    __atomic_store_n(&native_adreno_metal_diag.ext_status,uint32_t(result),__ATOMIC_RELAXED);
    return result;
}
/* A nonzero limit caps simultaneously live connections for one process name.
 * Mount reserves its slot and increments the live count under g.lock. */
extern "C" IOReturn native_adreno_new_user_client(IOService *provider,task_t owningTask,void *securityID,UInt32 type,
    OSDictionary *properties,IOUserClient **handler,unsigned limit)
{
    if(handler)*handler=nullptr;
    if(!handler || !g.published)return kIOReturnNotReady;
    /* Progress markers: open types 0x4147xxxx (xxxx nonzero) are not connections. The plugin sends one before and after each step so
     * that a sandbox that admits neither selectors nor mapping, or a crash, still leaves a trace of how far it got. */
    if((type>>16)==NativeAdrenoPluginOpenType && (type&0xffff)) {
        const uint32_t value=type&0xffff,slot=native_adreno_metal_diag.marks_total&15;
        if((value&0xff00)==0x6d00) {
            nabu_gpu_problem_log_marker(value);
            return kIOReturnUnsupported;
        }
        /* Text stream into report2, one character per marker: 0x7fff starts a line, 0x70cc appends character cc. It needs no connection,
         * so the plugin's Turnip watchdog can report while Turnip itself holds its connection slot. Not counted as markers. */
        if(value>=0x8000) {
            static unsigned row=48,length;
            if(value>=0xC000) {
                row=value&0x3f;length=0;
                if(row<48)memset(native_adreno_metal_rows[row],0,sizeof(native_adreno_metal_rows[row]));
                native_adreno_metal_rows_seq=native_adreno_metal_rows_seq+1;
            } else if(row<48) {
                const char pair[2]={char((value>>7)&0x7f),char(value&0x7f)};
                for(unsigned i=0;i<2;++i)if(pair[i] && length<sizeof(native_adreno_metal_rows[row])-1)native_adreno_metal_rows[row][length++]=pair[i];
            }
            return kIOReturnUnsupported;
        }
        if(value>=0x7000 && value<0x8000) {
            /* 0x7ffe starts a line in report (GPU7) instead: the Metal bridge's result, after the raw test's line has been read. */
            static uint32_t length;
            static char *line=native_adreno_metal_diag.report2;
            static_assert(sizeof(native_adreno_metal_diag.report)==sizeof(native_adreno_metal_diag.report2),"stream lines");
            if(value==0x7fff || value==0x7ffe) {
                line=value==0x7fff?native_adreno_metal_diag.report2:native_adreno_metal_diag.report;
                /* A line from a process other than the render server goes to that process's own row (native_adreno_client_text): the render server
                 * rewrites report2 every ten seconds, so the reason another process's device failed would be gone from it. */
                char who[20]={};
                proc_selfname(who,sizeof who);
                if(value==0x7fff && strncmp(who,"backboardd",sizeof who)) {
                    for(auto &entry:native_adreno_client_text) {
                        if(!entry.name[0]) strlcpy(entry.name,who,sizeof entry.name);
                        if(!strncmp(entry.name,who,sizeof entry.name)) {line=entry.text;break;}
                    }
                }
                memset(line,0,sizeof(native_adreno_metal_diag.report2));length=0;
            }
            else if(value<0x7100 && length<sizeof(native_adreno_metal_diag.report2)-1) {line[length++]=char(value&0xff);line[length]=0;}
            return kIOReturnUnsupported;
        }
        native_adreno_metal_diag.marks[slot]=value;
        native_adreno_metal_diag.marks_total=native_adreno_metal_diag.marks_total+1;
        if(value==1) {
            memcpy(native_adreno_metal_diag.starter[2],native_adreno_metal_diag.starter[1],17);
            memcpy(native_adreno_metal_diag.starter[1],native_adreno_metal_diag.starter[0],17);
            proc_selfname(native_adreno_metal_diag.starter[0],17);
        }
        return kIOReturnUnsupported;
    }
    shiftName(native_adreno_metal_diag.opener);
    AGXDeviceUserClient *connection=new AGXDeviceUserClient;
    if(!connection)return kIOReturnNoMemory;
    if(!connection->initWithTask(owningTask,securityID,type,properties) ||
        !connection->initializeMethodLocks()) {
        connection->release();
        native_adreno_metal_diag.bad=native_adreno_metal_diag.bad+1;
        return kIOReturnNoResources;
    }
    /* XNU gives ordinary external methods a shared IPC lock and gives close,
     * task death and legacy IOConnectMap/Unmap an exclusive IPC lock. The GPU
     * core uses g.lock; legacy lease/view changes also use legacyCpuLock_. */
    connection->setProperty(kIOUserClientDefaultLockingKey,kOSBooleanTrue);
    connection->setProperty(kIOUserClientDefaultLockingSingleThreadExternalMethodKey,kOSBooleanFalse);
    /* IOService::newUserClient attaches and starts what it creates (a client without a provider chain is also a reason for the sandbox to
     * refuse). Then the two sandbox gates IOServiceOpen would apply afterwards are run here for every opener and logged with the
     * process name and open type, so the boot page shows which processes the sandbox lets reach this class and with which type. */
    unsigned attach_state=0;
    if(provider) {
        const bool attached=connection->attach(provider);
        attach_state=attached?1:2;
        if(attached && !connection->start(provider)) {
            connection->detach(provider);
            attach_state=3;
        }
    }
    native_adreno_metal_diag.attach_status=attach_state;
    const uint32_t macf=uint32_t(mac_iokit_check_open(kauth_cred_get(),connection,type));
    uint32_t resolver=0xfeedfeed;
    if(gIOUCFilterCallbacks && gIOUCFilterCallbacks->io_filter_resolver) {
        io_filter_policy_t policy=0;
        const IOReturn resolved=gIOUCFilterCallbacks->io_filter_resolver(owningTask,connection,type,&policy);
        resolver=uint32_t(resolved);
        if(resolved==kIOReturnSuccess && policy && gIOUCFilterCallbacks->io_filter_release)gIOUCFilterCallbacks->io_filter_release(policy);
    }
    native_adreno_metal_diag.chk_type=type;
    native_adreno_metal_diag.chk_macf=macf;
    native_adreno_metal_diag.chk_res=resolver;
    {
        NativeAdrenoOpenRecord &record=native_adreno_open_log[native_adreno_open_log_count&7];
        proc_selfname(record.name,sizeof(record.name));
        record.type=type;record.macf=macf;record.res=resolver;record.attach=attach_state;record.foreign=type!=NativeAdrenoPluginOpenType;
        native_adreno_open_log_count=native_adreno_open_log_count+1;
    }
    if(type==NativeAdrenoHandoffOpenType) {
        connection->setHandoffOnly();
        *handler=connection;
        return kIOReturnSuccess;
    }
    /* Only the Adreno plugin's open (type NativeAdrenoPluginOpenType) is served. The original IOGPU library opens the accelerator with type 1
     * (and an API value in the upper bits) for an IOGPUDeviceUserClient, which this driver is not; refuse it cleanly. */
    if(type!=NativeAdrenoPluginOpenType) {
        connection->release();
        native_adreno_metal_diag.foreign_opens=native_adreno_metal_diag.foreign_opens+1;
        native_adreno_metal_diag.foreign_type=type;
        return kIOReturnUnsupported;
    }
    /* The diagnostic follows the kernel's process and signed-entitlement
     * predicates for this service, task and connection type. */
    const bool kernel=owningTask==kernel_task;
    char opener[20]={};
    proc_selfname(opener,sizeof opener);
    const OSString *grant=provider?OSDynamicCast(OSString,provider->getProperty("NativeSandboxGrantProcess")):nullptr;
    bool granted = false;
    if (grant) {
        for (const char *entry = grant->getCStringNoCopy(); *entry;) {
            size_t length = 0;
            while (entry[length] && entry[length] != ',') {
                ++length;
            }

            const size_t compared = length < 16 ? length : 16;
            if (length && !strncmp(opener, entry, compared) &&
                (length >= 16 || opener[length] == 0)) {
                granted = true;
                break;
            }

            entry += length + (entry[length] == ',' ? 1 : 0);
        }
    }

    if(!granted && provider) {
        const OSMetaClass *meta=provider->getMetaClass();
        const OSString *plugin=OSDynamicCast(OSString,provider->getProperty("MetalPluginName"));
        const OSString *pluginClass=OSDynamicCast(OSString,provider->getProperty("MetalPluginClassName"));
        const OSString *entitlement=OSDynamicCast(OSString,provider->getProperty("NativeSandboxGrantEntitlement"));
        const OSNumber *nativeType=OSDynamicCast(OSNumber,provider->getProperty("NativeSandboxGrantConnectType"));
        granted=meta && !strcmp(meta->getClassName(),"AGXAccelerator") &&
            plugin && plugin->isEqualTo("NativeAdrenoMetal") &&
            pluginClass && pluginClass->isEqualTo("NativeAdrenoMetalDevice") &&
            entitlement && entitlement->isEqualTo("com.apple.security.ts.opengl-or-metal") &&
            nativeType && nativeType->unsigned32BitValue()==NativeAdrenoPluginOpenType &&
            type==NativeAdrenoPluginOpenType &&
            IOTaskHasEntitlement(owningTask,entitlement->getCStringNoCopy());
    }

    if(!kernel && granted &&
       (macf || (resolver!=uint32_t(kIOReturnSuccess) && resolver!=uint32_t(kIOReturnUnsupported)))) {
        __atomic_fetch_add(&native_adreno_metal_diag.grants,1,__ATOMIC_RELAXED);
    }
    /* Closed enumeration and device connections return their admission credit.
     * The fixed client slots also bound every process's live GPU resources. */
    const unsigned cap=strncmp(opener,"backboardd",sizeof opener)?(limit && limit<8?limit:8):limit;
    const IOReturn refusal=connection->mount(kernel,cap);
    if(refusal) {
        connection->release();
        native_adreno_metal_diag.busy=native_adreno_metal_diag.busy+1;
        IOLockLock(g.lock);
        if(NativeAdrenoClientRecord *record=kernel?nullptr:clientRecord(opener)) {
            ++record->refused;
            record->last_refusal=uint32_t(refusal);
        }
        IOLockUnlock(g.lock);
        return refusal;
    }
    native_adreno_metal_diag.opens=native_adreno_metal_diag.opens+1;
    *handler=connection;
    return kIOReturnSuccess;
}
extern "C" void native_adreno_note_property_read(const char *key)
{
    if(key && !strcmp(key,"MetalPluginName")) {
        proc_selfname(native_adreno_metal_diag.name_reader,17);
        native_adreno_metal_diag.name_reads=native_adreno_metal_diag.name_reads+1;
        char name[20]={};
        proc_selfname(name,sizeof name);
        /* Unlocked: the discovery runs at process start, a lost count is harmless and a row is claimed by its first byte. */
        for(auto &asker:native_adreno_metal_askers) {
            if(!asker.name[0]) {strlcpy(asker.name,name,sizeof asker.name);asker.reads=1;break;}
            if(!strncmp(asker.name,name,sizeof asker.name)) {asker.reads=asker.reads+1;break;}
        }
    }
    shiftName(native_adreno_metal_diag.reader);
    native_adreno_metal_diag.property_reads=native_adreno_metal_diag.property_reads+1;
}
IOReturn NativeAdrenoGpuPublished::newUserClient(task_t owningTask,void *securityID,UInt32 type,
    OSDictionary *properties,IOUserClient **handler)
{
    return native_adreno_new_user_client(this,owningTask,securityID,type,properties,handler,0);
}

static uint64_t profileContinuousMicroseconds()
{
    uint64_t nanoseconds = 0;
    absolutetime_to_nanoseconds(mach_continuous_time(), &nanoseconds);
    return nanoseconds / 1000;
}

/* Publishes the service once. Everything passed in must outlive the system (the board probe keeps them in statics) and
 * the VM, submit object and ring must already be attached to the live GPU context. The core must not be open. */
extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *vm,NativeAdrenoSubmit *submit,NativeAdrenoRing *ring,
    const NativeAdrenoRingClock *clock,const NativeAdrenoTimer *timer,const NativeAdrenoAbiDeviceInfo *info,
    uint32_t input_bits,uint32_t output_bits)
{
    if(g.published || !vm || !submit || !ring || !clock || !clock->now_us)return kIOReturnBadArgument;
    g.lock=IOLockAlloc();
    g.export_lock=IOLockAlloc();
    g.completionCall=thread_call_allocate(completionWorker,nullptr);
    NativeAdrenoGpuPublished *service=new NativeAdrenoGpuPublished;
    if(!g.lock || !g.export_lock || !g.completionCall || !service || !service->init() || !service->attach(IOService::getPlatform())) {
        if(service)service->release();
        if(g.export_lock) {
            IOLockFree(g.export_lock);
            g.export_lock=nullptr;
        }
        if(g.lock) {
            IOLockFree(g.lock);
            g.lock=nullptr;
        }
        if(g.completionCall) {
            thread_call_free(g.completionCall);
            g.completionCall=nullptr;
        }
        return kIOReturnNoMemory;
    }
    g.vm=vm;g.submit=submit;g.ring=ring;g.clock=*clock;g.timer=timer;g.info=info;g.input_bits=input_bits;g.output_bits=output_bits;
    native_adreno_profile_set_continuous_clock(profileContinuousMicroseconds);
    nabu_gpu_problem_log_initialize();
    g.service=service;g.published=true;
    service->setName("NativeAdrenoGpu");
    service->registerService();

    unsigned metal=1;
    PE_parse_boot_argn("native-adreno-metal",&metal,sizeof(metal));
    if(metal) {
        const IOReturn status=native_adreno_accelerator_publish(service,info?info->chip_id:0);
        service->setProperty("MetalStatus",uint32_t(status),32);
        service->setProperty("MetalAvailable",status==kIOReturnSuccess?kOSBooleanTrue:kOSBooleanFalse);
        if(status) {
            IOLog("NativeAdrenoA6xx: Metal accelerator publication failed %08x\n",status);
        }
    }
    return kIOReturnSuccess;
}
extern "C" uint32_t native_adreno_publish_stats(uint32_t *closed)
{
    if(closed)*closed=g.closed;
    return g.opened;
}

extern "C" {
extern volatile uint32_t native_adreno_method_stats[20][4];
extern volatile uint32_t native_adreno_client_stats[5][6];
extern volatile uint32_t native_adreno_pool_stats[8];
extern volatile uint32_t native_adreno_import_stage;
extern volatile uint64_t native_adreno_import_fail_pa,native_adreno_import_fail_offset;
extern volatile uint64_t native_adreno_vm_phase_us[2][8];
extern volatile uint64_t native_adreno_memory_stage_stats[10][4];
extern volatile uint64_t native_adreno_backing_cache_stats[12];
}

/* The USB worker requests one sample of discovery, client and submission
 * counters. Logging takes place after releasing the GPU connection lock. */
extern "C" void native_adreno_metal_dump_stats(void)
{
    const NativeAdrenoMetalDiag diag=native_adreno_metal_diag;
    IOLog("NativeAdrenoMetal: begin published=%u trace=%u rows_seq=%u\n",
        unsigned(g.published),native_adreno_trace_stage,native_adreno_metal_rows_seq);
    IOLog("NativeAdrenoMetal: opens=%u busy=%u bad=%u closes=%u grants=%u reports=%u property_reads=%u name_reads=%u foreign_opens=%u foreign_type=%08x\n",
        diag.opens,diag.busy,diag.bad,diag.closes,diag.grants,diag.reports,
        diag.property_reads,diag.name_reads,diag.foreign_opens,diag.foreign_type);
    IOLog("NativeAdrenoMetal: calls=%u selector=%u status=%08x inflight=%u since_calls=%u ext_entries=%u ext_selector=%u ext_status=%08x ext_in=%u ext_out=%u tunnel=%u probes=%u\n",
        diag.calls,diag.last_selector,diag.last_status,diag.inflight,diag.inflight_since_calls,
        diag.ext_entries,diag.ext_selector,diag.ext_status,diag.ext_in,diag.ext_out,
        diag.tunnel_calls,diag.probes);
    IOLog("NativeAdrenoMetal: attach=%u macf=%u resolver=%u type=%08x pass=%08x/%08x mailbox_maps=%u mailbox_calls=%u forced_unmaps=%u\n",
        diag.attach_status,diag.chk_macf,diag.chk_res,diag.chk_type,diag.type_pass_hi,
        diag.type_pass_lo,diag.mb_maps,diag.mb_calls,diag.forced_unmaps);
    IOLog("NativeAdrenoMetal: selectors=%08x %08x %08x %08x %08x %08x %08x %08x\n",
        diag.sel_bits[0],diag.sel_bits[1],diag.sel_bits[2],diag.sel_bits[3],
        diag.sel_bits[4],diag.sel_bits[5],diag.sel_bits[6],diag.sel_bits[7]);
    IOLog("NativeAdrenoMetal: name_reader=%.*s report=%.*s report2=%.*s\n",
        17,diag.name_reader,64,diag.report,64,diag.report2);
    for(unsigned index=0;index<3;++index) {
        IOLog("NativeAdrenoMetal: recent=%u opener=%.*s reader=%.*s starter=%.*s\n",
            index,17,diag.opener[index],17,diag.reader[index],17,diag.starter[index]);
    }
    const uint32_t markCount=diag.marks_total<16?diag.marks_total:16;
    for(uint32_t index=0;index<markCount;++index) {
        const uint32_t serial=diag.marks_total-markCount+index;
        IOLog("NativeAdrenoMetal: marker=%u value=%04x\n",serial,diag.marks[serial&15]);
    }

    for(unsigned index=0;index<12;++index) {
        const NativeAdrenoAskerRecord asker=native_adreno_metal_askers[index];
        if(asker.name[0]) {
            IOLog("NativeAdrenoMetal: discovery process=%.*s reads=%u\n",20,asker.name,asker.reads);
        }
    }
    for(unsigned index=0;index<8;++index) {
        const NativeAdrenoOpenRecord open=native_adreno_open_log[index];
        if(open.name[0]) {
            IOLog("NativeAdrenoMetal: open=%u process=%.*s type=%08x macf=%u resolver=%u attach=%u foreign=%u\n",
                index,20,open.name,open.type,open.macf,open.res,open.attach,open.foreign);
        }
        const NativeAdrenoClientRecord client=native_adreno_client_log[index];
        if(client.name[0]) {
            IOLog("NativeAdrenoMetal: client=%.*s opens=%u refused=%u closes=%u slot=%u peak_mib=%u last_refusal=%08x\n",
                20,client.name,client.opens,client.refused,client.closes,client.slot,
                client.peak_mib,client.last_refusal);
        }
    }
    for(unsigned index=0;index<4;++index) {
        const NativeAdrenoClientText client=native_adreno_client_text[index];
        if(client.name[0]) {
            IOLog("NativeAdrenoMetal: client_report process=%.*s text=%.*s\n",20,client.name,64,client.text);
        }
    }
    for(unsigned index=0;index<48;++index) {
        char row[96];
        memcpy(row,native_adreno_metal_rows[index],sizeof(row));
        if(row[0]) {
            IOLog("NativeAdrenoMetal: row=%u text=%.*s\n",index,96,row);
        }
    }

    IOLog("NativeAdrenoMetal: mount_reason=%u status=%08x slot=%u failures=%u views=%u new=%u reused=%u refused=%u last_refusal=%08x inside=%u kept=%u\n",
        native_adreno_mount_fail[0],native_adreno_mount_fail[1],native_adreno_mount_fail[2],native_adreno_mount_fail[3],
        native_adreno_view_diag[0],native_adreno_view_diag[1],native_adreno_view_diag[2],native_adreno_view_diag[3],
        native_adreno_view_diag[4],native_adreno_view_diag[5],native_adreno_view_kept);
    for(unsigned index=0;index<5;++index) {
        const volatile uint32_t *counts=native_adreno_client_stats[index];
        IOLog("NativeAdrenoMetal: bo_slot=%u live=%u peak=%u refused=%u status=%08x refused_bytes=%u mapped_mib=%u\n",
            index,counts[0],counts[1],counts[2],counts[3],counts[4],counts[5]);
    }
    IOLog("NativeAdrenoMetal: pool_mib=%u pool_bos=%u fallback_bos=%u used_mib=%u peak_mib=%u scattered_bos=%u scattered_spans=%u imported_spans=%u\n",
        native_adreno_pool_stats[0],native_adreno_pool_stats[1],native_adreno_pool_stats[2],native_adreno_pool_stats[3],
        native_adreno_pool_stats[4],native_adreno_pool_stats[5],native_adreno_pool_stats[6],native_adreno_pool_stats[7]);
    IOLog("NativeAdrenoMetal: import_stage=%u fail_pa=%llx fail_offset=%llx waits=%llu total_us=%llu pauses=%llu longest_us=%llu\n",
        native_adreno_import_stage,native_adreno_import_fail_pa,native_adreno_import_fail_offset,
        native_adreno_wait_outer[0],native_adreno_wait_outer[1],native_adreno_wait_outer[2],native_adreno_wait_outer[3]);
    for(unsigned method=0;method<20;++method) {
        const volatile uint32_t *counts=native_adreno_method_stats[method];
        if(counts[0]) {
            IOLog("NativeAdrenoMetal: method=%u calls=%u failures=%u status=%08x total_us=%u\n",
                method,counts[0],counts[1],counts[2],counts[3]);
        }
    }

    if(g.published && g.lock && IOLockTryLock(g.lock)) {
        uint64_t vmPhases[2][8],tablePhases[2][8],memoryStages[10][4],backingCache[12];
        for(unsigned row=0;row<2;++row) {
            for(unsigned slot=0;slot<8;++slot) {
                vmPhases[row][slot]=native_adreno_vm_phase_us[row][slot];
                tablePhases[row][slot]=arm_smmu_4k_live_phase_us[row][slot];
            }
        }
        for(unsigned row=0;row<10;++row) {
            for(unsigned slot=0;slot<4;++slot) {
                memoryStages[row][slot]=native_adreno_memory_stage_stats[row][slot];
            }
        }
        for(unsigned slot=0;slot<12;++slot) {
            backingCache[slot]=native_adreno_backing_cache_stats[slot];
        }
        const NativeAdrenoSubmitStats submit=g.submit->stats();
        static NativeAdrenoSchedulerSample scheduled[256];
        g.submit->scheduler()->copySamples(scheduled);
        NativeAdrenoAbiResourceBudget budgets[NativeAdrenoMaxClients];
        memcpy(budgets,resourceBudgets,sizeof(budgets));
        const size_t mappingCapacity=g.vm->objectCapacity(),mappingReserved=g.vm->reservedObjects();
        const uint64_t mappingGeneration=g.vm->budgetGeneration();
        const size_t objects=g.vm->objectCount(),spans=g.vm->spanCount(),jobs=g.submit->outstanding();
        const uint32_t submitted=g.ring->lastSubmitted(),completed=g.ring->completedFence();
        const uint32_t opened=g.opened,closed=g.closed;
        const uint64_t faults=g.status.faults(),suspends=g.status.suspends();
        const bool failed=g.failed,vmPoisoned=g.vm->poisoned(),submitPoisoned=g.submit->poisoned(),ringPoisoned=g.ring->poisoned();
        IOLockUnlock(g.lock);

        IOLog("NativeAdrenoMetal: mapping_budget capacity=%zu used=%zu reserved=%zu generation=%llu\n",
            mappingCapacity,objects,mappingReserved,mappingGeneration);
        for(unsigned at=0;at<NativeAdrenoMaxClients;++at) {
            const auto &budget=budgets[at];
            if(budget.version) {
                IOLog("NativeAdrenoMetal: mapping_client slot=%u used=%u target=%u active=%u reason=%u status=%08x handle=%llx iova=%llx\n",
                    at,budget.connection_used,budget.connection_target,budget.active_connections,
                    budget.last_failure_reason,budget.last_failure_status,budget.last_failure_handle,budget.last_failure_iova);
            }
        }

        for(unsigned row=0;row<2;++row) {
            const uint64_t *phase=vmPhases[row];
            IOLog("NativeAdrenoMetal: vm_%s calls=%llu metadata_us=%llu cache_us=%llu tables_us=%llu index_us=%llu %s_us=%llu max_us=%llu bytes=%llu\n",
                row?"unmap":"map",phase[0],phase[1],phase[2],phase[3],phase[4],
                row?"release":"preflight",phase[5],phase[6],phase[7]);
            phase=tablePhases[row];
            IOLog("NativeAdrenoMetal: pte_%s calls=%llu validation_us=%llu old_leaf_us=%llu publish_us=%llu tlb_us=%llu new_leaf_us=%llu max_us=%llu pages=%llu\n",
                row?"unmap":"map",phase[0],phase[1],phase[2],phase[3],phase[4],phase[5],phase[6],phase[7]);
        }
        static const char *memoryNames[]={"allocation","prepare","spans","zero","initial_clean",
            "set_wc","reassert_wc","restore_attributes","complete","descriptor_release"};
        for(unsigned row=0;row<10;++row) {
            const uint64_t *phase=memoryStages[row];
            IOLog("NativeAdrenoMetal: memory_stage=%s calls=%llu total_us=%llu max_us=%llu bytes=%llu\n",
                memoryNames[row],phase[0],phase[1],phase[2],phase[3]);
        }
        IOLog("NativeAdrenoMetal: backing_cache hits=%llu misses=%llu puts=%llu evictions=%llu rejected=%llu objects=%llu bytes=%llu peak_bytes=%llu pressure=%llu allocation_retry=%llu reset_pages=%llu quarantined=%llu\n",
            backingCache[0],backingCache[1],backingCache[2],backingCache[3],backingCache[4],backingCache[5],
            backingCache[6],backingCache[7],backingCache[8],backingCache[9],backingCache[10],backingCache[11]);
        IOLog("NativeAdrenoMetal: opened=%u closed=%u failed=%u faults=%llu suspends=%llu objects=%zu spans=%zu jobs=%zu poisoned_vm=%u poisoned_submit=%u poisoned_ring=%u\n",
            opened,closed,unsigned(failed),faults,suspends,objects,spans,jobs,
            unsigned(vmPoisoned),unsigned(submitPoisoned),unsigned(ringPoisoned));
        IOLog("NativeAdrenoMetal: submissions=%u completed=%u timeouts=%u poisoned_jobs=%u held=%zu retired=%zu peak_jobs=%zu submitted_fence=%u completed_fence=%u\n",
            submit.submissions,submit.completed,submit.timeouts,submit.poisoned_jobs,
            submit.references_held,submit.references_retired,submit.peak_jobs,submitted,completed);
        for(const NativeAdrenoSchedulerSample &sample:scheduled) {
            if(!sample.completion) {
                continue;
            }
            IOLog("NativeAdrenoAsync: completion=%llu queue=%u hardware=%u accepted=%llu issued=%llu completed=%llu status=%08x\n",
                sample.completion,sample.queue,sample.hardware,sample.acceptedUs,
                sample.issuedUs,sample.completedUs,sample.status);
            IOLog("NativeAdrenoAsync: ranges completion=%llu resources=%u alias_queries=%llu alias_hits=%llu waits=%u signals=%u gpu_dependencies=%u dpu_readers=%u pages_walked=0\n",
                sample.completion,sample.resources,sample.aliasQueries,sample.aliasHits,
                sample.waits,sample.signals,sample.gpuDependencies,sample.dpuReaders);
        }
    } else {
        IOLog("NativeAdrenoMetal: submission_sample=%s\n",g.published?"lock_busy":"not_published");
    }
    dumpBoProfiles();
    native_adreno_profile_dump();
    /* native_legacy_frame_profile_dump is declared weak_import in the upstream
     * A642L tree, which a static Mach-O kernel link cannot leave undefined, and no
     * object here defines it. The hook is dropped rather than stubbed so the dump
     * path claims only what this kernel can actually report. */
    IOLog("NativeAdrenoMetal: end\n");
}

extern "C" void native_adreno_fault(void)
{
    if(!g.published || !g.lock) {
        return;
    }
    IOLockLock(g.lock);
    if(!g.failed) {
        g.failed=true;
        g.status.noteFault();
        g.ring->recordFault();
    }
    g.submit->scheduler()->fault(kIOReturnIOError,false);
    IOLockWakeup(g.lock,&g.completionEvent,false);
    IOLockUnlock(g.lock);
    thread_call_enter(g.completionCall);
}

extern "C" void native_adreno_dma_stopped(void)
{
    if(!g.published || !g.lock) {
        return;
    }
    IOLockLock(g.lock);
    g.submit->releaseAfterDmaStopped();
    IOLockWakeup(g.lock,&g.completionEvent,false);
    IOLockUnlock(g.lock);
    thread_call_enter(g.completionCall);
}

extern "C" void native_adreno_gpu_interrupt(bool failed)
{
    if(!g.published || !g.completionCall) {
        return;
    }
    if(failed) {
        __atomic_store_n(&g.interruptFailure,1,__ATOMIC_RELEASE);
    }
    thread_call_enter(g.completionCall);
}
extern "C" int native_adreno_progress(uint32_t *submitted,uint32_t *completed)
{
    if(!g.published || !g.lock || !submitted || !completed) {
        return 1;
    }
    IOLockLock(g.lock);
    IOReturn status=g.failed?kIOReturnNotReady:kIOReturnSuccess;
    *submitted=g.ring->lastSubmitted();
    uint32_t observed=0;
    if(!status && *submitted) {
        const NativeAdrenoRingFence result=g.ring->observeFence(*submitted,nullptr,0,&observed);
        if(result==NativeAdrenoRingFencePoisoned) {
            status=kIOReturnIOError;
        }
    }
    *completed=observed;
    IOLockUnlock(g.lock);
    return status?1:0;
}
extern "C" IOService *native_adreno_published_service()
{
    return g.service;
}

extern "C" IOReturn native_adreno_surface_acquire_read(IOMemoryDescriptor *memory,
    void (*callback)(void *,IOReturn),void *context,NativeAdrenoSurfaceRead **out)
{
    if(!out) {
        return kIOReturnBadArgument;
    }
    *out=nullptr;
    if(!g.published) {
        return kIOReturnSuccess;
    }
    IOLockLock(g.lock);
    const IOReturn status=g.failed?kIOReturnNotReady:
        g.submit->scheduler()->acquireRead(memory,callback,context,out);
    IOLockUnlock(g.lock);
    return status;
}

extern "C" IOReturn native_adreno_surface_read_status(NativeAdrenoSurfaceRead *read,
    uint32_t *latestHardware)
{
    if(!read) {
        if(latestHardware) {
            *latestHardware=0;
        }
        return kIOReturnSuccess;
    }
    IOLockLock(g.lock);
    const IOReturn status=g.submit->scheduler()->readStatus(read,latestHardware);
    IOLockUnlock(g.lock);
    return status;
}

extern "C" void native_adreno_surface_release_read(NativeAdrenoSurfaceRead *read)
{
    if(!read) {
        return;
    }
    IOLockLock(g.lock);
    NativeAdrenoScheduler *scheduler=g.submit->scheduler();
    scheduler->cancelRead(read);
    while(scheduler->callbackRunning(read)) {
        IOLockSleep(g.lock,read,THREAD_UNINT);
    }
    scheduler->releaseRead(read);
    IOLockWakeup(g.lock,&g.completionEvent,false);
    IOLockUnlock(g.lock);
}

static IOReturn pollSurfaceProducerLocked(uint32_t fence)
{
    if(g.failed || g.vm->poisoned() || g.submit->poisoned() || g.ring->poisoned()) {
        return kIOReturnIOError;
    }

    if(fence) {
        const NativeAdrenoRingFence observed=g.ring->wait(fence,nullptr,0,nullptr);
        if(observed==NativeAdrenoRingFenceTimeout) {
            return kIOReturnTimeout;
        }
        if(observed!=NativeAdrenoRingFenceComplete) {
            return kIOReturnIOError;
        }
        const IOReturn status=g.submit->retireCompleted();
        if(status) {
            return status;
        }
    }

    __asm__ volatile("dsb sy" ::: "memory");
    return kIOReturnSuccess;
}

extern "C" IOReturn native_adreno_capture_surface_producer(uint32_t *captured_fence)
{
    if(!captured_fence) {
        return kIOReturnBadArgument;
    }
    *captured_fence=0;
    if(!g.published) {
        return kIOReturnSuccess;
    }
    if(!g.lock) {
        return kIOReturnNotReady;
    }
    if(!IOLockTryLock(g.lock)) {
        return kIOReturnTimeout;
    }

    IOReturn status=kIOReturnSuccess;
    if(g.failed || g.vm->poisoned() || g.submit->poisoned() || g.ring->poisoned()) {
        status=kIOReturnIOError;
    } else {
        *captured_fence=g.ring->lastSubmitted();
    }
    IOLockUnlock(g.lock);
    return status;
}

extern "C" IOReturn native_adreno_poll_surface_producer(uint32_t captured_fence)
{
    if(!g.published) {
        return captured_fence?kIOReturnNotReady:kIOReturnSuccess;
    }
    if(!g.lock) {
        return kIOReturnNotReady;
    }
    if(!IOLockTryLock(g.lock)) {
        return kIOReturnTimeout;
    }

    const IOReturn status=pollSurfaceProducerLocked(captured_fence);
    IOLockUnlock(g.lock);
    return status;
}

extern "C" IOReturn native_adreno_surface_profile_capture(IOMemoryDescriptor *memory,
    uint32_t captured_fence, uint64_t composition_start_continuous_us,
    NativeAdrenoSurfaceProfile *profile)
{
    if (!memory || !profile) {
        return kIOReturnBadArgument;
    }
    *profile = {};
    if (!g.published || !g.lock) {
        return kIOReturnNotReady;
    }
    if (!IOLockTryLock(g.lock)) {
        return kIOReturnTimeout;
    }

    g.submit->captureSurfaceProfile(memory, captured_fence,
        composition_start_continuous_us, profile);
    IOLockUnlock(g.lock);
    return kIOReturnSuccess;
}

extern "C" IOReturn native_adreno_wait_surface_producer(uint32_t timeout_us,
    uint32_t *captured_fence)
{
    if(captured_fence) {
        *captured_fence=0;
    }
    if(!g.published) {
        return kIOReturnSuccess;
    }
    if(!g.lock || !g.clock.now_us) {
        return kIOReturnNotReady;
    }

    uint64_t deadline=0;
    clock_interval_to_deadline(timeout_us,kMicrosecondScale,&deadline);
    IOLockLock(g.lock);
    const uint32_t fence=g.ring->lastSubmitted();
    if(captured_fence) {
        *captured_fence=fence;
    }
    for(;;) {
        const IOReturn status=pollSurfaceProducerLocked(fence);
        if(status!=kIOReturnTimeout || !timeout_us) {
            IOLockUnlock(g.lock);
            return status;
        }
        const int awakened=IOLockSleepDeadline(g.lock,&g.completionEvent,deadline,THREAD_ABORTSAFE);
        if(awakened!=THREAD_AWAKENED) {
            IOLockUnlock(g.lock);
            return awakened==THREAD_TIMED_OUT?kIOReturnTimeout:kIOReturnAborted;
        }
    }
}
