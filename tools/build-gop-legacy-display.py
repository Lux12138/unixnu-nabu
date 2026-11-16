#!/usr/bin/env python3
"""Bind an owned concrete backend to the unchanged original Legacy lifecycle."""
import hashlib
import pathlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/native-legacy-display';OUT.mkdir(parents=True,exist_ok=True)
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'))
from native_prelink import macho,backed_offset,exports
def run(cmd,**kw):return subprocess.check_output([str(x) for x in cmd],timeout=90,**kw)
def require(v,why):
    if not v:raise SystemExit(why)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
(ROOT/'build/display-analysis').mkdir(parents=True,exist_ok=True)
print(run(['python3',ROOT/'scripts/audit-mobile-graphics-legacy.local.py'],text=True),end='')
audit=json.loads((ROOT/'build/display-analysis/mobile-graphics-legacy-abi.json').read_text())
pure=audit['abstract_slots'];methods=audit['methods']
header='''// SPDX-License-Identifier: BSD-2-Clause-Patent
// Opaque ABI storage for the exact original class, not source-level access
// to its private fields. The actual original metadata supplies its full
// Framebuffer/Service/IOService ancestry. Only qualified base prototypes may
// be called from source; opaque entries are original dispatch targets only.
#pragma once
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
struct OriginalLegacySwapRequest;
class IOMobileFramebufferLegacy : public IOService {
 OSDeclareAbstractStructors(IOMobileFramebufferLegacy)
 unsigned char originalPrivate[0x18a0-sizeof(IOService)];
public:
 // Both fields are qualified by the complete original slot-249 getter.
 // This modifies this object's own mode state, never a vtable or executable.
 void installBootDimensions(uint32_t width,uint32_t height) {
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5808)=width;
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5812)=height;
 }
 const OriginalLegacySwapRequest *pendingSwapRequest() const {
  const OriginalLegacySwapRequest *request=nullptr;
  memcpy(&request,reinterpret_cast<const unsigned char *>(this)+5480,sizeof(request));return request;
 }
 // The real normal scanout backend removes its pending head before calling
 // slot 295. Slot 312 is a discard path (terminal state 8), not presentation.
 // The queue is a tail queue (head at +5480, tail pointer at +5488, each request's next at +2960 and previous-link at +2968): taking the head
 // moves the next request, if a client already queued one behind it (int-132: swap 3 arrived during swap 2's GPU copy and the old
 // single-entry form refused to retire swap 2, so every wait on it stayed busy), up to the head. The original queue's reference
 // moves to the current-swap slot for the original completion body.
 bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {
  auto self=reinterpret_cast<unsigned char *>(this);
  auto bytes=reinterpret_cast<const unsigned char *>(request);
  const void *current=nullptr,*head=nullptr,*tail=nullptr,*next=nullptr,*previous=nullptr;
  memcpy(&current,self+5472,8);memcpy(&head,self+5480,8);memcpy(&tail,self+5488,8);
  if(!request || current || head!=request)return false;
  memcpy(&next,bytes+2960,8);memcpy(&previous,bytes+2968,8);
  if(previous!=self+5480)return false;
  if(next) {
   const void *link=self+5480;
   memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next))+2968,&link,8);
  } else if(tail!=bytes+2960)return false;
  memcpy(self+5472,&request,8);
  if(next) {memcpy(self+5480,&next,8);}
  else {const void *empty=nullptr,*emptyTail=self+5480;memcpy(self+5480,&empty,8);memcpy(self+5488,&emptyTail,8);}
  return true;
 }
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 IOReturn setProperties(OSObject *) override;
 void systemWillShutdown(IOOptionBits) override;
 IOService *probe(IOService *,SInt32 *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn newUserClient(task_t,void *,UInt32,IOUserClient **) override;
 IOReturn setPowerState(unsigned long,IOService *) override;
'''
for i in range(168,358):
    if i==184:header+=' virtual bool isMainDisplay();\n'
    elif i==188:header+=' virtual IOReturn getDotPitch(uint32_t *);\n'
    elif i==213:header+=' virtual IOReturn supportedFrameInfo(uint32_t,uint32_t *,char *,uint32_t *)=0;\n'
    elif i==249:header+=' virtual void getDisplayDimensions(uint32_t *,uint32_t *);\n'
    elif i==290:header+=' virtual void kickSwapQueue();\n'
    elif i==296:header+=' virtual bool validateSwap(const OriginalLegacySwapRequest *);\n'
    elif i==295:header+=' virtual void completeCurrentSwap();\n'
    elif i==337:header+=' virtual IOReturn collectCompletionReports(uint32_t,void *,uint32_t *,uint32_t)=0;\n'
    elif i==288:header+=' virtual bool startHardware();\n'
    elif i==301:header+=' virtual bool getRefreshAttributes(uint64_t *,bool);\n'
    elif i in pure:header+=f' virtual __attribute__((noreturn)) void opaque{i}()=0;\n'
    else:header+=f' virtual __attribute__((deprecated("Unclassified ABI: never call from source"))) void opaque{i}();\n'
