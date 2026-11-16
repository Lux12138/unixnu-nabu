#!/usr/bin/env python3
"""Install the toolchain the kernel step uses on a Linux host. Run once, as root:

    sudo scripts/linux/setup-toolchain.py [PREFIX]      (default /opt/unixnu-linux, or $UNIXNU_TOOLCHAIN)

It builds clang with __builtin_xnu_type_* (llvm-xnu-builtin-type.patch; xnu's kalloc_type signatures have no fallback
without them) into PREFIX/llvm, cctools-port 1030.6.3-ld64-956.6 (ld64, libtool, nm, lipo; its master, ld64-609,
corrupts the kernel link), mig from bootstrap_cmds, iig-port as iig, the host compiler wrapper for xnu's SETUP tools,
and lzfse for the kernelcache, and installs the shims of this directory. The Command Line Tools and Xcode toolchain
layouts that xnu's makefiles expect are under PREFIX.

Host dependencies come from apt, dnf, pacman, zypper, or a preconfigured environment.
Clang and libclang are selected through llvm-config. Libraries and build products live under PREFIX.
The xcrun, plutil and codesign shims are installed in /usr/bin for xnu's makefiles.
"""
import argparse
import os
import shlex
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
LLVM_COMMIT = '58c46bae21181fb4240cf964461bea738d96d50e'
CCTOOLS_PORT_REF = '1030.6.3-ld64-956.6'
LIBDISPATCH_REF = 'swift-6.4.0-RELEASE'
BOOTSTRAP_CMDS_TAG = 'bootstrap_cmds-138'
IIG_PORT_COMMIT = 'ea6d147d668ca15d490da638012243fcd705f30d'
LZFSE_COMMIT = 'e634ca58b4821d9f3d560cdc6df5dec02ffc93fd'
CLT_SHIMS = ('clang', 'clang++', 'cc', 'install', 'gm4')
USR_BIN_SHIMS = ('xcrun', 'plutil', 'codesign')
COMMON_PACKAGES = ['git', 'patch', 'flex', 'bison', 'bzip2', 'cpio', 'rsync', 'curl',
                   'wget', 'ca-certificates', 'tcsh', 'unifdef', 'cmake', 'clang', 'lld', 'mtools']
PACKAGES = {
    'apt': ['build-essential', 'pkg-config', 'ninja-build', 'python3', 'python3-dev',
            'xz-utils', 'libssl-dev', 'zlib1g-dev', 'libbsd-dev', 'uuid-dev', 'llvm-dev', 'libclang-dev', 'device-tree-compiler'],
    'dnf': ['gcc', 'gcc-c++', 'make', 'binutils', 'pkgconf-pkg-config', 'ninja-build', 'python3',
            'python3-devel', 'xz', 'openssl-devel', 'zlib-devel', 'libbsd-devel', 'libuuid-devel',
            'llvm-devel', 'clang-devel', 'dtc'],
    'pacman': ['base-devel', 'pkgconf', 'ninja', 'python', 'xz', 'openssl', 'zlib', 'libbsd',
               'util-linux', 'llvm', 'dtc'],
    'zypper': ['gcc', 'gcc-c++', 'make', 'binutils', 'pkg-config', 'ninja', 'python3', 'python3-devel',
               'xz', 'libopenssl-devel', 'zlib-devel', 'libbsd-devel', 'libuuid-devel',
               'llvm-devel', 'clang-devel', 'dtc'],
}
XNU_BUILTIN_CHECK = '#if __has_builtin(__builtin_xnu_type_summary)\nok\n#endif\n'

# Darwin names glibc lacks, for xnu's SETUP host tools (installfile uses setmode/getmode, getprogname).
HOST_PREFIX_H = '''/* Darwin-isms xnu's SETUP host tools use that glibc does not declare. */
#ifndef _UNIXNU_HOST_PREFIX_H_
#define _UNIXNU_HOST_PREFIX_H_
#include <bsd/unistd.h>   /* setmode(3)/getmode(3), used by SETUP/installfile */
static inline const char *getprogname(void)
{
	extern char *program_invocation_short_name;
	return program_invocation_short_name;
}
#endif
'''

