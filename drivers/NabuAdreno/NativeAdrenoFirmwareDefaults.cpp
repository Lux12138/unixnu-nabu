/* SPDX-License-Identifier: MIT */
/* Empty weak defaults for the embedded firmware images. The kernel is linked statically, where an undefined
 * weak reference is an error, so an image without the firmware archive still needs a definition. The
 * archive's strong definitions replace these. A zero length is refused by NativeAdrenoFirmware::validate
 * and gives no SQE upload, so nothing runs without real firmware. */
#include <stdint.h>
extern "C" __attribute__((weak)) const uint8_t native_adreno_firmware_image[1]={0};
extern "C" __attribute__((weak)) const uint64_t native_adreno_firmware_image_size=0;
extern "C" __attribute__((weak)) const uint8_t native_adreno_sqe_firmware_image[1]={0};
extern "C" __attribute__((weak)) const uint64_t native_adreno_sqe_firmware_image_size=0;
