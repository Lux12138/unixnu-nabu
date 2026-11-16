// SPDX-License-Identifier: Apache-2.0
// See NvtProtocol.h and NOTICE.
#include "NvtProtocol.h"
#include <string.h>

#define EVENT_HOST_CMD 0x50
#define EVENT_RESET_COMPLETE 0x60
#define RESET_STATE_INIT 0xa0
#define RESET_STATE_MAX 0xaf
#define HOST_CMD_ENABLE_FW_CRC 0xae

static const struct nvt_map map_single = {
    .name="NT36532", .event_buf=0x125800, .boot_rdy=0x1FB50D, .tx_auto_copy_en=0x1FC925, .spi_dma_tx_info=0x1FC914,
    .ilm_des=0x1FB528, .ilm_length=0x1FB518, .dlm_des=0x1FB52C, .dlm_length=0x1FB530,
    .g_ilm_checksum=0x1FB500, .g_dlm_checksum=0x1FB504, .r_ilm_checksum=0x1FB520, .r_dlm_checksum=0x1FB524,
    .bld_ilm_dlm_crc=0x1FB533, .enb_casc={0x1FB12C,0x01}};
static const struct nvt_map map_cascade = {
    .name="NT36532-cascade", .event_buf=0x125800, .boot_rdy=0x1FB50D, .tx_auto_copy_en=0x1FC925, .spi_dma_tx_info=0x1FC914,
    .ilm_des=0x1FB528, .ilm_length=0x1FB518, .dlm_des=0x1FB52C, .dlm_length=0x1FB530,
    .g_ilm_checksum=0x1FB500, .g_dlm_checksum=0x1FB504, .r_ilm_checksum=0x1FB520, .r_dlm_checksum=0x1FB524,
    .bld_ilm_dlm_crc=0x1FB533, .enb_casc={0x1FB12C,0x01}};
static const struct nvt_map map_e_cascade = {
    .name="NT36532E-cascade", .event_buf=0x11C400, .boot_rdy=0x1FB50D, .tx_auto_copy_en=0x1FC925, .spi_dma_tx_info=0x1FC914,
    .ilm_des=0x1FB528, .ilm_length=0x1FB518, .dlm_des=0x1FB52C, .dlm_length=0x1FB530,
    .g_ilm_checksum=0x1FB500, .g_dlm_checksum=0x1FB504, .r_ilm_checksum=0x1FB520, .r_dlm_checksum=0x1FB524,
    .bld_ilm_dlm_crc=0x1FB533, .enb_casc={0x1FB12C,0x01}};

static int bus_write(struct nvt_chip *c, size_t len) {   // scratch[0..len) already holds [offset, data...]
    if (len > NVT_TRANSFER_LEN+1) return NVT_ERR_PARAM;
    c->scratch[0] |= 0x80;
    return c->bus->xfer(c->bus->ctx, c->scratch, NULL, len) ? NVT_ERR_BUS : 0;
}
static void delay(struct nvt_chip *c, unsigned ms) {if (c->bus->delay_ms) c->bus->delay_ms(c->bus->ctx, ms);}

int nvt_set_page(struct nvt_chip *c, uint32_t addr) {
    c->scratch[0]=0xff;c->scratch[1]=(uint8_t)((addr>>15)&0xff);c->scratch[2]=(uint8_t)((addr>>7)&0xff);
    return bus_write(c,3);
}
int nvt_write(struct nvt_chip *c, uint32_t addr, const uint8_t *data, size_t len) {
    if (!len || len > NVT_TRANSFER_LEN) return NVT_ERR_PARAM;
    int r=nvt_set_page(c,addr);if (r) return r;
    c->scratch[0]=(uint8_t)(addr&0x7f);memcpy(c->scratch+1,data,len);
    return bus_write(c,len+1);
}
int nvt_write_byte(struct nvt_chip *c, uint32_t addr, uint8_t value) {return nvt_write(c,addr,&value,1);}
// Reads: tx = [offset, dummy, 0...], rx data starts at byte 2 (one address phase + one dummy byte).
static int read_at(struct nvt_chip *c, uint32_t offset7, uint8_t *out, size_t len) {
    if (!len || len > NVT_READ_MAX) return NVT_ERR_PARAM;
    memset(c->scratch,0,len+2);c->scratch[0]=(uint8_t)(offset7&0x7f);
    if (c->bus->xfer(c->bus->ctx,c->scratch,c->rxbuf,len+2)) return NVT_ERR_BUS;
    memcpy(out,c->rxbuf+2,len);return 0;
}
int nvt_read(struct nvt_chip *c, uint32_t addr, uint8_t *out, size_t len) {
    int r=nvt_set_page(c,addr);if (r) return r;
    return read_at(c,addr&0x7f,out,len);
}