# SETUP/installfile makes exactly one call into <copyfile.h>: fcopyfile(src, dst, NULL, COPYFILE_DATA).
COPYFILE_H = '''/* <copyfile.h> for xnu's SETUP/installfile.c, which uses exactly one call,
 * fcopyfile(src, dst, NULL, COPYFILE_DATA). */
#ifndef _UNIXNU_COPYFILE_H_
#define _UNIXNU_COPYFILE_H_
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>

typedef void *copyfile_state_t;
#define COPYFILE_DATA  (1 << 3)
#define COPYFILE_ALL   COPYFILE_DATA

static inline int
fcopyfile(int from_fd, int to_fd, copyfile_state_t state, unsigned int flags)
{
	char buf[65536];
	ssize_t n;
	(void)state; (void)flags;
	if (lseek(from_fd, 0, SEEK_SET) == (off_t)-1 && errno != ESPIPE) {
		return -1;
	}
	while ((n = read(from_fd, buf, sizeof(buf))) > 0) {
		ssize_t off = 0;
		while (off < n) {
			ssize_t w = write(to_fd, buf + off, (size_t)(n - off));
			if (w < 0) {
				if (errno == EINTR) {
					continue;
				}
				return -1;
			}
			off += w;
		}
	}
	return n < 0 ? -1 : 0;
}
#endif
'''

# The cross clang is built for AArch64 only, so xnu's SETUP tools (built for this machine) go through the
# distribution's clang. --ld-path: clang looks for "ld" next to itself first, and the Command Line Tools bin holds
# cctools' ld64; host tools link with the system ELF ld. A link (no -c) also gets cctools' libmacho and libstuff
# (the Mach-O arch table and byte swappers kextsymboltool and setsegname call; libmacho first, it calls into
# libstuff), libuuid and libbsd; xnu's SETUP makefiles set LDFLAGS literally.
HOST_CC = '''#!/bin/sh
# xnu's HOST_CC on Linux: the distribution's clang with the headers and libraries xnu's SETUP tools need.
compile_only=0
for a in "$@"; do [ "$a" = "-c" ] && compile_only=1; done
if [ "$compile_only" = 0 ]; then
    set -- "$@" "-L{lib}" -lmacho -lstuff -luuid -lbsd
fi
exec "{clang}" \\
    --ld-path=/usr/bin/ld {arch_defs} \\
    -include "{include}/unixnu-host-prefix.h" \\
    -idirafter "{include}" \\
    -idirafter "{include}/foreign" "$@"
'''


def run(command, cwd=None, env=None, input=None):
    command = [str(part) for part in command]
    print('+ ' + ' '.join(command), flush=True)
    return subprocess.run(command, cwd=cwd, env=env, input=input, check=True, text=True,
                          stdout=subprocess.PIPE if input is not None else None).stdout


def git_checkout(directory, url, ref):
    """One commit (or tag) of a repository, without its history."""
    directory.mkdir(parents=True, exist_ok=True)
    run(['git', 'init', '-q', directory])
    run(['git', '-C', directory, 'fetch', '-q', '--depth', '1', url, ref])
    run(['git', '-C', directory, 'checkout', '-q', '--force', 'FETCH_HEAD'])


def has_xnu_builtins(command, env=None):
    out = run([*command, '-E', '-'], env=env, input=XNU_BUILTIN_CHECK)
    return any(line.strip() == 'ok' for line in out.splitlines())


def package_manager(selected='auto'):
    if selected != 'auto':
        return selected
    try:
        release = platform.freedesktop_os_release()
    except OSError:
        release = {}
    families = {'debian': 'apt', 'ubuntu': 'apt', 'fedora': 'dnf', 'rhel': 'dnf',
                'centos': 'dnf', 'rocky': 'dnf', 'almalinux': 'dnf', 'arch': 'pacman',
                'manjaro': 'pacman', 'opensuse': 'zypper', 'opensuse-tumbleweed': 'zypper',
                'opensuse-leap': 'zypper', 'suse': 'zypper', 'sles': 'zypper'}
    for name in [release.get('ID', ''), *release.get('ID_LIKE', '').split()]:
        if name in families:
            return families[name]
    available = [name for name in PACKAGES if shutil.which('apt-get' if name == 'apt' else name)]
    if len(available) == 1:
        return available[0]
    sys.exit('select --package-manager apt, dnf, pacman, zypper, or none for preinstalled dependencies')


