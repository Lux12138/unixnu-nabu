// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
// Original SwapSetLayer sets the update bit and clears the disable bit for
// a nonnull surface; a null surface sets both. SwapSetBackgroundColor sets
// bit 31 in both masks and writes opaque RGB. These are masks, not counts.
// This backend only copies one complete premultiplied BGRA layer over black.
// Other layers, unknown bits and nonblack backgrounds require composition.
static inline int native_legacy_single_layer_state(uint32_t updated,
    uint32_t disabled, uint32_t background) {
    const uint32_t known = 0x8000000fU;
    if (((updated | disabled) & ~known) || (disabled & ~updated) ||
        (updated & ~disabled) != 1U) return 0;
    if (updated & 0x80000000U)
        return (disabled & 0x80000000U) && background == 0xff000000U;
    return background == 0;
}

// A swap that disables every updated layer over the black background asks the
// display to show nothing. It carries no surface; presenting it is a solid fill,
// not composition. Bit 31 is the background pseudo-layer, as above.
static inline int native_legacy_blank_state(uint32_t updated,
    uint32_t disabled, uint32_t background) {
    const uint32_t known = 0x8000000fU;
    if (((updated | disabled) & ~known) || !updated || updated != disabled) return 0;
    if (updated & 0x80000000U) return background == 0xff000000U;
    return background == 0;
}

// IOSurface rows and allocation have independent alignment. Copy visible
// pixels only, after bounding every row in the real memory descriptor.
static inline int native_legacy_surface_bounds(uint32_t width,uint32_t height,
    uint64_t stride,uint64_t allocation,uint64_t descriptorBytes) {
    if (!width || !height || width>16384 || height>16384 ||
        stride<(uint64_t)width*4 || stride>UINT64_MAX/height) return 0;
    return allocation>=stride*height && descriptorBytes>=allocation;
}

// The GOP destination is device memory: use aligned scalar pixel stores,
// never a general memcpy that may issue unaligned or wider device accesses.
// The source is normal mapped IOSurface memory and may have padded rows.
static inline int native_legacy_copy_layout(volatile void *destination,
    uint64_t destinationBytes, uint64_t destinationStride, const void *source,
    uint64_t sourceBytes, uint64_t sourceStride, uint32_t width, uint32_t height) {
    uintptr_t dst=(uintptr_t)destination, src=(uintptr_t)source;
    if (!dst || !src || (dst&3) || (destinationStride&3) ||
        !native_legacy_surface_bounds(width,height,destinationStride,destinationBytes,destinationBytes) ||
        !native_legacy_surface_bounds(width,height,sourceStride,sourceBytes,sourceBytes) ||
        destinationBytes>UINTPTR_MAX || sourceBytes>UINTPTR_MAX ||
        dst>UINTPTR_MAX-destinationBytes || src>UINTPTR_MAX-sourceBytes) return 0;
    return 1;
}
static inline int native_legacy_fill_pixels(volatile void *destination,
    uint64_t destinationBytes, uint64_t destinationStride,
    uint32_t width, uint32_t height, uint32_t value) {
    uintptr_t dst=(uintptr_t)destination;
    if (!dst || (dst&3) || (destinationStride&3) ||
        !native_legacy_surface_bounds(width,height,destinationStride,destinationBytes,destinationBytes) ||
        destinationBytes>UINTPTR_MAX || dst>UINTPTR_MAX-destinationBytes) return 0;
    for (uint32_t row=0;row<height;++row) {
        volatile uint32_t *out=(volatile uint32_t *)(dst+row*destinationStride);
        for (uint32_t column=0;column<width;++column)out[column]=value;
    }
    return 1;
}
// Observation only: count pixels whose color channels are not all zero and their bounding box, so
// hardware photographs can tell an empty compositor frame from a display-path loss. out = count, minx,
// miny, maxx, maxy (min fields are UINT64_MAX and max fields 0 when every pixel is black).
static inline int native_legacy_frame_scan(const void *source, uint64_t sourceBytes,
    uint64_t sourceStride, uint32_t width, uint32_t height, uint64_t out[5]) {
    out[0]=0;out[1]=UINT64_MAX;out[2]=UINT64_MAX;out[3]=0;out[4]=0;
    uintptr_t src=(uintptr_t)source;
    if (!src || !native_legacy_surface_bounds(width,height,sourceStride,sourceBytes,sourceBytes) ||
        sourceBytes>UINTPTR_MAX || src>UINTPTR_MAX-sourceBytes) return 0;
    for (uint32_t row=0;row<height;++row) {
        const unsigned char *in=(const unsigned char *)(src+row*sourceStride);
        for (uint32_t column=0;column<width;++column) {
            const unsigned char *pixel=in+(uint64_t)column*4;
            if (pixel[0]|pixel[1]|pixel[2]) {
                ++out[0];
                if (column<out[1]) out[1]=column;
                if (row<out[2]) out[2]=row;
                if (column>out[3]) out[3]=column;
                if (row>out[4]) out[4]=row;
            }
        }
    }
    return 1;
}
// Observation only: first-fit table of the first four distinct pixel values (with alpha) and their
// counts; every further distinct value is counted in *other.
static inline int native_legacy_frame_colors(const void *source, uint64_t sourceBytes,
    uint64_t sourceStride, uint32_t width, uint32_t height,
    uint32_t colors[4], uint64_t counts[4], uint64_t *other) {
    for (unsigned i=0;i<4;++i){colors[i]=0;counts[i]=0;}
    *other=0;
    uintptr_t src=(uintptr_t)source;
    if (!src || !native_legacy_surface_bounds(width,height,sourceStride,sourceBytes,sourceBytes) ||
        sourceBytes>UINTPTR_MAX || src>UINTPTR_MAX-sourceBytes) return 0;
    unsigned used=0;
    for (uint32_t row=0;row<height;++row) {
        const unsigned char *in=(const unsigned char *)(src+row*sourceStride);
        for (uint32_t column=0;column<width;++column) {
            const unsigned char *pixel=in+(uint64_t)column*4;
            uint32_t value=(uint32_t)pixel[0] | ((uint32_t)pixel[1]<<8) | ((uint32_t)pixel[2]<<16) | ((uint32_t)pixel[3]<<24);
            unsigned i=0;
            while (i<used && colors[i]!=value) ++i;
            if (i<used) ++counts[i];
            else if (used<4) {colors[used]=value;counts[used]=1;++used;}
            else ++*other;
        }
    }
    return 1;
}
static inline int native_legacy_copy_pixels(volatile void *destination,
    uint64_t destinationBytes, uint64_t destinationStride, const void *source,
    uint64_t sourceBytes, uint64_t sourceStride, uint32_t width, uint32_t height) {
    if (!native_legacy_copy_layout(destination,destinationBytes,destinationStride,
        source,sourceBytes,sourceStride,width,height)) return 0;
    uintptr_t dst=(uintptr_t)destination, src=(uintptr_t)source;
    // Source and destination are both BGRA8888 little-endian words, so the pixels move unchanged. Use aligned 64-bit
    // device stores (four per iteration) instead of byte-wise pixel assembly: the stores into device memory dominate.
    // The destination base and stride are 8-byte aligned in practice (checked here); otherwise 32-bit stores remain.
    int wide=!(dst&7) && !(destinationStride&7);
    for (uint32_t row=0;row<height;++row) {
        volatile uint32_t *out=(volatile uint32_t *)(dst+row*destinationStride);
        const unsigned char *in=(const unsigned char *)(src+row*sourceStride);
        uint32_t column=0;
        if (wide) {
            volatile uint64_t *out64=(volatile uint64_t *)out;
            for (;column+8<=width;column+=8) {
                uint64_t a,b,c,d;
                __builtin_memcpy(&a,in+(uint64_t)column*4,8);__builtin_memcpy(&b,in+(uint64_t)column*4+8,8);
                __builtin_memcpy(&c,in+(uint64_t)column*4+16,8);__builtin_memcpy(&d,in+(uint64_t)column*4+24,8);
                out64[column/2]=a;out64[column/2+1]=b;out64[column/2+2]=c;out64[column/2+3]=d;
            }
        }
        for (;column<width;++column) {
            uint32_t value;
            __builtin_memcpy(&value,in+(uint64_t)column*4,4);
            out[column]=value;
        }
    }
    return 1;
}

