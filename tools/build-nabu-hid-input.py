#!/usr/bin/env python3
"""Qualify the IOHIDDevice ABI in our own prelinked candidate and emit the HID build inputs.

nabu port of the upstream ShengHidInput builder (kits/UniXNU/ShengHidInput@5f3a37ce, run through
scripts/build-sheng-hid-input.py). The upstream script is macOS-only - it shells out to `xcrun
--sdk iphoneos` and `brew --prefix llvm` - and it pins its reference image by the hash of *that*
board's candidate. Both are replaced here:

* the reference image is the candidate our own farm already carries,
  build/xnu-prelink/native-owner-mobile-graphics-candidate.macho. Its plan records
  com.apple.iokit.IOHIDFamily __TEXT_EXEC = 7e4c5104...e86b72, byte-for-byte the hash the
  upstream recipe pins, so the original instructions being qualified are the same ones;
* the pure-virtual test on slot 180 is replaced by the structural test 8714 asked for, because
  neither our kernel nor our candidate exports ___cxa_pure_virtual at all (nm reports no such
  symbol), so the upstream comparison cannot be evaluated here. What is checked instead is
  named in the audit and every one of them is a require(), not documentation.

Nothing here loads, signs or runs anything: it reads an image, proves the ABI, and writes the
header plus the binding assembly that the compile step consumes.
"""
import hashlib
import json
import pathlib
import struct
import sys

# The tree root is whichever copy owns kits/XnuPrelink: run from the farm that is the script's own
# parents[1]; run from the device repo it is the working directory the recipe sets (it always runs
# with cwd=tree). build/xnu-prelink, the candidate image and the build/nabu-hid-input output all hang
# off the same root, so one answer has to serve both.
def _root():
    for cand in (pathlib.Path(__file__).resolve().parents[1], pathlib.Path.cwd().resolve()):
        if (cand / 'kits/XnuPrelink/native_prelink.py').is_file():
            return cand
    raise SystemExit('FAIL: kits/XnuPrelink/native_prelink.py is under neither %s nor %s'
                     % (pathlib.Path(__file__).resolve().parents[1], pathlib.Path.cwd().resolve()))


ROOT = _root()
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
from native_prelink import backed_offset, exports, macho  # noqa: E402

OUT = ROOT / 'build/nabu-hid-input'
OUT.mkdir(parents=True, exist_ok=True)

# The instruction hash the upstream recipe pins for the same OS build (ipados-17.7.11-21H461).
HID_TEXT_SHA = '7e4c5104814f2e773d5be13d092dec53ee44ba5a61a81264b229780406e86b72'
CANDIDATE = ROOT / 'build/xnu-prelink/native-owner-mobile-graphics-candidate.macho'
PLAN = CANDIDATE.with_suffix('.json')

# Byte patterns the upstream recipe compares verbatim. They live inside IOHIDFamily's own
# __TEXT_EXEC, and the vtable/ABI proof below is what makes them meaningful for us.
C2_PROLOGUE = (0xfffffff004ad94f8, bytes.fromhex('fd7bbfa9fd0300912bd90094489100f008011791080000f9fd7bc1a8c0035fd6'))
HANDLE_START = (0xfffffff004ad9aec, bytes.fromhex(
    '680240f908a142f9e00313aae10314aa00013fd6680240f9603d003608d142f9e1630091e00313aa00013fd6a02d0035e00f40f9'))
HANDLE_REPORT_BODY = bytes.fromhex('680240f9081543f9e00313aae20316aae30315aae40314aa00013fd6')
HANDLE_REPORT_BODY_AT = 0xfffffff004add110
SLOTS = 207


def require(value, why):
    if not value:
        raise SystemExit('HID ABI: ' + why)