def package_commands(manager):
    if manager == 'none':
        return []
    packages = COMMON_PACKAGES + PACKAGES[manager]
    return {
        'apt': [['apt-get', 'update'], ['apt-get', 'install', '-y', '--no-install-recommends', *packages]],
        'dnf': [['dnf', 'install', '-y', *packages]],
        'pacman': [['pacman', '-S', '--needed', '--noconfirm', *packages]],
        'zypper': [['zypper', '--non-interactive', 'install', '--no-recommends', *packages]],
    }[manager]


def host_tools(llvm_config=None):
    """Resolve the host compiler and libclang from one LLVM installation."""
    config = llvm_config or os.environ.get('LLVM_CONFIG') or shutil.which('llvm-config')
    if not config or not shutil.which(str(config)):
        sys.exit('llvm-config missing: install LLVM development files or pass --llvm-config FILE')
    bindir = Path(run([config, '--bindir'], input='').strip())
    libdir = Path(run([config, '--libdir'], input='').strip())
    includedir = Path(run([config, '--includedir'], input='').strip())
    cc, cxx = bindir / 'clang', bindir / 'clang++'
    required = ['git', 'patch', 'pkg-config', 'ninja', 'flex', 'bison', 'python3', 'xz', 'bzip2',
                'cpio', 'rsync', 'curl', 'wget', 'tcsh', 'unifdef', 'cmake', 'make', 'ld', 'ar',
                'mformat', 'mcopy', 'dtc', 'fdtget', 'fdtput']
    missing = [name for name in [str(cc), str(cxx), *required] if not shutil.which(name)]
    if missing:
        sys.exit('missing host tools: ' + ', '.join(missing))
    resource = run([cc, '-print-resource-dir'], input='').strip()
    if not (Path(resource) / 'include/stdint.h').is_file():
        sys.exit(f'clang resource headers missing: {resource}')
    # Compile and run against the selected libclang before the LLVM source build.
    with tempfile.TemporaryDirectory(prefix='unixnu-host-check-') as tmp:
        probe = Path(tmp) / 'probe'
        run([cxx, '-x', 'c++', '-', '-o', probe, f'-I{includedir}', f'-L{libdir}',
             f'-Wl,-rpath,{libdir}', '-lclang', '-lbsd', '-luuid', '-lssl', '-lz'],
            input='#include <clang-c/Index.h>\n#include <bsd/unistd.h>\n#include <uuid/uuid.h>\n'
                  '#include <openssl/ssl.h>\n#include <zlib.h>\n'
                  'int main() { CXIndex i = clang_createIndex(0, 0); clang_disposeIndex(i); }\n')
        run([probe])
    warning = '-Wno-error=unused-but-set-global'
    supports_warning = subprocess.run([cc, '-Werror=unknown-warning-option', warning, '-x', 'c',
                                       '-fsyntax-only', '-'], input='', text=True, capture_output=True).returncode == 0
    return argparse.Namespace(config=str(config), cc=cc, cxx=cxx, libdir=libdir,
                              includedir=includedir, resource=resource,
                              dispatch_cflags=warning if supports_warning else '')