uint32_t nvt_crc32_words(const uint8_t *data, size_t len, size_t avail) {
    uint32_t checksum=0xffffffffu;size_t words=(len+4)/4;
    for (size_t i=0;i<words;++i) {
        uint32_t w=0;
        for (unsigned b=0;b<4;++b) {size_t at=i*4+b;if (at<avail) w|=(uint32_t)data[at]<<(8*b);}
        checksum^=w;
        for (unsigned j=0;j<32;++j) checksum=(checksum&0x80000000u)?(checksum<<1)^0x04c11db7u:(checksum<<1);
    }
    return checksum;
}
static int word_at(const uint8_t *d,size_t size,size_t pos,uint32_t *out) {
    if (pos>size || size-pos<4) return NVT_ERR_FW;
    *out=(uint32_t)d[pos]|((uint32_t)d[pos+1]<<8)|((uint32_t)d[pos+2]<<16)|((uint32_t)d[pos+3]<<24);return 0;
}

int nvt_fw_parse(struct nvt_fw *fw, const uint8_t *d, size_t size) {
    memset(fw,0,sizeof(*fw));fw->data=d;fw->size=size;
    uint32_t end;
    if (size<0x40 || word_at(d,size,0,&end) || end<0x30 || end>size) return NVT_ERR_FW;
    fw->cascade_2nd_header=(d[0x20]&0x02)>>1;
    unsigned info_sec_num=0;uint32_t pos=0x30;
    if (fw->cascade_2nd_header) {
        while (pos<end/2) {++info_sec_num;pos+=0x10;}
        info_sec_num+=1;
    } else {
        while (pos<end) {++info_sec_num;pos+=0x10;}
    }
    unsigned ovly_info=(d[0x28]&0x10)>>4;
    unsigned ovly_sec_num=ovly_info?(d[0x28]&0x0f):0;
    const unsigned ilm_dlm_num=2;
    unsigned partition=ilm_dlm_num+ovly_sec_num+info_sec_num;
    if (partition>NVT_MAX_PARTITIONS) return NVT_ERR_FW;
    fw->count=partition;
    int find_bin_header=0;
    for (unsigned list=0;list<partition;++list) {
        struct nvt_fw_partition *p=&fw->part[list];
        if (list<ilm_dlm_num) {
            if (word_at(d,size,0+list*12,&p->bin_addr)||word_at(d,size,4+list*12,&p->sram_addr)||word_at(d,size,8+list*12,&p->size)) return NVT_ERR_FW;
            if ((uint64_t)p->bin_addr+p->size>size) return NVT_ERR_FW;
            p->crc=nvt_crc32_words(d+p->bin_addr,p->size,size-p->bin_addr);
            memcpy(p->name,list==0?"ILM":"DLM",4);
        } else if (list<ilm_dlm_num+info_sec_num) {
            if (!find_bin_header) pos=0x30+0x10*(list-ilm_dlm_num);
            else if (fw->cascade_2nd_header) pos=end-0x10;
            if (word_at(d,size,pos,&p->sram_addr)||word_at(d,size,pos+4,&p->size)||word_at(d,size,pos+8,&p->bin_addr)||word_at(d,size,pos+12,&p->crc)) return NVT_ERR_FW;
            if (p->bin_addr<end && p->size!=0) {memcpy(p->name,"Header",7);find_bin_header=1;}
            else {p->name[0]='I';p->name[1]='n';p->name[2]='f';p->name[3]='o';p->name[4]='-';p->name[5]=(char)('0'+((list-ilm_dlm_num)%10));p->name[6]=0;}
        } else {
            pos=fw->part[1].bin_addr+0x10*(list-ilm_dlm_num-info_sec_num);
            if (word_at(d,size,pos,&p->sram_addr)||word_at(d,size,pos+4,&p->size)||word_at(d,size,pos+8,&p->bin_addr)||word_at(d,size,pos+12,&p->crc)) return NVT_ERR_FW;
            p->name[0]='O';p->name[1]='v';p->name[2]='l';p->name[3]='y';p->name[4]=0;
        }
        if ((uint64_t)p->bin_addr+p->size>size) return NVT_ERR_FW;
    }
    return 0;
}