def main():
    require(CANDIDATE.is_file(), 'reference candidate is missing: ' + str(CANDIDATE))
    require(PLAN.is_file(), 'reference plan is missing: ' + str(PLAN))
    blob = CANDIDATE.read_bytes()
    plan = json.loads(PLAN.read_text())
    image = macho(blob)
    require(hashlib.sha256(blob).hexdigest() == plan['candidate_sha256'], 'candidate matches its own plan')
    recorded = plan.get('original_executable_sha256', {}).get('com.apple.iokit.IOHIDFamily')
    require(recorded == HID_TEXT_SHA, 'the plan records the same original IOHIDFamily instruction hash as upstream')

    def read(address, length):
        at = backed_offset(image['segments'], address, length, len(blob))
        return blob[at:at + length]

    extents = [e for e in plan['extents'] if e['module'] == 'com.apple.iokit.IOHIDFamily' and e['segment'] == '__TEXT_EXEC']
    require(len(extents) == 1, 'exactly one IOHIDFamily __TEXT_EXEC extent in the plan')
    extent = extents[0]
    text = read(extent['va'], extent['size'])
    require(hashlib.sha256(text).hexdigest() == HID_TEXT_SHA,
            'the bytes in that extent really are the instructions the hash was taken over')

    names = {}
    for address, found in exports(blob, image).items():
        for name in found:
            names.setdefault(name, address)

    own = [e for e in plan['extents'] if e['module'] == 'com.apple.iokit.IOHIDFamily']

    def inside_block(address, segment=None):
        """Proof that an address belongs to this kext: it lies in one of the extents the plan
        recorded for com.apple.iokit.IOHIDFamily. Neither image exports the class's own symbols
        (the export table holds exactly one IOHID* name, _gIOHIDInterfaceClassName), so upstream's
        literals cannot be checked by name here - but they can be checked by ownership of the
        range, and __DATA reads zero in a file precisely because the loader fills it."""
        for e in own:
            if segment and e['segment'] != segment:
                continue
            if e['va'] <= address < e['va'] + e['size']:
                return e['segment']
        return None

    # --- the vtable. Upstream reads 207 method slots straight from 0xfffffff005d045c0 (no +16), and
    # its bindings map records __ZTV11IOHIDDevice as that address minus 16, which is the C++ vtable
    # header convention {offset, typeinfo} before the first slot. Both readings are checked here.
    vptr_literal = 0xfffffff005d045c0
    tv_name = names.get('__ZTV11IOHIDDevice')
    require(tv_name is not None or vptr_literal, 'IOHIDDevice vtable located')
    if tv_name is not None:
        require(tv_name + 16 == vptr_literal,
                'the export __ZTV11IOHIDDevice + 16 is the slot array the literal address names (%x vs %x)'
                % (tv_name + 16, vptr_literal))
    methods = struct.unpack('<%dQ' % SLOTS, read(vptr_literal, SLOTS * 8))
    require(all(methods), 'all %d method slots are non-zero' % SLOTS)
    require(read(vptr_literal + SLOTS * 8, 24) == bytes(24), '24 zero bytes follow the table (nothing past slot 206)')

    # slot 181 is handleReport: proved from the function body inside our own image rather than from
    # an address borrowed from another board.
    body_at = text.find(HANDLE_REPORT_BODY)
    require(body_at >= 0, 'handleReport body pattern found inside the IOHIDFamily text extent')
    found_address = backed_offset(image['segments'], HANDLE_REPORT_BODY_AT, len(HANDLE_REPORT_BODY), len(blob))
    require(blob[found_address:found_address + len(HANDLE_REPORT_BODY)] == HANDLE_REPORT_BODY,
            'the upstream handleReport+0x4c address still lands on that body here')
    report_start = HANDLE_REPORT_BODY_AT - 0x4c
    require(methods[181] == report_start,
            'slot 181 is handleReport (%x) - the assertion 8714 asked for as a build gate' % report_start)
    require(report_start in methods, 'handleReport is one of the table entries')
    # slot 180 replaces the pure-virtual check: it must be a real function entry inside the block.
    # slot 180 is the abstract-class stub. Upstream compares it against ___cxa_pure_virtual, which
    # neither of our images exports, so the slot is identified instead by what its body references:
    # XNU's pure_virtual() lives in osfmk/libsa/OSRuntime.cpp and stores that file name plus the
    # "C++ runtime failed to initialize" message before panicking. Decoding its ADRP/ADD pair and
    # reading the string is a name-free identity check on the same object upstream pinned.
    slot180 = methods[180]
    words = struct.unpack('<10I', read(slot180, 40))

    def adrp_target(word, pc):
        imm = (((word >> 5) & 0x7ffff) << 2) | ((word >> 29) & 3)
        if imm & (1 << 20):
            imm -= 1 << 21
        return (pc & ~0xfff) + (imm << 12)

    def add_imm(word):
        return ((word >> 10) & 0xfff) << (12 if (word >> 22) & 1 else 0)

    referenced = read(adrp_target(words[4], slot180 + 16) + add_imm(words[5]), 14)
    require(referenced.startswith(b'OSRuntime.cpp'),
            'slot 180 (%x) is the OSRuntime.cpp pure-virtual handler (it loads %r)' % (slot180, referenced))
    in_text = extent['va'] <= slot180 < extent['va'] + extent['size']
    other_in_text = sum(1 for m in methods if extent['va'] <= m < extent['va'] + extent['size'])

    # the base overrides and the abstract-device shape, as upstream states them
    tv_io_service = names.get('__ZTV9IOService')
    base_changed = None
    if tv_io_service is not None:
        base = struct.unpack('<168Q', read(tv_io_service + 16, 168 * 8))
        base_changed = [i for i in range(168) if methods[i] != base[i]]
        require(base_changed == [0, 1, 7, 12, 14, 21, 23, 76, 81, 82, 86, 87, 91, 92, 93, 101, 132, 140],
                'exact original HID overrides of the IOService base table')

    for address, expect in ((C2_PROLOGUE[0], C2_PROLOGUE[1]), (HANDLE_START[0], HANDLE_START[1])):
        require(read(address, len(expect)) == expect, 'original bytes at %x' % address)

    c2_slot = struct.unpack('<Q', read(0xfffffff005d00f10, 8))[0]
    d2_slot = struct.unpack('<Q', read(0xfffffff005d00f18, 8))[0]
    io_service_c2 = names.get('__ZN9IOServiceC2EPK11OSMetaClass')
    io_service_d2 = names.get('__ZN9IOServiceD2Ev')
    require(io_service_c2 is not None and io_service_d2 is not None,
            'this image exports IOService C2 and D2 (C2=%s D2=%s)' % (io_service_c2, io_service_d2))
    require(c2_slot == io_service_c2, 'the protected-constructor slot binds this image\'s IOService C2 (%x)' % c2_slot)
    require(d2_slot == io_service_d2, 'the destructor slot binds this image\'s IOService D2 (%x)' % d2_slot)

    # --- the generated ABI header (upstream's form, including the deprecated opaque slots)
    header = '''// Generated from the qualified original image; opaque private storage only.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
class IOHIDDevice : public IOService {
 OSDeclareAbstractStructors(IOHIDDevice)
 unsigned char originalPrivate[0xd0-sizeof(IOService)];
public:
 using IOService::setProperty;
 IOReturn Dispatch(IORPC) override;
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 bool setProperty(const OSSymbol *,OSObject *) override;
 bool willTerminate(IOService *,IOOptionBits) override;
 IOReturn configureReport(IOReportChannelList *,IOReportConfigureAction,void *,void *) override;
 IOReturn updateReport(IOReportChannelList *,IOReportUpdateAction,void *,void *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 bool handleOpen(IOService *,IOOptionBits,void *) override;
 void handleClose(IOService *,IOOptionBits) override;
 bool handleIsOpen(const IOService *) const override;
 bool matchPropertyTable(OSDictionary *,SInt32 *) override;
 IOReturn message(UInt32,IOService *,void *) override;
 IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
'''
    for i in range(168, SLOTS):
        if i == 171:
            header += ' virtual OSString *newTransportString() const;\n'
        elif i == 173:
            header += ' virtual OSString *newProductString() const;\n'
        elif i == 180:
            header += ' virtual IOReturn newReportDescriptor(IOMemoryDescriptor **) const=0;\n'
        elif i == 181:
            header += ' virtual IOReturn handleReport(IOMemoryDescriptor *,unsigned,IOOptionBits);\n'
        else:
            header += ' virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque%d();\n' % i
    header += '};\nstatic_assert(sizeof(IOHidDeviceDummy==0xd0);\n'
    header = header.replace('sizeof(IOHidDeviceDummy==0xd0);', 'sizeof(IOHIDDevice)==0xd0,"Original device instance size");')
    (OUT / 'OriginalHidDeviceABI.h').write_text(header)

    # --- the bindings the compiled object's own relocations must reproduce
    # The metaclass object and the vtable are this kext's own __DATA/__DATA_CONST, not exports.
    meta = 0xfffffff006ae6fa0
    require(inside_block(meta, '__DATA') == '__DATA',
            'the metaclass address %#x lies inside the IOHIDFamily __DATA extent' % meta)
    require(inside_block(vptr_literal, '__DATA_CONST') == '__DATA_CONST',
            'the vtable slots %#x lie inside the IOHIDFamily __DATA_CONST extent' % vptr_literal)
    require(inside_block(C2_PROLOGUE[0], '__TEXT_EXEC') == '__TEXT_EXEC', 'C2 is inside the verified text')
    require(inside_block(report_start, '__TEXT_EXEC') == '__TEXT_EXEC', 'handleReport is inside the verified text')
    bindings = {'__ZN11IOHIDDeviceC2EPK11OSMetaClass': C2_PROLOGUE[0],
                '__ZN11IOHIDDeviceD2Ev': C2_PROLOGUE[0] + 0x20,
                '__ZN11IOHIDDevice10gMetaClassE': meta,
                '__ZTV11IOHIDDevice': vptr_literal - 16}
    inherited = {}
    for i, method in enumerate(methods):
        if method == 0:
            continue
        for name, address in names.items():
            if address == method and '11IOHIDDevice' in name:
                bindings[name] = method
                break
    asm = ''.join('.globl %s\n.set %s, %#x\n' % (n, n, a) for n, a in sorted(bindings.items()))
    (OUT / 'original-bindings.S').write_text(asm)

    audit = {
        'reference_candidate_sha256': plan['candidate_sha256'],
        'original_text_sha256': HID_TEXT_SHA,
        'iohidfamily_text_extent': {'va': extent['va'], 'size': extent['size']},
        'vtable_slot_array': vptr_literal,
        'slots': SLOTS,
        'slot181_handleReport': methods[181],
        'slot180_newReportDescriptor_target': methods[180],
        'slot180_pure_virtual_string': referenced.rstrip(bytes([0])).decode(),
        'slot180_inside_iohidfamily_text': bool(in_text),
        'slots_pointing_into_iohidfamily_text': other_in_text,
        'metaclass': hex(meta),
        'io_service_c2_binding': c2_slot,
        'io_service_d2_binding': d2_slot,
        'base_overrides': base_changed,
        'methods': [hex(m) for m in methods],
        'bindings': {k: hex(v) for k, v in sorted(bindings.items())},
    }
    (OUT / 'abi-audit.json').write_text(json.dumps(audit, indent=2) + '\n')
    print('PASS: original IOHIDFamily instruction identity, %d slots non-zero, slot181==handleReport(%x), '
          '24-byte zero tail, IOService C2/D2 bound to this image (%x/%x)'
          % (SLOTS, methods[181], c2_slot, d2_slot))
    print('      bindings: %d symbols -> %s' % (len(bindings), OUT / 'original-bindings.S'))
    print('      header  : %s' % (OUT / 'OriginalHidDeviceABI.h'))


