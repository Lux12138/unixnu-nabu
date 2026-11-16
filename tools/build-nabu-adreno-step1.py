#!/usr/bin/env python3
"""Compile the GPU step-1 base into the archive the XNU link already knows how to force-load.

Why this exists instead of `build-arm-driver.py <component>`: that script resolves exactly one
translation unit (`filename = next((n for n in source_files if n.endswith('.cpp')), ...)`) and puts it
at `drivers/<id>/<file>`. This component is 21 TUs, so the compile loop lives here; the flags are
still the kernel's own, replayed from the same `IOPlatformExpert.cpo.json` record that script uses.

Step 1 is deliberately "compile only, never start":
  * no `NabuGpuStart.c` (STARTUP_ARG EARLY_BOOT would call it by itself),
  * no Submit/Timer/UserClient/Client/Publish/Accelerator/Scheduler (publishing layer; Publish and
    Accelerator also do work in static initialisation),
  * no `NabuGpuResources.cpp` (the board resource table; it wants members only the a640 fork's
    `NativeAdrenoA6xxService.h` has, and step 1 does not read firmware),
  * no `NativeAdrenoPwrupRegs.h` (its own header says the table is A7XX-only).

The archive lands at `build/adreno740/libAdreno740.a` because that is the path
`scripts/build-xnu-4k-baseline.sh` force-loads under `XNU_NATIVE_ADRENO=1`; reusing it keeps this
package to three edits (defconfig switch, this recipe, one board.toml archive entry) instead of
touching a shared build script.
"""
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'drivers' / 'NabuAdreno'
if not SRC.is_dir():
    # the tree sync may not carry a driver directory that is not in components.toml yet;
    # fall back to the device checkout the tree was made from.
    SRC = Path('/root/merge/UniXNU-nabu/drivers/NabuAdreno')
OUT = ROOT / 'build' / 'adreno740'
ARCHIVE = OUT / 'libAdreno740.a'
RECORD = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'

# 20 from the 6xx skeleton + the SMMU streams table. NabuGpuResources.cpp is excluded on purpose.
TU6XX = [
    'NativeAdrenoMemory', 'NativeAdrenoVm', 'NativeAdrenoCpu', 'NativeAdrenoKernelTime',
    'NativeAdrenoRing', 'NativeAdrenoCpInit', 'NativeAdrenoIrq', 'NativeAdrenoSmmuMemory',
    'NativeAdrenoSmmuContext', 'NativeAdrenoGmuMemory', 'NativeAdrenoGmuContext',
    'NativeAdrenoGpuContext', 'NativeAdrenoA6xxPower', 'NativeAdrenoA6xxGmu',
    'NativeAdrenoA6xxHwInit', 'NativeAdrenoA6xxSmmu', 'NativeAdrenoA6xxZap',
    'NativeAdrenoService', 'NativeAdrenoSha256', 'NativeAdrenoGpuProfile',
    # the base layer references submit/timer attach; the board's plan allows these two
    'NativeAdrenoSubmit', 'NativeAdrenoTimer',
]
# real sources for the rest of the missing symbols (kept out of the 6xx name list on purpose)
C_EXTRA = ['ArmSmmu4K', 'ArmSmmu4KAudit', 'ArmSmmu4KContext', 'ArmSmmu4KLive',
           'ArmSmmu4KMmio', 'ArmSmmu4KProgram', 'ArmSmmu4KStreams', 'ArmSmmu4KTableMemory',
           'AdrenoPM4']
CPP_EXTRA = [
    # publishing layer: the base layer calls into it, and nothing in it runs at load time
    # (no constructors; the only global is a zero-initialised POD)
    'NativeAdrenoPublish', 'NativeAdrenoAccelerator', 'NativeAdrenoScheduler',
    'NativeAdrenoClient', 'NativeAdrenoUserClient', 'NativeAdrenoFirmwareDefaults',
    # weak firmware defaults live there; the two symbols only the resource table has:
    'nabu_adreno_step1_compat',
]
FORBIDDEN = [
    # step 2 already shipped the 9-line STARTUP_ARG shim; step 3 needs the real resource table,
    # and with the a640 base the struct layout finally matches it (that was the ODR blocker).
    'NativeAdrenoPwrupRegs.h',
]


