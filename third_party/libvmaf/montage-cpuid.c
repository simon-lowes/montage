// Montage — libvmaf's two processor queries (its x86/cpuid.asm, which needs NASM), in C for Montage's CMake build of
// it: CPUID for a leaf and subleaf, and XGETBV for an extended control register.
#include "config.h"

#if ARCH_X86
#include <stdint.h>

typedef struct {
    uint32_t eax, ebx, ecx, edx;
} CpuidRegisters;

#if defined(_MSC_VER) && !defined(__clang__)
#include <immintrin.h>
#include <intrin.h>

void vmaf_cpu_cpuid(CpuidRegisters* regs, unsigned leaf, unsigned subleaf) {
    int r[4];
    __cpuidex(r, (int)leaf, (int)subleaf);
    regs->eax = (uint32_t)r[0], regs->ebx = (uint32_t)r[1], regs->ecx = (uint32_t)r[2], regs->edx = (uint32_t)r[3];
}

uint64_t vmaf_cpu_xgetbv(unsigned xcr) { return (uint64_t)_xgetbv(xcr); }
#else
#include <cpuid.h>

void vmaf_cpu_cpuid(CpuidRegisters* regs, unsigned leaf, unsigned subleaf) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    __cpuid_count(leaf, subleaf, a, b, c, d);
    regs->eax = a, regs->ebx = b, regs->ecx = c, regs->edx = d;
}

uint64_t vmaf_cpu_xgetbv(unsigned xcr) {
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(xcr));
    return ((uint64_t)hi << 32) | lo;
}
#endif
#endif
