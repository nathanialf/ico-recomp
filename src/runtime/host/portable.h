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
#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <cwchar>
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
#include <timeapi.h>
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
/* fopen for a UTF-8 path. On Windows the narrow CRT entry points read a
 * path in the process code page, not UTF-8, so a path that came from
 * rt_exe_dir (which converts with CP_UTF8) cannot be handed to fopen
 * as-is. Converting back to UTF-16 and using _wfopen is the only form
 * that round-trips. std::filesystem already does this internally, which
 * is why directory creation can succeed on a path where fopen fails.
 * Everywhere else this is plain fopen. */
inline std::FILE* rt_fopen_utf8(const char* path, const char* mode) {
#ifdef _WIN32
    int wn = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    int wm = MultiByteToWideChar(CP_UTF8, 0, mode, -1, nullptr, 0);
    if (wn > 0 && wm > 0) {
        std::wstring wpath(size_t(wn), L'\0');
        std::wstring wmode(size_t(wm), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath.data(), wn);
        MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode.data(), wm);
        if (std::FILE* f = _wfopen(wpath.c_str(), wmode.c_str())) return f;
    }
    /* Fall back to the narrow form: a pure-ASCII path works either way,
     * and this keeps behavior unchanged when the wide open fails for a
     * reason that has nothing to do with encoding. */
    return std::fopen(path, mode);
#else
    return std::fopen(path, mode);
#endif
}

/* Opens the log for writing, truncating.
 *
 * On Windows this goes through CreateFileW rather than the CRT, for two
 * reasons that both matter when the folder is a network share:
 *
 *   FILE_FLAG_WRITE_THROUGH sends each write to the server instead of
 *   letting it sit in the client's cache. A log whose point is to survive
 *   the process dying is worth little if the last seconds of it are still
 *   on the client when the window closes, and a log on a share is being
 *   read from the server side, so cached writes are invisible there.
 *
 *   FILE_SHARE_READ lets the file be read while the run still holds it
 *   open. Without it the log can only be examined after the process
 *   exits, which is the wrong time when the run is hanging.
 *
 * Falls back to the CRT open if any of that fails, so the behavior is
 * never worse than it was. `err` is filled with the Win32 error when the
 * native path is what failed. */
inline std::FILE* rt_fopen_log(const char* path, unsigned long* err) {
    if (err) *err = 0;
#ifdef _WIN32
    int wn = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (wn > 0) {
        std::wstring wpath(size_t(wn), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath.data(), wn);
        /* The runtime joins paths with '/', which is fine for a drive
         * letter but leaves a UNC path mixing both separators. Normalize
         * rather than rely on every layer accepting that. */
        for (wchar_t& c : wpath) {
            if (c == L'/') c = L'\\';
        }
        HANDLE h = CreateFileW(wpath.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_WRONLY);
            if (fd >= 0) {
                if (std::FILE* f = _fdopen(fd, "w")) return f;
                _close(fd); /* closes the handle too */
            } else {
                CloseHandle(h);
            }
        } else if (err) {
            *err = GetLastError();
        }
    }
#endif
    return rt_fopen_utf8(path, "w");
}

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

/* Raises the OS timer resolution so a sleep can land near a millisecond.
 * Windows defaults to a ~15.6 ms scheduler tick, which is longer than a
 * 16.68 ms field, so a frame limiter without this overshoots every time.
 * Idempotent; the matching end call is left to process exit. */
inline void rt_time_begin_period() {
#ifdef _WIN32
    static bool done = false;
    if (!done) {
        timeBeginPeriod(1);
        done = true;
    }
#endif
}

/* Size and modification time of the running executable, as
 * "<bytes> <YYYY-MM-DD HH:MM:SS>". __DATE__/__TIME__ only records when the
 * translation unit holding it was last compiled, so on an incremental build
 * it names the wrong binary. This reads the file that is actually running. */
inline std::string rt_exe_identity() {
    char out[128];
#ifdef _WIN32
    constexpr DWORD kMax = 4096;
    wchar_t wpath[kMax];
    DWORD n = GetModuleFileNameW(nullptr, wpath, kMax);
    if (n == 0 || n >= kMax) return "unknown";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(wpath, GetFileExInfoStandard, &fa)) return "unknown";
    unsigned long long sz =
        ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    SYSTEMTIME st;
    FILETIME lt;
    if (!FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lt) ||
        !FileTimeToSystemTime(&lt, &st)) {
        std::snprintf(out, sizeof out, "%llu bytes", sz);
        return out;
    }
    std::snprintf(out, sizeof out, "%llu bytes, %04u-%02u-%02u %02u:%02u:%02u",
        sz, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return out;
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "unknown";
    buf[n] = 0;
    struct stat sb;
    if (stat(buf, &sb) != 0) return "unknown";
    struct tm tmv;
    localtime_r(&sb.st_mtime, &tmv);
    std::snprintf(out, sizeof out, "%llu bytes, %04d-%02d-%02d %02d:%02d:%02d",
        (unsigned long long)sb.st_size, tmv.tm_year + 1900, tmv.tm_mon + 1,
        tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return out;
#endif
}

#endif /* ICORECOMP_HOST_PORTABLE_H */
