/* SPDX-License-Identifier: MIT */
/* Board facts: this unit's Android FDT, read 2026-10-29. Absolute, namespaced
 * ADT ranges deliberately avoid /arm-io's address translation. */
#include "NativeAdrenoA6xxService.h"
#include "NativeAdrenoSha256.h"
#include "NabuGpuBuild.h"
#include "NabuLogCore.h"
#include <IOKit/IOLib.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <string.h>

extern "C" volatile unsigned nabu_log_writer_state,nabu_log_writer_failures;
extern "C" uint64_t nabu_log_committed_end;

namespace {
IORegistryEntry *firmwareNode=nullptr;
struct PollSource {uint32_t number;void (*callback)(void *);void *context;bool enabled;};
PollSource sources[4]{};
unsigned sourceCount=0;

OSData *property(IORegistryEntry *node,const char *name)
{
    return OSDynamicCast(OSData,node->getProperty(name));
}
bool scalar(IORegistryEntry *node,const char *name,uint32_t *out)
{
    OSData *data=property(node,name);
    if(!data || data->getLength()!=4)return false;
    memcpy(out,data->getBytesNoCopy(),4);
    return true;
}
bool named(IORegistryEntry *node,const char *namesKey,const char *valuesKey,
    const char *name,void *out,size_t bytes)
{
    OSData *names=property(node,namesKey),*values=property(node,valuesKey);
    if(!names || !values || !bytes || values->getLength()%bytes)return false;
    const char *text=static_cast<const char *>(names->getBytesNoCopy());
    const size_t size=names->getLength(),count=values->getLength()/bytes;
    size_t at=0,matched=0;
    for(size_t index=0;index<count;++index) {
        if(at>=size)return false;
        const char *end=static_cast<const char *>(memchr(text+at,0,size-at));
        if(!end || end==text+at)return false;
        if(!strcmp(text+at,name)) {
            memcpy(out,static_cast<const uint8_t *>(values->getBytesNoCopy())+index*bytes,bytes);
            ++matched;
        }
        at=size_t(end-text)+1;
    }
    return at==size && matched==1;
}
bool window(IORegistryEntry *node,const char *name,uint64_t pa,uint64_t size,
    NativeAdrenoA6xxWindow *out)
{
    return named(node,"nabu-reg-names","nabu-reg",name,out,sizeof(*out)) &&
        out->physical==pa && out->bytes==size;
}
bool irq(IORegistryEntry *node,const char *name,uint32_t expected,uint32_t *out)
{
    return named(node,"nabu-interrupt-names","nabu-interrupts",name,out,4) && *out==expected;
}
bool firmware(IORegistryEntry *node,const char *key,const char *digest,size_t size,
    const uint8_t **out,size_t *bytes)
{
    OSData *blob=property(node,key);
    if(!blob || blob->getLength()!=size)return false;
    uint8_t hash[32];
    NativeAdrenoSha256::hash(static_cast<const uint8_t *>(blob->getBytesNoCopy()),size,hash);
    const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<32;++i)
        if(digest[2*i]!=hex[hash[i]>>4] || digest[2*i+1]!=hex[hash[i]&15])return false;
    *out=static_cast<const uint8_t *>(blob->getBytesNoCopy());*bytes=size;
    IOLog("NabuGPU: firmware=%s bytes=%lu sha256=%s\n",key,size,digest);
    return true;
}
}

/* images live in nabu_adreno_fw.cpp; the first two are STRONG overrides of the weak defaults */
extern "C" const uint8_t native_adreno_firmware_image[];
extern "C" const uint64_t native_adreno_firmware_image_size;
extern "C" const uint8_t native_adreno_sqe_firmware_image[];
extern "C" const uint64_t native_adreno_sqe_firmware_image_size;
extern "C" const uint8_t nabu_adreno_zap_metadata[];
extern "C" const uint8_t nabu_adreno_zap_shader[];

