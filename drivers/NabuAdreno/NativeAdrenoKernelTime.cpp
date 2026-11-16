/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoKernelTime.h"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
int NativeAdrenoKernelTime::now_us(void *cookie,uint64_t *value)
{
    if(cookie || !value)return 1;
    uint64_t nanoseconds=0;
    absolutetime_to_nanoseconds(mach_absolute_time(),&nanoseconds);
    *value=nanoseconds/1000;
    return 0;
}
int NativeAdrenoKernelTime::wait_us(void *cookie,unsigned microseconds)
{
    if(cookie)return 1;
    IODelay(microseconds);
    return 0;
}
