/* SPDX-License-Identifier: MIT */
#include "ArmSmmu4K.h"
#define PAGE_BYTES UINT64_C(4096)
#define ADDRESS_MASK UINT64_C(0x0000fffffffff000)

static ArmSmmu4KStatus build_region(const ArmSmmu4KConfig *config,
    const ArmSmmu4KRange *ranges, size_t range_count, uint64_t *tables,
    size_t capacity, ArmSmmu4KResult *result, unsigned upper)
{
    if (!result) return ArmSmmu4KArgument;
    result->root_physical = result->mair = 0;
    result->table_pages = result->leaf_pages = result->start_level = 0;
    result->upper_region = 0;
    if (!config || !ranges || !range_count || !tables || !capacity ||
        capacity > SIZE_MAX / PAGE_BYTES || range_count > SIZE_MAX / sizeof(*ranges) ||
        config->input_bits < 32 || config->input_bits > 48 ||
        !(config->output_bits == 32 || config->output_bits == 36 || config->output_bits == 40 ||
          config->output_bits == 42 || config->output_bits == 44 || config->output_bits == 48) ||
        !config->leaf_budget || config->leaf_budget > (SIZE_MAX - 1) / 3 ||
        ((uintptr_t)tables & 7)) return ArmSmmu4KArgument;
    if (range_count > config->leaf_budget) return ArmSmmu4KBudget;
    uint64_t input_limit = UINT64_C(1) << config->input_bits;
    uint64_t output_limit = UINT64_C(1) << config->output_bits;
    uint64_t pool_bytes = (uint64_t)capacity * PAGE_BYTES;
    uint64_t pool = config->table_physical;
    if ((pool & 4095) || pool >= output_limit || pool_bytes > output_limit - pool)
        return ArmSmmu4KBadRange;
    size_t leaves = 0;
    uint64_t previous_end = 0;
    for (size_t i = 0; i < range_count; ++i) {
        const ArmSmmu4KRange *r = &ranges[i];
        if (!r->bytes || ((r->iova | r->physical | r->bytes) & 4095) ||
            (!(r->permissions & ArmSmmu4KRead) || (r->permissions & ~15u)) || r->writeback > 1 || (r->shareability != 2 && r->shareability != 3) ||
            r->physical >= output_limit || r->bytes > output_limit - r->physical)
            return ArmSmmu4KBadRange;
        if (upper ? (r->iova < UINT64_MAX-input_limit+1 || r->bytes-1 > UINT64_MAX-r->iova) :
                    (r->iova >= input_limit || r->bytes > input_limit-r->iova))
            return ArmSmmu4KBadRange;
        if (i && r->iova <= previous_end) return ArmSmmu4KOverlap;
        previous_end = r->iova + (r->bytes-1);
        if (r->physical < pool + pool_bytes && pool < r->physical + r->bytes)
            return ArmSmmu4KOverlap;
        for (size_t j = 0; j < i; ++j) {
            if ((r->writeback != ranges[j].writeback || r->shareability != ranges[j].shareability || ((r->permissions ^ ranges[j].permissions) & ArmSmmu4KPrivileged)) && r->physical < ranges[j].physical + ranges[j].bytes &&
                ranges[j].physical < r->physical + r->bytes) return ArmSmmu4KOverlap;
        }
        uint64_t pages = r->bytes / PAGE_BYTES;
        if (pages > config->leaf_budget - leaves) return ArmSmmu4KBudget;
        leaves += (size_t)pages;
    }
    /* Each range has at least one leaf, so the validated leaf budget also
     * bounds the range/alias loops. Sorted IOVAs make prefix counting exact. */
    unsigned levels = (config->input_bits - 12 + 8) / 9;
    unsigned start = 4 - levels;
    size_t required = 1;
    uint64_t last[3] = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
    for (size_t i = 0; i < range_count; ++i) {
        for (uint64_t offset = 0; offset < ranges[i].bytes; offset += PAGE_BYTES) {
            uint64_t va = ranges[i].iova + offset;
            for (unsigned level = start; level < 3; ++level) {
                uint64_t prefix = va >> (39 - 9 * level);
                if (prefix != last[level]) { ++required; last[level] = prefix; }
            }
        }
    }
    if (required > capacity) return ArmSmmu4KCapacity;
    for (size_t i = 0; i < required * 512; ++i) tables[i] = 0;
    size_t next = 1;
    for (size_t i = 0; i < range_count; ++i) {
        const ArmSmmu4KRange *r = &ranges[i];
        uint64_t attributes = (1u << 11) | (1u << 10) | 3;
        if (!(r->permissions & ArmSmmu4KExecute)) attributes |= UINT64_C(3) << 53;
        if (!(r->permissions & ArmSmmu4KPrivileged)) attributes |= 1u << 6;
        if (!(r->permissions & ArmSmmu4KWrite)) attributes |= 1u << 7;
        attributes |= (uint64_t)r->shareability << 8;
        if (r->writeback) attributes |= 1u << 2;
        for (uint64_t offset = 0; offset < r->bytes; offset += PAGE_BYTES) {
            uint64_t va = r->iova + offset;
            size_t table = 0;
            for (unsigned level = start; level < 3; ++level) {
                unsigned shift = 39-9*level;
                unsigned width = level==start ? config->input_bits-shift : 9;
                size_t slot = table * 512 + ((va >> shift) & ((1u<<width)-1));
                if (!tables[slot]) tables[slot] = (pool + PAGE_BYTES * next++) | 3;
                table = (size_t)(((tables[slot] & ADDRESS_MASK) - pool) / PAGE_BYTES);
            }
            tables[table * 512 + ((va >> 12) & 511)] = (r->physical + offset) | attributes;
        }
    }
    result->root_physical = pool;
    result->mair = 0xff44;
    result->table_pages = required;
    result->leaf_pages = leaves;
    result->start_level = start;
    result->upper_region = upper;
    return ArmSmmu4KSuccess;
}

ArmSmmu4KStatus ArmSmmu4KBuild(const ArmSmmu4KConfig *config,
    const ArmSmmu4KRange *ranges, size_t range_count, uint64_t *tables,
    size_t capacity, ArmSmmu4KResult *result)
{
    return build_region(config,ranges,range_count,tables,capacity,result,0);
}

ArmSmmu4KStatus ArmSmmu4KBuildUpper(const ArmSmmu4KConfig *config,
    const ArmSmmu4KRange *ranges, size_t range_count, uint64_t *tables,
    size_t capacity, ArmSmmu4KResult *result)
{
    return build_region(config,ranges,range_count,tables,capacity,result,1);
}
