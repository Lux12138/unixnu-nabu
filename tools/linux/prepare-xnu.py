#!/usr/bin/env python3
"""The Linux-only steps of scripts/build-xnu-4k-baseline.sh, run from the build tree.

    prepare-xnu.py source   after the xnu patches: rewrite the xnu sources that assume Apple's clang or BSD tools,
                            and install TargetConditionals.h, which the iPhoneOS SDK provides, into the fakeroot.
    prepare-xnu.py libs     after installhdrs: libcc_kext.a in the fakeroot. MakeInc.def links it as -lcc_kext; a Mac
                            takes it from Apple clang.

Each rewrite checks whether it is still needed, so both modes can run on every build.
"""
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
XNU = ROOT / '.cache/xnu-work'
FAKEROOT = ROOT / '.cache/fakeroot'
# llvm-project at the commit setup-toolchain.py builds clang from, and the Libc that macOS 14.6 ships with
# xnu-10063.141.1 (distribution-macOS release.json).
LLVM_COMMIT = '58c46bae21181fb4240cf964461bea738d96d50e'
LIBC_COMMIT = '899a3b2d52d95d75e05fb286a5e64975ec3de757'   # Libc-1592.100.35
PROFILE_SOURCES = ('InstrProfiling', 'InstrProfilingBuffer', 'InstrProfilingPlatformDarwin', 'InstrProfilingWriter',
                   'InstrProfilingInternal', 'InstrProfilingVersionVar')

ISA_APPLE = '#if defined(__clang__) && ((defined(__apple_build_version__) && __apple_build_version__ > 5010000))\n'
ISA_ANY = '#if defined(__clang__) && (!defined(__apple_build_version__) || __apple_build_version__ > 5010000)\n'
SEEK_OLD = '''	if (offset != lseek(fd, offset, SEEK_SET)) {
		err = kErrorDiskFull;
	} else {
		err = kErrorNone;
	}
'''
SEEK_NEW = '''	if (offset != lseek(fd, offset, SEEK_SET)) {
		struct stat st;
		/* Linux lseek() returns 0 on character devices such as the
		 * /dev/null that check_all_exports writes to; not an error. */
		if (0 == fstat(fd, &st) && S_ISCHR(st.st_mode)) {
			return kErrorNone;
		}
		err = kErrorDiskFull;
	} else {
		err = kErrorNone;
	}
'''


def info(message):
    print(f'[info] {message}', file=sys.stderr)


def rewrite_sources():
    # bsd/kern/makesyscalls.sh writes `\(sy_call_t *\)` inside an awk string. BSD awk and gawk drop the unknown
    # escape; Debian's default mawk keeps the backslash, and init_sysent.c does not compile. The escape is bogus
    # either way: remove it, and any generated file that carries it.
    script = XNU / 'bsd/kern/makesyscalls.sh'
    text = script.read_text()
    if '\\(sy_call_t *\\)' in text:
        script.write_text(text.replace('\\(sy_call_t *\\)', '(sy_call_t *)'))
        info('patched bsd/kern/makesyscalls.sh: dropped the \\( escapes mawk keeps')
    for generated in (ROOT / 'build/xnu').rglob('*.c') if (ROOT / 'build/xnu').is_dir() else ():
        if b'\\(sy_call_t' in generated.read_bytes():
            generated.unlink()
            info(f'removed mis-generated {generated}')

    # arm64_isa_compatibility.h gives Apple clang newer than 5010000 the standard `v0.4s` SHA/AES assembler forms and
    # every other compiler the pre-2014 `q0, q1` forms, which no current assembler accepts. Upstream clang has no
    # __apple_build_version__ and is newer than 5010000.
    isa = XNU / 'osfmk/arm64/corecrypto/arm64_isa_compatibility.h'
    text = isa.read_text()
    if ISA_APPLE in text:
        isa.write_text(text.replace(ISA_APPLE, ISA_ANY))
        info('patched corecrypto/arm64_isa_compatibility.h: standard vector syntax for non-Apple clang')

    # SETUP/kextsymboltool's seekFile() treats `offset != lseek()` as a full disk. check_all_exports runs it with
    # `-output /dev/null`, where Linux lseek() returns 0, so the export check failed after the kernel link. make
    # rebuilds the tool when its source changes.
    tool = XNU / 'SETUP/kextsymboltool/kextsymboltool.c'
    text = tool.read_text()
    if 'S_ISCHR' not in text:
        if text.count(SEEK_OLD) != 1:
            sys.exit(f'{tool}: seekFile() body not found')
        text = text.replace(SEEK_OLD, SEEK_NEW)
        if '#include <sys/stat.h>' not in text:
            text = text.replace('#include <sys/types.h>', '#include <sys/types.h>\n#include <sys/stat.h>', 1)
        tool.write_text(text)
        info('patched SETUP/kextsymboltool/kextsymboltool.c: lseek() on /dev/null is not a write error')

    (FAKEROOT / 'usr/include').mkdir(parents=True, exist_ok=True)
    shutil.copyfile(HERE / 'TargetConditionals.h', FAKEROOT / 'usr/include/TargetConditionals.h')