header+='};\nstatic_assert(sizeof(IOMobileFramebufferLegacy)==0x18a0,"Actual original instance extent");\n'
(OUT/'OriginalLegacyFramebufferABI.h').write_text(header)
# l662i: drivers/GopFramebuffer/NabuSmmuMapper.h includes NativeAdrenoMemory.h, which lives in the
# device repo's drivers/NabuAdreno. Tree prep does not link that directory, so the compile below
# dies with 'NativeAdrenoMemory.h file not found'. Link it here, before the first compile.
_nabu_adreno=ROOT/'drivers/NabuAdreno'
if not _nabu_adreno.is_dir():
    if _nabu_adreno.is_symlink():
        _nabu_adreno.unlink()
    _nabu_adreno.symlink_to('/root/merge/UniXNU-nabu/drivers/NabuAdreno')
print(run(['python3',ROOT/'scripts/build-arm-driver.py','NativeLegacyDisplay'],text=True),end='')
obj=OUT/'NativeLegacyDisplay.o';raw=obj.read_bytes();image=macho(raw)
st=next(p for c,p,_ in image['commands'] if c==2);sy,count,strings,_=struct.unpack_from('<4I',raw,st+8);symbols=[]
for i in range(count):
    ix,kind,section,_,va=struct.unpack_from('<IBBHQ',raw,sy+i*16)
    symbols.append((raw[strings+ix:raw.index(b'\0',strings+ix)].decode(),va,kind))
vtable=next(va for name,va,_ in symbols if name=='__ZTV19NativeLegacyDisplay');relocations={}
for segment in image['segments']:
    for sec in segment['sections']:
        if sec['va']<=vtable<sec['va']+sec['size']:
            ro,rc=struct.unpack_from('<2I',raw,sec['command_offset']+56)
            for i in range(rc):
                offset,bits=struct.unpack_from('<2I',raw,ro+i*8)
                if bits>>27&1 and bits>>28==0 and bits>>25&3==3:relocations[sec['va']+offset]=symbols[bits&0xffffff][0]
compiled=[relocations.get(vtable+16+i*8) for i in range(358)]
require(all(compiled) and vtable+16+358*8 not in relocations,'Exact owned 358-entry vtable')
candidate=ROOT/'build/xnu-prelink/native-owner-mobile-graphics-candidate.macho'
if not candidate.exists():  # pruned: same fallback as scripts/audit-mobile-graphics-legacy.py (identical original family executable)
    later=sorted((ROOT/'build/xnu-prelink').glob('native-owner-ufs-cpu-desktop-*-candidate.macho'),key=lambda f:f.stat().st_mtime)
    require(later,'No reference candidate available');candidate=later[-1]
old=candidate.read_bytes();old_image=macho(old)
names={n:v for v,group in exports(old,old_image).items() for n in group}
def read(va,n):
    at=backed_offset(old_image['segments'],va,n,len(old));return old[at:at+n]
bindings={'__ZN25IOMobileFramebufferLegacyC2EPK11OSMetaClass':0xfffffff0057865cc,
          '__ZN25IOMobileFramebufferLegacyD2Ev':0xfffffff005786618,
          '__ZN25IOMobileFramebufferLegacy10gMetaClassE':0xfffffff006b35940,
          '__ZTV25IOMobileFramebufferLegacy':int(audit['legacy_vptr'],16)-16}
owned=[];native=[]
for i,name in enumerate(compiled):
    expected=int(methods[i]['address'],16)
    if name.startswith(('__ZN19NativeLegacyDisplay','__ZNK19NativeLegacyDisplay')):
        owned.append(i);require(i in (0,1,7,14,84,86,87,141,155,184,188,288,290,296,301) or i in pure,'Only qualified own methods and guarded leaves override')
    elif '25IOMobileFramebufferLegacy' in name:
        require(i not in pure,'No unresolved original abstract backend target');bindings[name]=expected
    else:
        require(i<168 and names.get(name)==expected,'Actual inherited native base symbol '+str(i));native.append(i)
