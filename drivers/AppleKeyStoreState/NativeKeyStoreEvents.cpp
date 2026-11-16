// SPDX-License-Identifier: BSD-2-Clause-Patent
// AppleKeyStore for an ephemeral offline fixture: a notification service (the public IOKit matching class used by
// MKBEventsRegister) and, only when sheng-aks=1 is set, a user client that answers the state queries of the unchanged
// libaks/MobileKeyBag truthfully for this environment: no keybag, no passcode, nothing locked. It provides no key,
// keybag, passcode or data-protection operation; every other selector fails with kIOReturnError exactly as an unreachable
// AppleKeyStore does. It protects nothing and must not be taken for secure storage.
//
// Answers (selectors and layouts read from the unchanged target's libaks/MobileKeyBag, see docs/touch-nt36532e.md; the set that is
// enough for the system daemons on iOS 16.7 was established on hardware by github.com/dipa-studio/ios-sweet, whose notes document it):
//   0, 4, 5, 9, 12, 13, 26, 27, 55, 101, 102   succeed, no outputs (open handshake, bag/assertion/notify bookkeeping)
//   14       aks_get_system                        scalar out: system keybag handle 0 (keybagd then needs no systembag.kb)
//   7        aks_get_lock_state(device)            scalar out: bit 2 = unlocked since boot
//   17, 35   aks_get_(extended_)device_state       struct out: DER SET{ SEQUENCE{ UTF8String key, INTEGER value } ... }
//            keys ss sls sb sfa sgs sas sgpe srcd sms bh; ss = 4, sls = lock state (0 unlocked; 3 would be "no passcode").
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
extern "C" void native_keyevents_open_refused(void);
extern "C" bool native_aks_state_enabled(void);
extern "C" uint32_t native_aks_lock_state(void);
extern "C" void native_aks_call(uint32_t selector, int32_t result);
// l531: record the SHAPE of each selector's first call (scalar in/out counts, structure in/out sizes),
// read-only and before any answer is made. Nothing here changes what we reply; the point is to find
// out, before choosing an answer for the selectors that are not implemented (the histogram showed 34
// and 93 in the field), whether a bare kIOReturnSuccess with no output would even be well-formed for
// the caller. Device-repo side: package/KeyStoreProbe.cpp. Backup of this file: .pre-l531.
extern "C" void native_aks_shape(uint32_t selector, const IOExternalMethodArguments *args);
extern "C" void native_aks_opened(void);

namespace {
constexpr uint32_t kSelLockState = 7, kSelGetSystem = 14, kSelDeviceState = 17, kSelExtendedDeviceState = 35;
constexpr uint64_t kUnlockedSinceBoot = 4;
constexpr uint32_t kStateFlags = 4;
constexpr uint64_t kSystemHandle = 0;
constexpr IOReturn kAksError = (IOReturn)0xe00002bc;   // what libaks reports itself when the connection is missing
constexpr IOReturn kAksUnsupported = (IOReturn)0xe00002c7;

struct DerWriter {
    uint8_t *p; size_t cap, n = 0; bool ok = true;
    void put(uint8_t b) { if (n < cap) p[n++] = b; else ok = false; }
    void len(size_t l) { if (l < 0x80) put((uint8_t)l); else { put(0x81); put((uint8_t)l); } }   // entries stay below 256 bytes
};
// Smallest two's-complement big-endian INTEGER content for a non-negative value.
size_t derIntegerContent(uint64_t v, uint8_t out[9]) {
    uint8_t raw[8];
    for (int i = 0; i < 8; ++i) raw[i] = (uint8_t)(v >> (56 - 8 * i));
    size_t start = 0;
    while (start < 7 && raw[start] == 0) ++start;
    size_t n = 0;
    if (raw[start] & 0x80) out[n++] = 0;
    while (start < 8) out[n++] = raw[start++];
    return n;
}
size_t entrySize(const char *key, uint64_t value) {
    uint8_t c[9]; size_t kl = 0; while (key[kl]) ++kl;
    size_t body = (2 + kl) + (2 + derIntegerContent(value, c));
    return 2 + body;
}
}  // namespace

// DER SET of SEQUENCE{UTF8String, INTEGER}. Returns the length, or 0 when the buffer is too small.
extern "C" size_t native_aks_encode_state(uint8_t *out, size_t capacity, uint32_t lockState) {
    struct Entry { const char *key; uint64_t value; };
    const Entry entries[] = {{"ss", kStateFlags}, {"sls", lockState}, {"sb", 0}, {"sfa", 0}, {"sgs", 0},
                             {"sas", 0}, {"sgpe", 0}, {"srcd", 0}, {"sms", 0}, {"bh", 0}};
    size_t total = 0;
    for (const Entry &e : entries) total += entrySize(e.key, e.value);
    DerWriter w{out, capacity};
    w.put(0x31); w.len(total);
    for (const Entry &e : entries) {
        uint8_t c[9]; size_t kl = 0; while (e.key[kl]) ++kl;
        size_t il = derIntegerContent(e.value, c);
        w.put(0x30); w.len((2 + kl) + (2 + il));
        w.put(0x0c); w.put((uint8_t)kl); for (size_t i = 0; i < kl; ++i) w.put((uint8_t)e.key[i]);
        w.put(0x02); w.put((uint8_t)il); for (size_t i = 0; i < il; ++i) w.put(c[i]);
    }
    return w.ok ? w.n : 0;
}