def fetch(directory, url, commit, *paths):
    """A sparse, blobless checkout of paths at commit."""
    head = subprocess.run(['git', '-C', directory, 'rev-parse', 'HEAD'], capture_output=True, text=True)
    if head.returncode == 0 and head.stdout.strip() == commit:
        return
    shutil.rmtree(directory, ignore_errors=True)
    for command in (['init', '-q', directory], ['-C', directory, 'sparse-checkout', 'set', '--no-cone', *paths],
                    ['-C', directory, 'fetch', '-q', '--depth', '1', '--filter=blob:none', url, commit],
                    ['-C', directory, 'checkout', '-q', 'FETCH_HEAD']):
        subprocess.run(['git', *map(str, command)], check=True)


def build_cc_kext():
    # The profile half of compiler-rt's cc_kext recipe (cmake/Modules/CompilerRTDarwinUtils.cmake: PROFILE_SOURCES,
    # -mkernel, KERNEL_USE). The builtins half is left out: the kernel link needs none of it. KERNEL_USE selects the
    # kernel headers (Kernel.framework); inttypes.h, stdio.h and gethostuuid.h come from Libc and Libsyscall, as they
    # do from an SDK's usr/include.
    out = FAKEROOT / 'usr/local/lib/kernel/libcc_kext.a'
    if out.is_file():
        return
    src = ROOT / '.cache/linux'
    fetch(src / 'llvm-project', 'https://github.com/llvm/llvm-project', LLVM_COMMIT,
          'compiler-rt/lib/profile', 'compiler-rt/include/profile')
    fetch(src / 'Libc', 'https://github.com/apple-oss-distributions/Libc.git', LIBC_COMMIT, 'include')
    rt = src / 'llvm-project/compiler-rt'
    objects = src / 'cc_kext'
    objects.mkdir(parents=True, exist_ok=True)
    out.parent.mkdir(parents=True, exist_ok=True)
    for name in PROFILE_SOURCES:
        subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-arch', 'arm64', '-isysroot', FAKEROOT,
                        '-mkernel', '-DKERNEL_USE', '-O3', '-fPIC', '-w',
                        '-I', rt / 'include', '-I', FAKEROOT / 'System/Library/Frameworks/Kernel.framework/Headers',
                        '-idirafter', src / 'Libc/include', '-idirafter', XNU / 'libsyscall/wrappers',
                        '-c', rt / f'lib/profile/{name}.c', '-o', objects / f'{name}.o'], check=True)
    subprocess.run(['xcrun', 'libtool', '-static', '-o', out, *(objects / f'{n}.o' for n in PROFILE_SOURCES)],
                   check=True)
    info(f'libcc_kext.a: {out}')


if __name__ == '__main__':
    mode = sys.argv[1] if len(sys.argv) > 1 else ''
    if mode == 'source':
        rewrite_sources()
    elif mode == 'libs':
        build_cc_kext()
    else:
        sys.exit('usage: prepare-xnu.py source|libs')
