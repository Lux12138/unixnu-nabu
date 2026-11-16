/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_FIRMWARE_H
#define NATIVE_ADRENO_FIRMWARE_H
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>
struct NativeAdrenoFirmwareRules {
    uint64_t bytes;
    uint8_t sha256[32];
};
struct NativeAdrenoFirmwareView {
    const uint8_t *bytes;
    size_t length;
    uint8_t sha256[32];
};
/* Validated view of an in-image firmware blob. The exact byte count is checked
 * before any hashing, then SHA-256 must match. Refusal never returns a view and
 * never copies the image; the caller keeps ownership and lifetime. This proves
 * the bytes handed to the GMU loader are the pinned ones; it does not prove the
 * payload is loadable or that a GMU exists. */
class NativeAdrenoFirmware final {
    NativeAdrenoFirmware()=delete;
public:
    static IOReturn validate(const uint8_t *image,uint64_t length,
        const NativeAdrenoFirmwareRules *rules,NativeAdrenoFirmwareView *view);
};
#endif