// Differential copy for the slow scanout memory: only 64-byte blocks that differ from the previous frame (kept in an ordinary
// cached shadow buffer, rows packed at width*4) are stored to the device mapping. The shadow must start from a full copy
// (valid=0 copies everything). Returns the number of bytes stored, or UINT64_MAX when the geometry needs the plain copy.
static inline uint64_t native_legacy_copy_diff(volatile void *destination,
    uint64_t destinationBytes, uint64_t destinationStride, const void *source,
    uint64_t sourceBytes, uint64_t sourceStride, uint32_t width, uint32_t height,
    void *shadow, int valid) {
    if (!native_legacy_copy_layout(destination,destinationBytes,destinationStride,
        source,sourceBytes,sourceStride,width,height)) return UINT64_MAX;
    uintptr_t dst=(uintptr_t)destination, src=(uintptr_t)source;
    if (!shadow || ((uintptr_t)shadow&7) || (dst&7) || (destinationStride&7) || (width&1)) return UINT64_MAX;
    uint64_t stored=0;
    const uint32_t words=width/2;
    for (uint32_t row=0;row<height;++row) {
        volatile uint64_t *out=(volatile uint64_t *)(dst+row*destinationStride);
        const unsigned char *in=(const unsigned char *)(src+row*sourceStride);
        uint64_t *old=(uint64_t *)((unsigned char *)shadow+(uint64_t)row*width*4);
        uint32_t w=0;
        for (;w+8<=words;w+=8) {
            uint64_t v[8];
            __builtin_memcpy(v,in+(uint64_t)w*8,64);
            if (valid && !((v[0]^old[w])|(v[1]^old[w+1])|(v[2]^old[w+2])|(v[3]^old[w+3])|
                           (v[4]^old[w+4])|(v[5]^old[w+5])|(v[6]^old[w+6])|(v[7]^old[w+7]))) continue;
            for (unsigned i=0;i<8;++i){out[w+i]=v[i];old[w+i]=v[i];}
            stored+=64;
        }
        for (;w<words;++w) {
            uint64_t v;
            __builtin_memcpy(&v,in+(uint64_t)w*8,8);
            if (valid && v==old[w]) continue;
            out[w]=v;old[w]=v;stored+=8;
        }
    }
    return stored;
}