static int write_sram(struct nvt_chip *c, const uint8_t *fw, uint32_t sram, uint32_t size, uint32_t bin) {
    while (size) {
        uint32_t len=size<NVT_TRANSFER_LEN?size:NVT_TRANSFER_LEN;
        int r=nvt_write(c,sram,fw+bin,len);if (r) return r;
        sram+=NVT_TRANSFER_LEN;bin+=NVT_TRANSFER_LEN;
        if (size<=NVT_TRANSFER_LEN) break;
        size-=NVT_TRANSFER_LEN;
    }
    return 0;
}
static int write_firmware(struct nvt_chip *c, const struct nvt_fw *fw) {
    for (unsigned i=0;i<fw->count;++i) {
        const struct nvt_fw_partition *p=&fw->part[i];
        if (!p->size) continue;
        // The vendor flow writes size+1 bytes per partition; the extra byte never leaves the firmware image.
        uint32_t size=p->size+1;
        if ((uint64_t)p->bin_addr+size>fw->size) size=p->size;
        int r=write_sram(c,fw->data,p->sram_addr,size,p->bin_addr);if (r) return r;
    }
    return 0;
}
static int write_3(struct nvt_chip *c, uint32_t addr, uint32_t v) {
    uint8_t b[3]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16)};return nvt_write(c,addr,b,3);
}
static int write_4(struct nvt_chip *c, uint32_t addr, uint32_t v) {
    uint8_t b[4]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)};return nvt_write(c,addr,b,4);
}
static int bootloader_reset(struct nvt_chip *c) {
    int r=nvt_write_byte(c,c->swrst_sif,0x69);delay(c,5);return r;
}
static int set_bld_crc(struct nvt_chip *c, const struct nvt_fw *fw) {
    const struct nvt_map *m=c->map;int r;
    if ((r=write_3(c,m->ilm_des,fw->part[0].sram_addr))||(r=write_3(c,m->ilm_length,fw->part[0].size))||(r=write_4(c,m->g_ilm_checksum,fw->part[0].crc))) return r;
    if ((r=write_3(c,m->dlm_des,fw->part[1].sram_addr))||(r=write_3(c,m->dlm_length,fw->part[1].size))||(r=write_4(c,m->g_dlm_checksum,fw->part[1].crc))) return r;
    return 0;
}
static int wait_auto_copy(struct nvt_chip *c) {
    for (unsigned i=0;i<200;++i) {
        uint8_t v=0xff;int r=nvt_read(c,c->map->tx_auto_copy_en,&v,1);if (r) return r;
        c->dl_wait_value=v;c->dl_polls=i+1;
        if (!v) return 0;
        delay(c,1);
    }
    return NVT_ERR_TIMEOUT;
}
static int fw_crc_enable(struct nvt_chip *c) {
    uint8_t clear[7]={0};int r;
    if ((r=nvt_write(c,c->map->event_buf+EVENT_RESET_COMPLETE,clear,sizeof(clear)))) return r;
    return nvt_write_byte(c,c->map->event_buf+EVENT_HOST_CMD,HOST_CMD_ENABLE_FW_CRC);
}
static int check_reset_state(struct nvt_chip *c) {
    unsigned max=c->reset_retries?c->reset_retries:10;
    for (unsigned retry=0;retry<=max;++retry) {
        uint8_t b[6]={0};int r=nvt_read(c,c->map->event_buf+EVENT_RESET_COMPLETE,b,5);if (r) return r;
        memcpy(c->dl_reset_state,b,5);c->dl_polls=retry+1;
        if (b[0]>=RESET_STATE_INIT && b[0]<=RESET_STATE_MAX) return 0;
        delay(c,10);
    }
    return NVT_ERR_TIMEOUT;
}