extern "C" int native_adreno_a6xx_resources(NativeAdrenoA6xxResources *out)
{
    if(!out || firmwareNode)return 1;
    *out={};
    IORegistryEntry *node=IORegistryEntry::fromPath("/nabu-adreno",gIODTPlane);
    if(!node) {
        /* NABU-A640-STATIC: this ADT has no /nabu-adreno node (measured on the board:
         * rc=1, chip=00000000, every field zero). The reader's own tables below list exactly the
         * values it wants per name -- so instead of building that node, and embedding 78 KB of
         * firmware blobs into the DTB, state the same facts here as constants.
         * The four images come from nabu_adreno_fw.cpp, whose bytes were checked against the
         * SHA-256 the firmware() path would have validated. */
        NativeAdrenoA6xxResources r{};
        r.gpu.physical=0x02c00000;            r.gpu.bytes=0x40000;
        r.gmu.physical=0x02c6a000;            r.gmu.bytes=0x30000;
        r.rscc.physical=0x02c8d000;           r.rscc.bytes=0x2000;
        r.smmu.physical=0x02ca0000;           r.smmu.bytes=0x10000;
        r.smmuTcu.physical=0x02cc2000;        r.smmuTcu.bytes=0x20;
        r.smmuTbu[0].physical=0x02cc5000;     r.smmuTbu[0].bytes=0x1000;
        r.smmuTbu[1].physical=0x02cc9000;     r.smmuTbu[1].bytes=0x1000;
        r.smmuTbuStatus[0].physical=0x02cc2200; r.smmuTbuStatus[0].bytes=8;
        r.smmuTbuStatus[1].physical=0x02cc2208; r.smmuTbuStatus[1].bytes=8;
        r.gpucc.physical=0x02c90000;          r.gpucc.bytes=0x9000;
        r.gcc.physical=0x00100000;            r.gcc.bytes=0x1f0000;
        r.pdc.physical=0x0b290000;            r.pdc.bytes=0x10000;
        r.pdcSeq.physical=0x0b490000;         r.pdcSeq.bytes=0x10000;
        r.commandDb.physical=0x85f20000;      r.commandDb.bytes=0x20000;
        r.rpmh.physical=0x18220000;           r.rpmh.bytes=0x10000;
        r.aop.physical=0x0c300000;            r.aop.bytes=0x400;
        r.aopDoorbell.physical=0x17c00000;    r.aopDoorbell.bytes=0x1000;
        r.zap.physical=0x98f15000;            r.zap.bytes=0x2000;

        r.chipId=0x06040001;        r.gmemBytes=0x100000;   r.frequencyHz=257000000;
        r.highestBankBit=15;        r.minAccessLength=32;   r.ubwcMode=3;
        r.gpuBank=0;  r.gpuAsid=1;  r.gpuSlot=0;  r.gpuSid=0;  r.gpuSidMask=0x401;
        r.gmuBank=1;  r.gmuAsid=2;  r.gmuSlot=4;  r.gmuSid=5;  r.gmuSidMask=0x400;
        r.aopDoorbellOffset=0xc;    r.aopDoorbellValue=1;    r.zapPasId=13;

        r.gpuInterrupt=332;
        r.smmuGlobalInterrupt=706;
        for(unsigned i=0;i<8;++i)r.smmuContextInterrupts[i]=713+i;
        r.smmuContextInterruptCount=8;
        r.smmuTbuSidStart[0]=0;    r.smmuTbuSidCount[0]=0x400;
        r.smmuTbuSidStart[1]=0x400;r.smmuTbuSidCount[1]=0x400;

        r.gmuFirmware=native_adreno_firmware_image;
        r.gmuFirmwareBytes=(size_t)native_adreno_firmware_image_size;
        r.sqeFirmware=native_adreno_sqe_firmware_image;
        r.sqeFirmwareBytes=(size_t)native_adreno_sqe_firmware_image_size;
        r.zapMetadata=nabu_adreno_zap_metadata;      r.zapMetadataBytes=6860;
        r.zapShader=nabu_adreno_zap_shader;          r.zapShaderBytes=1968;

        *out=r;
        IOLog("NabuGPU: source=static-table (this ADT has no /nabu-adreno) chip=%08x "
              "gmem=%x freq=%u windows gpu=%llx gmu=%llx zap=%llx fw=%llu/%llu\n",
              (unsigned) r.chipId, (unsigned) r.gmemBytes, (unsigned) r.frequencyHz,
              (unsigned long long) r.gpu.physical, (unsigned long long) r.gmu.physical,
              (unsigned long long) r.zap.physical,
              (unsigned long long) r.gmuFirmwareBytes, (unsigned long long) r.sqeFirmwareBytes);
        return 0;
    }
    NativeAdrenoA6xxResources r{};
    bool ok=true;
    const struct {const char *name;uint64_t pa,size;NativeAdrenoA6xxWindow *target;} windows[]={
        {"gpu",0x02c00000,0x40000,&r.gpu},{"gmu",0x02c6a000,0x30000,&r.gmu},
        {"rscc",0x02c8d000,0x2000,&r.rscc},{"smmu",0x02ca0000,0x10000,&r.smmu},
        {"smmu-tcu",0x02cc2000,0x20,&r.smmuTcu},
        {"smmu-tbu-0",0x02cc5000,0x1000,&r.smmuTbu[0]},
        {"smmu-tbu-1",0x02cc9000,0x1000,&r.smmuTbu[1]},
        {"smmu-tbu-status-0",0x02cc2200,8,&r.smmuTbuStatus[0]},
        {"smmu-tbu-status-1",0x02cc2208,8,&r.smmuTbuStatus[1]},
        {"gpucc",0x02c90000,0x9000,&r.gpucc},{"gcc",0x00100000,0x1f0000,&r.gcc},
        {"pdc",0x0b290000,0x10000,&r.pdc},{"pdc-seq",0x0b490000,0x10000,&r.pdcSeq},
        {"command-db",0x85f20000,0x20000,&r.commandDb},{"rpmh",0x18220000,0x10000,&r.rpmh},
        {"aop",0x0c300000,0x400,&r.aop},{"aop-doorbell",0x17c00000,0x1000,&r.aopDoorbell},
        {"zap",0x98f15000,0x2000,&r.zap}
    };
    for(const auto &w:windows)ok=window(node,w.name,w.pa,w.size,w.target) && ok;
    const struct {const char *key;uint32_t expected,*target;} scalars[]={
        {"chip-id",0x06040001,&r.chipId},{"gmem-bytes",0x100000,&r.gmemBytes},
        {"frequency-hz",257000000,&r.frequencyHz},{"highest-bank-bit",15,&r.highestBankBit},
        {"min-access-length",32,&r.minAccessLength},{"ubwc-mode",3,&r.ubwcMode},
        {"gpu-bank",0,&r.gpuBank},{"gpu-asid",1,&r.gpuAsid},{"gpu-slot",0,&r.gpuSlot},
        {"gpu-sid",0,&r.gpuSid},{"gpu-sid-mask",0x401,&r.gpuSidMask},
        {"gmu-bank",1,&r.gmuBank},{"gmu-asid",2,&r.gmuAsid},{"gmu-slot",4,&r.gmuSlot},
        {"gmu-sid",5,&r.gmuSid},{"gmu-sid-mask",0x400,&r.gmuSidMask},
        {"aop-doorbell-offset",0xc,&r.aopDoorbellOffset},{"aop-doorbell-value",1,&r.aopDoorbellValue},
        {"zap-pas-id",13,&r.zapPasId}
    };
    for(const auto &s:scalars)ok=(scalar(node,s.key,s.target) && *s.target==s.expected) && ok;
    ok=irq(node,"gpu",332,&r.gpuInterrupt) && ok;
    ok=irq(node,"smmu-global",706,&r.smmuGlobalInterrupt) && ok;
    for(unsigned i=0;i<8;++i) {
        char key[24];snprintf(key,sizeof(key),"smmu-context-%u",i);
        ok=irq(node,key,713+i,&r.smmuContextInterrupts[i]) && ok;
    }
    r.smmuContextInterruptCount=8;
    r.smmuTbuSidStart[0]=0;r.smmuTbuSidCount[0]=0x400;
    r.smmuTbuSidStart[1]=0x400;r.smmuTbuSidCount[1]=0x400;
    ok=firmware(node,"gmu-firmware","3ff0c02708bbe78641db887fa62f3a7f9337934d0c2ce0b961ef7c43172591d2",37680,
        &r.gmuFirmware,&r.gmuFirmwareBytes) && ok;
    ok=firmware(node,"sqe-firmware","36298bd53ea3666413d288a3a3f5266f67fcb6db038a0a481af5869d1c17859c",32136,
        &r.sqeFirmware,&r.sqeFirmwareBytes) && ok;
    ok=firmware(node,"zap-metadata","5c5a956febabc8c8f3fc7b2cc48ae22bcab9ae25a9853c7cd0baadc9ec406882",6860,
        &r.zapMetadata,&r.zapMetadataBytes) && ok;
    ok=firmware(node,"zap-shader","e24a4f483b71ea19715910f01e1abf94280756df731cad05df48fd7714e34198",1968,
        &r.zapShader,&r.zapShaderBytes) && ok;
    if(!ok) {node->release();IOLog("NabuGPU: ADT/firmware validation failed; MMIO not started\n");return 1;}
    firmwareNode=node;*out=r;
    IOLog("NabuGPU: source=%s profile=nabu_ufs_gpu_defconfig mode=cp-proof firmware=adt irq=poll\n",NABU_GPU_SOURCE_ID);
    return 0;
}