class AppleKeyStoreUserClient final : public IOUserClient {
    OSDeclareDefaultStructors(AppleKeyStoreUserClient)
public:
    IOReturn clientClose() override { terminate(); return kIOReturnSuccess; }
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *,
                            OSObject *, void *) override {
        native_aks_shape(selector, args);          /* l531: records the call shape, changes nothing */
        IOReturn rc = kAksUnsupported;
        if (!args) rc = kAksError;
        else switch (selector) {
        // Handshake right after IOServiceOpen (a failure makes libaks drop the connection and reopen on every call), and the
        // bag/assertion/notification calls that need no answer in an environment without keybags.
        // l532 adds 31 to this group, on the strength of the shape l531b recorded in hardware:
        // scalarInputCount=1, scalarOutputCount=0, structureInputSize=0, structureOutputSize=0 -- the
        // caller asks for no scalar and no bytes back (it does hand a structureOutput pointer with size
        // 0, which is what a "nothing to write" call looks like), so a success with no output is
        // well-formed here and matches the handshake/bookkeeping calls above.
        case 0: case 4: case 5: case 9: case 12: case 13: case 26: case 27: case 31: case 55: case 101: case 102:
            args->scalarOutputCount = 0;
            rc = kIOReturnSuccess;
            break;
        // l532. Recorded shape from the board (l531b): in=2/1, si=0, so=0, f=12 -- two scalars in and
        // ONE scalar out. That is why this is not folded into the group above: answering success without
        // writing scalarOutput[0] would hand the caller an uninitialised word. It gets 0, which is the
        // same answer selector 14 already gives for "there is no system keybag" (kSystemHandle = 0).
        case 34:
            if (args->scalarOutputCount < 1 || !args->scalarOutput) { rc = kIOReturnBadArgument; break; }
            args->scalarOutput[0] = 0;
            args->scalarOutputCount = 1;
            rc = kIOReturnSuccess;
            break;
        // l532 deliberately does NOT answer 93: its recorded shape is si=18, so=4096, which means the
        // caller wants a 4096-byte structure back for 18 bytes of input. Returning success with nothing
        // written there is worse than returning unsupported -- the consumer would read an untouched
        // buffer as if it were real state. That one needs the actual output type from libaks on
        // ipados-17.7.11 before it can be answered honestly.
        case kSelGetSystem:
            if (args->scalarOutputCount < 1 || !args->scalarOutput) { rc = kIOReturnBadArgument; break; }
            args->scalarOutput[0] = kSystemHandle;
            args->scalarOutputCount = 1;
            rc = kIOReturnSuccess;
            break;
        case kSelLockState:
            rc = (args->scalarInputCount == 1 && args->scalarOutputCount == 1 && args->scalarOutput)
                     ? kIOReturnSuccess : kIOReturnBadArgument;
            if (rc == kIOReturnSuccess) args->scalarOutput[0] = kUnlockedSinceBoot;
            break;
        case kSelDeviceState:
        case kSelExtendedDeviceState: {
            if (args->scalarInputCount > 1 || args->scalarOutputCount != 0) { rc = kIOReturnBadArgument; break; }
            uint8_t der[160];
            size_t n = native_aks_encode_state(der, sizeof der, native_aks_lock_state());
            if (!n) { rc = kIOReturnNoSpace; break; }
            if (args->structureOutputDescriptor) {
                if (n > args->structureOutputDescriptor->getLength()) { rc = kIOReturnNoSpace; break; }
                args->structureOutputDescriptor->writeBytes(0, der, n);
                args->structureOutputDescriptorSize = (uint32_t)n;
                rc = kIOReturnSuccess;
            } else if (args->structureOutput && n <= args->structureOutputSize) {
                for (size_t k = 0; k < n; ++k) ((uint8_t *)args->structureOutput)[k] = der[k];
                args->structureOutputSize = (uint32_t)n;
                rc = kIOReturnSuccess;
            } else {
                rc = kIOReturnNoSpace;
            }
            break;
        }
        default: break;
        }
        native_aks_call(selector, (int32_t)rc);
        return rc;
    }
};
OSDefineMetaClassAndStructors(AppleKeyStoreUserClient, IOUserClient)

class AppleKeyStore final : public IOService {
    OSDeclareDefaultStructors(AppleKeyStore)
    IOReturn open(task_t task, void *security, UInt32 type, OSDictionary *properties, IOUserClient **out) {
        if (out) *out = nullptr;
        if (!native_aks_state_enabled() || !out) { native_keyevents_open_refused(); return kIOReturnNotReady; }
        AppleKeyStoreUserClient *client = new AppleKeyStoreUserClient;
        if (!client) return kIOReturnNoMemory;
        if (!client->initWithTask(task, security, type, properties) || !client->attach(this)) {
            client->release(); return kIOReturnError;
        }
        if (!client->start(this)) { client->detach(this); client->release(); return kIOReturnError; }
        native_aks_opened();
        *out = client;
        return kIOReturnSuccess;
    }
public:
    bool start(IOService *provider) override {
        if (!IOService::start(provider)) return false;
        setName("Native keystore state service");
        if (!setProperty("NativeKeyStoreEvents", true) ||
            !setProperty("KeyOperationsAvailable", false)) {
            IOService::stop(provider);
            return false;
        }
        registerService();
        return true;
    }
    IOReturn newUserClient(task_t t, void *s, UInt32 type, OSDictionary *p, IOUserClient **out) override {
        return open(t, s, type, p, out);
    }
    IOReturn newUserClient(task_t t, void *s, UInt32 type, IOUserClient **out) override {
        return open(t, s, type, nullptr, out);
    }
};
OSDefineMetaClassAndStructors(AppleKeyStore, IOService)
extern "C" IOService *native_keyevents_allocate(void) { return new AppleKeyStore; }
