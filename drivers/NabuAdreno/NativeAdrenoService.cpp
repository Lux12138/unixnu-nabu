/* SPDX-License-Identifier: MIT */
/* nabu A640 service: the SM7325 Adreno6xx bring-up with the SM8150 board,
 * firmware from the board ADT, an automatic start after IOBSD (nabu-gpu=1)
 * and persisted phase checkpoints. Publication is the board's choice. */
#include "NativeAdrenoA6xxService.h"
#include "NativeAdrenoA6xxGmu.h"
#include "NativeAdrenoA6xxHwInit.h"
#include "NativeAdrenoA6xxZap.h"
#include "NativeAdrenoA6xxSmmu.h"
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoGpuProfile.h"
#include "NativeAdrenoCpInit.h"
#include "NativeAdrenoIrq.h"
#include "NativeAdrenoKernelTime.h"
#include "NabuSmmuPolicy.h"
#include "ArmSmmu4KMmio.h"
#include "AdrenoPM4.h"
#include "NativeAdrenoABI.h"
#include "NativeAdrenoSubmit.h"
#include "NativeAdrenoTimer.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern "C" uint64_t (*native_adreno_vm_now_us)(void);
extern "C" volatile uint32_t native_adreno_pool_mib;
#if defined(NABU_GPU_METAL) && NABU_GPU_METAL
/* Publication path. The diagnostic archive links none of these objects, so
 * they must not be referenced there. */
#include "NativeAdrenoIntegration.h"
extern "C" IOReturn nabu_gpu_scanout_display_call(uint32_t,task_t,const void *,const void *,size_t,void *,size_t);
extern "C" IOMemoryDescriptor *nabu_gpu_scanout_display_memory(task_t,const void *);
extern "C" void nabu_gpu_scanout_display_close(const void *);
extern "C" IOReturn native_adreno_initialize_classes(void);
extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *,NativeAdrenoSubmit *,NativeAdrenoRing *,
    const NativeAdrenoRingClock *,const NativeAdrenoTimer *,const NativeAdrenoAbiDeviceInfo *,uint32_t,uint32_t);
extern "C" void native_adreno_fault(void);
extern "C" void native_adreno_dma_stopped(void);
extern "C" void native_adreno_gpu_interrupt(bool failed);
extern "C" int native_adreno_progress(uint32_t *,uint32_t *);
/* Fixed diagnostic layouts exported by the pinned NativeAdrenoPublish.cpp.
 * The observation regression checks the complete field layout at both sites. */
extern "C" {
struct NativeAdrenoMetalDiag {
    uint32_t opens, busy, bad, closes, reports, property_reads, foreign_opens, foreign_type, name_reads, calls, last_selector, last_status, ext_entries, ext_selector, ext_status, ext_in, ext_out, tunnel_calls, probes, sel_bits[8], mb_maps, mb_calls, marks_total, marks[16], chk_macf, chk_res, chk_type, type_pass_lo, type_pass_hi, attach_status, inflight, inflight_since_calls, forced_unmaps;
    char opener[3][17];
    char reader[3][17];
    char name_reader[17];
    char starter[3][17];
    char report[64];
    char report2[64];
    uint32_t grants;
};
struct NativeAdrenoOpenRecord {char name[20]; uint32_t type, macf, res, attach, foreign;};
struct NativeAdrenoClientText {char name[20]; char text[64];};
extern NativeAdrenoMetalDiag native_adreno_metal_diag;
extern NativeAdrenoOpenRecord native_adreno_open_log[8];
extern volatile uint32_t native_adreno_open_log_count;
extern NativeAdrenoClientText native_adreno_client_text[4];
/* The Metal plugin's telemetry rows (NativeAdrenoPublish.cpp marker stream). */
extern char native_adreno_metal_rows[48][96];
extern volatile uint32_t native_adreno_metal_rows_seq;
/* BoImport refusal record (NativeAdrenoUserClient.cpp) and importTask's stage (NativeAdrenoMemory.cpp). */
extern volatile uint64_t native_adreno_import_diag[10];
extern volatile uint32_t native_adreno_import_stage;
extern volatile uint64_t native_adreno_import_fail_pa,native_adreno_import_fail_offset;
extern volatile uint32_t native_adreno_method_stats[20][4];
extern void (*nabu_log_probe_poll)(void);
extern void (*nabu_log_probe_process_snapshot)(uint32_t *);
/* Secondary-core bring-up evidence written by the linked ArmCpu (sheng-smp=1) and the
 * scheduler's own counts; ArmCpu only stores these, so the ESP writer is their sole reader. */
extern volatile uint32_t armcpu_smp_requested,armcpu_smp_running,armcpu_smp_conduit,armcpu_boot_mpidr;
extern volatile uint32_t armcpu_smp_step[8],armcpu_smp_mpidr[8],armcpu_smp_seen_mpidr[8],armcpu_smp_fail[8];
extern volatile uint32_t armcpu_smp_sys[8][5],armcpu_core_timer[8],armcpu_core_sgi[8];
extern volatile int32_t armcpu_smp_psci_rc[8];
extern volatile uint64_t armcpu_ipi_interrupts,armcpu_ipi_sent;
extern volatile uint64_t nabu_scanout_offers,nabu_scanout_completed,nabu_scanout_fallbacks;
extern volatile uint64_t nabu_scanout_waits,nabu_scanout_timeouts,nabu_scanout_abandoned;
extern volatile uint64_t nabu_scanout_maps,nabu_scanout_map_hits;
extern volatile uint32_t nabu_scanout_ready,nabu_scanout_last_status,nabu_scanout_pending;
extern unsigned int real_ncpus;
extern uint32_t processor_avail_count;
unsigned int ml_get_cpu_count(void);
unsigned int ml_get_cluster_count(void);
}
extern "C" void nabu_capture_append(const char *,unsigned);
#endif
extern "C" {
volatile uint64_t native_adreno_fence_phys=0;
volatile uint32_t native_adreno_a6xx_stats[24];
}