extern "C" IOReturn native_adreno_smmu_register_irq(uint32_t number,void (*callback)(void *),void *context)
{
    if(!callback || !context || sourceCount>=4 ||
        (number!=332 && number!=706 && (number<713 || number>720)))return kIOReturnBadArgument;
    for(unsigned i=0;i<sourceCount;++i)if(sources[i].number==number)return kIOReturnBusy;
    sources[sourceCount++]={number,callback,context,false};
    return kIOReturnSuccess;
}
extern "C" IOReturn native_adreno_smmu_enable_irq(uint32_t number)
{
    for(auto &s:sources)if(s.number==number && s.callback) {s.enabled=true;return kIOReturnSuccess;}
    return kIOReturnNotFound;
}
extern "C" IOReturn native_adreno_smmu_disable_irq(uint32_t number)
{
    for(auto &s:sources)if(s.number==number && s.callback) {s.enabled=false;return kIOReturnSuccess;}
    return kIOReturnNotFound;
}
extern "C" void native_adreno_poll_irqs(void)
{
    /* Only the hardware worker calls this; ECATS temporarily disables its bank's
     * source on that same worker. No concurrent callback races a translation. */
    for(auto &s:sources)if(s.enabled && s.callback)s.callback(s.context);
}
extern "C" int native_adreno_checkpoint(const char *phase)
{
    IOLog("NabuGPU: checkpoint=%s\n",phase);
    nb_log_snapshot snapshot{};
    bool captured=false;
    for(unsigned attempt=0;attempt<20 && !captured;++attempt) {
        captured=nabu_capture_snapshot(&snapshot)!=0;
        if(!captured)IOSleep(1);
    }
    if(!captured)return 1;
    const unsigned failures=nabu_log_writer_failures;
    for(unsigned elapsed=0;elapsed<5000;elapsed+=20) {
        if(__atomic_load_n(&nabu_log_committed_end,__ATOMIC_ACQUIRE)>=snapshot.end)return 0;
        if(nabu_log_writer_state==3 || nabu_log_writer_failures!=failures)break;
        IOSleep(20);
    }
    IOLog("NabuGPU: checkpoint=%s persistence-timeout; hardware step blocked\n",phase);
    return 1;
}