int nvt_identify(struct nvt_chip *c) {
    c->chip_ver_trim=0x1FB104;c->swrst_sif=0x1FB43E;c->hw_crc_len=3;
    // Same sequence as nvt_ts_probe / nvt_ts_check_chip_ver_trim in the sheng mainline port (nt36xxx.c): one engine reset
    // (ENG_RST_ADDR 0x7FFF80 <- 0x5A, then the POR delays), and up to five attempts of bootloader reset, page select, a
    // 7-byte address write and the 6-byte id read.
    int r=nvt_write_byte(c,0x7FFF80,0x5A);if (r) return r;
    delay(c,1);delay(c,10);
    uint8_t id[6]={0};
    int matched=0;
    for (unsigned retry=0;retry<5 && !matched;++retry) {
        if ((r=bootloader_reset(c))) return r;
        if ((r=nvt_set_page(c,c->chip_ver_trim))) return r;
        memset(c->scratch,0,7);c->scratch[0]=(uint8_t)(c->chip_ver_trim&0x7f);
        if ((r=bus_write(c,7))) return r;
        if ((r=read_at(c,c->chip_ver_trim&0x7f,id,6))) return r;
        memcpy(c->id,id,6);
        matched=(id[3]==0x32 && id[4]==0x65 && id[5]==0x03);
        if (!matched) delay(c,10);
    }
    int matches_e=(id[0]==0x0e && id[3]==0x32 && id[4]==0x65 && id[5]==0x03);
    if (!matched) return NVT_ERR_CHIP;
    uint8_t casc=0;
    if ((r=nvt_read(c,map_single.enb_casc.addr,&casc,1))) return r;
    // nvt_ts_check_chip_ver_trim: ENB_CASC bit set = "Single Chip" (mmap), clear = "Cascade Chip" (mmap_casc).
    c->cascade=(casc&map_single.enb_casc.mask)==0;
    if (matches_e) c->map=c->cascade?&map_e_cascade:&map_single;
    else c->map=c->cascade?&map_cascade:&map_single;
    return 0;
}

int nvt_read_bld_crc(struct nvt_chip *c, const struct nvt_fw *fw) {
    if (!c->map || fw->count<2) return NVT_ERR_PARAM;
    uint8_t b[4]={0};int r;
    if ((r=nvt_read(c,c->map->bld_ilm_dlm_crc,b,1))) return r;
    c->crc_flags=b[0];
    const uint32_t g[2]={c->map->g_ilm_checksum,c->map->g_dlm_checksum},res[2]={c->map->r_ilm_checksum,c->map->r_dlm_checksum};
    for (unsigned i=0;i<2;++i) {
        if ((r=nvt_read(c,g[i],b,4))) return r;
        c->crc_golden[i]=(uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);
        if ((r=nvt_read(c,res[i],b,4))) return r;
        c->crc_result[i]=(uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);
        c->crc_bin[i]=fw->part[i].crc;
    }
    c->crc_valid=1;
    return 0;
}

static uint32_t crc_word(uint32_t checksum, uint32_t w) {
    checksum^=w;
    for (unsigned j=0;j<32;++j) checksum=(checksum&0x80000000u)?(checksum<<1)^0x04c11db7u:(checksum<<1);
    return checksum;
}
// Read ILM and DLM back completely: count differing bytes against the image and recompute the CRC over what the chip holds.
static int verify_sram_full(struct nvt_chip *c, const struct nvt_fw *fw) {
    uint8_t b[4096];
    for (unsigned p=0;p<2;++p) {
        const struct nvt_fw_partition *part=&fw->part[p];
        size_t total=((part->size+4)/4)*4;                   // the CRC covers (size+4)/4 words
        uint32_t crc=0xffffffffu,word=0;unsigned nb=0;
        c->sram_first_bad[p]=-1;c->sram_bad[p]=0;
        for (size_t off=0;off<total;off+=sizeof b) {
            size_t n=total-off<sizeof b?total-off:sizeof b;
            int r=nvt_read(c,part->sram_addr+(uint32_t)off,b,n);if (r) return r;
            for (size_t i=0;i<n;++i) {
                size_t at=off+i;
                if (at<part->size && b[i]!=fw->data[part->bin_addr+at]) {
                    if (c->sram_first_bad[p]<0) c->sram_first_bad[p]=(int32_t)at;
                    c->sram_bad[p]++;
                }
                word|=(uint32_t)b[i]<<(8*nb);
                if (++nb==4) {crc=crc_word(crc,word);word=0;nb=0;}
            }
        }
        c->sram_crc[p]=crc;
    }
    return 0;
}

