/* <TargetConditionals.h> for the UniXNU kernel step on Linux (scripts/linux/prepare-xnu.py installs it).
 *
 * On a Mac this header comes from the iPhoneOS SDK's usr/include. xnu itself skips it under
 * XNU_KERNEL_PRIVATE; what includes it is code built against Kernel.framework from outside xnu
 * (libfirehose_kernel, the kext-style archives). The TARGET_OS_* values are derived from the
 * compiler's target triple, as the SDK header does, so an arm64-apple-ios17.0 compile sees iOS.
 * Clang predefines most TARGET_OS_* for Darwin targets itself; each one here applies only when
 * the compiler did not.
 */
#ifndef __TARGETCONDITIONALS__
#define __TARGETCONDITIONALS__

#if !defined(__is_target_os)
#error "TargetConditionals.h stand-in needs clang's __is_target_os"
#endif

#ifndef TARGET_OS_MAC
#define TARGET_OS_MAC          1
#endif
#ifndef TARGET_OS_OSX
#define TARGET_OS_OSX          (__is_target_os(macos))
#endif
#ifndef TARGET_OS_IOS
#define TARGET_OS_IOS          (__is_target_os(ios) && !__is_target_environment(macabi))
#endif
#ifndef TARGET_OS_TV
#define TARGET_OS_TV           (__is_target_os(tvos))
#endif
#ifndef TARGET_OS_WATCH
#define TARGET_OS_WATCH        (__is_target_os(watchos))
#endif
#ifndef TARGET_OS_VISION
#define TARGET_OS_VISION       (__is_target_os(xros))
#endif
#ifndef TARGET_OS_BRIDGE
#define TARGET_OS_BRIDGE       0
#endif
#ifndef TARGET_OS_DRIVERKIT
#define TARGET_OS_DRIVERKIT    (__is_target_os(driverkit))
#endif
#ifndef TARGET_OS_MACCATALYST
#define TARGET_OS_MACCATALYST  (__is_target_environment(macabi))
#endif
#ifndef TARGET_OS_UIKITFORMAC
#define TARGET_OS_UIKITFORMAC  TARGET_OS_MACCATALYST
#endif
#ifndef TARGET_OS_IPHONE
#define TARGET_OS_IPHONE       (TARGET_OS_IOS || TARGET_OS_TV || TARGET_OS_WATCH || TARGET_OS_VISION || TARGET_OS_BRIDGE || TARGET_OS_MACCATALYST)
#endif
#ifndef TARGET_OS_SIMULATOR
#define TARGET_OS_SIMULATOR    (__is_target_environment(simulator))
#endif
#ifndef TARGET_OS_EMBEDDED
#define TARGET_OS_EMBEDDED     (TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR && !TARGET_OS_MACCATALYST)
#endif
#ifndef TARGET_IPHONE_SIMULATOR
#define TARGET_IPHONE_SIMULATOR TARGET_OS_SIMULATOR
#endif
#ifndef TARGET_OS_NANO
#define TARGET_OS_NANO         TARGET_OS_WATCH
#endif
#ifndef TARGET_OS_LINUX
#define TARGET_OS_LINUX        0
#endif
#ifndef TARGET_OS_WINDOWS
#define TARGET_OS_WINDOWS      0
#endif
#ifndef TARGET_OS_WIN32
#define TARGET_OS_WIN32        0
#endif
#ifndef TARGET_OS_UNIX
#define TARGET_OS_UNIX         0
#endif
#ifndef TARGET_OS_EXCLAVECORE
#define TARGET_OS_EXCLAVECORE  0
#endif
#ifndef TARGET_OS_EXCLAVEKIT
#define TARGET_OS_EXCLAVEKIT   0
#endif

#if defined(__arm64__) || defined(__aarch64__)
#ifndef TARGET_CPU_ARM64
#define TARGET_CPU_ARM64       1
#endif
#else
#ifndef TARGET_CPU_ARM64
#define TARGET_CPU_ARM64       0
#endif
#endif
#ifndef TARGET_CPU_ARM
#define TARGET_CPU_ARM         (defined(__arm__) && !TARGET_CPU_ARM64)
#endif
#ifndef TARGET_CPU_X86_64
#define TARGET_CPU_X86_64      (defined(__x86_64__))
#endif
#ifndef TARGET_CPU_X86
#define TARGET_CPU_X86         (defined(__i386__))
#endif
#ifndef TARGET_CPU_PPC
#define TARGET_CPU_PPC         0
#endif
#ifndef TARGET_CPU_PPC64
#define TARGET_CPU_PPC64       0
#endif
#ifndef TARGET_CPU_68K
#define TARGET_CPU_68K         0
#endif
#ifndef TARGET_CPU_ALPHA
#define TARGET_CPU_ALPHA       0
#endif
#ifndef TARGET_CPU_MIPS
#define TARGET_CPU_MIPS        0
#endif
#ifndef TARGET_CPU_SPARC
#define TARGET_CPU_SPARC       0
#endif
#ifndef TARGET_RT_LITTLE_ENDIAN
#define TARGET_RT_LITTLE_ENDIAN (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#endif
#ifndef TARGET_RT_BIG_ENDIAN
#define TARGET_RT_BIG_ENDIAN    (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#endif
#ifndef TARGET_RT_64_BIT
#define TARGET_RT_64_BIT        (__LP64__)
#endif
#ifndef TARGET_RT_MAC_CFM
#define TARGET_RT_MAC_CFM       0
#endif
#ifndef TARGET_RT_MAC_MACHO
#define TARGET_RT_MAC_MACHO     1
#endif
#ifndef TARGET_ABI_USES_IOS_VALUES
#define TARGET_ABI_USES_IOS_VALUES (!TARGET_CPU_X86_64 || (TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST))
#endif

#endif /* __TARGETCONDITIONALS__ */
