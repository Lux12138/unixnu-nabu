/* Portable on-medium contract. No allocation, transport, or platform headers. */
#ifndef NABU_LOG_FORMAT_H
#define NABU_LOG_FORMAT_H
#include <stdint.h>
#define NB_LOG_BLOCK 4096u
#define NB_LOG_CAPACITY (4u * 1024u * 1024u)
#define NB_LOG_SLOT_BYTES (NB_LOG_BLOCK + NB_LOG_CAPACITY)
#define NB_LOG_FILE_BYTES (NB_LOG_BLOCK + 2u * NB_LOG_SLOT_BYTES)
#define NB_LOG_NAME "XNU2LOG BIN"
static inline uint32_t nb_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static inline uint64_t nb_le64(const uint8_t *p) { return nb_le32(p) | ((uint64_t)nb_le32(p+4)<<32); }
static inline void nb_put32(uint8_t *p, uint32_t n) {
  for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(n>>(8*i));
}
static inline void nb_put64(uint8_t *p, uint64_t n) {
  nb_put32(p,(uint32_t)n); nb_put32(p+4,(uint32_t)(n>>32));
}
static inline int nb_equal(const void *a, const void *b, unsigned n) {
  const uint8_t *x=(const uint8_t *)a,*y=(const uint8_t *)b;
  for (unsigned i=0;i<n;i++) if(x[i]!=y[i]) return 0;
  return 1;
}
static inline void nb_copy(void *a, const void *b, unsigned n) {
  uint8_t *x=(uint8_t *)a; const uint8_t *y=(const uint8_t *)b;
  for(unsigned i=0;i<n;i++) x[i]=y[i];
}
static inline void nb_zero(void *p,unsigned n) {
  uint8_t *x=(uint8_t *)p; for(unsigned i=0;i<n;i++) x[i]=0;
}
static inline uint32_t nb_crc_update(uint32_t crc,const uint8_t *p,unsigned n) {
  for(unsigned i=0;i<n;i++) { crc^=p[i]; for(unsigned b=0;b<8;b++) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u))); }
  return crc;
}
static inline uint32_t nb_crc(const void *p,unsigned n) { return ~nb_crc_update(~0u,(const uint8_t *)p,n); }
static inline uint32_t nb_header_crc(const uint8_t *p,unsigned zero_at) {
  uint32_t crc=~0u; uint8_t zero[4]={0};
  crc=nb_crc_update(crc,p,zero_at); crc=nb_crc_update(crc,zero,4);
  return ~nb_crc_update(crc,p+zero_at+4,NB_LOG_BLOCK-zero_at-4);
}
/* Config: magic, version, header bytes, capacity, slot/file bytes, zero,
 * token[16], logical block bytes, zero, CRC32 at 56, reserved zeros. */
static inline int nb_config_valid(const uint8_t *p,const uint8_t *token) {
  if(!nb_equal(p,"NABUCFG2",8) || nb_le32(p+8)!=2 || nb_le32(p+12)!=NB_LOG_BLOCK ||
     nb_le32(p+16)!=NB_LOG_CAPACITY || nb_le32(p+20)!=NB_LOG_SLOT_BYTES ||
     nb_le32(p+24)!=NB_LOG_FILE_BYTES || nb_le32(p+28) || nb_le32(p+48)!=NB_LOG_BLOCK ||
     nb_le32(p+52) || nb_le32(p+56)!=nb_header_crc(p,56)) return 0;
  if(token && !nb_equal(token,p+32,16)) return 0;
  for(unsigned i=60;i<NB_LOG_BLOCK;i++) if(p[i]) return 0;
  return 1;
}
/* Slot: magic, version, header bytes, committed=1, capacity, sequence u64,
 * boot_id[16], committed stream end u64, lock-lost u64, first u64,
 * length u32, payload CRC u32, header CRC u32, zero, ring-overwritten u64,
 * allocation token[16], build identity[32], reserved zeros. */
static inline int nb_slot_valid(const uint8_t *p,const uint8_t *token) {
  if(!nb_equal(p,"NABULOG2",8) || nb_le32(p+8)!=2 || nb_le32(p+12)!=NB_LOG_BLOCK ||
     nb_le32(p+16)!=1 || nb_le32(p+20)!=NB_LOG_CAPACITY || !nb_le64(p+24) ||
     nb_le32(p+72)>NB_LOG_CAPACITY || nb_le32(p+84) ||
     nb_le32(p+80)!=nb_header_crc(p,80) || !nb_equal(token,p+96,16)) return 0;
  uint64_t end=nb_le64(p+48),first=nb_le64(p+64);
  uint32_t n=nb_le32(p+72);
  if(first>end || end-first!=n || n!=(end<NB_LOG_CAPACITY?end:NB_LOG_CAPACITY) || nb_le64(p+88)!=first) return 0;
  for(unsigned i=144;i<NB_LOG_BLOCK;i++) if(p[i]) return 0;
  return 1;
}
#endif
