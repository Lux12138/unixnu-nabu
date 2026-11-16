// SPDX-License-Identifier: MIT
// l626/l627: publish a Metal-visible accelerator service.
//
// l623 measured that IOAcceleratorES is not in the runtime metaclass table; l625 then showed
// why: the class lives in the prelinked IOGPUFamily component, which nothing loads, and
// OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily") registers it (load=0x0,
// classSize 136, superclass IOService) — exactly what OriginalAcceleratorABI.h asserts.
//
// We cannot subclass a kext that is not part of this link, and an abstract base cannot be
// instantiated, so this file supplies both: the base definition (same name, same layout as
// the measured real class) and one concrete subclass to publish.
//
// l627 adds the two things the open path needs: a newUserClient that forwards to the driver's
// own client class, and the provider properties NativeAdrenoPublish.cpp:1759-1793 checks
// before it will serve an open. `NativeSandboxGrantProcess` is the process-name path and does
// not require the opener to carry com.apple.security.ts.opengl-or-metal; the entitlement
// property stays set as the second, stricter path.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSNumber.h>
#include <mach/task.h>

#include "OriginalAcceleratorABI.h"

extern "C" IOReturn native_adreno_new_user_client(IOService *provider, task_t owningTask,
    void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler,
    unsigned limit);

OSDefineMetaClassAndAbstractStructors(IOAcceleratorES, IOService);

class AGXAccelerator : public IOAcceleratorES {
	OSDeclareDefaultStructors(AGXAccelerator);
public:
	IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
	    OSDictionary *properties, IOUserClient **handler) override;
};

OSDefineMetaClassAndStructors(AGXAccelerator, IOAcceleratorES);

IOReturn
AGXAccelerator::newUserClient(task_t owningTask, void *securityID, UInt32 type,
    OSDictionary *properties, IOUserClient **handler)
{
	return native_adreno_new_user_client(this, owningTask, securityID, type, properties,
	    handler, 0);
}

extern "C" IOReturn
native_adreno_accelerator_publish(IOService *service, uint32_t flags)
{
	(void)flags;
	if (!service) {
		return kIOReturnBadArgument;
	}
	AGXAccelerator *accelerator = new AGXAccelerator;
	if (!accelerator) {
		return kIOReturnNoMemory;
	}
	if (!accelerator->init()) {
		accelerator->release();
		IOLog("NABU GPUACC init failed\n");
		return kIOReturnError;
	}
	if (!accelerator->attach(service)) {
		accelerator->detach(service);
		accelerator->release();
		IOLog("NABU GPUACC attach failed\n");
		return kIOReturnBadArgument;
	}
	accelerator->setName("AGXAccelerator");
	/* The properties the open path checks (NativeAdrenoPublish.cpp:1779-1793). The process
	 * name is what actually admits backboardd today; the entitlement triple is kept so the
	 * stricter path works unchanged once the opener really carries it. */
	accelerator->setProperty("MetalPluginName", "NativeAdrenoMetal");
	accelerator->setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice");
	accelerator->setProperty("NativeSandboxGrantEntitlement",
	    "com.apple.security.ts.opengl-or-metal");
	OSNumber *connectType = OSNumber::withNumber(0x4147ULL, 32);
	if (connectType) {
		accelerator->setProperty("NativeSandboxGrantConnectType", connectType);
		connectType->release();
	}
	accelerator->setProperty("NativeSandboxGrantProcess", "backboardd");
	accelerator->registerService();
	IOLog("NABU GPUACC published cls=%s id=%llu props=5\n",
	    accelerator->getMetaClass()->getClassName(),
	    (unsigned long long)accelerator->getRegistryEntryID());
	return kIOReturnSuccess;
}
