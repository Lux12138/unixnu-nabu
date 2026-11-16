/* SPDX-License-Identifier: MIT */
/* Bounded, per-process frontend problem records. No MMIO, IOService connection,
 * policy change or wait: a completed marker line goes straight to RAM capture. */
#ifndef NABU_GPU_PROBLEM_LOG_H
#define NABU_GPU_PROBLEM_LOG_H
#include <IOKit/IOLib.h>
#include <sys/proc.h>

extern "C" void nabu_capture_append(const char *,unsigned);
struct NabuGpuProblemEntry {
    int pid=0;
    unsigned length=0,records=0;
    bool active=false;
    char name[20]{},text[64]{};
};
static IOLock *nabu_gpu_problem_lock;
static NabuGpuProblemEntry nabu_gpu_problem_entries[8];
static inline void nabu_gpu_problem_log_initialize()
{
    nabu_gpu_problem_lock=IOLockAlloc(); // Logging remains disabled if allocation fails.
}
static inline void nabu_gpu_problem_log_marker(uint32_t value)
{
    if(!nabu_gpu_problem_lock)return;
    const int pid=proc_selfpid();
    if(pid<=0)return;
    IOLockLock(nabu_gpu_problem_lock);
    NabuGpuProblemEntry *entry=nullptr;
    for(auto &candidate:nabu_gpu_problem_entries) {
        if(candidate.pid==pid){entry=&candidate;break;}
    }
    if(!entry && value==0x6d00) {
        for(auto &candidate:nabu_gpu_problem_entries)if(!candidate.pid) {
            candidate.pid=pid;proc_selfname(candidate.name,sizeof(candidate.name));entry=&candidate;break;
        }
    }
    if(entry && entry->records<96) {
        if(value==0x6d00) {
            entry->length=0;entry->text[0]=0;entry->active=true;
        } else if(value==0x6dff && entry->active) {
            char line[160];
            const int bytes=snprintf(line,sizeof(line),"NabuGPU: plugin_problem pid=%d process=%.*s text=%s\n",
                entry->pid,20,entry->name,entry->text);
            entry->active=false;++entry->records;
            if(bytes>0 && unsigned(bytes)<sizeof(line))nabu_capture_append(line,unsigned(bytes));
        } else if(value>0x6d00 && value<0x6d80 && entry->active && entry->length<sizeof(entry->text)-1) {
            const unsigned ch=value&0xff;
            entry->text[entry->length++]=ch>=32 && ch<127?char(ch):'_';
            entry->text[entry->length]=0;
        }
    }
    IOLockUnlock(nabu_gpu_problem_lock);
}
#endif
