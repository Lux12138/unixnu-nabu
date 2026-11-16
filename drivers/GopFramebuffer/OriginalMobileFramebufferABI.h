// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// Experimental binary dispatch layout for the exact supplied iOS17 family.
// Unclassified entries have no usable prototype or return contract. Derived
// implementations must fail without returning; never advertise them as supported.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
class IOMobileFramebuffer : public IOService {
    OSDeclareAbstractStructors(IOMobileFramebuffer)
    uint64_t familyPrivate;
public:
    virtual __attribute__((noreturn)) void unclassified168() = 0;
    virtual __attribute__((noreturn)) void unclassified169() = 0;
    virtual __attribute__((noreturn)) void unclassified170() = 0;
    virtual __attribute__((noreturn)) void unclassified171() = 0;
    virtual __attribute__((noreturn)) void unclassified172() = 0;
    virtual __attribute__((noreturn)) void unclassified173() = 0;
    virtual __attribute__((noreturn)) void unclassified174() = 0;
    virtual __attribute__((noreturn)) void unclassified175() = 0;
    virtual __attribute__((noreturn)) void unclassified176() = 0;
    virtual __attribute__((noreturn)) void unclassified177() = 0;
    virtual __attribute__((noreturn)) void unclassified178() = 0;
    virtual __attribute__((noreturn)) void unclassified179() = 0;
    virtual __attribute__((noreturn)) void unclassified180() = 0;
    virtual __attribute__((noreturn)) void unclassified181() = 0;
    virtual IOReturn beforeExternalMethod() = 0;
    virtual __attribute__((noreturn)) void unclassified183() = 0;
    virtual __attribute__((noreturn)) void unclassified184() = 0;
    virtual __attribute__((noreturn)) void unclassified185() = 0;
    virtual __attribute__((noreturn)) void unclassified186() = 0;
    virtual __attribute__((noreturn)) void unclassified187() = 0;
    virtual __attribute__((noreturn)) void unclassified188() = 0;
    virtual __attribute__((noreturn)) void unclassified189() = 0;
    virtual __attribute__((noreturn)) void unclassified190() = 0;
    virtual __attribute__((noreturn)) void unclassified191() = 0;
    virtual __attribute__((noreturn)) void unclassified192() = 0;
    virtual IOReturn releaseClientState(IOUserClient *) = 0;
    virtual __attribute__((noreturn)) void unclassified194() = 0;
    virtual __attribute__((noreturn)) void unclassified195() = 0;
    virtual __attribute__((noreturn)) void unclassified196() = 0;
    virtual __attribute__((noreturn)) void unclassified197() = 0;
    virtual __attribute__((noreturn)) void unclassified198() = 0;
    virtual __attribute__((noreturn)) void unclassified199() = 0;
    virtual __attribute__((noreturn)) void unclassified200() = 0;
    virtual __attribute__((noreturn)) void unclassified201() = 0;
    virtual __attribute__((noreturn)) void unclassified202() = 0;
    virtual __attribute__((noreturn)) void unclassified203() = 0;
    virtual __attribute__((noreturn)) void unclassified204() = 0;
    virtual __attribute__((noreturn)) void unclassified205() = 0;
    virtual __attribute__((noreturn)) void unclassified206() = 0;
    virtual __attribute__((noreturn)) void unclassified207() = 0;
    virtual __attribute__((noreturn)) void unclassified208() = 0;
    virtual __attribute__((noreturn)) void unclassified209() = 0;
    virtual __attribute__((noreturn)) void unclassified210() = 0;
    virtual __attribute__((noreturn)) void unclassified211() = 0;
    virtual __attribute__((noreturn)) void unclassified212() = 0;
    virtual __attribute__((noreturn)) void unclassified213() = 0;
    virtual __attribute__((noreturn)) void unclassified214() = 0;
    virtual __attribute__((noreturn)) void unclassified215() = 0;
    virtual __attribute__((noreturn)) void unclassified216() = 0;
    virtual __attribute__((noreturn)) void unclassified217() = 0;
    virtual __attribute__((noreturn)) void unclassified218() = 0;
    virtual __attribute__((noreturn)) void unclassified219() = 0;
    virtual __attribute__((noreturn)) void unclassified220() = 0;
    virtual __attribute__((noreturn)) void unclassified221() = 0;
    virtual __attribute__((noreturn)) void unclassified222() = 0;
    virtual __attribute__((noreturn)) void unclassified223() = 0;
    virtual __attribute__((noreturn)) void unclassified224() = 0;
    virtual __attribute__((noreturn)) void unclassified225() = 0;
    virtual __attribute__((noreturn)) void unclassified226() = 0;
    virtual __attribute__((noreturn)) void unclassified227() = 0;
    virtual __attribute__((noreturn)) void unclassified228() = 0;
    virtual __attribute__((noreturn)) void unclassified229() = 0;
    virtual __attribute__((noreturn)) void unclassified230() = 0;
    virtual __attribute__((noreturn)) void unclassified231() = 0;
    virtual __attribute__((noreturn)) void unclassified232() = 0;
    virtual __attribute__((noreturn)) void unclassified233() = 0;
    virtual __attribute__((noreturn)) void unclassified234() = 0;
    virtual __attribute__((noreturn)) void unclassified235() = 0;
    virtual __attribute__((noreturn)) void unclassified236() = 0;
    virtual __attribute__((noreturn)) void unclassified237() = 0;
    virtual __attribute__((noreturn)) void unclassified238() = 0;
    virtual __attribute__((noreturn)) void unclassified239() = 0;
    virtual __attribute__((noreturn)) void unclassified240() = 0;
    virtual __attribute__((noreturn)) void unclassified241() = 0;
    virtual __attribute__((noreturn)) void unclassified242() = 0;
    virtual __attribute__((noreturn)) void unclassified243() = 0;
    virtual __attribute__((noreturn)) void unclassified244() = 0;
    virtual __attribute__((noreturn)) void unclassified245() = 0;
    virtual __attribute__((noreturn)) void unclassified246() = 0;
    virtual __attribute__((noreturn)) void unclassified247() = 0;
    virtual __attribute__((noreturn)) void unclassified248() = 0;
    virtual void getDisplayDimensions(uint32_t *, uint32_t *) = 0;
    virtual __attribute__((noreturn)) void unclassified250() = 0;
    virtual __attribute__((noreturn)) void unclassified251() = 0;
    virtual __attribute__((noreturn)) void unclassified252() = 0;
    virtual __attribute__((noreturn)) void unclassified253() = 0;
    virtual __attribute__((noreturn)) void unclassified254() = 0;
    virtual __attribute__((noreturn)) void unclassified255() = 0;
    virtual __attribute__((noreturn)) void unclassified256() = 0;
    virtual __attribute__((noreturn)) void unclassified257() = 0;
    virtual __attribute__((noreturn)) void unclassified258() = 0;
    virtual __attribute__((noreturn)) void unclassified259() = 0;
    virtual __attribute__((noreturn)) void unclassified260() = 0;
    virtual __attribute__((noreturn)) void unclassified261() = 0;
};
static_assert(sizeof(IOService)==136 && sizeof(IOMobileFramebuffer)==144);
