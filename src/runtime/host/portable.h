/* host/portable.h: small OS portability helpers shared by the runtime .cpp
 * files. Everything here is header-only and behavior-identical across
 * Linux and Windows; keep per-OS ifdefs in this file so the callers stay
 * clean.
 */
#ifndef ICORECOMP_HOST_PORTABLE_H
#define ICORECOMP_HOST_PORTABLE_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>

/* Aligned, zero-initialized allocation for the long-lived guest memory
 * blocks (EE RAM, scratchpad, VU window). Uses C++17 aligned operator new
 * because std::aligned_alloc does not exist on MSVC. The runtime never
 * frees these; a caller that ever does must use
 * ::operator delete(p, std::align_val_t(align)). Returns nullptr on
 * failure so callers keep their own fatal paths. */
inline void* rt_aligned_zalloc(std::size_t align, std::size_t size) {
    void* p = ::operator new(size, std::align_val_t(align), std::nothrow);
    if (p) std::memset(p, 0, size);
    return p;
}

/* localtime into caller-provided storage (localtime_r is POSIX only). */
inline void rt_localtime(std::time_t t, std::tm* out) {
#ifdef _WIN32
    localtime_s(out, &t);
#else
    localtime_r(&t, out);
#endif
}

/* 64-bit stdio positioning: long is 32 bits on Windows, so plain
 * fseek/ftell cap at 2 GB there (the disc image and a long WAV capture
 * can exceed that). */
inline int rt_fseek64(std::FILE* f, int64_t off, int whence) {
#ifdef _WIN32
    return _fseeki64(f, off, whence);
#else
    return fseeko(f, off, whence);
#endif
}

inline int64_t rt_ftell64(std::FILE* f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return ftello(f);
#endif
}

#endif /* ICORECOMP_HOST_PORTABLE_H */
