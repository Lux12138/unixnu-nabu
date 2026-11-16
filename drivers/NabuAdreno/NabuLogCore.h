/* Compiled into serial_console.c only for the explicit logging build option.
 * A producer attempts the lock once, drops/counts on contention or reentry, and
 * holds it for at most 256 bytes. No mutex, allocator, I/O, or console calls.
 * This deliberately prefers measurable loss to waiting on a halted producer.
 */
#ifndef NABU_LOG_CORE_H
#define NABU_LOG_CORE_H
#include "NabuLogFormat.h"
#ifdef __cplusplus
extern "C" {
#endif
struct nb_log_snapshot { uint64_t end, first, lost; uint32_t length; };
void nabu_capture_append(const char *,unsigned);
int nabu_capture_snapshot(struct nb_log_snapshot *);
int nabu_capture_copy(uint64_t,unsigned,uint8_t *);
/* Optional owner-probe callbacks. Disabled builds supply no GPU dependency;
 * the writer invokes only a registered bounded RAM-only snapshot callback. */
extern void (*nabu_log_probe_poll)(void);
extern void (*nabu_log_probe_process_snapshot)(uint32_t *);
#ifdef __cplusplus
}
#endif
#ifdef NABU_LOG_IMPLEMENTATION
static uint8_t nabu_capture_ring[NB_LOG_CAPACITY];
static unsigned nabu_capture_lock;
static uint64_t nabu_capture_end, nabu_capture_lost;
void (*nabu_log_probe_poll)(void)=0;
void (*nabu_log_probe_process_snapshot)(uint32_t *)=0;
static int nb_capture_try(void) {
  unsigned expected=0;
  return __atomic_compare_exchange_n(&nabu_capture_lock,&expected,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);
}
static void nb_capture_unlock(void) { __atomic_store_n(&nabu_capture_lock,0,__ATOMIC_RELEASE); }
void nabu_capture_append(const char *p,unsigned n) {
  if(!p) return;
  while(n) {
    unsigned chunk=n>256?256:n;
    if(!nb_capture_try()) { __atomic_fetch_add(&nabu_capture_lost,(uint64_t)n,__ATOMIC_RELAXED); return; }
    for(unsigned i=0;i<chunk;i++) nabu_capture_ring[(nabu_capture_end+i)&(NB_LOG_CAPACITY-1)]=(uint8_t)p[i];
    nabu_capture_end+=chunk;
    nb_capture_unlock(); p+=chunk; n-=chunk;
  }
}
int nabu_capture_snapshot(struct nb_log_snapshot *s) {
  if(!nb_capture_try()) return 0;
  s->end=nabu_capture_end;
  s->length=s->end<NB_LOG_CAPACITY?(uint32_t)s->end:NB_LOG_CAPACITY;
  s->first=s->end-s->length;
  s->lost=__atomic_load_n(&nabu_capture_lost,__ATOMIC_RELAXED);
  nb_capture_unlock(); return 1;
}
int nabu_capture_copy(uint64_t at,unsigned n,uint8_t *out) {
  if(n>1024 || !nb_capture_try()) return 0;
  uint64_t oldest=nabu_capture_end>NB_LOG_CAPACITY?nabu_capture_end-NB_LOG_CAPACITY:0;
  int ok=at>=oldest && at<=nabu_capture_end && n<=nabu_capture_end-at;
  if(ok) for(unsigned i=0;i<n;i++) out[i]=nabu_capture_ring[(at+i)&(NB_LOG_CAPACITY-1)];
  nb_capture_unlock(); return ok;
}
#endif
#endif
