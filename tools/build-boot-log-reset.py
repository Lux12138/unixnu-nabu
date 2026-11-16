#!/usr/bin/env python3
"""Compile BootLog.c (sheng-log=1) and BootReset.c (sheng-autoreset) and add them to libBootProgress.a.

The recipe is copied from UniXNU-sheng scripts/build-gop-legacy-display.py:207-236 (process_cmd from the
startup.o.json record, source swapped, "-I kits/sheng" appended). The official build-boot-progress.py only
builds five files and does not include these two; this entry adds exactly those two objects to its archive.
"""
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/boot-progress'
OUT.mkdir(parents=True, exist_ok=True)
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/'
                     'startup.o.json').read_text().strip().rstrip(','))
process_source = ROOT / 'kits/sheng/BootProcessProbe.c'   # substitution template only
process_obj = OUT / 'BootProcessProbe.o'
process_cmd = []
args = record['arguments']
i = 0
while i < len(args):
    value = args[i]
    if value == record['file']:
        process_cmd.append(str(process_source))
    elif value in ('-o', '-MF', '-MT', '-MJ'):
        if value == '-o':
            process_cmd += ['-o', str(process_obj)]
        i += 1
    elif value == '-Xclang' and args[i + 1].startswith('-W'):
        i += 1
    elif value == '-D' and args[i + 1].startswith('XNU_BOOT_PROGRESS='):
        i += 1
    elif value.startswith('-DXNU_BOOT_PROGRESS='):
        pass
    elif not value.startswith('-W'):
        process_cmd.append(value)
    i += 1
process_cmd += ['-DXNU_BOOT_PROGRESS=1', '-Wall', '-Wextra', '-Werror', '-Wno-array-bounds', '-mgeneral-regs-only',
                '-I', str(ROOT / '.cache/xnu-work/osfmk')]

objects = []
for source_name in ('BootProcessProbe.c', 'BootUiTraceProbe.c', 'BootLog.c', 'BootReset.c'):
    source = ROOT / 'kits/sheng' / source_name
    obj = OUT / (source.stem + '.o')
    command = [str(source) if a == str(process_source) else str(obj) if a == str(process_obj) else a
               for a in process_cmd]
    command += ['-I', str(ROOT / 'kits/sheng')]
    print('+', ' '.join(command))
    subprocess.run(command, cwd=record['directory'], check=True, timeout=180)
    objects.append(str(obj))

archive = OUT / 'libBootProgress.a'
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *objects], check=True, timeout=30)
print('PASS: BootProcess/BootUiTrace/BootLog/BootReset added to', archive)
