#!/usr/bin/env python3
"""Build the nabu VerifiedRoot XNU provider archive from the signed OS/SystemOS policies."""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TREE = ROOT
OUT = ROOT / 'build/nabu-root-bundle'
POLICY = ROOT.parent / 'root-policy'
OUT.mkdir(parents=True, exist_ok=True)

OS_MANIFEST = POLICY / 'OS/manifest.bin'
SYSTEMOS_MANIFEST = POLICY / 'SystemOS/manifest.bin'
KEY = POLICY / 'OS/trusted-key.bin'
for path in (OS_MANIFEST, SYSTEMOS_MANIFEST, KEY):
    if not path.is_file():
        raise SystemExit(f'missing {path}; run gen-root-policy.py first')

record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/'
                     'IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))


def compile_from(source, destination, extra=()):
    arguments = record['arguments']
    command = [arguments[0], '-xc']
    index = 1
    while index < len(arguments):
        value = arguments[index]
        if value.startswith('-x'):
            index += 1
            continue
        if value.startswith('-std='):
            index += 1
            continue
        if value == record['file']:
            command.append(str(source))
        elif value == '-o':
            command.extend(['-o', str(destination)])
            index += 1
        elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W'):
            command.append(value)
        index += 1
    command.extend(['-I', str(ROOT / 'kits/VerifiedRoot/include'), '-I', str(ROOT / '.cache/xnu-work/osfmk'),
                    '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    '-Wno-missing-field-initializers', '-Wno-missing-braces', '-Wno-unused-const-variable',
                    '-Wno-c99-designator', '-Wno-reorder-init-list',
                    '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier', *extra])
    result = subprocess.run(command, cwd=record['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        raise SystemExit(result.stdout[-4000:])


asm = OUT / 'RootBundleData.S'
asm.write_text(f'''.section __DATA,__data
.globl _verified_root_manifest
_verified_root_manifest:
.incbin "{OS_MANIFEST}"
.globl _verified_root_manifest_bytes
_verified_root_manifest_bytes:
.quad . - _verified_root_manifest
.globl _verified_root_systemos_manifest
_verified_root_systemos_manifest:
.incbin "{SYSTEMOS_MANIFEST}"
.globl _verified_root_systemos_manifest_bytes
_verified_root_systemos_manifest_bytes:
.quad . - _verified_root_systemos_manifest
.globl _verified_root_key
_verified_root_key:
.incbin "{KEY}"
.globl _verified_root_policies
_verified_root_policies:
.quad _verified_root_manifest, {OS_MANIFEST.stat().st_size}
.quad _verified_root_systemos_manifest, {SYSTEMOS_MANIFEST.stat().st_size}
.globl _verified_root_policy_count
_verified_root_policy_count:
.quad 2
''')

compile_from(ROOT / 'kits/VerifiedRoot/src/verified_root.c', OUT / 'verified_root.o')
compile_from(ROOT / 'kits/VerifiedRoot/adapters/xnu_crypto.c', OUT / 'xnu_crypto.o')
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-c', str(asm),
                '-o', str(OUT / 'RootBundleData.o')], check=True)
archive = OUT / 'libNabuRootBundle.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(OUT / 'verified_root.o'),
                str(OUT / 'xnu_crypto.o'), str(OUT / 'RootBundleData.o')], check=True)
print(f'PASS: {archive} (OS {OS_MANIFEST.stat().st_size} B, SystemOS {SYSTEMOS_MANIFEST.stat().st_size} B)')
