// Montage — built into libvmx (fetched) with GCC, which has no __declspec(align(n)) (Clang takes it with -fdeclspec).
// MinGW's GCC defines __declspec as __attribute__ of its argument, which spells align "aligned": the declarations
// MinGW's own headers make are kept.
#pragma once
#if defined(__GNUC__) && !defined(__clang__)
#undef __declspec
#define __declspec(x) __montage_declspec_##x
#define __montage_declspec_align(n) __attribute__((aligned(n)))
#define __montage_declspec_dllimport __attribute__((__dllimport__))
#define __montage_declspec_dllexport __attribute__((__dllexport__))
#define __montage_declspec_noreturn __attribute__((__noreturn__))
#define __montage_declspec_nothrow __attribute__((__nothrow__))
#define __montage_declspec_selectany __attribute__((__selectany__))
#define __montage_declspec___dllimport__ __attribute__((__dllimport__))
#define __montage_declspec___dllexport__ __attribute__((__dllexport__))
#endif
