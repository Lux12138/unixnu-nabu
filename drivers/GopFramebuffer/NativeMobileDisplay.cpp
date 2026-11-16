// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "OriginalMobileFramebufferABI.h"
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <libkern/c++/OSSymbol.h>
// Prototype only. No registry publication or display operation is enabled.
class NativeMobileDisplay : public IOMobileFramebuffer {
    OSDeclareDefaultStructors(NativeMobileDisplay)
    PE_Video video = {};
public:
    bool init(OSDictionary *properties = nullptr) override {
        if (!IOService::init(properties)) return false;
        if (PE_current_console(&video) || !video.v_baseAddr || !video.v_width ||
            !video.v_height || video.v_depth != 32 || video.v_rotate ||
            video.v_width > 16384 || video.v_height > 16384 ||
            video.v_rowBytes < video.v_width * 4 ||
            video.v_rowBytes > UINT64_MAX / video.v_height) return false;
        return true;
    }
    IOReturn beforeExternalMethod() override {
        return video.v_width && video.v_height ? kIOReturnSuccess : kIOReturnNotReady;
    }
    void getDisplayDimensions(uint32_t *width,uint32_t *height) override {
        if (!width || !height) panic("NativeMobileDisplay: invalid dimensions query");
        *width = (uint32_t)video.v_width; *height = (uint32_t)video.v_height;
    }
    __attribute__((noreturn)) void unclassified169() override { panic("NativeMobileDisplay: unclassified ABI entry 169"); }
    __attribute__((noreturn)) void unclassified170() override { panic("NativeMobileDisplay: unclassified ABI entry 170"); }
    __attribute__((noreturn)) void unclassified168() override { panic("NativeMobileDisplay: unclassified ABI entry 168"); }
    __attribute__((noreturn)) void unclassified171() override { panic("NativeMobileDisplay: unclassified ABI entry 171"); }
    __attribute__((noreturn)) void unclassified172() override { panic("NativeMobileDisplay: unclassified ABI entry 172"); }
    __attribute__((noreturn)) void unclassified173() override { panic("NativeMobileDisplay: unclassified ABI entry 173"); }
    __attribute__((noreturn)) void unclassified174() override { panic("NativeMobileDisplay: unclassified ABI entry 174"); }
    __attribute__((noreturn)) void unclassified175() override { panic("NativeMobileDisplay: unclassified ABI entry 175"); }
    __attribute__((noreturn)) void unclassified176() override { panic("NativeMobileDisplay: unclassified ABI entry 176"); }
    __attribute__((noreturn)) void unclassified177() override { panic("NativeMobileDisplay: unclassified ABI entry 177"); }
    __attribute__((noreturn)) void unclassified178() override { panic("NativeMobileDisplay: unclassified ABI entry 178"); }
    __attribute__((noreturn)) void unclassified179() override { panic("NativeMobileDisplay: unclassified ABI entry 179"); }
    __attribute__((noreturn)) void unclassified180() override { panic("NativeMobileDisplay: unclassified ABI entry 180"); }
    __attribute__((noreturn)) void unclassified181() override { panic("NativeMobileDisplay: unclassified ABI entry 181"); }
    __attribute__((noreturn)) void unclassified183() override { panic("NativeMobileDisplay: unclassified ABI entry 183"); }
    __attribute__((noreturn)) void unclassified184() override { panic("NativeMobileDisplay: unclassified ABI entry 184"); }
    __attribute__((noreturn)) void unclassified185() override { panic("NativeMobileDisplay: unclassified ABI entry 185"); }
    __attribute__((noreturn)) void unclassified186() override { panic("NativeMobileDisplay: unclassified ABI entry 186"); }
    __attribute__((noreturn)) void unclassified187() override { panic("NativeMobileDisplay: unclassified ABI entry 187"); }
    __attribute__((noreturn)) void unclassified188() override { panic("NativeMobileDisplay: unclassified ABI entry 188"); }
    __attribute__((noreturn)) void unclassified189() override { panic("NativeMobileDisplay: unclassified ABI entry 189"); }
    __attribute__((noreturn)) void unclassified190() override { panic("NativeMobileDisplay: unclassified ABI entry 190"); }
    __attribute__((noreturn)) void unclassified191() override { panic("NativeMobileDisplay: unclassified ABI entry 191"); }
    __attribute__((noreturn)) void unclassified192() override { panic("NativeMobileDisplay: unclassified ABI entry 192"); }
    IOReturn releaseClientState(IOUserClient *client) override {
        // Original client stop calls this with itself and checks IOReturn.
        // Only metadata queries are implemented: this backend has allocated
        // no client buffers, queues, notification hooks or surfaces to free.
        if (!client || client->getProvider() != this) return kIOReturnBadArgument;
        return kIOReturnSuccess;
    }
    __attribute__((noreturn)) void unclassified194() override { panic("NativeMobileDisplay: unclassified ABI entry 194"); }
    __attribute__((noreturn)) void unclassified195() override { panic("NativeMobileDisplay: unclassified ABI entry 195"); }
    __attribute__((noreturn)) void unclassified196() override { panic("NativeMobileDisplay: unclassified ABI entry 196"); }
    __attribute__((noreturn)) void unclassified197() override { panic("NativeMobileDisplay: unclassified ABI entry 197"); }
    __attribute__((noreturn)) void unclassified198() override { panic("NativeMobileDisplay: unclassified ABI entry 198"); }
    __attribute__((noreturn)) void unclassified199() override { panic("NativeMobileDisplay: unclassified ABI entry 199"); }
    __attribute__((noreturn)) void unclassified200() override { panic("NativeMobileDisplay: unclassified ABI entry 200"); }
    __attribute__((noreturn)) void unclassified201() override { panic("NativeMobileDisplay: unclassified ABI entry 201"); }
    __attribute__((noreturn)) void unclassified202() override { panic("NativeMobileDisplay: unclassified ABI entry 202"); }
    __attribute__((noreturn)) void unclassified203() override { panic("NativeMobileDisplay: unclassified ABI entry 203"); }
    __attribute__((noreturn)) void unclassified204() override { panic("NativeMobileDisplay: unclassified ABI entry 204"); }
    __attribute__((noreturn)) void unclassified205() override { panic("NativeMobileDisplay: unclassified ABI entry 205"); }
    __attribute__((noreturn)) void unclassified206() override { panic("NativeMobileDisplay: unclassified ABI entry 206"); }
    __attribute__((noreturn)) void unclassified207() override { panic("NativeMobileDisplay: unclassified ABI entry 207"); }
    __attribute__((noreturn)) void unclassified208() override { panic("NativeMobileDisplay: unclassified ABI entry 208"); }
    __attribute__((noreturn)) void unclassified209() override { panic("NativeMobileDisplay: unclassified ABI entry 209"); }
    __attribute__((noreturn)) void unclassified210() override { panic("NativeMobileDisplay: unclassified ABI entry 210"); }
    __attribute__((noreturn)) void unclassified211() override { panic("NativeMobileDisplay: unclassified ABI entry 211"); }
    __attribute__((noreturn)) void unclassified212() override { panic("NativeMobileDisplay: unclassified ABI entry 212"); }
    __attribute__((noreturn)) void unclassified213() override { panic("NativeMobileDisplay: unclassified ABI entry 213"); }
    __attribute__((noreturn)) void unclassified214() override { panic("NativeMobileDisplay: unclassified ABI entry 214"); }
    __attribute__((noreturn)) void unclassified215() override { panic("NativeMobileDisplay: unclassified ABI entry 215"); }
    __attribute__((noreturn)) void unclassified216() override { panic("NativeMobileDisplay: unclassified ABI entry 216"); }
    __attribute__((noreturn)) void unclassified217() override { panic("NativeMobileDisplay: unclassified ABI entry 217"); }
    __attribute__((noreturn)) void unclassified218() override { panic("NativeMobileDisplay: unclassified ABI entry 218"); }
    __attribute__((noreturn)) void unclassified219() override { panic("NativeMobileDisplay: unclassified ABI entry 219"); }
    __attribute__((noreturn)) void unclassified220() override { panic("NativeMobileDisplay: unclassified ABI entry 220"); }
    __attribute__((noreturn)) void unclassified221() override { panic("NativeMobileDisplay: unclassified ABI entry 221"); }
    __attribute__((noreturn)) void unclassified222() override { panic("NativeMobileDisplay: unclassified ABI entry 222"); }
    __attribute__((noreturn)) void unclassified223() override { panic("NativeMobileDisplay: unclassified ABI entry 223"); }
    __attribute__((noreturn)) void unclassified224() override { panic("NativeMobileDisplay: unclassified ABI entry 224"); }
    __attribute__((noreturn)) void unclassified225() override { panic("NativeMobileDisplay: unclassified ABI entry 225"); }
    __attribute__((noreturn)) void unclassified226() override { panic("NativeMobileDisplay: unclassified ABI entry 226"); }
    __attribute__((noreturn)) void unclassified227() override { panic("NativeMobileDisplay: unclassified ABI entry 227"); }
    __attribute__((noreturn)) void unclassified228() override { panic("NativeMobileDisplay: unclassified ABI entry 228"); }
    __attribute__((noreturn)) void unclassified229() override { panic("NativeMobileDisplay: unclassified ABI entry 229"); }
    __attribute__((noreturn)) void unclassified230() override { panic("NativeMobileDisplay: unclassified ABI entry 230"); }
    __attribute__((noreturn)) void unclassified231() override { panic("NativeMobileDisplay: unclassified ABI entry 231"); }
    __attribute__((noreturn)) void unclassified232() override { panic("NativeMobileDisplay: unclassified ABI entry 232"); }
    __attribute__((noreturn)) void unclassified233() override { panic("NativeMobileDisplay: unclassified ABI entry 233"); }
    __attribute__((noreturn)) void unclassified234() override { panic("NativeMobileDisplay: unclassified ABI entry 234"); }
    __attribute__((noreturn)) void unclassified235() override { panic("NativeMobileDisplay: unclassified ABI entry 235"); }
    __attribute__((noreturn)) void unclassified236() override { panic("NativeMobileDisplay: unclassified ABI entry 236"); }
    __attribute__((noreturn)) void unclassified237() override { panic("NativeMobileDisplay: unclassified ABI entry 237"); }
    __attribute__((noreturn)) void unclassified238() override { panic("NativeMobileDisplay: unclassified ABI entry 238"); }
    __attribute__((noreturn)) void unclassified239() override { panic("NativeMobileDisplay: unclassified ABI entry 239"); }
    __attribute__((noreturn)) void unclassified240() override { panic("NativeMobileDisplay: unclassified ABI entry 240"); }
    __attribute__((noreturn)) void unclassified241() override { panic("NativeMobileDisplay: unclassified ABI entry 241"); }
    __attribute__((noreturn)) void unclassified242() override { panic("NativeMobileDisplay: unclassified ABI entry 242"); }
    __attribute__((noreturn)) void unclassified243() override { panic("NativeMobileDisplay: unclassified ABI entry 243"); }
    __attribute__((noreturn)) void unclassified244() override { panic("NativeMobileDisplay: unclassified ABI entry 244"); }
    __attribute__((noreturn)) void unclassified245() override { panic("NativeMobileDisplay: unclassified ABI entry 245"); }
    __attribute__((noreturn)) void unclassified246() override { panic("NativeMobileDisplay: unclassified ABI entry 246"); }
    __attribute__((noreturn)) void unclassified247() override { panic("NativeMobileDisplay: unclassified ABI entry 247"); }
    __attribute__((noreturn)) void unclassified248() override { panic("NativeMobileDisplay: unclassified ABI entry 248"); }
    __attribute__((noreturn)) void unclassified250() override { panic("NativeMobileDisplay: unclassified ABI entry 250"); }
    __attribute__((noreturn)) void unclassified251() override { panic("NativeMobileDisplay: unclassified ABI entry 251"); }
    __attribute__((noreturn)) void unclassified252() override { panic("NativeMobileDisplay: unclassified ABI entry 252"); }
    __attribute__((noreturn)) void unclassified253() override { panic("NativeMobileDisplay: unclassified ABI entry 253"); }
    __attribute__((noreturn)) void unclassified254() override { panic("NativeMobileDisplay: unclassified ABI entry 254"); }
    __attribute__((noreturn)) void unclassified255() override { panic("NativeMobileDisplay: unclassified ABI entry 255"); }
    __attribute__((noreturn)) void unclassified256() override { panic("NativeMobileDisplay: unclassified ABI entry 256"); }
    __attribute__((noreturn)) void unclassified257() override { panic("NativeMobileDisplay: unclassified ABI entry 257"); }
    __attribute__((noreturn)) void unclassified258() override { panic("NativeMobileDisplay: unclassified ABI entry 258"); }
    __attribute__((noreturn)) void unclassified259() override { panic("NativeMobileDisplay: unclassified ABI entry 259"); }
    __attribute__((noreturn)) void unclassified260() override { panic("NativeMobileDisplay: unclassified ABI entry 260"); }
    __attribute__((noreturn)) void unclassified261() override { panic("NativeMobileDisplay: unclassified ABI entry 261"); }
};
OSDefineMetaClassAndStructors(NativeMobileDisplay,IOMobileFramebuffer)

extern "C" IOService *native_mobile_display_create(void);
extern "C" IOService *native_mobile_display_create(void) {
    // Caller must load the original parent and register this real metaclass
    // through the normal kernel/kext C++ initialization machinery first.
    const OSSymbol *name = OSSymbol::withCString("NativeMobileDisplay");
    if (!name) return nullptr;
    const OSMetaClass *meta = OSMetaClass::getMetaClassWithName(name);
    name->release();
    if (meta != &NativeMobileDisplay::gMetaClass) return nullptr;
    NativeMobileDisplay *display = new NativeMobileDisplay;
    if (display && !display->init()) { display->release(); return nullptr; }
    return display;
}