def llvm(prefix, work, jobs, host):
    """clang with llvm-xnu-builtin-type.patch, for AArch64 only."""
    clang = prefix / 'llvm/bin/clang'
    if not clang.exists():
        src = work / 'llvm-project'
        git_checkout(src, 'https://github.com/llvm/llvm-project', LLVM_COMMIT)
        run(['git', '-C', src, 'apply', HERE / 'llvm-xnu-builtin-type.patch'])
        run(['cmake', '-G', 'Ninja', '-S', src / 'llvm', '-B', src / 'build',
             '-DLLVM_ENABLE_PROJECTS=clang', '-DLLVM_TARGETS_TO_BUILD=AArch64',
             '-DCMAKE_BUILD_TYPE=Release', '-DLLVM_ENABLE_ASSERTIONS=ON',
             '-DLLVM_INCLUDE_TESTS=OFF', '-DLLVM_INCLUDE_EXAMPLES=OFF', '-DLLVM_INCLUDE_BENCHMARKS=OFF',
             '-DCLANG_ENABLE_STATIC_ANALYZER=OFF', '-DCLANG_ENABLE_ARCMT=OFF', '-DLLVM_PARALLEL_LINK_JOBS=1',
             f'-DCMAKE_C_COMPILER={host.cc}', f'-DCMAKE_CXX_COMPILER={host.cxx}',
             f'-DCMAKE_INSTALL_PREFIX={prefix / "llvm"}'])
        run(['ninja', '-C', src / 'build', f'-j{jobs}', 'install-clang', 'install-clang-resource-headers'])
        shutil.rmtree(src)
    if not has_xnu_builtins([clang, '-target', 'arm64e-apple-macos26.0']):
        sys.exit(f'{clang} has no __builtin_xnu_type_summary')
    return clang


def shims(clt, xctoolchain, clang):
    (clt / 'bin').mkdir(parents=True, exist_ok=True)
    for name, directory in [(n, clt / 'bin') for n in CLT_SHIMS] + [(n, Path('/usr/bin')) for n in USR_BIN_SHIMS]:
        shutil.copyfile(HERE / name, directory / name)
        (directory / name).chmod(0o755)
    env = dict(os.environ, CROSS_CLANG=str(clang), XNU_TOOLCHAIN_BIN=f'{clt / "bin"} {xctoolchain / "bin"}')
    found = run(['xcrun', '-find', 'clang'], env=env, input='').strip()
    if found != str(clt / 'bin/clang'):
        sys.exit(f'xcrun -find clang gives {found}, not {clt / "bin/clang"}')
    if not has_xnu_builtins(['xcrun', 'clang'], env):
        sys.exit('xcrun clang has no __builtin_xnu_type_summary')


def cctools(clt, include, work, jobs, host):
    """ld64 that links a kernel Mach-O (ld64.lld rejects -kernel), the Mach-O tools, and the Darwin <mach-o/*.h>,
    <mach/*.h> and libmacho that xnu's SETUP tools use. libtapi is not built: a kernel links from objects and
    archives only."""
    if all((clt / path).exists() for path in ('bin/ld', 'lib/libmacho.a', 'lib/libstuff.a')):
        return
    env = dict(os.environ, CC=str(host.cc), CXX=str(host.cxx))
    src = work / 'cctools-port'
    git_checkout(src, 'https://github.com/tpoechtrager/cctools-port.git', CCTOOLS_PORT_REF)
    # Keep libdispatch and BlocksRuntime under the toolchain prefix.
    dispatch_prefix = clt / 'libdispatch'
    if not (dispatch_prefix / 'lib/libdispatch.so').exists():
        dispatch = work / 'swift-corelibs-libdispatch'
        git_checkout(dispatch, 'https://github.com/swiftlang/swift-corelibs-libdispatch.git', LIBDISPATCH_REF)
        run(['cmake', '-G', 'Ninja', '-S', dispatch, '-B', dispatch / 'build', '-DCMAKE_BUILD_TYPE=Release',
             f'-DCMAKE_C_FLAGS={host.dispatch_cflags}',
             f'-DCMAKE_INSTALL_PREFIX={dispatch_prefix}', '-DCMAKE_INSTALL_LIBDIR=lib'], env=env)
        run(['ninja', '-C', dispatch / 'build', f'-j{jobs}'], env=env)
        run(['ninja', '-C', dispatch / 'build', 'install'], env=env)
    tree = src / 'cctools'
    # --disable-lto-support: the kernel link uses no LTO, and libstuff/lto.c needs LLVM headers cctools-port does not
    # ship. --target sets the default -arch; --program-prefix= and PROGRAM_PREFIX= keep the plain names (ld, lipo,
    # otool) that xcrun is asked for and that the tools use to run each other.
    run(['./configure', f'--prefix={clt}', '--target=arm64-apple-darwin', '--program-prefix=',
         '--disable-lto-support', f'--with-libdispatch={dispatch_prefix}',
         f'--with-libblocksruntime={dispatch_prefix}'], cwd=tree, env=env)
    run(['make', f'-j{jobs}', 'PROGRAM_PREFIX='], cwd=tree, env=env)
    run(['make', 'install', 'PROGRAM_PREFIX='], cwd=tree, env=env)
    # The headers xnu's SETUP tools include (<libc.h>, <mach-o/arch.h>, <mach-o/swap.h>, <mach/mach_init.h>); the
    # host compiler wrapper adds them with -idirafter, after glibc's.
    include.mkdir(parents=True, exist_ok=True)
    shutil.copytree(tree / 'include', include, dirs_exist_ok=True)
    # The host compiler wrapper links these archives from the toolchain's lib directory.
    (clt / 'lib').mkdir(exist_ok=True)
    for name in ('libmacho/.libs/libmacho.a', 'libstuff/.libs/libstuff.a'):
        shutil.copy2(tree / name, clt / 'lib')


