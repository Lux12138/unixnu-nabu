/* SPDX-License-Identifier: MIT */
#include "AdrenoPM4.h"

/* Add one bit to make the field's total population odd. */
static uint32_t odd_bit(uint32_t field)
{
    uint32_t parity = 1;
    while (field) {
        parity ^= 1;
        field &= field - 1;
    }
    return parity;
}

AdrenoPM4Status AdrenoPM4Type4(uint32_t reg, uint32_t count, uint32_t *header)
{
    if (!header || reg > 0x3ffff || !count || count > 127 ||
        count - 1 > 0x3ffff - reg)
        return AdrenoPM4InvalidArgument;
    *header = 0x40000000u | count | (odd_bit(count) << 7) |
        (reg << 8) | (odd_bit(reg) << 27);
    return AdrenoPM4Success;
}

AdrenoPM4Status AdrenoPM4Type7(uint32_t opcode, uint32_t count, uint32_t *header)
{
    if (!header || opcode > 127 || count > 0x3fff)
        return AdrenoPM4InvalidArgument;
    *header = 0x70000000u | count | (odd_bit(count) << 15) |
        (opcode << 16) | (odd_bit(opcode) << 23);
    return AdrenoPM4Success;
}

static AdrenoPM4Status decode(uint32_t word, AdrenoPM4Packet *packet)
{
    uint32_t rebuilt;
    AdrenoPM4Status status;
    packet->type = word >> 28;
    if (packet->type == 4) {
        packet->identifier = (word >> 8) & 0x3ffff;
        packet->payload_words = word & 127;
        status = AdrenoPM4Type4(packet->identifier, packet->payload_words, &rebuilt);
    } else if (packet->type == 7) {
        packet->identifier = (word >> 16) & 127;
        packet->payload_words = word & 0x3fff;
        status = AdrenoPM4Type7(packet->identifier, packet->payload_words, &rebuilt);
    } else {
        return AdrenoPM4InvalidHeader;
    }
    if (status != AdrenoPM4Success || rebuilt != word)
        return AdrenoPM4InvalidHeader;
    return AdrenoPM4Success;
}

AdrenoPM4Status AdrenoPM4Plan(const uint32_t *words, size_t word_count,
    AdrenoPM4Packet *output, size_t capacity, size_t *count)
{
    size_t at = 0, packets = 0;
    AdrenoPM4Packet packet;
    if (!count)
        return AdrenoPM4InvalidArgument;
    *count = 0;
    if ((word_count && !words) || (capacity && !output) ||
        word_count > SIZE_MAX / sizeof(*words) || capacity > SIZE_MAX / sizeof(*output))
        return AdrenoPM4InvalidArgument;
    while (at < word_count) {
        AdrenoPM4Status status = decode(words[at], &packet);
        if (status != AdrenoPM4Success)
            return status;
        /* Subtraction keeps both bounds checking and the next offset safe. */
        if (packet.payload_words > word_count - at - 1)
            return AdrenoPM4Truncated;
        at += 1 + packet.payload_words;
        ++packets;
    }
    if (packets > capacity)
        return AdrenoPM4Capacity;
    at = packets = 0;
    while (at < word_count) {
        (void)decode(words[at], &packet); /* Immutable input, already checked. */
        packet.header_word = at;
        output[packets++] = packet;
        at += 1 + packet.payload_words;
    }
    *count = packets;
    return AdrenoPM4Success;
}
