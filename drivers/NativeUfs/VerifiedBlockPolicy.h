// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef NATIVE_VERIFIED_BLOCK_POLICY_H
#define NATIVE_VERIFIED_BLOCK_POLICY_H
#include <stdint.h>
struct NativeVerifiedBlockPolicy {
  const uint8_t *manifest;
  uint64_t manifestBytes;
};
static_assert(sizeof(NativeVerifiedBlockPolicy) == 16 && alignof(NativeVerifiedBlockPolicy) == 8);
extern "C" {
extern const NativeVerifiedBlockPolicy verified_root_policies[];
extern const uint64_t verified_root_policy_count;
}
#endif
