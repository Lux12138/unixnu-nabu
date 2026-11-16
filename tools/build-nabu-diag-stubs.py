#!/usr/bin/env python3
"""Define weak zero-filled providers for the sheng GPU statistics the diagnostic layer reads.

BootProgress.c/BootLog.c declare native_adreno_* as weak_import; a static kernel link still requires a
definition. These weak definitions answer with zeros and are overridden automatically once the real
Adreno component is linked. AdspFirmware.o (fat_* VFS reader, not used by the page) is dropped from the
archive here; the other official members stay.
"""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/boot-progress'
MEMBERS = ['BootProgress.o', 'BootFaultProbe.o', 'BootWatchdogPolicy.o', 'BootDiagExport.o',
           'BootProcessProbe.o', 'BootUiTraceProbe.o', 'BootLog.o', 'BootReset.o']

objects = [OUT / name for name in MEMBERS]
missing = [str(o) for o in objects if not o.is_file()]
if missing:
    raise SystemExit('missing diagnostic objects: ' + ', '.join(missing))

undefined = set()
for obj in objects:
    for line in subprocess.check_output(['llvm-nm', '-u', str(obj)], text=True).splitlines():
        name = line.strip()
        if name.startswith('_native_adreno'):
            undefined.add(name[1:])
if not undefined:
    raise SystemExit('no native_adreno_* references found')

source = OUT / 'nabu-diag-stubs.c'
lines = ['/* generated: weak zero providers for absent sheng GPU statistics */']
for name in sorted(undefined):
    lines.append('__attribute__((weak, used, aligned(16))) volatile unsigned char %s[256] = {0};' % name)
source.write_text('\n'.join(lines) + '\n')

obj = OUT / 'nabu-diag-stubs.o'
subprocess.run(['clang', '-target', 'arm64-apple-ios17.0', '-O2', '-c', str(source), '-o', str(obj)],
               check=True, timeout=120)

archive = OUT / 'libBootProgress.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive)] + [str(o) for o in objects] + [str(obj)],
               check=True, timeout=60)
print('PASS: %d weak stubs; archive = %d members (AdspFirmware.o excluded)'
      % (len(undefined), len(objects) + 1))