def mig(clt, include, work, host):
    """mig and migcom from bootstrap_cmds, built with make-free compiler calls: the Xcode project's only work is the
    yacc/lex C program migcom.tproj and the mig.sh driver, which finds migcom as ../libexec/migcom."""
    if (clt / 'libexec/migcom').exists():
        return
    if not (include / 'foreign/mach/message.h').exists():
        sys.exit(f'no {include}/foreign/mach/message.h: cctools first')
    src = work / 'bootstrap_cmds'
    git_checkout(src, 'https://github.com/apple-oss-distributions/bootstrap_cmds.git', BOOTSTRAP_CMDS_TAG)
    tproj = src / 'migcom.tproj'
    run(['bison', '-y', '-d', 'parser.y'], cwd=tproj)      # y.tab.h is the name lexxer.l includes
    run(['flex', '-o', 'lexxer.c', 'lexxer.l'], cwd=tproj)
    # handler.c is dead code the Xcode target does not compile. The <mach/*.h> headers come from cctools-port's
    # include/foreign after glibc (-idirafter), so glibc keeps every name it has. -Ulinux takes type.h's macOS branch
    # (<sys/types.h>, for u_int); foreign/mach/machine/*.h select on __arm64__, which arm64 Linux clang does not define.
    sources = ['error.c', 'global.c', 'header.c', 'lexxer.c', 'y.tab.c', 'routine.c', 'server.c', 'statement.c',
               'string.c', 'type.c', 'user.c', 'utils.c', 'mig.c']
    arch = ['-D__arm64__=1'] if platform.machine() == 'aarch64' else []
    (clt / 'libexec').mkdir(parents=True, exist_ok=True)
    run([host.cc, *arch, '-Ulinux', f'-DMIG_VERSION="{BOOTSTRAP_CMDS_TAG}"',
         '-idirafter', include / 'foreign', '-o', clt / 'libexec/migcom', *sources], cwd=tproj)
    shutil.copyfile(tproj / 'mig.sh', clt / 'bin/mig')
    (clt / 'bin/mig').chmod(0o755)


def iig(xctoolchain, work, host):
    """Build iig with the selected libclang and its resource headers."""
    src = work / 'iig-port'
    git_checkout(src, 'https://github.com/Nyktis/iig-port', IIG_PORT_COMMIT)
    run(['patch', '-d', src, '-p1', '--batch', '--forward', '-i', HERE / 'iig-port-darwin-target.patch'])
    flags = shlex.split(run([host.config, '--cxxflags', '--ldflags'], input=''))
    (xctoolchain / 'bin').mkdir(parents=True, exist_ok=True)
    run([host.cxx, *flags, '-std=c++17',
         f'-DUNIXNU_CLANG_RESOURCE_DIR="{host.resource}"', src / 'iig.cpp',
         f'-Wl,-rpath,{host.libdir}', '-lclang', '-ldl', '-o', xctoolchain / 'bin/iig'])


