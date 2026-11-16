/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_SHA256_H
#define NATIVE_ADRENO_SHA256_H
#include <stddef.h>
#include <stdint.h>
/* FIPS 180-4 SHA-256 for firmware and manifest validation inside the kernel.
 * One-shot only, no allocation, no I/O. Inputs may be empty; a null pointer is
 * accepted only for zero bytes. The digest is big-endian as specified. */
class NativeAdrenoSha256 final {
    NativeAdrenoSha256()=delete;
public:
    static void hash(const uint8_t *bytes,size_t length,uint8_t digest[32]);
};
#endif