namespace {
#if defined(NABU_GPU_METAL) && NABU_GPU_METAL
/* Called only by the existing ESP writer, outside its transport lock. It
 * reads bounded software state, never polls hardware or waits for g.lock. */
void nabuGpuProbeWriterSample()
{
    static unsigned samples;
    static uint64_t next;
    uint64_t current=0;
    if(samples>=64 || NativeAdrenoKernelTime::now_us(nullptr,&current))return;
    if(current<next || next==UINT64_MAX)return;
    next=current>=UINT64_MAX-10000000?UINT64_MAX:current+10000000;
    const unsigned sample=++samples;
    const NativeAdrenoMetalDiag diag=native_adreno_metal_diag;
    uint32_t process[6]={};
    void (*snapshot)(uint32_t *)=__atomic_load_n(&nabu_log_probe_process_snapshot,__ATOMIC_ACQUIRE);
    if(snapshot)snapshot(process);
    char line[256],report[64];
    auto emit=[&](int bytes) {
        if(bytes>0 && unsigned(bytes)<sizeof(line))nabu_capture_append(line,unsigned(bytes));
    };
    auto text=[&](const char *source) {
        for(unsigned i=0;i<sizeof(report)-1;++i) {
            const unsigned char ch=static_cast<unsigned char>(source[i]);
            report[i]=ch>=32 && ch<127?char(ch):'_';
            if(!ch){report[i]=0;break;}
        }
        report[sizeof(report)-1]=0;
    };
    emit(snprintf(line,sizeof(line),
        "NabuGPU: writer_probe=%u now_us=%llu opens=%u closes=%u calls=%u selector=%u status=%08x marks=%u snapshot=non-atomic render_proof=0\n",
        sample,static_cast<unsigned long long>(current),diag.opens,diag.closes,diag.calls,
        diag.last_selector,diag.last_status,diag.marks_total));
    emit(snprintf(line,sizeof(line),
        "NabuGPU: writer_exec=%u hook=%u attempts=%u returned=%u error=%u pid=%u exits=%u exit_status=%x\n",
        sample,snapshot?1u:0u,process[0],process[1],process[2],process[3],process[4],process[5]));
    text(diag.report);
    emit(snprintf(line,sizeof(line),"NabuGPU: writer_meta=%u opener=%.*s text=%s\n",
        sample,17,diag.opener[0],report));
    /* Every process's own text row: the probe's and any Metal plugin client's failure reason. */
    for(unsigned i=0;i<4;++i) {
        const NativeAdrenoClientText client=native_adreno_client_text[i];
        if(!client.name[0])continue;
        text(client.text);
        emit(snprintf(line,sizeof(line),"NabuGPU: writer_client=%u name=%.*s text=%s\n",
            sample,20,client.name,report));
    }
    /* The render server's plugin lines: its Turnip/status line and the processes that started the plugin. */
    text(diag.report2);
    emit(snprintf(line,sizeof(line),"NabuGPU: writer_report2=%u text=%s\n",sample,report));
    char starters[3][17];
    for(unsigned i=0;i<3;++i) {
        char name[sizeof(report)]={};
        memcpy(name,diag.starter[i],sizeof(diag.starter[i])-1);
        text(name);
        snprintf(starters[i],sizeof(starters[i]),"%.16s",report[0]?report:"-");
    }
    emit(snprintf(line,sizeof(line),"NabuGPU: writer_starters=%u a=%s b=%s c=%s\n",
        sample,starters[0],starters[1],starters[2]));
    emit(snprintf(line,sizeof(line),"NabuGPU: writer_rows=%u seq=%u\n",sample,native_adreno_metal_rows_seq));
    /* The first three non-empty rows, then rows 45..47 (the plugin's QuartzCore failures) whenever they are set. */
    for(unsigned row=0,shown=0;row<48;++row) {
        if(shown>=3 && row<45)continue;
        char cells[96];
        memcpy(cells,native_adreno_metal_rows[row],sizeof(cells));
        cells[sizeof(cells)-1]=0;
        if(!cells[0])continue;
        for(unsigned i=0;cells[i];++i)
            if(static_cast<unsigned char>(cells[i])<32 || static_cast<unsigned char>(cells[i])>=127)cells[i]='_';
        emit(snprintf(line,sizeof(line),"NabuGPU: writer_row=%u r=%u %s\n",sample,row,cells));
        ++shown;
    }
    {
        uint64_t import[10];
        for(unsigned i=0;i<10;++i)import[i]=native_adreno_import_diag[i];
        emit(snprintf(line,sizeof(line),
            "NabuGPU: writer_import=%u calls=%llu refused=%llu last=%x/%u/%llx/%llx first=%x/%u/%llx/%llx stage=%u fail_pa=%llx\n",
            sample,static_cast<unsigned long long>(import[0]),static_cast<unsigned long long>(import[1]),
            static_cast<unsigned>(import[2]),static_cast<unsigned>(import[3]),static_cast<unsigned long long>(import[4]),
            static_cast<unsigned long long>(import[5]),static_cast<unsigned>(import[6]),static_cast<unsigned>(import[7]),
            static_cast<unsigned long long>(import[8]),static_cast<unsigned long long>(import[9]),
            native_adreno_import_stage,static_cast<unsigned long long>(native_adreno_import_fail_pa)));
    }
    emit(snprintf(line,sizeof(line),
        "NabuGPU: writer_marks=%u total=%u last=%x/%x/%x/%x macf=%x resolver=%x attach=%u ext=%u/%08x\n",
        sample,diag.marks_total,diag.marks[(diag.marks_total-1)&15],diag.marks[(diag.marks_total-2)&15],
        diag.marks[(diag.marks_total-3)&15],diag.marks[(diag.marks_total-4)&15],
        diag.chk_macf,diag.chk_res,diag.attach_status,diag.ext_entries,diag.ext_status));
    /* Which system processes open the accelerator, with which type, and which
     * sandbox gate refuses them: the open log the disabled IOLog observation
     * would print, read here without changing any admission decision. */
    emit(snprintf(line,sizeof(line),"NabuGPU: writer_opens=%u count=%u foreign=%u foreign_type=%08x reads=%u\n",
        sample,native_adreno_open_log_count,diag.foreign_opens,diag.foreign_type,diag.name_reads));
    for(unsigned index=0;index<8;++index) {
        const NativeAdrenoOpenRecord open=native_adreno_open_log[index];
        if(!open.name[0])continue;
        emit(snprintf(line,sizeof(line),
            "NabuGPU: writer_open=%u slot=%u process=%.*s type=%08x macf=%x resolver=%x attach=%u foreign=%u\n",
            sample,index,20,open.name,open.type,open.macf,open.res,open.attach,open.foreign));
    }
    emit(snprintf(line,sizeof(line),
        "NabuGPU: writer_scanout=%u ready=%u pending=%u offered=%llu done=%llu fallback=%llu status=%x\n",
        sample,nabu_scanout_ready,nabu_scanout_pending,
        static_cast<unsigned long long>(nabu_scanout_offers),static_cast<unsigned long long>(nabu_scanout_completed),
        static_cast<unsigned long long>(nabu_scanout_fallbacks),nabu_scanout_last_status));
    emit(snprintf(line,sizeof(line),
        "NabuGPU: writer_scanout_wait=%u calls=%llu timeout=%llu abandoned=%llu maps=%llu hits=%llu\n",
        sample,static_cast<unsigned long long>(nabu_scanout_waits),static_cast<unsigned long long>(nabu_scanout_timeouts),
        static_cast<unsigned long long>(nabu_scanout_abandoned),static_cast<unsigned long long>(nabu_scanout_maps),
        static_cast<unsigned long long>(nabu_scanout_map_hits)));
    /* Which cores ArmCpu started and the scheduler counts them in: slot 0 (boot core)
     * always, a secondary slot once ArmCpu has touched it. */
    emit(snprintf(line,sizeof(line),
        "NabuGPU: writer_smp=%u req=%u run=%u conduit=%u boot=%x ncpus=%u avail=%u topo=%u clusters=%u ipi=%llu sent=%llu\n",
        sample,armcpu_smp_requested,armcpu_smp_running,armcpu_smp_conduit,armcpu_boot_mpidr,
        real_ncpus,processor_avail_count,ml_get_cpu_count(),ml_get_cluster_count(),
        static_cast<unsigned long long>(armcpu_ipi_interrupts),static_cast<unsigned long long>(armcpu_ipi_sent)));
    for(unsigned slot=0;slot<8;++slot) {
        const uint32_t step=armcpu_smp_step[slot],fail=armcpu_smp_fail[slot],seen=armcpu_smp_seen_mpidr[slot];
        const int32_t psci=armcpu_smp_psci_rc[slot];
        if(slot && !step && !fail && !seen && !psci)continue;
        emit(snprintf(line,sizeof(line),
            "NabuGPU: writer_cpu=%u slot=%u step=%u mpidr=%x seen=%x fail=%x psci=%d el=%x sctlr=%x timer=%u sgi=%u\n",
            sample,slot,step,armcpu_smp_mpidr[slot],seen,fail,psci,armcpu_smp_sys[slot][0],
            armcpu_smp_sys[slot][1],armcpu_core_timer[slot],armcpu_core_sgi[slot]));
    }
}
#endif
#if defined(NABU_GPU_METAL) && NABU_GPU_METAL
/* Snapshot only fixed software counters: the full upstream dump can print
 * thousands of history rows and is unsuitable for the 10 ms hardware worker. */
class NabuMetalObservation final {
    bool enabled_=false;
    unsigned next_=0;
    uint64_t since_=0;
    bool probeEnabled_=false;
    unsigned probeSamples_=0;
    uint64_t probeNext_=0;
    void probeSample(uint64_t current,uint32_t submitted,uint32_t completed)
    {
        if(!probeEnabled_ || probeSamples_>=32 || current<since_)return;
        const uint64_t elapsed=current-since_;
        if(elapsed<probeNext_ || probeNext_==UINT64_MAX)return;
        probeNext_=elapsed>=UINT64_MAX-10000000?UINT64_MAX:elapsed+10000000;
        const unsigned sample=++probeSamples_;
        const NativeAdrenoMetalDiag diag=native_adreno_metal_diag;
        char line[256],report[sizeof(diag.report)];
        for(unsigned i=0;i<sizeof(report);++i) {
            const unsigned char ch=static_cast<unsigned char>(diag.report[i]);
            report[i]=ch>=32 && ch<127?char(ch):'_';
            if(!ch){report[i]=0;break;}
        }
        report[sizeof(report)-1]=0;
        int bytes=snprintf(line,sizeof(line),
            "NabuGPU: userprobe_tick=%u since_ready_us=%llu opens=%u closes=%u calls=%u selector=%u status=%08x inflight=%u submitted=%u completed=%u snapshot=non-atomic render_proof=0\n",
            sample,static_cast<unsigned long long>(elapsed),diag.opens,diag.closes,diag.calls,diag.last_selector,diag.last_status,
            diag.inflight,submitted,completed);
        if(bytes>0 && unsigned(bytes)<sizeof(line))nabu_capture_append(line,unsigned(bytes));
        bytes=snprintf(line,sizeof(line),"NabuGPU: userprobe_report=%u opener=%.*s text=%s\n",
            sample,17,diag.opener[0],report);
        if(bytes>0 && unsigned(bytes)<sizeof(line))nabu_capture_append(line,unsigned(bytes));
    }
public:
    void start(bool enabled,uint64_t current)
    {
        enabled_=enabled;next_=0;since_=current;
        probeEnabled_=false;probeSamples_=0;probeNext_=0;
        if(enabled_) {
            IOLog("NabuGPU: Metal observations enabled slots=4 at_seconds=0,10,30,120\n");
        }
    }
    void enableProbe(bool enabled) {probeEnabled_=enabled;}
    void sample(uint64_t current,uint32_t submitted,uint32_t completed,int progress)
    {
        /* The explicit user probe gets at most 32 pairs of GPU software-state
         * records through the existing RAM ring, independent of IOLog console
         * routing. This neither adds a thread nor changes the GPU verdict. */
        probeSample(current,submitted,completed);
        static const unsigned seconds[4]={0,10,30,120};
        if(!enabled_ || next_>=4 || current<since_)return;
        const uint64_t elapsed=current-since_;
        if(elapsed<uint64_t(seconds[next_])*1000000)return;
        const unsigned slot=++next_;
        /* A delayed worker emits one current snapshot, not a burst of stale
         * catch-up samples for every missed deadline. */
        while(next_<4 && elapsed>=uint64_t(seconds[next_])*1000000)++next_;
        const NativeAdrenoMetalDiag diag=native_adreno_metal_diag;
        IOLog("NabuGPU: observation=%u/4 since_ready_us=%llu snapshot=non-atomic render_proof=0 submitted=%u completed=%u progress=%08x\n",
            slot,elapsed,submitted,completed,uint32_t(progress));
        IOLog("NabuGPU: metal_opens=%u busy=%u bad=%u closes=%u discovery_reads=%u foreign_opens=%u foreign_type=%08x\n",
            diag.opens,diag.busy,diag.bad,diag.closes,diag.name_reads,diag.foreign_opens,diag.foreign_type);
        IOLog("NabuGPU: metal_calls=%u selector=%u status=%08x inflight=%u ext_entries=%u ext_selector=%u ext_status=%08x\n",
            diag.calls,diag.last_selector,diag.last_status,diag.inflight,
            diag.ext_entries,diag.ext_selector,diag.ext_status);
        for(unsigned index=0;index<8;++index) {
            const NativeAdrenoOpenRecord open=native_adreno_open_log[index];
            if(open.name[0]) {
                IOLog("NabuGPU: metal_open_slot=%u process=%.*s type=%08x macf=%u resolver=%u attach=%u foreign=%u\n",
                    index,20,open.name,open.type,open.macf,open.res,open.attach,open.foreign);
            }
        }
        /* These upstream counters cover methods 1..19 only. Newer selectors
         * remain visible in the latest selector/status and raw fence fields. */
        for(unsigned method=1;method<20;++method) {
            const volatile uint32_t *counts=native_adreno_method_stats[method];
            const uint32_t calls=counts[0],failures=counts[1],status=counts[2];
            if(calls) {
                IOLog("NabuGPU: metal_method=%u calls=%u failures=%u status=%08x\n",
                    method,calls,failures,status);
            }
        }
        IOLog("NabuGPU: observation_end=%u/4 remaining_slots=%u\n",slot,4-next_);
    }
};
#endif
void nabuGpuState(uint32_t step,uint32_t status,const char *name)
{
    IOLog("NabuGPU: state=%s step=%u status=%08x\n",name,step,status);
}
/* A640State owns all mappings,
 * contexts and DMA allocations for this boot. A failed attempt retains them
 * while the GMU and CP are stopped. The CPU framebuffer continues
 * independently of whether this object publishes a GPU. */
class A640State final {
    OSSharedPtr<IOMemoryMap> maps_[NativeAdrenoA6xxDomains];
    OSSharedPtr<IOMemoryMap> zapMap_;
    OSSharedPtr<IOMemoryMap> smmuMap_;
    NativeAdrenoA6xxResources resources_{};
    NativeAdrenoA6xxWindow windows_[NativeAdrenoA6xxDomains]{};
    NativeAdrenoA6xxPower power_;
    NativeAdrenoA6xxGmu gmu_;
    NativeAdrenoA6xxReport failedPowerReport_{},failedGmuReport_{};
    bool failureCaptured_=false;
    NativeAdrenoA6xxZap zap_;
    NativeAdrenoA6xxSmmu smmuProvider_;
    NativeAdrenoGmuContext gmuContext_;
    NativeAdrenoGpuContext gpuContext_;
    NativeAdrenoGmuContextReport gmuContextReport_{};
    NativeAdrenoGmuContextReport gpuContextReport_{};
    NativeAdrenoRing ring_;
#if defined(NABU_GPU_METAL) && NABU_GPU_METAL
    NativeAdrenoSubmit submit_;
    NativeAdrenoTimer timer_;
    NativeAdrenoAbiDeviceInfo info_{};
    NabuMetalObservation observation_;
    uint32_t watchedFence_=0;
    uint64_t watchedSince_=0;
#endif
    NativeAdrenoIrq irq_;
    ArmSmmu4KMmio smmuMmio_{};
    ArmSmmu4KIo smmuIo_{};
    NativeAdrenoRingIo ringIo_{};
    NativeAdrenoRingClock clock_{};
    bool cpTouched_=false;
    bool cpTrace_=false;
    bool ready_=false;
    bool irqAttached_=false;
    bool hardwareIrqEnabled_=false;
    uint32_t hardwareIrqFailed_=0;
    unsigned gpuSlots_[4]{},gmuSlots_[2]{};

