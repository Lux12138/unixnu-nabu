// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual firmware scanout transfer, not a compositor or hardware vblank ABI.
#pragma once
#include <stdint.h>
enum { NGP_BGRX8888=1,NGP_VERSION=1 };
struct NGPMode {uint32_t version,format;uint64_t width,height,stride,bytes;};
struct NGPPresent {uint64_t address,bytes;uint32_t width,height,stride,format;};
struct NGPTransfer {uint64_t sequence,bytes;};
// GPU hand-off (boot-arg sheng-fb-gpu=1): the display driver offers a finished frame instead of copying it. A privileged client waits for it
// (selector 2: the frame's pages are mapped read/write into the caller, at address), lets the GPU copy it into the scanout mapping (client
// memory type 1, writable) and then completes it (selector 3), which retires the swap. Nothing here copies pixels.
enum { NGP_METHOD_MODE=0,NGP_METHOD_PRESENT=1,NGP_METHOD_WAIT_SWAP=2,NGP_METHOD_COMPLETE_SWAP=3,NGP_MEMORY_SCANOUT_RW=1 };
struct NGPWait {uint64_t timeout_ms;};
struct NGPSwap {uint64_t sequence,address,bytes,stride;uint32_t width,height,format,reserved;};
struct NGPComplete {uint64_t sequence;uint32_t status,reserved;};
static_assert(sizeof(NGPWait)==8 && sizeof(NGPSwap)==48 && sizeof(NGPComplete)==16,"GOP hand-off ABI");
static_assert(sizeof(NGPMode)==40 && sizeof(NGPPresent)==32 && sizeof(NGPTransfer)==16,"GOP transfer ABI");