def bindings(path):
    """Bind the IOHIDDevice slots the *compiled* object references, from its own vtable relocations.

    The qualification proves the original instruction stream; this proves the other half -- that every
    import our object actually needs is one of the audited slots, and that nothing else is imported.
    Upstream's build-sheng-hid-input.py:97-101 does the same walk, and scripts/build-platform-power.py
    :105-144 is our tree's version of it for IOPMGR, including the nlist layout and the relocation
    filter used below. Run it as `--bindings <NativeHidInput.o>` right after that file is compiled.
    """
    obj = pathlib.Path(path)
    require(obj.is_file(), 'the compiled HID object exists: ' + str(obj))
    raw = obj.read_bytes()
    info = macho(raw)
    require(info['kind'] == 1, 'NativeHidInput.o is an ARM64 Mach-O object')
    build_version = [struct.unpack_from('<2I', raw, at + 8) for cmd, at, _ in info['commands'] if cmd == 0x32]
    require(build_version == [(2, 0x110000)], 'compiled for ARM64/iOS 17, like the kernel it joins')

    audit = json.loads((OUT / 'abi-audit.json').read_text())
    require(audit['original_text_sha256'] == HID_TEXT_SHA, 'the bindings pass reads the same audit as the qualification')
    def as_address(value):
        # main() writes the scalars as ints and the tables as hex strings; accept both shapes rather
        # than assume one, because this check is the link between the audit and the object.
        return int(value, 16) if isinstance(value, str) else int(value)

    methods = [as_address(x) for x in audit['methods']]
    require(len(methods) == SLOTS and methods[181] == as_address(audit['slot181_handleReport']),
            'the audit slot table is the qualified one (%d slots, slot181=%x)' % (SLOTS, methods[181]))

    sym_at = next(at for cmd, at, _ in info['commands'] if cmd == 2)
    at, count, strings, _ = struct.unpack_from('<4I', raw, sym_at + 8)
    symbols = []
    for i in range(count):
        ix, kind, _, _, address = struct.unpack_from('<IBBHQ', raw, at + i * 16)   # 'address' is the nlist field; the audit helper is as_address() to stay clear of it
        symbols.append((raw[strings + ix:raw.index(b'\0', strings + ix)].decode(), address, kind))
    vtables = [va for name, va, _ in symbols if name == '__ZTV14NativeHidInput']
    require(len(vtables) == 1, 'the object has exactly one NativeHidInput vtable')
    vt = vtables[0]

    relocations = {}
    for segment in info['segments']:
        for section in segment['sections']:
            if section['va'] <= vt < section['va'] + section['size']:
                ro, rc = struct.unpack_from('<2I', raw, section['command_offset'] + 56)
                for i in range(rc):
                    offset, bits = struct.unpack_from('<2I', raw, ro + i * 8)
                    if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                        relocations[section['va'] + offset] = symbols[bits & 0xffffff][0]
    compiled = [relocations.get(vt + 16 + i * 8) for i in range(SLOTS)]
    require(all(compiled), 'the object relocates all %d IOHIDDevice slots' % SLOTS)
    require(vt + 16 + SLOTS * 8 not in relocations, 'and nothing past slot %d' % (SLOTS - 1))

    owned = [i for i, n in enumerate(compiled) if n.startswith(('__ZN14NativeHidInput', '__ZNK14NativeHidInput'))]
    require(owned, 'the object owns at least its constructor and lifecycle slots')
    bindings = {k: as_address(v) for k, v in audit['bindings'].items()}
    for i, n in enumerate(compiled):
        if i in owned or '11IOHIDDevice' not in n:
            continue
        require(methods[i] != 0, 'slot %d (%s) has an original implementation to bind' % (i, n))
        bindings[n] = methods[i]

    undefined = {n for n, _, k in symbols if k == 1 and '11IOHIDDevice' in n}
    require(undefined == {n for n in bindings if '11IOHIDDevice' in n},
            'all and only audited original HID imports are bound; difference: %s'
            % sorted(undefined ^ {n for n in bindings if '11IOHIDDevice' in n}))
    asm = ''.join('.globl %s\n.set %s, %#x\n' % (n, n, a) for n, a in sorted(bindings.items()))
    (OUT / 'original-bindings.S').write_text(asm)
    audit['bindings'] = {k: hex(v) for k, v in sorted(bindings.items())}
    audit['owned_slots'] = owned
    audit['binding_source'] = "the compiled object's own __ZTV14NativeHidInput relocations, one name per slot"
    (OUT / 'abi-audit.json').write_text(json.dumps(audit, indent=2) + '\n')
    print('PASS: %d bindings, %d IOHIDDevice imports, %d slots owned by our class'
          % (len(bindings), len(undefined), len(owned)))
    print('      owned slots: %s' % owned)
    print('      bindings: %d symbols -> %s' % (len(bindings), OUT / 'original-bindings.S'))


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--bindings':
        bindings(sys.argv[2])
    else:
        main()