int nvt_verify_sram(struct nvt_chip *c, const struct nvt_fw *fw) {
    if (!c->map || fw->count<2) return NVT_ERR_PARAM;
    (void)verify_sram_full(c,fw);
    struct {uint32_t sram,bin;} w[3]={
        {fw->part[0].sram_addr,fw->part[0].bin_addr},
        {fw->part[0].sram_addr+fw->part[0].size-32,fw->part[0].bin_addr+fw->part[0].size-32},
        {fw->part[1].sram_addr,fw->part[1].bin_addr}};
    uint8_t b[32];
    for (unsigned i=0;i<3;++i) {
        int r=nvt_read(c,w[i].sram,b,sizeof b);if (r) return r;
        c->sram_ok[i]=memcmp(b,fw->data+w[i].bin,sizeof b)==0;
    }
    c->sram_valid=1;
    return 0;
}

int nvt_download_firmware(struct nvt_chip *c, const struct nvt_fw *fw) {
    if (!c->map || fw->count<2 || !fw->part[0].size || !fw->part[1].size) return NVT_ERR_PARAM;
    int ret=0;
    for (unsigned attempt=0;attempt<3;++attempt) {
        c->dl_attempt=attempt+1;
#define NVT_STAGE(n,expr) do {c->dl_stage=(n);if ((ret=(expr))) {c->dl_rc=ret;goto next;}} while (0)
        NVT_STAGE(1,bootloader_reset(c));
        NVT_STAGE(2,set_bld_crc(c,fw));
        if (fw->cascade_2nd_header) {
            NVT_STAGE(3,nvt_write_byte(c,c->map->tx_auto_copy_en,0x56));
            NVT_STAGE(4,write_firmware(c,fw));
            NVT_STAGE(5,wait_auto_copy(c));
        } else NVT_STAGE(4,write_firmware(c,fw));
        NVT_STAGE(6,fw_crc_enable(c));
        NVT_STAGE(7,nvt_write_byte(c,c->map->boot_rdy,1));
        delay(c,5);
        NVT_STAGE(8,check_reset_state(c));
        c->dl_stage=0;
        return 0;
next:   ;
#undef NVT_STAGE
    }
    (void)nvt_read_bld_crc(c,fw);      // evidence for the failure, as the vendor driver reports it
    (void)nvt_verify_sram(c,fw);
    return ret?ret:NVT_ERR_TIMEOUT;
}

int nvt_read_frame(struct nvt_chip *c, uint8_t *frame) {
    // The vendor driver's thp_frame has the zero offset byte of CTP_SPI_READ at index 0 and the chip's data from index 1
    // (nvt_thp_read_frame -> CTP_SPI_READ(thp_frame, NVT_THP_FRAME_LEN) copies rbuf+2 into thp_frame+1), and every THP
    // header offset below is defined on that buffer. Reproduce the layout: hardware showed a valid header only one byte earlier.
    frame[0]=0;
    size_t total=NVT_THP_FRAME_LEN-1,done=0;
    while (done<total) {
        size_t n=total-done;if (n>NVT_READ_MAX) n=NVT_READ_MAX;
        int r=nvt_read(c,c->map->event_buf+(uint32_t)done,frame+1+done,n);if (r) return r;
        done+=n;
    }
    return 0;
}
void nvt_frame_matrix(const uint8_t *frame, int16_t *matrix) {
    for (int i=0;i<2400;++i) matrix[i]=(int16_t)((uint16_t)frame[NVT_THP_MATRIX_OFFSET+i*2]|((uint16_t)frame[NVT_THP_MATRIX_OFFSET+i*2+1]<<8));
}
unsigned nvt_frame_type(const uint8_t *frame) {return frame[NVT_THP_HEADER_LEN+24];}
unsigned nvt_frame_counter(const uint8_t *frame) {return (unsigned)frame[NVT_THP_HEADER_LEN+2]|((unsigned)frame[NVT_THP_HEADER_LEN+3]<<8);}