require(owned==sorted([0,1,7,14,84,86,87,141,155,184,188,288,290,296,301]+pure),'Exact owned method set')
undefined={n for n,_,k in symbols if k==1 and 'IOMobileFramebufferLegacy' in n}
require(undefined==set(bindings),'All and only actual original-class imports qualified')
# Exact original IOSurface getters, independently observed through the real
# original swap request. These aliases call original code; no object layout
# is substituted and no original method/vtable is patched.
for name,address,field,wide in [
    ('descriptor',0xfffffff004b9ead0,48,True),('width',0xfffffff004b9ebbc,88,True),
    ('height',0xfffffff004b9ebc4,96,True),('stride',0xfffffff004b9ebd4,104,True),
    ('allocation',0xfffffff004b9ebfc,144,True),('format',0xfffffff004b9ebac,128,False),
    ('planes',0xfffffff004b9ebf4,152,False)]:
    expected=(0xf9400000 if wide else 0xb9400000)|((field//(8 if wide else 4))<<10)
    require(read(address,8)==struct.pack('<2I',expected,0xd65f03c0),'Exact original surface getter '+name)
    bindings['_native_legacy_surface_'+name]=address
# The original display-refresh notification sender (self, timestamp, continuous host time, frame interval): it sends the six-word
# vsync message (type 5) to every client entry queued at +5592, the tail queue that selector 72's registration (slot 208) fills at
# +5512 + type*16. The platform subclass that normally calls it on each panel vblank is not part of this port.
require(read(0xfffffff005796720,16)==bytes.fromhex('ff0302d1fd7b07a9fdc30191a0831ff8') and
        read(0xfffffff005796830,4)==bytes.fromhex('08ed4af9') and
        read(0xfffffff00579685c,16)==bytes.fromhex('e2230091c3008052240080521684ff97') and
        read(0xfffffff005795fbc,4)==bytes.fromhex('0ab182d2'),
        'Original vsync sender, its +5592 queue and six-word drop-allowed async send')
bindings['_native_legacy_vsync_notify']=0xfffffff005796720
# Slot 301 fills the six refresh words that setDisplayRefreshProperties publishes as IOMFBDisplayRefresh (minimum interval, its mach
# time, maximum interval, its mach time, step, its mach time; intervals in 32.32 seconds): the generic body fixes the minimum at
# 0x04444444 (1/60 s), and the render server paces its display timer by it.
require(int(methods[301]['address'],16)==0xfffffff00579ea5c and read(0xfffffff00579ea68,8)==bytes.fromhex('888888d28888a0f2') and
        read(0xfffffff00579ec54,4)==bytes.fromhex('08b544f9'),
        'Original slot-301 refresh words with the 1/60 s minimum, called by setDisplayRefreshProperties')
# setDisplayRefreshProperties itself (self): fills the six words through slot 301 and publishes them as IOMFBDisplayRefresh. Nothing in
# the family calls it (the platform subclass does), so without it the render server falls back to its own 1/60 s heartbeat.
require(read(0xfffffff00579ebb8,16)==bytes.fromhex('ff0302d1fd7b07a9fdc30191a0031ff8') and
        read(0xfffffff00579ed54,16)==bytes.fromhex('086540f90173ff90218c0c9100013fd6'),
        'Original setDisplayRefreshProperties publishing IOMFBDisplayRefresh through setProperty')
bindings['_native_legacy_set_refresh_properties']=0xfffffff00579ebb8
# Both protected destructors/constructors must follow the exact original chain.
def branch(pc):
    word=struct.unpack('<I',read(pc,4))[0];require(word&0xfc000000==0x94000000,'Actual protected base call')
    value=word&0x3ffffff;return pc+4*(value-(1<<26) if value&(1<<25) else value)
require(branch(0xfffffff0057865fc)==0xfffffff00577b510 and branch(0xfffffff005786630)==0xfffffff00577b55c,
        'Original protected Legacy C2/D2 really call their original Service bases')
# Actual caller passes only self at slot 288 and tests bit zero of w0.
require(read(0xfffffff005787ef8,16)==bytes.fromhex('080040f9088144f900013fd6e0000037'),
        'Original bool start_hardware() call and result check')
getter=int(methods[249]['address'],16)
for offset,field in [(0x30,5808),(0x58,5812)]:
    require(struct.unpack('<I',read(getter+offset,4))[0]==0xb9400000|((field//4)<<10)|(8<<5)|8,
            'Actual original width/height getter fields')
require(int(methods[295]['address'],16)==0xfffffff005795b88 and
        read(0xfffffff005795b98,16)==bytes.fromhex('e00740f9280080520101001272d3ff97'),
        'Actual original slot-295 passes true to the normal current completion body')
require(read(0xfffffff00578aa7c,24)==bytes.fromhex('080040f9084545f9030080d2e20303aa0400805200013fd6'),
        'Original completion calls optional report slot 337 with null outputs and zero limit')
require(read(0xfffffff005777e9c,16)==bytes.fromhex('080040f908e142f900013fd6c0000036'),
        'Original isMainDisplay dispatch uses only self and tests boolean bit zero')
asm=OUT/'original-bindings.S';asm.write_text(''.join(f'.globl {name}\n.set {name}, {va:#x}\n' for name,va in sorted(bindings.items())))
run(['/opt/unixnu-linux/llvm/bin/clang-24','-target','arm64-apple-ios17.0','-c',asm,'-o',OUT/'original-bindings.o'])
llvm=Path('/usr/bin');late=OUT/'NativeLegacyDisplay.late.o'
late_staging='none'
try:
    run([llvm/'llvm-objcopy','--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_NativeLegacyDisplay.cpp',obj,late])
    late_staging='objcopy'
except Exception:
    late.write_bytes(obj.read_bytes())

def code(data,info):
    sec=next(s for g in info['segments'] for s in g['sections'] if s['name']=='__text');return data[sec['fileoff']:sec['fileoff']+sec['size']]
require(code(raw,image)==code(late.read_bytes(),macho(late.read_bytes())),'Only owned initializer section registration changes')

# nabu: compile our lifecycle probe with the same kernel recipe
cc=json.loads((OUT/'compile-command.json').read_text())
probe_src=pathlib.Path('/root/merge/UniXNU-nabu/package/NativeLegacyProbe.cpp')
probe_obj=OUT/'NativeLegacyProbe.o'
probe_cmd=[str(probe_src) if str(x)==str(ROOT/'drivers/GopFramebuffer/NativeLegacyDisplay.cpp') else str(probe_obj) if str(x)==str(obj) else x for x in cc]
probe_cmd+= ['-I', str(probe_src.parent)]
# l575: package/NabuGpuScanoutHandoff.h is upstream's hand-off contract, staged verbatim, and it counts
# offers/completions/fallbacks as `++` on `volatile uint64_t`. clang 24 deprecates that and this recipe
# compiles with -Wextra -Werror, so the header alone stops the build (measured in l575-run.log: 10
# errors, all -Werror,-Wdeprecated-volatile, all inside that header; the identical command with
# -Wno-deprecated-volatile returns 0). Scoped to this probe object instead of build-arm-driver.py, so
# no other component's warning set moves, and the header stays byte-identical to upstream.
probe_cmd+= ['-Wno-deprecated-volatile']
run(probe_cmd,cwd=str((ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT')))
print('probe object', sha(probe_obj))
# nabu: the touch bring-up stack, compiled with the same kernel recipe and linked into the same late
# archive (the pattern the sheng device recipe uses). Default off: nothing in TouchProbe.cpp runs until
# sheng-touch=<1..4> is on the kernel command line.
DEV=pathlib.Path('/root/merge/UniXNU-nabu')
touch_dir=OUT/'touch'; touch_dir.mkdir(exist_ok=True)
def c_command(cmd):
    """the C++ kernel compile line, retyped for a C translation unit.
    The XNU line spells its language flags joined ('-xc++', '--driver-mode=g++'), so those exact
    forms are what has to change; -fsized-deallocation and -fc++-static-destructors=none are
    C++-only and are dropped."""
    out=[]
    for x in cmd:
        if x in ('-xc++', '--driver-mode=g++', '-fsized-deallocation') or x.startswith('-fc++-') or x.startswith('-fno-c++'):
            if x == '-xc++':
                out.append('-xc')
            continue
        if x.startswith('-std='):
            out.append('-std=gnu11'); continue
        if x.startswith('-stdlib='):
            continue
        out.append(x)
    return out
touch_includes=[]
for d in (ROOT/'drivers/QcomGeniSpi', ROOT/'drivers/ShengNovatekTouch', DEV/'package'):
    require(d.is_dir(), 'touch source directory present: %s' % d)
    touch_includes += ['-I', str(d)]
# l662i: the NabuAdreno link is created above, before the first compile.
touch_includes += ['-I', str(_nabu_adreno)]
# l662i: OriginalHidDeviceABI.h is generated into touch/abi/ and included by package/NabuTouch.cpp
# (line 9). The -I list did not carry that directory, so the compile stopped there.
touch_abi=touch_dir/'abi'
require(touch_abi.is_dir(), 'touch abi directory present: %s' % touch_abi)
touch_includes += ['-I', str(touch_abi)]
XCWD=str((ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT'))
# cc is the recorded command line of the display TU itself: source NativeLegacyDisplay.cpp -> obj.
DSRC=str(ROOT/'drivers/GopFramebuffer/NativeLegacyDisplay.cpp'); DOBJ=str(obj)
def retarget(cmd, src, out):
    return [str(src) if str(x)==DSRC else str(out) if str(x)==DOBJ else x for x in cmd]
touch_c=[ROOT/'drivers/QcomGeniSpi/GeniSpi.c', ROOT/'drivers/ShengNovatekTouch/NvtProtocol.c',
         ROOT/'drivers/ShengNovatekTouch/NvtTouchCore.c']
touch_objs=[]
for src in touch_c:
    tobj=touch_dir/(src.stem+'.o')
    run(retarget(c_command(cc), src, tobj)+touch_includes, cwd=XCWD)
    touch_objs.append(tobj)
touch_cpp=DEV/'package/TouchProbe.cpp'; touch_cpp_obj=touch_dir/'TouchProbe.o'
run(retarget(cc, touch_cpp, touch_cpp_obj)+touch_includes, cwd=XCWD)
touch_objs.append(touch_cpp_obj)
# l645: NabuTouch.cpp is back; IOHIDDevice is supplied by libiohidabs.a (absolute symbols).
nabutouch_cpp=DEV/'package/NabuTouch.cpp'; nabutouch_obj=touch_dir/'NabuTouch.o'
run(retarget(cc, nabutouch_cpp, nabutouch_obj)+touch_includes, cwd=XCWD)
touch_objs.append(nabutouch_obj)
# l645: GpuAbiProbe is a same-layer dependency of NabuTouch.o (it calls nabu_gpu_abi_probe).
# Without it the link fails with "_nabu_gpu_abi_probe, referenced from touchThread".
gpuabi_cpp=DEV/'package/GpuAbiProbe.cpp'; gpuabi_obj=touch_dir/'GpuAbiProbe.o'
run(retarget(cc, gpuabi_cpp, gpuabi_obj)+touch_includes, cwd=XCWD)
touch_objs.append(gpuabi_obj)
smmu_cpp=ROOT/'drivers/GopFramebuffer/NabuSmmuMapper.cpp'; smmu_obj=touch_dir/'NabuSmmuMapper.o'
run(retarget(cc, smmu_cpp, smmu_obj)+touch_includes, cwd=XCWD)
touch_objs.append(smmu_obj)
boot_diag=DEV/'package/BootDiagExport.c'; boot_diag_obj=touch_dir/'BootDiagExport.o'
run(retarget(c_command(cc), boot_diag, boot_diag_obj)+touch_includes+['-D', 'BSD_KERNEL_PRIVATE=1'], cwd=XCWD)
touch_objs.append(boot_diag_obj)
# the two providers this board has no object for yet (firmware blob, HID mailbox) are weakly defined in
# package/NabuTouchStubs.cpp so the archive closes; a real provider's strong definition wins over them.
stubs=DEV/'package/NabuTouchStubs.cpp'; stubs_obj=touch_dir/'NabuTouchStubs.o'
run(retarget(cc, stubs, stubs_obj)+touch_includes, cwd=XCWD)
touch_objs.append(stubs_obj)
# l538: the strong definition that replaces the weak firmware placeholder in the stubs -- the
# download path has been wired since l511 and has never had an image. Generated by
# E:/nabu/tmp/l515/gen_firmware_l538.py from the TWRP boot_a kernel at 0x13e7e30 (fw0); its
# sha256 and first 32 bytes are asserted by the package gate, and a strong symbol wins the
# link against the weak one, which is what the stubs' own comment promises.
fwm=DEV/'package/NabuTouchFirmware.cpp'; fwm_obj=touch_dir/'NabuTouchFirmware.o'
run(retarget(cc, fwm, fwm_obj)+touch_includes, cwd=XCWD)
touch_objs.append(fwm_obj)
print('touch objects', [(str(o.name), sha(o)) for o in touch_objs])

# l529: the keystore probe half. The service class itself comes in through board.toml (archives +=
# "component:AppleKeyStoreState" and "build-arm-driver.py AppleKeyStoreState"; components.toml:252-271
# declares path drivers/AppleKeyStoreState, os_abi ipados-17.7.11-21H461 and outputs
# build/apple-key-store-state/libAppleKeyStoreState.a), so what this archive still has to carry is
# package/KeyStoreProbe.cpp: the open/call counters, the publish-once guard and the two report rows.
# Upstream handed that object to the display probe TU -- package/LegacyDisplayProbe.cpp:494 calls
# native_keystore_probe() -- and that object has never been a member of this archive (the l518 link
# proved the symbols it adds are undefined), which is why the call now lives in NativeUfs.cpp.
# Upstream also ran llvm-objcopy over NativeKeyStoreEvents.o to strip __DATA,__mod_init_func and
# globalize __GLOBAL__sub_I_NativeKeyStoreEvents.cpp, then called the initialiser by name. No Mach-O
# objcopy exists on this farm (the HID note below records Ubuntu LLVM 18 rejecting both options, and
# /opt/unixnu-linux/llvm/bin carries clang-24 but no objcopy), so the initialiser is left where the
# compiler put it, and KeyStoreProbe.cpp's guard was rewritten from "no class of that name" to
# "we have not published one yet" -- with the class-name test kept as a recorded dependency, not a
# silent one.
ks_dir=OUT/'keystore'; ks_dir.mkdir(exist_ok=True)
ks_probe=DEV/'package/KeyStoreProbe.cpp'; ks_probe_obj=ks_dir/'KeyStoreProbe.o'
ks_src=ROOT/'drivers/AppleKeyStoreState/NativeKeyStoreEvents.cpp'
ks_events=ROOT/'build/apple-key-store-state/NativeKeyStoreEvents.o'
ks_a=ROOT/'build/apple-key-store-state/libAppleKeyStoreState.a'
# ALWAYS build the component here, before this archive is assembled. The first l532 build is the reason
# this line exists: the source was edited at 08:07, the recipe then put the PREVIOUS round's
# NativeKeyStoreEvents.o into libNativeLegacyDisplay.late.a at 08:08:14, and the kernel stage rebuilt
# the component at 08:08:34 -- too late, because the framework does not put libAppleKeyStoreState.a on
# the link line (the fifth l529 build proved that with the undefined _native_keyevents_allocate), so the
# object that reaches the kernel is the one THIS archive carries. Reading the delivered image proved the
# damage: externalMethod's disassembled body was byte-identical between l531b and l532 (130 instructions,
# same sha256), i.e. the two new cases were NOT in the package I was about to ship. Rebuilding
# unconditionally costs seconds, and the mtime check is what turns "I asked for a rebuild" into evidence.
require((ROOT/'scripts/build-arm-driver.py').is_file(),
        'the component build entry point is farmed into the tree: '+str(ROOT/'scripts/build-arm-driver.py'))
print(run(['python3',ROOT/'scripts/build-arm-driver.py','AppleKeyStoreState'],cwd=str(ROOT),text=True),end='')
require(ks_events.is_file(),'the component object exists: '+str(ks_events))
require(ks_events.stat().st_mtime >= ks_src.stat().st_mtime,
        'the component object is not newer than the component source, so the archive would carry a stale'
        ' object: obj=%d src=%d' % (ks_events.stat().st_mtime, ks_src.stat().st_mtime))
require(ks_a.is_file(),'the AppleKeyStoreState component archive exists: '+str(ks_a)+
      ' -- if this fails, build-arm-driver.py did not produce the output components.toml:269 declares')
require('NativeKeyStoreEvents' in subprocess.check_output(['llvm-ar','-t',str(ks_a)],text=True),
        'the component archive carries the AppleKeyStore service class object')
require(ks_probe.is_file(),'the keystore probe source is present: %s'%ks_probe)
run(retarget(cc, ks_probe, ks_probe_obj)+['-I',str(ROOT/'drivers/AppleKeyStoreState')]+touch_includes, cwd=XCWD)
print('keystore probe object', sha(ks_probe_obj), 'component object', sha(ks_events), 'archive', sha(ks_a))

# l516: the HID layer. Upstream keeps it in its own component (components.toml ShengHidInput,
# revision 5f3a37ce) built by build-sheng-hid-input.py, which can only run on macOS - it shells out
# to `xcrun --sdk iphoneos` for the SDK and `brew --prefix llvm` for objcopy, and it pins its
# reference image by the hash of that board's own prelink candidate. scripts/build-nabu-hid-input.py
# is our port of its qualification half: the reference is the candidate this farm already carries
# (its recorded com.apple.iokit.IOHIDFamily __TEXT_EXEC hash is byte-identical to upstream's pin),
# and the three checks 8714 asked for are require() calls, not documentation: 207 slots non-zero,
# 24 zero bytes after the table, slot181 == handleReport. It writes the ABI header and the
# metaclass/vtable bindings; everything is compiled by the same kernel recipe as the rest of this
# archive, and the static initialiser is deferred exactly the way the display object's is because
# package/HidInputProbe.cpp calls it by name after OSMetaClass::preModLoad.
# 8714's rule: the touch package carries no HID layer, so a touch reading can never be
# explained by a second subsystem arriving in the same flash. The HID sources are in the tree
# now, so the decision has to be made here: NABU_HID=1 builds them into the archive; anything
# else leaves the l515d shape, and the weak stubs in package/NabuTouchStubs.cpp answer the
# census fields with the same 'never attempted' values HidInputProbe.cpp initialises them to.
if os.environ.get('NABU_HID') != '1':
    hid_objs=[]
    print('hid: NABU_HID unset -- this package carries no HID layer')
else:
    hidbuild=ROOT/'scripts/build-nabu-hid-input.py'
    require(hidbuild.is_file(),'the HID qualification must run from the farm tree: it reads kits/XnuPrelink '
            'and build/xnu-prelink there, and the device repo scripts/ copy is only its source: '+str(hidbuild))
    hidrun=subprocess.run(['python3',hidbuild],cwd=str(ROOT),capture_output=True,text=True)
    require(hidrun.returncode==0,'HID ABI qualification failed: '+(hidrun.stdout+hidrun.stderr)[-500:])
    HID=ROOT/'build/nabu-hid-input'
    require((HID/'OriginalHidDeviceABI.h').is_file() and (HID/'original-bindings.S').is_file(),
            'HID qualification wrote its header and bindings')
    hid_includes=['-I',str(HID)]+touch_includes
    hid_objs=[]
    for src,name in ((DEV/'package/NativeHidInput.cpp','NativeHidInput.o'),
                     (DEV/'package/HidInputProbe.cpp','HidInputProbe.o')):
        require(src.is_file(),'HID source present: %s'%src)
        o=HID/name
        run(retarget(cc,src,o)+hid_includes,cwd=XCWD)
        hid_objs.append(o)
    # The qualification could only name the four base bindings; the slots our object actually imports
    # (opaque201..206, handleIsOpen and whatever else the compiler put in the vtable) come from the
    # object itself -- upstream's rule, applied by --bindings right here, before the bindings assemble.
    base_before=(HID/'original-bindings.S').read_text()
    brun=subprocess.run(['python3',hidbuild,'--bindings',str(HID/'NativeHidInput.o')],cwd=str(ROOT),
                        capture_output=True,text=True)
    require(brun.returncode==0,'HID bindings pass failed: '+(brun.stdout+brun.stderr)[-600:])
    print('  '+brun.stdout.strip().replace('\n','\n  '))
    base_after=(HID/'original-bindings.S').read_text()
    require(len(base_after.splitlines())>len(base_before.splitlines()),
            'the bindings pass added slots to the four base bindings')
    # Named by the method, not by the mangling: the linker's report of the first l516 build says
    # __ZNK11IOHIDDevice12handleIsOpenEPK9IOService (a const member), and my first version of this check
    # typed __ZN... and failed on a name that was bound all along. If it fails now, it prints what is
    # actually bound instead of leaving me to guess again.
    for must in ('handleIsOpen','opaque201','opaque206'):
        require(must in base_after,'the import %s is not bound. bound names: %s'
                % (must, sorted({w for w in base_after.split() if w.startswith('__Z')})))
    run(['/opt/unixnu-linux/llvm/bin/clang-24','-target','arm64-apple-ios17.0','-c',HID/'original-bindings.S',
         '-o',HID/'hid-original-bindings.o'])
    # Upstream hands this object to llvm-objcopy to take its static initialiser out of
    # __DATA,__mod_init_func and make __GLOBAL__sub_I_NativeHidInput.cpp external, so the probe can run
    # it between OSMetaClass::preModLoad and postModLoad. This farm has Ubuntu LLVM 18 objcopy only and
    # it rejects both options for Mach-O ('option is not supported for MachO'); there is no second
    # objcopy on the box (/opt/unixnu-linux/llvm/bin carries clang-24 but no objcopy). The display half
    # of this same archive has always hit that and swallowed it -- build-report.json records
    # late_staging="none", all 96 __GLOBAL__sub_I_* in the linked kernel are still local, and that class
    # registers on hardware anyway. scripts/build-platform-power.py:152 states the fallback for the same
    # reason: "Linux llvm-objcopy cannot edit Mach-O sections; the kernel link merges __mod_init_func".
    # So the initialiser stays where the compiler put it and runs with the kernel's own static-init
    # table. Two checks instead of a claim, both read off the archived object: __mod_init_func is still
    # in it (the class really does register at boot), and the initialiser is still non-external (so
    # nothing can call it by name from another TU: the kernel link rejects the reference, so
# package/HidInputProbe.cpp only asks OSMetaClass whether the class is registered).
    hid_late=HID/'NativeHidInput.o'
    hid_blob=hid_late.read_bytes()
    require(sum(1 for g in macho(hid_blob)['segments'] for s in g['sections'] if s['name']=='__mod_init_func')==1,
            'the HID object keeps its __mod_init_func entry: boot-time registration is the staging here')
    import subprocess as _sp
    _nm=_sp.run(['llvm-nm','-m',str(hid_late)],capture_output=True,text=True).stdout
    require('non-external __GLOBAL__sub_I_NativeHidInput.cpp' in _nm,
            'the HID initialiser is present and local: it runs with the kernel static-init table and is not callable by name from another TU')
    hid_objs=[hid_late,HID/'hid-original-bindings.o']+hid_objs[1:]
    print('hid objects',[(str(o.name),sha(o)) for o in hid_objs])
    print('hid staging: __mod_init_func kept (no Mach-O section surgery available on this farm)')

archive=OUT/'libNativeLegacyDisplay.late.a' 
archive.unlink(missing_ok=True)
# ks_events is rebuilt at its own line above (right after the archive path is defined), before this
# ar command -- see the l532 note there for why the rebuild must not be conditional.
require(ks_events.is_file(),'the component object to archive: '+str(ks_events))
require(ks_events.stat().st_mtime >= ks_src.stat().st_mtime,
        'the component object is stale at archive time: obj=%d src=%d'
        % (ks_events.stat().st_mtime, ks_src.stat().st_mtime))
run(['llvm-ar','rcs',archive,late,OUT/'original-bindings.o',probe_obj]+touch_objs+hid_objs
    +[ks_probe_obj,ks_events])
sources=[ROOT/'drivers/GopFramebuffer/NativeLegacyDisplay.cpp',
         ROOT/'drivers/GopFramebuffer/NativeLegacyRequest.h',
         OUT/'OriginalLegacyFramebufferABI.h',
         ROOT/'drivers/AppleKeyStoreState/NativeKeyStoreEvents.cpp',
         ROOT/'drivers/GopFramebuffer/NabuSmmuMapper.cpp',
         ROOT/'drivers/GopFramebuffer/NabuSmmuMapper.h',
         ]
report=dict(
    source_sha256={str(p.relative_to(ROOT)):sha(p) for p in sources},
    generated_abi_sha256=sha(OUT/'OriginalLegacyFramebufferABI.h'),
    archive_sha256=sha(archive),
    original_candidate_sha256=sha(candidate),
    original_audit_sha256=sha(ROOT/'build/display-analysis/mobile-graphics-legacy-abi.json'),
    compiled_object_sha256=sha(obj), late_object_sha256=sha(late), probe_object_sha256=sha(probe_obj),
    touch_objects_sha256={str(o.name):sha(o) for o in touch_objs},
    # the probe source lives in the device repo, outside the farm tree, so it cannot go through the
    # relative_to() path map above; it is recorded here by name instead (l529).
    keystore_probe_object_sha256=sha(ks_probe_obj), keystore_probe_source_sha256=sha(ks_probe),
    # NOT in sources[]: build-xnu-4k-baseline.sh:676-683 re-hashes every source_sha256 entry before it
    # will link, and the kernel stage runs "build-arm-driver.py AppleKeyStoreState" again (kernel.log
    # shows it twice: component-AppleKeyStoreState and archive-18-build-arm-driver), so a rebuilt .a is
    # byte-different by the time that check reads it. The first l529 build failed on exactly that, with
    # the message "Rebuild the owned Legacy archive after changing
    # build/apple-key-store-state/libAppleKeyStoreState.a". Record it as an output, not as a source.
    keystore_component_archive_sha256=sha(ks_a),
    # The service class goes into THIS archive as an object, not as a library: the fifth l529 build got
    # as far as the kernel link and died on
    #     Undefined symbols for architecture arm64: "_native_keyevents_allocate", referenced from
    #     _native_keystore_probe in libNativeLegacyDisplay.late.a(KeyStoreProbe.o)
    # which says the board's archives entries farmed and built the component (kernel.log shows both
    # "component-AppleKeyStoreState" and "archive-18-build-arm-driver" running) but never put
    # libAppleKeyStoreState.a on the link line. Upstream has always handled it the way we do now:
    # build-gop-legacy-display.py.bak-l324:313-314 puts keyevents_late into the same ar command.
    keystore_event_object_sha256=sha(ks_events),
    bindings={n:hex(v) for n,v in bindings.items()},
    virtual_entries=358, owned_entries=owned, inherited_native_entries=native,
    guarded_backend_leaves=[i for i in pure if i not in (213,337)],
    late_staging=late_staging,
    runtime_gate='skipped: QEMU gdb-stop.json checkpoint unpublished (FAMILY_TEXT_SHA matched)',
    original_instructions_modified=False, driver_initialized=False, display_presented=False, desktop=False)
# l662i: the touch translation units live in the device repo, outside the farm tree, so they cannot
# go through the relative_to() path map above. Record them by absolute path instead:
# build-xnu-4k-baseline.sh re-hashes every source_sha256 entry as (root/name), and pathlib resolves
# an absolute name to itself, so a stale archive now fails the link instead of silently producing a
# package whose kernel still carries the pre-edit touch code (the 86ee16b5 incident).
for _touch_src in (DEV/'package/NabuTouch.cpp', DEV/'package/TouchProbe.cpp',
                   DEV/'package/GpuAbiProbe.cpp', DEV/'package/NabuTouchFirmware.cpp',
                   DEV/'package/NabuTouchStubs.cpp', DEV/'package/BootDiagExport.c',
                   DEV/'package/NativeLegacyProbe.cpp'):
    report['source_sha256'][str(_touch_src)]=sha(_touch_src)
(OUT/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
print('nabu legacy build OK')
print('archive', sha(archive))
print('abi_header', sha(OUT/'OriginalLegacyFramebufferABI.h'))
print('bindings_S', sha(OUT/'original-bindings.S'))
print('owned', owned)
