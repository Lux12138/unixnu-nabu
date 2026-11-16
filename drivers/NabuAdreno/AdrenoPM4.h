/* SPDX-License-Identifier: MIT */
#ifndef ADRENO_PM4_H
#define ADRENO_PM4_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AdrenoPM4Success = 0,
    AdrenoPM4InvalidArgument,
    AdrenoPM4InvalidHeader,
    AdrenoPM4Truncated,
    AdrenoPM4Capacity
} AdrenoPM4Status;

typedef struct {
    size_t header_word;
    uint32_t type;          /* 4 (register sequence) or 7 (opcode). */
    uint32_t identifier;    /* Register index or opcode; NOT authorization. */
    uint32_t payload_words;
} AdrenoPM4Packet;

/* Supported emission profile: 18-bit registers, 1..127 values; 7-bit
 * opcodes, 0..16383 values. No silent masking. Failure preserves *header. */
AdrenoPM4Status AdrenoPM4Type4(uint32_t reg, uint32_t count, uint32_t *header);
AdrenoPM4Status AdrenoPM4Type7(uint32_t opcode, uint32_t count, uint32_t *header);

/* Structural decoding only. A successful plan DOES NOT authorize register
 * writes, addresses, opcodes, indirect buffers or hardware submission.
 * Input is an immutable array of logical 32-bit words (little-endian on
 * the target). All extents must be real caller-owned accessible storage.
 * Input, output and count must not alias; input must remain immutable.
 * Validate the whole input and capacity before writing any packet record.
 * On failure, *count=0 and output is untouched. Empty input succeeds.
 * No allocation, recursion, payload interpretation, MMIO or completion. */
AdrenoPM4Status AdrenoPM4Plan(const uint32_t *words, size_t word_count,
    AdrenoPM4Packet *output, size_t capacity, size_t *count);
#ifdef __cplusplus
}
#endif
#endif
