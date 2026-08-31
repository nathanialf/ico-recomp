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
#include <string>

#ifdef _WIN32
#include <io.h>
/* Lean include: this header is pulled into most runtime translation units,
 * and the full windows.h drags in macros (min/max especially) that collide
 * with the C++ standard library. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

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

/* stdio file-descriptor plumbing, used by the log sink to point fd 2 at a
 * file so every module in the process (the runtime, the GS shared library,
 * SDL and the Vulkan loader, each of which may carry its own CRT copy)
 * lands in the same log. */
inline int rt_fileno(std::FILE* f) {
#ifdef _WIN32
    return _fileno(f);
#else
    return fileno(f);
#endif
}

inline int rt_dup(int fd) {
#ifdef _WIN32
    return _dup(fd);
#else
    return dup(fd);
#endif
}

inline int rt_dup2(int src, int dst) {
#ifdef _WIN32
    return _dup2(src, dst);
#else
    return dup2(src, dst);
#endif
}

inline std::FILE* rt_fdopen(int fd, const char* mode) {
#ifdef _WIN32
    return _fdopen(fd, mode);
#else
    return fdopen(fd, mode);
#endif
}

/* Directory holding the running executable, with no trailing separator.
 * A packaged run resolves its config, saves, log and disc probe against
 * this rather than the working directory, so launching from a shortcut or
 * another folder still finds the files shipped next to the exe. Returns
 * "." when the path cannot be determined, which restores the old
 * working-directory behavior. */
inline std::string rt_exe_dir() {
#ifdef _WIN32
    constexpr DWORD kMax = 4096;
    wchar_t wpath[kMax];
    DWORD n = GetModuleFileNameW(nullptr, wpath, kMax);
    /* n == kMax means truncation on every Windows version we target. */
    if (n == 0 || n >= kMax) return ".";
    /* UTF-8 so the rest of the runtime keeps using narrow stdio paths. */
    int len = WideCharToMultiByte(CP_UTF8, 0, wpath, int(n), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return ".";
    std::string path(size_t(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wpath, int(n), path.data(), len, nullptr, nullptr);
    size_t cut = path.find_last_of("\\/");
    return cut == std::string::npos ? std::string(".") : path.substr(0, cut);
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    std::string path(buf);
    size_t cut = path.find_last_of('/');
    return cut == std::string::npos ? std::string(".") : path.substr(0, cut);
#endif
}

#endif /* ICORECOMP_HOST_PORTABLE_H */
