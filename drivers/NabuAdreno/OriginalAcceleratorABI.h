// SPDX-License-Identifier: MIT
#pragma once
// Exact supplied iOS17 base, independently audited against its complete
// original vtable and protected constructor/destructor bodies. No extra fields
// or operational overrides beyond IOService. This declares a real ancestry;
// no original class, method, vtable or metaclass is replaced.
#include <IOKit/IOService.h>
class IOAcceleratorES : public IOService {
 OSDeclareAbstractStructors(IOAcceleratorES)
};
static_assert(sizeof(IOService)==136 && sizeof(IOAcceleratorES)==136,"Qualified original accelerator base extent");
