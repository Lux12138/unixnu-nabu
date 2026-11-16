/* SPDX-License-Identifier: MIT */
#ifndef NABU_GPU_COPY_PLAN_H
#define NABU_GPU_COPY_PLAN_H
#include "AdrenoPM4.h"
#include <stddef.h>
#include <stdint.h>

enum : size_t { NabuGpuCopyMaxBytes=32u*1024u*1024u,
    NabuGpuCopyChunkBytes=64u*1024u, NabuGpuCopyMaxWords=3074 };

static inline bool nabu_gpu_copy_rows(uint32_t width,uint32_t height,
    uint64_t stride,uint64_t bytes)
{
    const uint64_t row=uint64_t(width)*4;
    return width && height && width<=16384 && height<=16384 && !(stride&3) &&
        stride>=row && row<=bytes && uint64_t(height-1)<=(bytes-row)/stride;
}

static inline bool nabu_gpu_firmware_range(uint64_t physical,uint64_t bytes,
    uint64_t managed,uint64_t managedBytes,unsigned outputBits)
{
    if(!physical || !bytes || ((physical|bytes)&4095) ||
        bytes>NabuGpuCopyMaxBytes || outputBits<32 || outputBits>40 ||
        physical>UINT64_MAX-bytes || managed>UINT64_MAX-managedBytes) return false;
    const uint64_t limit=UINT64_C(1)<<outputBits;
    return physical<limit && bytes<=limit-physical &&
        (physical+bytes<=managed || managed+managedBytes<=physical);
}

/* CP_MEMCPY (0x75), not the arithmetic CP_MEM_TO_MEM (0x73). Payload order:
 * dword count, source low/high, destination low/high; A6XX+ adreno_pm4.xml.
 * Counts are bytes/4, never bytes or count-1. No user packets are accepted. */
static inline size_t nabu_gpu_copy_plan(uint32_t *words,size_t capacity,
    uint64_t source,uint64_t sourceBytes,uint64_t destination,uint64_t destinationBytes,
    uint64_t destinationStride,uint32_t width,uint32_t height)
{
    const uint64_t row=uint64_t(width)*4, packed=row*height;
    if(!words || !source || !destination || ((source|destination)&3) ||
        !nabu_gpu_copy_rows(width,height,row,sourceBytes) ||
        !nabu_gpu_copy_rows(width,height,destinationStride,destinationBytes) ||
        sourceBytes>NabuGpuCopyMaxBytes || destinationBytes>NabuGpuCopyMaxBytes ||
        source>UINT64_MAX-sourceBytes || destination>UINT64_MAX-destinationBytes ||
        (source<destination+destinationBytes && destination<source+sourceBytes)) return 0;
    const uint64_t chunks=destinationStride==row?
        (packed+NabuGpuCopyChunkBytes-1)/NabuGpuCopyChunkBytes:
        uint64_t(height)*((row+NabuGpuCopyChunkBytes-1)/NabuGpuCopyChunkBytes);
    if(chunks>(NabuGpuCopyMaxWords-2)/6 || chunks>(capacity<2?0:(capacity-2)/6)) return 0;
    size_t at=0;
    const uint32_t rows=destinationStride==row?1:height;
    for(uint32_t y=0;y<rows;++y) {
        const uint64_t bytes=rows==1?packed:row;
        for(uint64_t offset=0;offset<bytes;offset+=NabuGpuCopyChunkBytes) {
            const uint64_t remaining=bytes-offset;
            const uint32_t count=uint32_t(remaining<NabuGpuCopyChunkBytes?remaining:NabuGpuCopyChunkBytes)/4;
            const uint64_t src=source+uint64_t(y)*row+offset;
            const uint64_t dst=destination+uint64_t(y)*destinationStride+offset;
            if(AdrenoPM4Type7(0x75,5,&words[at])!=AdrenoPM4Success) return 0;
            words[at+1]=count; words[at+2]=uint32_t(src); words[at+3]=uint32_t(src>>32);
            words[at+4]=uint32_t(dst); words[at+5]=uint32_t(dst>>32); at+=6;
        }
    }
    if(AdrenoPM4Type7(0x26,0,&words[at++])!=AdrenoPM4Success ||
        AdrenoPM4Type7(0x13,0,&words[at++])!=AdrenoPM4Success) return 0;
    return at;
}
#endif