    static int read(void *,uint32_t,uint32_t,uint32_t *);
    static int write(void *,uint32_t,uint32_t,uint32_t);
    static void delay(void *,uint32_t);
    static int gpuRead(void *,uint32_t,uint32_t *);
    static int gpuWrite(void *,uint32_t,uint32_t);
    static int writeWptr(void *,uint32_t);
    static int readGpuBusy(void *,uint32_t *);
    static int readAlwaysOn(void *, uint64_t *);
    static IOReturn acquireProfileAccess(void *);
    static IOReturn releaseProfileAccess(void *);
    static bool profileAccessible(void *);
    static int barrier(void *);
    static int publishRing(void *,const volatile uint32_t *,size_t);
    static int acquireFence(void *,volatile uint32_t *,size_t);
    static uint64_t now(void *);
    static int idle(void *);
    static void gpuInterrupt(void *);
    IOReturn observeInitialization();
    IOReturn checkGpuTranslations();
    IOReturn mapWindows();
    IOReturn contexts();
    IOReturn reserveSmmuResources();
    IOReturn startCp();
    IOReturn fail(IOReturn);
public:
    IOReturn start();
    void poll();
    void dump() const;
};

A640State *state=nullptr;
uint32_t launched=0;
uint32_t hardwareLaunched=0;

OSSharedPtr<IOMemoryMap> mapWindow(const NativeAdrenoA6xxWindow &window,
    IOOptionBits cacheMode=kIOMapInhibitCache)
{
    if(!window.physical || !window.bytes || window.bytes>SIZE_MAX ||
        window.physical>UINT64_MAX-window.bytes) {
        return {};
    }
    IODeviceMemory *memory=IODeviceMemory::withRange(window.physical,window.bytes);
    if(!memory) {
        return {};
    }
    OSSharedPtr<IOMemoryMap> map(memory->map(kIOMapAnywhere|cacheMode),OSNoRetain);
    memory->release();
    return map;
}

int A640State::read(void *cookie,uint32_t domain,uint32_t offset,uint32_t *value)
{
    A640State *self=static_cast<A640State *>(cookie);
    if(!self || !value || domain>=NativeAdrenoA6xxDomains || (offset&3) ||
        !self->maps_[domain] || uint64_t(offset)+4>self->windows_[domain].bytes) {
        return 1;
    }
    const uintptr_t base=self->maps_[domain]->getVirtualAddress();
    if(self->cpTrace_) {
        IOLog("NativeAdrenoCpMmio: read begin domain=%u offset=%x reg=%x\n",domain,offset,offset/4);
    }
    *value=*reinterpret_cast<volatile uint32_t *>(base+offset);
    __asm__ volatile("dsb ld" ::: "memory");
    if(self->cpTrace_) {
        IOLog("NativeAdrenoCpMmio: read end domain=%u offset=%x value=%08x\n",domain,offset,*value);
    }
    return 0;
}

int A640State::write(void *cookie,uint32_t domain,uint32_t offset,uint32_t value)
{
    A640State *self=static_cast<A640State *>(cookie);
    if(!self || domain>=NativeAdrenoA6xxDomains || (offset&3) ||
        !self->maps_[domain] || uint64_t(offset)+4>self->windows_[domain].bytes) {
        return 1;
    }
    const uintptr_t base=self->maps_[domain]->getVirtualAddress();
    if(self->cpTrace_) {
        IOLog("NativeAdrenoCpMmio: write begin domain=%u offset=%x reg=%x value=%08x\n",domain,offset,offset/4,value);
    }
    __asm__ volatile("dsb st" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(base+offset)=value;
    if(self->cpTrace_) {
        IOLog("NativeAdrenoCpMmio: write end domain=%u offset=%x\n",domain,offset);
    }
    return 0;
}

void A640State::delay(void *,uint32_t microseconds)
{
    IODelay(microseconds);
}

int A640State::gpuRead(void *cookie,uint32_t reg,uint32_t *value)
{
    return reg>UINT32_MAX/4?1:read(cookie,NativeAdrenoA6xxGpu,reg*4,value);
}

int A640State::gpuWrite(void *cookie,uint32_t reg,uint32_t value)
{
    return reg>UINT32_MAX/4?1:write(cookie,NativeAdrenoA6xxGpu,reg*4,value);
}

int A640State::writeWptr(void *cookie,uint32_t value)
{
    auto *self=static_cast<A640State *>(cookie);
    if(self->cpTrace_) {
        IOLog("NativeAdrenoCp: WPTR publication begin value=%x\n",value);
    }

    for(unsigned attempt=0;attempt<200;++attempt) {
        if(gpuWrite(cookie,0x807,value)) {
            return 1;
        }

        if(self->cpTrace_ && !attempt) {
            IOLog("NativeAdrenoCp: WPTR store completed; synchronizing\n");
        }

        __asm__ volatile("dsb sy" ::: "memory");

        uint32_t fence=0;
        if(read(cookie,NativeAdrenoA6xxGmuRegisters,0x23b13*4-0x6a000,&fence)) {
            return 1;
        }

        if(!(fence&1)) {
            if(self->cpTrace_) {
                IOLog("NativeAdrenoCp: WPTR acknowledged attempts=%u fence=%08x\n",attempt+1,fence);
            }

            return 0;
        }

        IODelay(10);
    }

    IOLog("NativeAdrenoCp: WPTR acknowledgement timed out value=%x\n",value);
    return 1;
}

int A640State::readGpuBusy(void *cookie,uint32_t *value)
{
    return read(cookie,NativeAdrenoA6xxGmuRegisters,0x23b0c*4-0x6a000,value);
}

bool A640State::profileAccessible(void *cookie)
{
    uint32_t gx=0,fence=0;
    return !read(cookie,NativeAdrenoA6xxGmuRegisters,0x1f8d0*4-0x6a000,&gx) &&
        !read(cookie,NativeAdrenoA6xxGmuRegisters,0x23b10*4-0x6a000,&fence) &&
        !(gx&0xc0) && !fence;
}

int A640State::readAlwaysOn(void *cookie, uint64_t *ticks)
{
    if (!ticks) {
        return 1;
    }

    // Samsung a6xx_read_alwayson uses GMU AO for CPU AHB reads on this target.
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        uint32_t before = 0;
        uint32_t low = 0;
        uint32_t after = 0;
        if (read(cookie, NativeAdrenoA6xxGmuRegisters, 0x1f889 * 4 - 0x6a000, &before) ||
            read(cookie, NativeAdrenoA6xxGmuRegisters, 0x1f888 * 4 - 0x6a000, &low) ||
            read(cookie, NativeAdrenoA6xxGmuRegisters, 0x1f889 * 4 - 0x6a000, &after)) {
            return 1;
        }
        if (before == after) {
            *ticks = (uint64_t(after) << 32) | low;
            return 0;
        }
    }

    return 1;
}

IOReturn A640State::acquireProfileAccess(void *cookie)
{
    return static_cast<A640State *>(cookie)->gmu_.acquirePerformanceCounters();
}

IOReturn A640State::releaseProfileAccess(void *cookie)
{
    return static_cast<A640State *>(cookie)->gmu_.releasePerformanceCounters();
}

int A640State::barrier(void *)
{
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

int A640State::publishRing(void *cookie,const volatile uint32_t *,size_t bytes)
{
    A640State *self=static_cast<A640State *>(cookie);
    if(self->cpTrace_) {
        IOLog("NativeAdrenoCp: ring publication begin bytes=%lx\n",bytes);
    }
    const IOReturn status=self->gpuContext_.buffer(1).publishForDevice(0,bytes);
    if(self->cpTrace_) {
        IOLog("NativeAdrenoCp: ring publication end status=%08x\n",status);
    }
    return status?1:0;
}

int A640State::acquireFence(void *cookie,volatile uint32_t *,size_t bytes)
{
    A640State *self=static_cast<A640State *>(cookie);
    return self->gpuContext_.buffer(2).acquireDeviceWrites(0,bytes)?1:0;
}

uint64_t A640State::now(void *)
{
    uint64_t value=0;
    NativeAdrenoKernelTime::now_us(nullptr,&value);
    return value;
}

int A640State::idle(void *cookie)
{
    IODelay(5);
    return static_cast<A640State *>(cookie)->observeInitialization()?1:0;
}

IOReturn A640State::observeInitialization()
{
    IOReturn status=smmuProvider_.observe();
    if(!status) {
        status=gmu_.poll();
    }
    if(status || !irqAttached_ || hardwareIrqEnabled_) {
        return status;
    }
    NativeAdrenoIrqResult result{};
    status=irq_.handle(&result);
    if(status || (result.flags&NativeAdrenoIrqFatal)) {
        IOLog("NativeAdrenoCp: initialization fault status=%08x rbbm=%08x cp=%08x protect=%08x flags=%08x\n",
            status,result.status,result.cp_status,result.protect_status,result.flags);
        return status?status:kIOReturnIOError;
    }
    return kIOReturnSuccess;
}

void A640State::gpuInterrupt(void *cookie)
{
    A640State *self=static_cast<A640State *>(cookie);
    NativeAdrenoIrqResult result{};
    const IOReturn status=self->irq_.handle(&result);
    native_adreno_a6xx_stats[11]=result.status;
    native_adreno_a6xx_stats[12]=result.cp_status;
    native_adreno_a6xx_stats[13]=result.protect_status;
    const bool failed=status || (result.flags&NativeAdrenoIrqFatal);
    if(failed) {
        __atomic_store_n(&self->hardwareIrqFailed_,1,__ATOMIC_RELEASE);
        native_adreno_smmu_disable_irq(self->resources_.gpuInterrupt);
    }
#if defined(NABU_GPU_METAL) && NABU_GPU_METAL
    if(failed || (result.flags&NativeAdrenoIrqFence)) {
        /* Fence completion and faults drive the published scheduler; this is
         * an explicit no-op before a publish. */
        native_adreno_gpu_interrupt(failed);
    }
#else
    if(failed || (result.flags&NativeAdrenoIrqFence)) {
        if(failed)self->ring_.recordFault();
    }
#endif
}

IOReturn A640State::checkGpuTranslations()
{
    const uint64_t addresses[]={GpuSqeIova,GpuRingIova,GpuMemptrsIova,GpuPwrupIova,GpuScratchIova};
    const uint16_t sids[]={0,1,0x400,0x401};
    for(unsigned stream=0;stream<4;++stream) {
        const uint16_t sid=sids[stream];
        for(unsigned index=0;index<GpuBufferCount;++index) {
            const NativeAdrenoMemory &buffer=gpuContext_.buffer(index);
            if(buffer.spanCount()!=1) {
                return kIOReturnUnsupported;
            }
            const bool privileged=false;
            const bool instruction=index==0;
            const bool writes=index>=2;
            for(size_t offset=0;offset<buffer.bytes();offset+=4096) {
                const IOReturn status=smmuProvider_.checkTranslation(resources_.gpuBank,sid,
                    addresses[index]+offset,buffer.spans()[0].physical+offset,
                    privileged,instruction,writes);
                if(status) {
                    return status;
                }
            }
        }
    }
    IOLog("NativeAdrenoSmmu: all four GPU streams in one bank translated every bootstrap page\n");
    return smmuProvider_.observe();
}

IOReturn A640State::mapWindows()
{
    /* NABU-A640-RESOURCES: the expected values in this file came from another board
     * (gmem 0x100000 / 257 MHz / gpuSidMask 0x401 / gmuSid 5). Our device tree already carries the
     * right windows, clocks and SIDs, so a hard equality here turns every machine difference into
     * a build instead of a boot. Keep only "the device tree was read at all", print everything,
     * and let the later stages report what is actually wrong. */
    {
        const IOReturn read = native_adreno_a6xx_resources(&resources_);
        IOLog("NativeAdrenoA6xx: resources rc=%d chip=%08x gmem=%x freq=%u "
              "gpuSid=%u/%x gmuSid=%u/%x gpuAsid=%u gmuAsid=%u fw=%d%d%d%d "
              "win gpu=%llx/%llx gmu=%llx/%llx pdc=%llx pdcSeq=%llx zap=%llx\n",
              (int) read, (unsigned) resources_.chipId, (unsigned) resources_.gmemBytes,
              (unsigned) resources_.frequencyHz,
              (unsigned) resources_.gpuSid, (unsigned) resources_.gpuSidMask,
              (unsigned) resources_.gmuSid, (unsigned) resources_.gmuSidMask,
              (unsigned) resources_.gpuAsid, (unsigned) resources_.gmuAsid,
              resources_.gmuFirmware ? 1 : 0, resources_.sqeFirmware ? 1 : 0,
              resources_.zapMetadata ? 1 : 0, resources_.zapShader ? 1 : 0,
              (unsigned long long) resources_.gpu.physical, (unsigned long long) resources_.gpu.bytes,
              (unsigned long long) resources_.gmu.physical, (unsigned long long) resources_.gmu.bytes,
              (unsigned long long) resources_.pdc.physical,
              (unsigned long long) resources_.pdcSeq.physical,
              (unsigned long long) resources_.zap.physical);
        if (read || !resources_.chipId) {
            IOLog("NativeAdrenoA6xx: resources unusable (rc=%d chip=%08x)\n",
                  (int) read, (unsigned) resources_.chipId);
            return kIOReturnBadArgument;
        }
    }
    const NativeAdrenoA6xxWindow supplied[]={
        resources_.gpu,resources_.gmu,resources_.gpucc,resources_.gcc,
        resources_.pdc,resources_.rscc,resources_.commandDb,resources_.rpmh,
        resources_.aop,resources_.aopDoorbell,resources_.pdcSeq
    };
    for(unsigned index=0;index<NativeAdrenoA6xxDomains;++index) {
        windows_[index]=supplied[index];
        maps_[index]=mapWindow(windows_[index]);
        if(!maps_[index]) {
            return kIOReturnNoMemory;
        }
    }
    // The ZAP reservation is DDR loaded through Normal NC, matching PIL's ioremap_wc.
    zapMap_=mapWindow(resources_.zap,kIOMapWriteCombineCache);
    if(!zapMap_) {
        return kIOReturnNoMemory;
    }
    return kIOReturnSuccess;
}

IOReturn A640State::reserveSmmuResources()
{
    uint32_t id0=0,id1=0;
    if(smmuIo_.read32(smmuIo_.cookie,0x20,&id0) ||
        smmuIo_.read32(smmuIo_.cookie,0x24,&id1)) {
        return kIOReturnIOError;
    }
    const unsigned slotCount=id0&255,bankCount=id1&255,firstStage1=(id1>>16)&255;
    const size_t stride=size_t(1)<<((id1>>31)?16:12);
    const size_t pages=size_t(1)<<(((id1>>28)&7)+1);
    const bool extended=id0&(1u<<8);
    if(!(id0&(1u<<27)) || slotCount<6 || slotCount>128 || bankCount>64 ||
        firstStage1>=bankCount || 2*pages*stride>smmuIo_.register_bytes) {
        IOLog("NativeAdrenoSmmu: allocation unsupported id=%08x/%08x slots=%u stage1=%u..%u\n",
            id0,id1,slotCount,firstStage1,bankCount);
        return kIOReturnUnsupported;
    }
    bool freeSlots[128]={},reservedSlots[128]={},referencedBanks[64]={},freeBanks[64]={};
    const uint16_t requestedSids[6]={0,1,0x400,0x401,5,0x405};
    const uint32_t sidBits=(id0>>9)&15;
    const uint32_t sidMask=extended?0xffffu:((1u<<sidBits)-1);
    for(unsigned slot=0;slot<slotCount;++slot) {
        uint32_t smr=0,s2cr=0;
        if(smmuIo_.read32(smmuIo_.cookie,0x800+4*slot,&smr) ||
            smmuIo_.read32(smmuIo_.cookie,0xc00+4*slot,&s2cr)) {
            return kIOReturnIOError;
        }
        const bool valid=extended?bool(s2cr&(1u<<10)):bool(smr&(1u<<31));
        const unsigned type=(s2cr>>16)&3,bank=s2cr&255;
        freeSlots[slot]=!valid && type==2;
        if(valid && !type && bank<64) {
            referencedBanks[bank]=true;
        }
        if(valid) {
            const uint32_t mask=(smr>>16)&(extended?0xffffu:0x7fffu);
            for(uint16_t sid:requestedSids) {
                if(!((sid^(smr&0xffff))&~mask&sidMask)) {
                    IOLog("NativeAdrenoSmmu: allocation conflicts slot=%u smr=%08x s2cr=%08x sid=%04x\n",
                        slot,smr,s2cr,sid);
                    return kIOReturnBusy;
                }
            }
        }
    }
    for(unsigned bank=firstStage1;bank<bankCount;++bank) {
        const size_t offset=(pages+bank)*stride;
        uint32_t sctlr=0;
        if(offset>smmuIo_.register_bytes || smmuIo_.register_bytes-offset<0x7f8 ||
            smmuIo_.read32(smmuIo_.cookie,offset,&sctlr)) {
            return kIOReturnIOError;
        }
        freeBanks[bank]=!(sctlr&1) && !referencedBanks[bank];
    }

    unsigned selectedSlots[6]={UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    for(unsigned index=0;index<6;++index) {
        for(unsigned slot=0;slot<slotCount;++slot) {
            if(freeSlots[slot] && !reservedSlots[slot]) {
                selectedSlots[index]=slot;
                reservedSlots[slot]=true;
                break;
            }
        }
        if(selectedSlots[index]==UINT32_MAX)return kIOReturnNoResources;
    }
    unsigned selectedBanks[2]={UINT32_MAX,UINT32_MAX};
    const unsigned preferredBanks[2]={resources_.gpuBank,resources_.gmuBank};
    for(unsigned role=0;role<2;++role) {
        const unsigned bank=preferredBanks[role];
        if(bank>=firstStage1 && bank<bankCount && freeBanks[bank]) {
            selectedBanks[role]=bank;
            freeBanks[bank]=false;
        }
    }
    for(unsigned role=0;role<2;++role) {
        if(selectedBanks[role]!=UINT32_MAX)continue;
        for(unsigned bank=firstStage1;bank<bankCount;++bank) {
            if(freeBanks[bank]) {
                selectedBanks[role]=bank;freeBanks[bank]=false;break;
            }
        }
        if(selectedBanks[role]==UINT32_MAX)return kIOReturnNoResources;
    }
    IOLog("NativeAdrenoSmmu: allocated GPU cb=%u slots=%u/%u/%u/%u GMU cb=%u slots=%u/%u\n",
        selectedBanks[0],selectedSlots[0],selectedSlots[1],selectedSlots[2],selectedSlots[3],
        selectedBanks[1],selectedSlots[4],selectedSlots[5]);
    for(unsigned slot:selectedSlots) {
        uint32_t smr=0,before=0,after=0;
        if(smmuIo_.read32(smmuIo_.cookie,0x800+4*slot,&smr) ||
            smmuIo_.read32(smmuIo_.cookie,0xc00+4*slot,&before))return kIOReturnIOError;
        const bool valid=extended?bool(before&(1u<<10)):bool(smr&(1u<<31));
        /* NABU-A640-TAKEOVER: the old test was `valid || stage2!=2` -> Busy. On this board the two GMU
         * slots arrive with S2CR 0x3c1edaff / 0x232ec2ff: bit10 already set (so `valid` was true) and a
         * type field of 0x3c, which is not translation/bypass/fault in the ARM encoding at all -- a
         * leftover the firmware never initialised. Returning Busy there meant the FAULT write below
         * never ran, so the S2CR never changed and scan() later failed the same way (status 10).
         * A stage-2-disabled stream is still left alone; a stage-2-enabled one with an unencodable
         * type is ours to reclaim, and that is exactly what the write below does. */
        if(((before>>16)&3)!=2)return kIOReturnBusy;
        if(valid)IOLog("NativeAdrenoSmmu: taking over slot=%u before=%08x type=%02x\n",
            slot,before,(unsigned)(before>>24));
        /* NABU-A640-NO-STAGE2: with no stage-2 bank the S2CR write is accepted and discarded
         * (measured). Skipping it keeps the loop honest instead of failing on a register the
         * hardware does not implement; the stream is matched through SMR + stage-1. */
        uint32_t smmuId1=0;
        if(smmuIo_.read32(smmuIo_.cookie,0x24,&smmuId1))return kIOReturnIOError;
        if(((smmuId1>>16)&255)==0) {
            IOLog("NativeAdrenoSmmu: no stage-2 banks (id1=%08x); stream matched via SMR+stage-1, "
                  "S2CR left as-is\n",smmuId1);
            continue;
        }
        const uint32_t faultRoute=(2u<<16)|(3u<<8);
        if(smmuIo_.write32(smmuIo_.cookie,0xc00+4*slot,faultRoute) ||
            smmuIo_.barrier(smmuIo_.cookie) ||
            smmuIo_.read32(smmuIo_.cookie,0xc00+4*slot,&after)) {
            IOLog("NativeAdrenoSmmu: fault route I/O failed slot=%u before=%08x requested=%08x\n",
                slot,before,faultRoute);
            return kIOReturnIOError;
        }
        const uint32_t compareMask=NabuSmmuS2crMask(id0,faultRoute);
        IOLog("NativeAdrenoSmmu: fault route slot=%u before=%08x requested=%08x actual=%08x mask=%08x\n",
            slot,before,faultRoute,after,compareMask);
        /* NABU-A640-WRITEPROBE: S2CR came back unchanged. Before deciding "writes are ignored",
         * write SMR in the same stream block and read it back -- one writable and one not splits
         * "the whole block is unwritable" (secure/power) from "only S2CR is" (stage-2 semantics,
         * and this instance reports s2banks=0). SMR is restored immediately; this only runs on the
         * failure path, never on a successful route. */
        if(after==before) {
            const uint32_t wantSmr=(extended?0u:0x80000000u)|0x5u;
            uint32_t smrProbe=0;
            (void)smmuIo_.write32(smmuIo_.cookie,0x800+4*slot,wantSmr);
            (void)smmuIo_.barrier(smmuIo_.cookie);
            (void)smmuIo_.read32(smmuIo_.cookie,0x800+4*slot,&smrProbe);
            (void)smmuIo_.write32(smmuIo_.cookie,0x800+4*slot,smr);
            (void)smmuIo_.barrier(smmuIo_.cookie);
            IOLog("NativeAdrenoSmmu: write probe slot=%u smr old=%08x wrote=%08x read=%08x | s2cr wrote=%08x read=%08x\n",
                slot,smr,wantSmr,smrProbe,faultRoute,after);
        }
        uint32_t smrAfter=0;
        if(!NabuSmmuS2crMatches(id0,after,faultRoute) ||
            smmuIo_.read32(smmuIo_.cookie,0x800+4*slot,&smrAfter) ||
            (extended?bool(after&(1u<<10)):bool(smrAfter&(1u<<31)))) {
            IOLog("NativeAdrenoSmmu: fault route rejected slot=%u smr=%08x s2cr=%08x\n",
                slot,smrAfter,after);
            return kIOReturnIOError;
        }
    }
    resources_.gpuBank=selectedBanks[0];resources_.gmuBank=selectedBanks[1];
    for(unsigned index=0;index<4;++index)gpuSlots_[index]=selectedSlots[index];
    for(unsigned index=0;index<2;++index)gmuSlots_[index]=selectedSlots[index+4];
    return kIOReturnSuccess;
}

IOReturn A640State::contexts()
{
    smmuMap_=mapWindow(resources_.smmu);
    if(!smmuMap_) {
        return kIOReturnNoMemory;
    }
    /* The context callbacks retain the mapped register pointer for BO updates. */
    smmuMmio_={reinterpret_cast<volatile unsigned char *>(smmuMap_->getVirtualAddress()),
        size_t(resources_.smmu.bytes)};
    if(ArmSmmu4KBindMmio(&smmuMmio_,&smmuIo_)!=ArmSmmu4KSuccess) {
        return kIOReturnBadArgument;
    }
    constexpr uint32_t cm3ResetOffset=0x1f800*4-0x6a000;
    uint32_t resetBefore=0,resetAfter=0;
    if(read(this,NativeAdrenoA6xxGmuRegisters,cm3ResetOffset,&resetBefore) ||
        write(this,NativeAdrenoA6xxGmuRegisters,cm3ResetOffset,1)) {
        IOLog("NativeAdrenoSmmu: GMU CM3 reset access failed\n");
        return kIOReturnIOError;
    }
    IODelay(1);
    if(read(this,NativeAdrenoA6xxGmuRegisters,cm3ResetOffset,&resetAfter) || !(resetAfter&1)) {
        IOLog("NativeAdrenoSmmu: GMU CM3 reset did not assert before=%08x after=%08x\n",resetBefore,resetAfter);
        return kIOReturnIOError;
    }
    IOLog("NativeAdrenoSmmu: GMU CM3 held reset before=%08x after=%08x\n",resetBefore,resetAfter);
    NativeAdrenoSnapshotSmmu(&smmuIo_,"before-platform-setup");

    IOReturn status=reserveSmmuResources();
    if(status) {
        return status;
    }
    status=smmuProvider_.prepare(resources_,smmuIo_);
    if(status) {
        return status;
    }
    NativeAdrenoGmuContextReport &report=gmuContextReport_;
    status=gmuContext_.bringUp(&smmuIo_,resources_.gmuBank,resources_.gmuAsid,
        gmuSlots_[0],uint16_t(resources_.gmuSid),nullptr,&report,
        uint16_t(resources_.gmuSid|resources_.gmuSidMask),gmuSlots_[1]);
    native_adreno_a6xx_stats[4]=report.step;
    native_adreno_a6xx_stats[5]=report.mismatch;
    NativeAdrenoLogSmmuReport("gmu",&report);
    if(status) {
        if(report.program_writes) {
            NativeAdrenoSnapshotSmmu(&smmuIo_,"after-gmu-failure");
        }
        return status;
    }
    const uint16_t sids[4]={0,1,0x400,0x401};
    status=gpuContext_.bringUp(&smmuIo_,resources_.gpuBank,resources_.gpuAsid,gpuSlots_,sids,nullptr,&gpuContextReport_);
    native_adreno_a6xx_stats[6]=gpuContextReport_.step;
    native_adreno_a6xx_stats[7]=gpuContextReport_.program_status;
    NativeAdrenoLogSmmuReport("gpu",&gpuContextReport_);
    if(status)return status;
    return kIOReturnSuccess;
}

IOReturn A640State::startCp()
{
    ringIo_={this,gpuWrite,gpuRead,barrier,writeWptr,readGpuBusy,readAlwaysOn};
    IOLog("NativeAdrenoCp: checking GMU keepalive and full AHB fence before GX registers\n");
    IOReturn status=gmu_.keepAlive(true);
    if(status) {
        return status;
    }
    cpTrace_=true;
    uint32_t gxStatus=0,fenceControl=0,gxDomain=0,spClock=0;
    if(read(this,NativeAdrenoA6xxGmuRegisters,0x1f8d0*4-0x6a000,&gxStatus) ||
        read(this,NativeAdrenoA6xxGmuRegisters,0x23b10*4-0x6a000,&fenceControl) ||
        read(this,NativeAdrenoA6xxGpuCc,0x100c,&gxDomain) ||
        read(this,NativeAdrenoA6xxGmuRegisters,0x1a880*4-0x6a000,&spClock)) {
        return kIOReturnIOError;
    }
    IOLog("NativeAdrenoCp: GX power/clock=%08x AHB fence=%08x GX GDSCR=%08x SP clock=%08x\n",
        gxStatus,fenceControl,gxDomain,spClock);
    if((gxStatus&0xc0) || fenceControl || !(gxDomain&(1u<<31))) {
        return kIOReturnNotReady;
    }
    cpTouched_=true;
    IOLog("NativeAdrenoCp: core register initialization begin with SP clock retained\n");
    status=NativeAdrenoA6xxHwInit::program(&ringIo_,resources_.highestBankBit);
    if(status) {
        return status;
    }

    IOLog("NativeAdrenoCp: GMU hardware clock controls begin\n");
    const uint32_t clockRegisters[][2]={
        {0x23b09,0x20202},{0x23b0a,0x10111},{0x23b0b,0x5555},{0x1f802,0}
    };
    for(const auto &entry:clockRegisters) {
        if(write(this,NativeAdrenoA6xxGmuRegisters,entry[0]*4-0x6a000,entry[1])) {
            return kIOReturnIOError;
        }
    }

    uint32_t topClock=0;
    if(gpuRead(this,0xae,&topClock)) {
        return kIOReturnIOError;
    }

    IOLog("NativeAdrenoCp: inherited RBBM_CLOCK_CNTL=%08x\n",topClock);
    if(topClock!=0x8aa8aa82) {
        if(read(this,NativeAdrenoA6xxGmuRegisters,0x1a880*4-0x6a000,&spClock) ||
            write(this,NativeAdrenoA6xxGmuRegisters,0x1a880*4-0x6a000,spClock&~1u)) {
            return kIOReturnIOError;
        }

        status=NativeAdrenoA6xxHwInit::programClockGating(&ringIo_);
        if(status) {
            return status;
        }

        if(read(this,NativeAdrenoA6xxGmuRegisters,0x1a880*4-0x6a000,&spClock) ||
            write(this,NativeAdrenoA6xxGmuRegisters,0x1a880*4-0x6a000,spClock|1) ||
            gpuWrite(this,0xae,0x8aa8aa82)) {
            return kIOReturnIOError;
        }
    } else {
        IOLog("NativeAdrenoCp: hardware clock gating already configured\n");
    }

    status=NativeAdrenoA6xxHwInit::finish(&ringIo_);
    if(status) {
        return status;
    }

    IOLog("NativeAdrenoCp: checking TBU translations before SQE start\n");
    status=checkGpuTranslations();
    if(status) {
        return status;
    }
    status=irq_.attach(&ringIo_,NativeAdrenoA640);
    if(!status) {
        irqAttached_=true;
        status=irq_.programMask(nullptr);
    }
    if(status) {
        return status;
    }

    const uint8_t *const native_adreno_sqe_firmware_image=resources_.sqeFirmware;
    const uint64_t native_adreno_sqe_firmware_image_size=resources_.sqeFirmwareBytes;
    if(native_adreno_sqe_firmware_image_size<=4 ||
        native_adreno_sqe_firmware_image_size-4>gpuContext_.buffer(0).bytes()) {
        return kIOReturnBadArgument;
    }
    IOLog("NativeAdrenoCp: SQE copy begin cpu=%p physical=%llx iova=%llx bytes=%llx\n",
        gpuContext_.buffer(0).mapping(),gpuContext_.buffer(0).spans()[0].physical,
        uint64_t(GpuSqeIova),native_adreno_sqe_firmware_image_size-4);
    memcpy(gpuContext_.buffer(0).mapping(),native_adreno_sqe_firmware_image+4,
        size_t(native_adreno_sqe_firmware_image_size-4));
    IOLog("NativeAdrenoCp: SQE copy completed\n");
    /* The empty register-list header is valid with IFPC held disabled. */
    memset(gpuContext_.buffer(3).mapping(),0,GpuPageBytes);
    memset(gpuContext_.buffer(4).mapping(),0,GpuPageBytes);
    IOLog("NativeAdrenoCp: SQE publication begin\n");
    status=gpuContext_.buffer(0).publishForDevice(0,gpuContext_.buffer(0).bytes());
    IOLog("NativeAdrenoCp: SQE publication completed status=%08x\n",status);
    if(status) {
        return status;
    }
    __asm__ volatile("dsb sy" ::: "memory");

    uint32_t inheritedRptr=0,inheritedWptr=0,inheritedSqe=0;
    if(gpuRead(this,0x806,&inheritedRptr) ||
        gpuRead(this,0x807,&inheritedWptr) ||
        gpuRead(this,0x808,&inheritedSqe)) {
        return kIOReturnIOError;
    }
    IOLog("NativeAdrenoCp: inherited ring rptr=%08x wptr=%08x sqe=%08x\n",
        inheritedRptr,inheritedWptr,inheritedSqe);
    if(inheritedRptr || inheritedWptr) {
        return kIOReturnNotReady;
    }

    NativeAdrenoRingCache cache{this,publishRing,acquireFence};
    IOLog("NativeAdrenoCp: ring attach begin ring=%llx fence=%llx\n",uint64_t(GpuRingIova),uint64_t(GpuMemptrsIova));
    status=ring_.attach(&ringIo_,&cache,gpuContext_.buffer(1).mapping(),GpuRingBytes,GpuRingIova,
        gpuContext_.buffer(2).mapping(),GpuPageBytes,GpuMemptrsIova,NativeAdrenoA640);
    IOLog("NativeAdrenoCp: ring attach completed status=%08x\n",status);
    if(status) {
        return status;
    }
    native_adreno_fence_phys=gpuContext_.buffer(2).spans()[0].physical;
    NativeAdrenoCpInit cp;
    NativeAdrenoCpInitConfig config{GpuSqeIova,GpuPwrupIova,0,NativeAdrenoA640};
    uint32_t bootstrapWptr=0;
    IOLog("NativeAdrenoCp: SQE start and CP_ME_INIT submit begin\n");
    cpTrace_=false;
    status=cp.runBootstrap(&ringIo_,&ring_,&config,&bootstrapWptr);
    cpTrace_=true;
    IOLog("NativeAdrenoCp: CP_ME_INIT submit completed status=%08x wptr=%u\n",status,bootstrapWptr);
    clock_={this,now,idle};
    cpTrace_=false;
    uint32_t observedRptr=0;
    IOLog("NativeAdrenoCp: CP_ME_INIT idle wait begin\n");
    if(!status && ring_.waitBootstrap(bootstrapWptr,&clock_,2000000,&observedRptr)!=NativeAdrenoRingFenceComplete) {
        status=kIOReturnTimeout;
    }
    cpTrace_=true;
    IOLog("NativeAdrenoCp: CP_ME_INIT idle wait completed status=%08x rptr=%u/%u\n",status,observedRptr,bootstrapWptr);
    if(status) {
        return status;
    }

    if(native_adreno_checkpoint("cp-init-done-zap-next"))return kIOReturnNotReady;
    IOLog("NativeAdrenoA6xx: CP init completed, authenticating PAS %u shader\n",resources_.zapPasId);
    status=zap_.load(resources_,reinterpret_cast<void *>(zapMap_->getVirtualAddress()));
    IOLog("NativeAdrenoCp: PAS shader startup completed status=%08x phase=%u SCM=%lld/%lld\n",
        status,zap_.phase(),zap_.transport(),zap_.result());
    native_adreno_a6xx_stats[16]=zap_.phase();
    native_adreno_a6xx_stats[17]=uint32_t(zap_.transport());
    native_adreno_a6xx_stats[18]=uint32_t(zap_.result());
    if(status) {
        return status;
    }

    uint32_t secureMode[2]={0};
    if(AdrenoPM4Type7(0x66,1,&secureMode[0])!=AdrenoPM4Success) {
        return kIOReturnBadArgument;
    }

    IOLog("NativeAdrenoCp: CP_SET_SECURE_MODE submit begin\n");
    status=ring_.submitBootstrap(secureMode,2,&bootstrapWptr);
    cpTrace_=false;
    if(!status && ring_.waitBootstrap(bootstrapWptr,&clock_,2000000,&observedRptr)!=NativeAdrenoRingFenceComplete) {
        status=kIOReturnTimeout;
    }
    cpTrace_=true;
    IOLog("NativeAdrenoCp: secure mode exit completed status=%08x rptr=%u/%u\n",status,observedRptr,bootstrapWptr);
    if(status) {
        return status;
    }

    uint32_t words[4]={0};
    if(AdrenoPM4Type7(0x3d,3,&words[0])!=AdrenoPM4Success) {
        return kIOReturnBadArgument;
    }

    words[1]=uint32_t(GpuScratchIova);
    words[2]=uint32_t(GpuScratchIova>>32);
    words[3]=0x6401cafe;
    uint32_t fence=0;
    IOLog("NativeAdrenoCp: scratch write submit begin\n");
    const uint32_t scratchBefore=*static_cast<volatile uint32_t *>(gpuContext_.buffer(4).mapping());
    const uint32_t fenceBefore=static_cast<volatile uint32_t *>(gpuContext_.buffer(2).mapping())[1];
    if(scratchBefore!=0 || fenceBefore!=0 || ring_.completedFence()!=0)return kIOReturnIOError;
    IOLog("NabuGPU: pre-submit scratch=%08x fence=%u expected=6401cafe\n",scratchBefore,fenceBefore);
    status=ring_.submit(words,4,&fence);
    IOLog("NativeAdrenoCp: scratch submit completed status=%08x fence=%u\n",status,fence);
    cpTrace_=false;
    if(!status && ring_.wait(fence,&clock_,20000,nullptr)!=NativeAdrenoRingFenceComplete) {
        status=kIOReturnTimeout;
    }
    cpTrace_=true;
    IOLog("NativeAdrenoCp: scratch fence wait completed status=%08x observed=%u\n",status,ring_.completedFence());
    native_adreno_a6xx_stats[8]=fence;
    native_adreno_a6xx_stats[9]=ring_.completedFence();
    if(!status) {
        status=gpuContext_.buffer(4).acquireDeviceWrites(0,GpuPageBytes);
    }
    native_adreno_a6xx_stats[10]=*static_cast<volatile uint32_t *>(gpuContext_.buffer(4).mapping());
    if(!status && native_adreno_a6xx_stats[10]!=0x6401cafe) {
        status=kIOReturnIOError;
    }
    if(status) {
        return status;
    }

    IOLog("NabuGPU: proof=cp-scratch-fence chip=%08x scratch=%08x fence=%u/%u gmem=100000/100000\n",
        resources_.chipId,native_adreno_a6xx_stats[10],fence,ring_.completedFence());
#if defined(NABU_GPU_METAL) && NABU_GPU_METAL
    /* Publish only after the live VM, ring, submit and timer are attached and
     * the core is closed. A failure must leave the service unpublished, so it
     * returns into the fail path instead of being logged and ignored. */
    const IOReturn registered=native_adreno_initialize_classes();
    if(registered && registered!=kIOReturnBusy) {
        IOLog("NativeAdrenoA6xx: publication class registration failed %08x\n",registered);
        return registered;
    }
    status=submit_.attach(gpuContext_.vm(),&ring_);
    if(status) {
        /* NABU-A640-PUBLISH-L576: stage marker -- the publication block runs on every boot once
         * NABU_GPU_METAL is compiled in, and without this line a failure here looks like silence. */
        IOLog("NativeAdrenoA6xx: publication step=submit-attach status=%08x\n",status);
        return status;
    }
    const NativeAdrenoTimeIo time{this,gpuRead};
    status=timer_.attach(&time,NativeAdrenoTimeCp);
    if(status) {
        IOLog("NativeAdrenoA6xx: publication step=timer-attach status=%08x\n",status);
        return status;
    }
    info_={};
    info_.chip_id=resources_.chipId;          /* ADT qcom,chipid 0x06040001 */
    info_.gmem_bytes=resources_.gmemBytes;    /* ADT 0x100000 */
    info_.gmem_base=0x100000;                 /* A640 HwInit 0xE0B/0xE0D route 1..2 MiB */
    info_.uche_trap_base=0x1fffffffff000;     /* A640 HwInit 0xE09/0xE0A */
    info_.va_start=0x20000000;                /* client VA contract, not A640-verified */
    info_.va_size=0x60000000;
    info_.highest_bank_bit=resources_.highestBankBit;
    info_.ubwc_swizzle=6;                     /* UBWC 3.0 keeps bank swizzle levels 2 and 3. */
    info_.macrotile_mode=0;                   /* 4 channels; 8-channel mode starts at UBWC 3.1. */
    info_.ubwc_version=resources_.ubwcMode;   /* ADT qcom,ubwc-mode 3 */
    info_.flags=NativeAdrenoDeviceClientIova; /* import capabilities come from the VM */
    const NativeAdrenoIntegration callbacks={nabu_gpu_scanout_display_call,nabu_gpu_scanout_display_memory,
        nullptr,nabu_gpu_scanout_display_close};
    native_adreno_set_integration(&callbacks);
    const IOReturn published=native_adreno_publish(gpuContext_.vm(),&submit_,&ring_,&clock_,
        &timer_,&info_,32,gpuContextReport_.output_bits);
    if(published) {
        IOLog("NativeAdrenoA6xx: publish failed %08x\n",published);
        return published;
    }
    /* A zero return only means the service registered; the accelerator result is
     * reported separately through MetalStatus/MetalAvailable. */
    IOLog("NativeAdrenoA6xx: service published chip=%08x input_bits=32 output_bits=%u\n",
        info_.chip_id,gpuContextReport_.output_bits);
#endif
    return kIOReturnSuccess;
}

IOReturn A640State::fail(IOReturn status)
{
    NativeAdrenoGpuProfile::fault();
    if(hardwareIrqEnabled_) {
        native_adreno_smmu_disable_irq(resources_.gpuInterrupt);
        hardwareIrqEnabled_=false;
    }
    smmuProvider_.observe();
    smmuProvider_.disableInterrupts();
    native_adreno_a6xx_stats[1]=uint32_t(status);
    nabuGpuState(native_adreno_a6xx_stats[0],uint32_t(status),"failed");
    failedPowerReport_=power_.report();
    failedGmuReport_=gmu_.report();
    failureCaptured_=true;
    ring_.recordFault();
#if defined(NABU_GPU_METAL) && NABU_GPU_METAL
    /* No-op before a publish; afterwards it marks the published core failed so
     * a mounted client cannot keep opening it. */
    native_adreno_fault();
#endif
    native_adreno_checkpoint("failed");
    bool stopped=true;
    if(cpTouched_) {
        stopped=NativeAdrenoA6xxHwInit::stop(&ringIo_,1000)==kIOReturnSuccess;
    }
    if (stopped && gmu_.running()) {
        const IOReturn releaseStatus = gmu_.releasePerformanceCounters();
        if (releaseStatus) {
            IOLog("NativeAdrenoGpuProfile: kind=error phase=fault_release status=%08x\n",
                uint32_t(releaseStatus));
        }
    }
    const IOReturn gmuStopped=native_adreno_a6xx_stats[0]>=4?gmu_.stop():kIOReturnNotReady;
    if (!gmuStopped) {
        NativeAdrenoGpuProfile::hardwareStopped();
    }
    native_adreno_a6xx_stats[20]=stopped;
    native_adreno_a6xx_stats[21]=uint32_t(gmuStopped);
    if(stopped && !gmuStopped) {
#if defined(NABU_GPU_METAL) && NABU_GPU_METAL
        native_adreno_dma_stopped();
#endif
        power_.stop(true);
    }
    dump();
    return status;
}

IOReturn A640State::start()
{
    native_adreno_a6xx_stats[0]=1;
    nabuGpuState(1,0,"starting");
    if(native_adreno_checkpoint("map"))return fail(kIOReturnNotReady);
    IOLog("NativeAdrenoA6xx: mapping SM8150 board resources\n");
    IOReturn status=mapWindows();
    if(status) {
        return fail(status);
    }
    native_adreno_a6xx_stats[0]=2;
    nabuGpuState(2,0,"starting");
    if(native_adreno_checkpoint("power-cx"))return fail(kIOReturnNotReady);
    IOLog("NativeAdrenoA6xx: enabling CX clocks and parent votes\n");
    const NativeAdrenoA6xxIo io{this,read,write,delay};
    status=power_.enableCx(io,resources_.aopDoorbellOffset,resources_.aopDoorbellValue);
    if(status) {
        return fail(status);
    }
    native_adreno_a6xx_stats[0]=3;
    nabuGpuState(3,0,"starting");
    if(native_adreno_checkpoint("smmu-contexts"))return fail(kIOReturnNotReady);
    IOLog("NativeAdrenoA6xx: programming GMU and GPU SMMU contexts\n");
    status=contexts();
    if(status) {
        return fail(status);
    }
    NativeAdrenoGmuAllocator &memory=gmuContext_.allocator();
    const auto &layout=memory.layout();
    NativeAdrenoA6xxGmuConfig config{io,resources_.gmuFirmware,
        resources_.gmuFirmwareBytes,{
            {memory.icache().mapping(),layout.icache_iova,memory.icache().bytes()},
            {memory.dummy().mapping(),layout.dummy_iova,memory.dummy().bytes()},
            {memory.debug().mapping(),layout.debug_iova,memory.debug().bytes()},
            {memory.log().mapping(),layout.log_iova,memory.log().bytes()},
            {memory.hfi().mapping(),layout.hfi_iova,memory.hfi().bytes()}},&power_.tables(),
        resources_.chipId};
    native_adreno_a6xx_stats[0]=4;
    nabuGpuState(4,0,"starting");
    if(native_adreno_checkpoint("gmu-start"))return fail(kIOReturnNotReady);
    IOLog("NativeAdrenoA6xx: loading A640 GMU and starting HFI\n");
    status=gmu_.start(config);
    if(!status) {
        status=smmuProvider_.observe();
    }
    if(status) {
        return fail(status);
    }
    native_adreno_a6xx_stats[0]=5;
    nabuGpuState(5,0,"starting");
    if(native_adreno_checkpoint("gx"))return fail(kIOReturnNotReady);
    IOLog("NativeAdrenoA6xx: requesting GX at 257 MHz\n");
    status=power_.enableGx();
    if(!status) {
        status=gmu_.keepAlive(true);
    }
    if(status) {
        return fail(status);
    }
    native_adreno_a6xx_stats[0]=6;
    nabuGpuState(6,0,"starting");
    if(native_adreno_checkpoint("cp"))return fail(kIOReturnNotReady);
    IOLog("NativeAdrenoA6xx: starting A640 CP and GPU memory write\n");
    status=startCp();
    cpTrace_=false;
    if(status) {
        return fail(status);
    }
    status=native_adreno_smmu_register_irq(resources_.gpuInterrupt,gpuInterrupt,this);
    if(!status) {
        hardwareIrqEnabled_=true;
        status=native_adreno_smmu_enable_irq(resources_.gpuInterrupt);
    }
    if(status) {
        return fail(status);
    }
    ready_=true;
#if defined(NABU_GPU_METAL) && NABU_GPU_METAL
    unsigned observe=0;
    PE_parse_boot_argn("nabu-gpu-observe",&observe,sizeof(observe));
    observation_.start(observe==1,now(nullptr));
    unsigned probe=0;
    PE_parse_boot_argn("nabu-gpu-userprobe",&probe,sizeof(probe));
    observation_.enableProbe(probe==1);
    if(probe==1)__atomic_store_n(&nabu_log_probe_poll,nabuGpuProbeWriterSample,__ATOMIC_RELEASE);
#endif
    native_adreno_a6xx_stats[0]=7;
    nabuGpuState(7,0,"ready");
    dump();
    native_adreno_checkpoint("ready");
    return kIOReturnSuccess;
}

void A640State::poll()
{
    if(!ready_) {
        return;
    }

    native_adreno_poll_irqs();
    IOReturn gmuStatus=smmuProvider_.observe();
    if(!gmuStatus) {
        gmuStatus=gmu_.poll();
    }
    if(gmuStatus) {
        ready_=false;
        native_adreno_a6xx_stats[0]=8;
        fail(gmuStatus);
        return;
    }

    const IOReturn status=__atomic_load_n(&hardwareIrqFailed_,__ATOMIC_ACQUIRE)?
        kIOReturnIOError:kIOReturnSuccess;
#if defined(NABU_GPU_METAL) && NABU_GPU_METAL
    uint32_t submitted=0,completed=0;
    const int progress=native_adreno_progress(&submitted,&completed);
    const uint64_t current=now(nullptr);
    observation_.sample(current,submitted,completed,progress);
    if(completed!=watchedFence_ || submitted==completed) {
        watchedFence_=completed;
        watchedSince_=current;
    }
    const bool timeout=submitted!=completed && current-watchedSince_>500000;
    if(status || progress || timeout) {
        ready_=false;
        native_adreno_a6xx_stats[0]=8;
        fail(status?status:(timeout?kIOReturnTimeout:kIOReturnIOError));
        return;
    }
#else
    if(status) {
        ready_=false;
        native_adreno_a6xx_stats[0]=8;
        fail(status);
    }
#endif
}

void A640State::dump() const
{
    const auto &power=failureCaptured_?failedPowerReport_:power_.report();
    const auto &gmu=failureCaptured_?failedGmuReport_:gmu_.report();
    IOLog("NativeAdrenoA6xx: step=%u status=%08x cp_fence=%u/%u readback=%08x irq=%08x cp=%08x protect=%08x\n",
        native_adreno_a6xx_stats[0],native_adreno_a6xx_stats[1],native_adreno_a6xx_stats[8],
        native_adreno_a6xx_stats[9],native_adreno_a6xx_stats[10],native_adreno_a6xx_stats[11],
        native_adreno_a6xx_stats[12],native_adreno_a6xx_stats[13]);
    IOLog("NativeAdrenoA6xx: power=%u/%08x domain=%u offset=%x value=%x expected=%x mask=%x\n",
        power.phase,power.status,power.domain,power.offset,power.value,power.expected,power.mask);
    IOLog("NativeAdrenoA6xx: gmu=%u/%08x domain=%u offset=%x value=%x hfi=%x/%x ack=%u zap=%u scm=%lld/%lld\n",
        gmu.phase,gmu.status,gmu.domain,gmu.offset,gmu.value,gmu.hfiMessage,gmu.hfiError,
        gmu.acknowledged,zap_.phase(),zap_.transport(),zap_.result());
    if(gmuContextReport_.step) {
        NativeAdrenoLogSmmuReport("gmu-retained",&gmuContextReport_);
    }
    if(gpuContextReport_.step) {
        NativeAdrenoLogSmmuReport("gpu-retained",&gpuContextReport_);
    }
}

uint64_t vmNow()
{
    uint64_t value=0;
    NativeAdrenoKernelTime::now_us(nullptr,&value);
    return value;
}

void registrationWorker(void *,wait_result_t)
{
    thread_set_thread_name(current_thread(),"adreno-control");
    OSDictionary *matching=IOService::resourceMatching("IOBSD");
    IOService *provider=matching?IOService::waitForMatchingService(matching,60000000000ULL):nullptr;
    OSSafeReleaseNULL(matching);
    if(!provider) {
        IOLog("NativeAdrenoA6xx: IOBSD resource unavailable\n");
        native_adreno_a6xx_stats[1]=uint32_t(kIOReturnNotReady);
        return;
    }
    provider->release();
    /* Start the explicitly enabled hardware diagnostic after the system settles. */
    unsigned delay=20;
    PE_parse_boot_argn("nabu-gpu-delay",&delay,sizeof(delay));
    if(delay>600) {
        delay=600;
    }
    IOLog("NativeAdrenoA6xx: A640 hardware start in %u s\n",delay);
    IOSleep(delay*1000);
    const IOReturn started=native_adreno_a6xx_request_start();
    if(started) {
        IOLog("NativeAdrenoA6xx: hardware start request failed %08x\n",started);
    }
}

void hardwareWorker(void *,wait_result_t)
{
    thread_set_thread_name(current_thread(),"adreno-a640");
    IOLog("NativeAdrenoA6xx: hardware diagnostic worker started\n");
    native_adreno_vm_now_us=vmNow;
    native_adreno_pool_mib=0;
    state=new A640State;
    if(!state) {
        native_adreno_a6xx_stats[1]=uint32_t(kIOReturnNoMemory);
        nabuGpuState(0,uint32_t(kIOReturnNoMemory),"failed");
        return;
    }
    if(state->start()!=kIOReturnSuccess) {
        return;
    }
    for(;;) {
        IOSleep(10);
        state->poll();
    }
}
void registrationEntry(void *argument,wait_result_t result)
{
    registrationWorker(argument,result);
    thread_terminate(current_thread());
}
void hardwareEntry(void *argument,wait_result_t result)
{
    hardwareWorker(argument,result);
    thread_terminate(current_thread());
}
}

extern "C" void native_adreno_a6xx_start(void)
{
    unsigned enabled=0;
    if(!PE_parse_boot_argn("nabu-gpu",&enabled,sizeof(enabled)) || enabled!=1) {
        return;
    }

    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&launched,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        return;
    }
    thread_t thread=nullptr;
    const kern_return_t status=kernel_thread_start(registrationEntry,nullptr,&thread);
    if(status==KERN_SUCCESS) {
        thread_deallocate(thread);
    } else {
        native_adreno_a6xx_stats[1]=uint32_t(status);
    }
}

extern "C" IOReturn native_adreno_a6xx_request_start(void)
{
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&hardwareLaunched,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        return kIOReturnBusy;
    }
    IOLog("NativeAdrenoA6xx: automatic hardware diagnostic accepted\n");
    nabuGpuState(0,0,"starting");
    thread_t thread=nullptr;
    const kern_return_t status=kernel_thread_start(hardwareEntry,nullptr,&thread);
    if(status!=KERN_SUCCESS) {
        native_adreno_a6xx_stats[1]=uint32_t(status);
        nabuGpuState(0,uint32_t(status),"failed");
        return kIOReturnNoResources;
    }
    thread_deallocate(thread);
    return kIOReturnSuccess;
}

extern "C" void native_adreno_a6xx_dump_stats(void)
{
    IOLog("NativeAdrenoA6xx: registration_requested=%u hardware_requested=%u step=%u status=%08x\n",
        launched,hardwareLaunched,native_adreno_a6xx_stats[0],native_adreno_a6xx_stats[1]);
    if(state) {
        state->dump();
    }
}
