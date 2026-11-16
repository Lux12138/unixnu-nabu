// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../NativeLegacyRequest.h"
#include <assert.h>
#include <string.h>
int main(void) {
    assert(native_legacy_single_layer_state(1,0,0));
    assert(native_legacy_single_layer_state(0x80000007U,0x80000006U,0xff000000U));
    // Exhaust the four-layer masks: exactly layer zero may remain active.
    for(uint32_t updated=0;updated<16;updated++)
        for(uint32_t disabled=0;disabled<16;disabled++) {
            int expected=!(disabled & ~updated) && (updated & ~disabled)==1;
            assert(native_legacy_single_layer_state(updated,disabled,0)==expected);
            assert(native_legacy_single_layer_state(updated|0x80000000U,
                disabled|0x80000000U,0xff000000U)==expected);
        }
    assert(!native_legacy_single_layer_state(0x80000001U,0,0xff000000U));
    assert(!native_legacy_single_layer_state(0x80000001U,0x80000000U,0xffff0000U));
    assert(!native_legacy_single_layer_state(1,0,0xff000000U));
    assert(!native_legacy_single_layer_state(0x101,0x100,0));
    assert(!native_legacy_single_layer_state(0x80000001U,0x80000000U,0));
    // Blank swaps: every updated layer disabled over black, nothing enabled.
    assert(native_legacy_blank_state(0x80000007U,0x80000007U,0xff000000U));
    assert(native_legacy_blank_state(0x80000000U,0x80000000U,0xff000000U));
    assert(native_legacy_blank_state(3,3,0));
    assert(!native_legacy_blank_state(0,0,0));
    assert(!native_legacy_blank_state(0x80000007U,0x80000006U,0xff000000U));
    assert(!native_legacy_blank_state(0x80000007U,0x80000007U,0));
    assert(!native_legacy_blank_state(3,3,0xff000000U));
    assert(!native_legacy_blank_state(0x80000007U,0x80000007U,0xffff0000U));
    assert(!native_legacy_blank_state(0x100,0x100,0));
    for(uint32_t updated=0;updated<16;updated++)
        for(uint32_t disabled=0;disabled<16;disabled++)
            assert(!native_legacy_blank_state(updated,disabled,0) || (updated && updated==disabled));
    {
        uint32_t frame[24];memset(frame,0xcc,sizeof(frame));
        assert(native_legacy_fill_pixels(frame,64,16,3,4,0xff000000U));
        for(unsigned row=0;row<4;++row) {
            for(unsigned column=0;column<3;++column)assert(frame[row*4+column]==0xff000000U);
            assert(frame[row*4+3]==0xcccccccc);
        }
        for(unsigned i=16;i<24;++i)assert(frame[i]==0xcccccccc);
        uint32_t before[24];memcpy(before,frame,sizeof(frame));
        assert(!native_legacy_fill_pixels((unsigned char *)frame+1,64,16,3,4,0));
        assert(!native_legacy_fill_pixels(frame,63,16,3,4,0));
        assert(!native_legacy_fill_pixels(frame,64,14,3,4,0));
        assert(!native_legacy_fill_pixels(frame,64,16,0,4,0));
        assert(!memcmp(before,frame,sizeof(frame)));
    }
    {
        unsigned char image[4*4*4];memset(image,0,sizeof(image));uint64_t scan[5];
        assert(native_legacy_frame_scan(image,sizeof(image),16,4,4,scan)&&!scan[0]&&scan[1]==UINT64_MAX&&scan[2]==UINT64_MAX&&!scan[3]&&!scan[4]);
        image[3]=0xff;image[3*16+2*4+3]=0xff; /* alpha alone is black */
        assert(native_legacy_frame_scan(image,sizeof(image),16,4,4,scan)&&!scan[0]);
        image[1*16+2*4+1]=1;image[3*16+0*4]=7;image[2*16+3*4+2]=9;
        assert(native_legacy_frame_scan(image,sizeof(image),16,4,4,scan)&&scan[0]==3&&scan[1]==0&&scan[2]==1&&scan[3]==3&&scan[4]==3);
        assert(!native_legacy_frame_scan(image,sizeof(image)-1,16,4,4,scan)&&!scan[0]);
        assert(!native_legacy_frame_scan(NULL,sizeof(image),16,4,4,scan));
    }
    {
        unsigned char image[4*4*4];uint32_t colors[4];uint64_t counts[4],other=99;
        for(unsigned i=0;i<4*4;++i){image[i*4]=1;image[i*4+1]=2;image[i*4+2]=3;image[i*4+3]=0xff;}
        assert(native_legacy_frame_colors(image,sizeof(image),16,4,4,colors,counts,&other)&&colors[0]==0xff030201U&&counts[0]==16&&!counts[1]&&!other);
        for(unsigned i=0;i<6;++i)image[i*4]=(unsigned char)(10+i); /* 6 distinct values + 10 original */
        assert(native_legacy_frame_colors(image,sizeof(image),16,4,4,colors,counts,&other));
        assert(counts[0]+counts[1]+counts[2]+counts[3]+other==16&&counts[0]==1&&counts[3]==1&&other==2+10);
        assert(!native_legacy_frame_colors(image,sizeof(image)-1,16,4,4,colors,counts,&other)&&!counts[0]&&!other);
    }
    assert(native_legacy_surface_bounds(800,600,3328,1998848,1998848));
    assert(native_legacy_surface_bounds(800,600,3200,1920000,1920000));
    assert(!native_legacy_surface_bounds(800,600,3199,1920000,1920000));
    assert(!native_legacy_surface_bounds(800,600,3328,1996799,1998848));
    assert(!native_legacy_surface_bounds(800,600,3328,1998848,1998847));
    assert(!native_legacy_surface_bounds(800,600,UINT64_MAX,UINT64_MAX,UINT64_MAX));
    assert(!native_legacy_surface_bounds(0,600,3328,1998848,1998848));
    for(unsigned offset=0;offset<8;++offset) {
        unsigned char storage[80]; uint32_t target[20];
        for(unsigned i=0;i<sizeof(storage);++i)storage[i]=(unsigned char)(i+1);
        memset(target,0xcc,sizeof(target));
        assert(native_legacy_copy_pixels(target,60,20,storage+offset,51,17,3,3));
        const unsigned char *result=(const unsigned char *)target;
        for(unsigned row=0;row<3;++row) {
            assert(!memcmp(result+row*20,storage+offset+row*17,12));
            for(unsigned i=12;i<20;++i)assert(result[row*20+i]==0xcc);
        }
        for(unsigned i=60;i<sizeof(target);++i)assert(result[i]==0xcc);
        uint32_t before[20];memcpy(before,target,sizeof(target));
        assert(!native_legacy_copy_pixels((unsigned char *)target+1,60,20,storage,51,17,3,3));
        assert(!native_legacy_copy_pixels(target,63,21,storage,51,17,3,3));
        assert(!native_legacy_copy_pixels(target,60,20,storage,50,17,3,3));
        assert(!native_legacy_copy_pixels(target,59,20,storage,51,17,3,3));
        assert(!native_legacy_copy_pixels(target,60,20,(const void *)(UINTPTR_MAX-16),51,17,3,3));
        assert(!memcmp(before,target,sizeof(target)));
    }
    return 0;
}