def host_cc(clt, include, host):
    """xnu-host-cc, xnu's HOST_CC: SETUP tools built for this machine with the distribution's clang."""
    include.mkdir(parents=True, exist_ok=True)
    (include / 'unixnu-host-prefix.h').write_text(HOST_PREFIX_H)
    (include / 'copyfile.h').write_text(COPYFILE_H)
    # cctools' foreign/machine/*.h select on __arm64__; arm64 Linux also makes plain char unsigned, and
    # SETUP/decomment compares a char from getc() to EOF.
    arch = '-D__arm64__ -fsigned-char' if platform.machine() == 'aarch64' else ''
    wrapper = clt / 'bin/xnu-host-cc'
    wrapper.write_text(HOST_CC.format(clang=host.cc, arch_defs=arch, include=include, lib=clt / 'lib'))
    wrapper.chmod(0o755)


def lzfse(prefix, work):
    """Apple's reference LZFSE tool, which decodes the IPSW kernelcache (compression_tool on macOS)."""
    if (prefix / 'bin/lzfse').exists():
        return
    src = work / 'lzfse'
    git_checkout(src, 'https://github.com/lzfse/lzfse', LZFSE_COMMIT)
    run(['make', '-C', src, 'install', f'INSTALL_PREFIX={prefix}', 'CC=cc'])


def main(argv=None):
    parser = argparse.ArgumentParser(description='Install the UniXNU toolchain on a glibc Linux host.')
    parser.add_argument('prefix', nargs='?', default=os.environ.get('UNIXNU_TOOLCHAIN', '/opt/unixnu-linux'))
    parser.add_argument('--package-manager', choices=('auto', *PACKAGES, 'none'), default='auto')
    parser.add_argument('--print-packages', action='store_true', help='print package commands and exit')
    parser.add_argument('--check', action='store_true', help='check installed host tools and libraries without installing')
    parser.add_argument('--llvm-config', help='llvm-config for the host Clang/libclang installation; or LLVM_CONFIG')
    parser.add_argument('-j', '--jobs', type=int, default=os.cpu_count() or 4)
    args = parser.parse_args(argv)
    if args.print_packages:
        for command in package_commands(package_manager(args.package_manager)):
            print(shlex.join(command))
        return
    if platform.system() != 'Linux':
        parser.error('setup requires a Linux host')
    if platform.libc_ver()[0] != 'glibc':
        parser.error('the XNU host tools require glibc')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if not args.check:
        if os.geteuid():
            parser.error('installation requires root; use --check to inspect host dependencies')
        for command in package_commands(package_manager(args.package_manager)):
            run(command, env=dict(os.environ, DEBIAN_FRONTEND='noninteractive'))
    host = host_tools(args.llvm_config)
    if args.check:
        print(f'host dependencies ready: {host.cc}, {host.config}')
        return
    prefix = Path(args.prefix).expanduser().resolve()
    clt, xctoolchain = prefix / 'CommandLineTools/usr', prefix / 'XcodeDefault.xctoolchain/usr'
    include = clt.parent / 'SDKs/MacOSX.sdk/usr/include'
    jobs = args.jobs
    prefix.mkdir(parents=True, exist_ok=True)
    # The llvm-project build tree takes several GB: build on disk ($TMPDIR, else /var/tmp), not in a tmpfs /tmp.
    with tempfile.TemporaryDirectory(prefix='unixnu-toolchain-', dir=os.environ.get('TMPDIR', '/var/tmp')) as tmp:
        work = Path(tmp)
        clang = llvm(prefix, work, jobs, host)
        shims(clt, xctoolchain, clang)
        cctools(clt, include, work, jobs, host)
        mig(clt, include, work, host)
        iig(xctoolchain, work, host)
        host_cc(clt, include, host)
        lzfse(prefix, work)
    for tool in (clt / 'bin/ld', clt / 'bin/libtool', clt / 'libexec/migcom', xctoolchain / 'bin/iig',
                 clt / 'bin/xnu-host-cc', prefix / 'bin/lzfse'):
        if not tool.exists():
            sys.exit(f'{tool} missing')
    print(f'toolchain ready in {prefix}')


if __name__ == '__main__':
    main()