int nvt_get_fw_info(struct nvt_chip *c, struct nvt_fw_info *info) {
    uint8_t b[38];
    memset(info,0,sizeof *info);
    for (int attempt=0;attempt<4;++attempt) {
        int r=nvt_read(c,c->map->event_buf+NVT_EVENT_FWINFO,b,sizeof b);if (r) return r;
        if ((uint8_t)(b[0]+b[1])!=0xff) continue;
        info->fw_ver=b[0];info->x_num=b[2];info->y_num=b[3];
        info->abs_x_max=(uint16_t)((b[4]<<8)|b[5]);info->abs_y_max=(uint16_t)((b[6]<<8)|b[7]);
        info->fw_type=b[13];info->pid=(uint16_t)((b[35]<<8)|b[34]);
        return 0;
    }
    info->defaulted=1;
    return NVT_ERR_FW;
}
int nvt_set_custom_cmd(struct nvt_chip *c, uint8_t cmd, uint16_t value) {
    delay(c,35);
    for (int tries=0;tries<5;++tries) {
        uint8_t b[5]={0xbf,cmd,0x00,(uint8_t)value,(uint8_t)(value>>8)};
        int r=nvt_write(c,c->map->event_buf+NVT_EVENT_HOST_CMD,b,sizeof b);if (r) return r;
        delay(c,20);
        uint8_t st=0xff;r=nvt_read(c,c->map->event_buf+NVT_EVENT_HOST_CMD,&st,1);if (r) return r;
        if (!st) return 0;
    }
    return NVT_ERR_TIMEOUT;
}
int nvt_set_doze_delay(struct nvt_chip *c, uint16_t value) {
    uint8_t b[2]={(uint8_t)value,(uint8_t)(value>>8)};
    return nvt_write(c,NVT_DOZE_DELAY_ADDR,b,sizeof b);
}
int nvt_start_scanning(struct nvt_chip *c, struct nvt_fw_info *info) {
    int r=nvt_get_fw_info(c,info);                 // a bad info block is reported but does not stop the sequence
    int info_err=r;
    if (r==NVT_ERR_BUS) return r;
    if ((r=nvt_set_custom_cmd(c,0x19,0x00))) return r;   // idle baseline update
    if ((r=nvt_set_doze_delay(c,120))) return r;
    if ((r=nvt_set_custom_cmd(c,0x01,0x02))) return r;   // doze mode
    return info_err;
}
int nvt_peek_valid(const uint8_t *h) {
    uint16_t crc=(uint16_t)(h[4]|(h[5]<<8)),inv=(uint16_t)(h[12]|(h[13]<<8));
    uint32_t mk=(uint32_t)h[8]|((uint32_t)h[9]<<8)|((uint32_t)h[10]<<16)|((uint32_t)h[11]<<24);
    uint32_t mi=(uint32_t)h[16]|((uint32_t)h[17]<<8)|((uint32_t)h[18]<<16)|((uint32_t)h[19]<<24);
    return inv==(uint16_t)~crc && mi==~mk;
}
int nvt_frame_header_valid(const uint8_t *frame) {return nvt_peek_valid(frame+NVT_THP_HEADER_LEN);}
unsigned nvt_peek_counter(const uint8_t *h) {return (unsigned)h[2]|((unsigned)h[3]<<8);}
unsigned nvt_peek_type(const uint8_t *h) {return h[24];}
int nvt_peek_frame_header(struct nvt_chip *c, uint8_t *hdr) {
    // frame[i] = chip byte i-1 (see nvt_read_frame), so frame[NVT_THP_HEADER_LEN] is chip byte NVT_THP_HEADER_LEN-1.
    return nvt_read(c,c->map->event_buf+NVT_THP_HEADER_LEN-1,hdr,NVT_THP_PEEK_LEN);
}
