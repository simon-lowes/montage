// Montage — the config.h libvmaf's own (Meson) build would write, for Montage's CMake build of it (CMakeLists.txt):
// its x86-64 code (AVX2 and AVX-512, chosen at run time) or its AArch64 NEON code, as the compiler targets, the
// built-in VMAF models, and the integer features only (as its build's defaults). A universal macOS build defines
// MONTAGE_VMAF_NO_SIMD, as each half would need the other's files left out.
#pragma once

#if !defined(MONTAGE_VMAF_NO_SIMD) && (defined(__x86_64__) || defined(_M_X64))
#define HAVE_ASM 1
#define ARCH_X86 1
#define ARCH_X86_64 1
#define ARCH_X86_32 0
#define HAVE_AVX512 1
#elif !defined(MONTAGE_VMAF_NO_SIMD) && (defined(__aarch64__) || defined(_M_ARM64))
#define HAVE_ASM 1
#define ARCH_AARCH64 1
#define ARCH_ARM 0
#endif

#define VMAF_BUILT_IN_MODELS 1
#define VMAF_FLOAT_FEATURES 0
