#!/usr/bin/env python3
"""Build the minimal boot progress observer against this source kernel's actual ABI.

kits/<kit>/BootProgress.c is the full sheng monitor and needs the UFS, Virtio block, ADSP, touch,
GPU and cpufreq components to be built: it reads their counters directly, so a reduced config
fails the link on ~118 undefined symbols. BootProgressMin.c implements the same thirteen entry
points with nothing outside the kernel image, which is what this script builds.

The compile recipe is taken from a real object of this tree rather than written out here, so the
archive always matches the kernel it is linked into. The undefined-symbol check at the end is the
guard that keeps the failure above from coming back: an object that needs a symbol this build does
not define is rejected here instead of at the kernel link.
"""
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/boot-progress'
SOURCE = ROOT / 'kits/sheng/BootProgressMin.c'
RECORD = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json'
# The host has no Apple ar; llvm-ar writes the same BSD archive the linker reads.
AR = next((c for c in ('llvm-ar', 'ar') if shutil.which(c)), 'ar')
NM = next((c for c in ('llvm-nm', 'nm') if shutil.which(c)), 'nm')
# The UEFI checkout is a sibling of this repository, not part of the assembled tree.
UEFI = next((p for p in (ROOT / 'uefi-sheng', ROOT.parent / 'uefi-sheng',
                         Path(os.environ.get('UNIXNU_UEFI_ROOT', '/')))
             if (p / 'Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib').is_dir()),
            ROOT / 'uefi-sheng')
FONT = UEFI / 'Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib/Font.h'
if not FONT.is_file():
    raise SystemExit(f'missing {FONT}: the boot progress panel needs its 5x12 glyph table')

OUT.mkdir(parents=True, exist_ok=True)
record = json.loads(RECORD.read_text().strip().rstrip(','))
obj = OUT / 'BootProgressMin.o'

command = []
args = record['arguments']
index = 0
while index < len(args):
    arg = args[index]
    if arg == record['file']:
        command.append(str(SOURCE))
    elif arg in ('-o', '-MF', '-MT', '-MJ'):
        if arg == '-o':
            command += ['-o', str(obj)]
        index += 1
    elif arg == '-Xclang' and args[index + 1].startswith('-W'):
        index += 1
    elif arg == '-D' and args[index + 1].startswith('XNU_BOOT_PROGRESS='):
        index += 1
    elif not arg.startswith(('-W', '-DXNU_BOOT_PROGRESS=')):
        command.append(arg)
    index += 1
command += ['-DXNU_BOOT_PROGRESS=1', '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror',
            '-I', str(ROOT / '.cache/xnu-work/osfmk'),
            '-I', str(FONT.parent)]
subprocess.run(command, cwd=record['directory'], check=True, timeout=120)
(OUT / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')

# The kernel link force-loads this archive, so every symbol the object needs must already exist in
# the kernel image. Anything else is exactly the failure this file exists to avoid: check against
# the kernel that is on disk, plus the few names the compiler and the physmap always contribute.
KERNEL = ROOT / 'kernel/kernel.macho'
undefined = sorted({
    line.split()[-1]
    for line in subprocess.run([NM, '-u', str(obj)], capture_output=True, text=True, check=True)
    .stdout.splitlines() if line.strip()
})
provided = {'memset', 'memcpy', 'memmove',              # compiler-generated, provided by libkern
            '___stack_chk_fail', '___stack_chk_guard',  # CONFIG_STACK_PROTECTION
            '_physmap_base'}                            # the physical aperture, read at bind
if KERNEL.is_file():
    provided |= {
        line.split()[-1]
        for line in subprocess.run([NM, '--defined-only', str(KERNEL)],
                                   capture_output=True, text=True, check=False).stdout.splitlines()
        if line.strip() and len(line.split()) >= 3
    }
unexpected = [name for name in undefined if name not in provided]
if unexpected:
    raise SystemExit('BootProgressMin.o needs symbols this build does not provide: ' +
                     ', '.join(unexpected))

archive = OUT / 'libBootProgress.a'
archive.unlink(missing_ok=True)
subprocess.run([AR, 'rcs', str(archive), str(obj)], check=True, timeout=30)
(OUT / 'build-report.json').write_text(json.dumps({
    'source_sha256': hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
    'font_sha256': hashlib.sha256(FONT.read_bytes()).hexdigest(),
    'object_bytes': obj.stat().st_size,
    'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
    'undefined_symbols': undefined,
    'hardware_execution': False,
}, indent=2) + '\n')
print(f'PASS: minimal boot progress archive ({obj.stat().st_size} B object, '
      f'{len(undefined)} allowed undefined symbol(s))')
