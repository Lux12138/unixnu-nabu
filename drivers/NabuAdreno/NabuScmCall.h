/* SPDX-License-Identifier: MIT */
#ifndef NABU_SCM_CALL_H
#define NABU_SCM_CALL_H
#include <stdint.h>

struct NabuScmRegisters {uint64_t x[8];};
struct NabuScmIo {
    void *cookie;
    void (*invoke)(void *,NabuScmRegisters *);
    int (*now_us)(void *,uint64_t *);
    int (*wait_us)(void *,unsigned);
};
enum class NabuScmStop {Complete,Deadline,CallBudget,BusyBudget,Clock,Wait,InvalidIo};
struct NabuScmReply {
    NabuScmStop stop=NabuScmStop::InvalidIo;
    int64_t transport=0,result=0;
    uint64_t continuation=0,elapsed_us=0;
    unsigned calls=0,interrupted=0,busy=0;
};

constexpr uint64_t NabuScmTimeoutUs=5000000;
constexpr unsigned NabuScmMaxCalls=4096,NabuScmMaxBusyRetries=20,NabuScmBusyWaitUs=30000;

/* The deadline bounds returned calls, not an SMC instruction that never returns.
 * An interrupted request is still active: preserve x6, do not restart PAS or
 * release its buffers. Terminal firmware errors remain separate from our limits. */
static inline NabuScmReply nabuScmCall(const NabuScmIo &io,uint32_t initialFunction,
    uint32_t info,uint64_t arg0,uint64_t arg1,uint64_t arg2)
{
    NabuScmReply reply;
    if(!io.invoke || !io.now_us || !io.wait_us)return reply;
    uint64_t start=0;
    if(io.now_us(io.cookie,&start)) {reply.stop=NabuScmStop::Clock;return reply;}
    uint64_t previous=start,function=initialFunction,continuation=0;
    for(;;) {
        uint64_t now=0;
        if(io.now_us(io.cookie,&now) || now<previous) {reply.stop=NabuScmStop::Clock;return reply;}
        previous=now;
        reply.elapsed_us=now-start;
        if(reply.elapsed_us>=NabuScmTimeoutUs) {reply.stop=NabuScmStop::Deadline;return reply;}
        if(reply.calls>=NabuScmMaxCalls) {reply.stop=NabuScmStop::CallBudget;return reply;}
        NabuScmRegisters regs{{function,info,arg0,arg1,arg2,0,continuation,0}};
        io.invoke(io.cookie,&regs);
        ++reply.calls;
        reply.transport=int32_t(regs.x[0]);
        reply.result=int32_t(regs.x[1]);
        reply.continuation=regs.x[6];
        if(io.now_us(io.cookie,&now) || now<previous) {reply.stop=NabuScmStop::Clock;return reply;}
        previous=now;
        reply.elapsed_us=now-start;
        if(regs.x[0]==1) {
            ++reply.interrupted;
            function=1;
            continuation=regs.x[6];
            continue;
        }
        if(reply.transport==-12) {
            if(++reply.busy>NabuScmMaxBusyRetries) {reply.stop=NabuScmStop::BusyBudget;return reply;}
            if(reply.elapsed_us>=NabuScmTimeoutUs) {reply.stop=NabuScmStop::Deadline;return reply;}
            function=initialFunction;
            continuation=0;
            if(io.wait_us(io.cookie,NabuScmBusyWaitUs)) {reply.stop=NabuScmStop::Wait;return reply;}
            continue;
        }
        reply.stop=NabuScmStop::Complete;
        return reply;
    }
}

static inline const char *nabuScmStopName(NabuScmStop stop)
{
    switch(stop) {
    case NabuScmStop::Complete:return "complete";
    case NabuScmStop::Deadline:return "deadline";
    case NabuScmStop::CallBudget:return "call-budget";
    case NabuScmStop::BusyBudget:return "busy-budget";
    case NabuScmStop::Clock:return "clock";
    case NabuScmStop::Wait:return "wait";
    case NabuScmStop::InvalidIo:return "invalid-io";
    }
    return "unknown";
}
#endif