def replay_arguments(record, source, obj):
    """The same filter build-arm-driver.py applies: drop the record's own source/-o, drop -W*,
    then add this tree's warning set and the osfmk include."""
    args = record['arguments']
    out, i = [], 0
    while i < len(args):
        v = args[i]
        if v == record['file']:
            out.append(str(source))
            i += 1
            continue
        if v == '-o':
            out.extend(['-o', str(obj)])
            i += 2
            continue
        if v == '-Xclang' and i + 1 < len(args) and args[i + 1].startswith('-W'):
            i += 2
            continue
        if not v.startswith('-W'):
            out.append(v)
        i += 1
    if str(source).endswith('.c'):
        # record 用的是 clang 的合并写法 -xc++；直接用 -x c 会被它覆盖，必须换掉这个参数本身
        out = [('-xc' if a == '-xc++' else a) for a in out if not a.startswith('-std=')]
        out = [out[0], '-std=gnu11'] + out[1:]
    # l575: the publication block in NativeAdrenoService.cpp is inside
    # `#if defined(NABU_GPU_METAL) && NABU_GPU_METAL`, and nothing in this tree ever defined it,
    # so setName("NativeAdrenoGpu")/registerService() were not in the image at all. The display
    # side now supplies the 16 hand-off symbols this turns on (package/NativeLegacyProbe.cpp).
    out += ['-DNABU_GPU_METAL=1',
           '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
            '-Wno-missing-designated-field-initializers', '-Wno-error=missing-designated-field-initializers',
            '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
            '-Wno-unnecessary-virtual-specifier',
            # upstream never builds with -Wextra; (S){0} in ArmSmmu4KStreams.c is not a defect
            '-Wno-missing-field-initializers', '-Wno-missing-braces', '-Wno-deprecated-volatile',
            '-I', str(ROOT / '.cache/xnu-work/osfmk'),
            '-I', str(SRC)]
    return out


def main():
    if not SRC.is_dir():
        raise SystemExit('NabuAdreno sources missing at %s' % SRC)
    present = {p.name for p in SRC.iterdir()}
    leaked = [n for n in FORBIDDEN if n in present]
    if leaked:
        raise SystemExit('step 1 must not ship: %s' % ', '.join(leaked))

    # 扫目录而不是维护清单：这一轮换过三次基底，清单漂移过两次，目录不会说谎。
    # FORBIDDEN 在上面已经保证那几个文件根本不在目录里；这里再滤一次是防手滑。
    names = sorted(n for n in os.listdir(SRC)
                   if n.endswith(('.c', '.cpp')) and n not in FORBIDDEN)
    tUs = [(SRC / n, OUT / (n[:-4] + '.o')) for n in names]
    for src, _ in tUs:
        if not src.is_file():
            raise SystemExit('missing translation unit: %s' % src)

    if not RECORD.is_file():
        raise SystemExit('no compile record at %s (an XNU build must have run once)' % RECORD)
    record, _ = json.JSONDecoder().raw_decode(RECORD.read_text(encoding='utf-8', errors='replace'))

    OUT.mkdir(parents=True, exist_ok=True)
    objects = []
    for src, obj in tUs:
        cmd = replay_arguments(record, src, obj)
        r = subprocess.run(cmd, cwd=record['directory'], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True)
        if r.returncode:
            (OUT / 'compile-fail.log').write_text(r.stdout)
            raise SystemExit('%s failed:\n%s' % (src.name, r.stdout[-2500:]))
        objects.append(obj)

    if ARCHIVE.exists():
        ARCHIVE.unlink()
    ar = shutil.which('llvm-ar') or shutil.which('ar')
    if not ar:
        raise SystemExit('no ar/llvm-ar on PATH')
    subprocess.run([ar, 'rcs', str(ARCHIVE), *[str(o) for o in objects]], check=True)
    size = ARCHIVE.stat().st_size
    print('nabu-adreno step1: %d objects -> %s (%d bytes)' % (len(objects), ARCHIVE, size))
    if size <= 0:
        raise SystemExit('archive is empty')
    return 0


if __name__ == '__main__':
    sys.exit(main())
