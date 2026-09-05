/* rhi/d3d12/rhi_d3d12_loader.cpp: see rhi_d3d12_loader.h.
 *
 * Ours (MIT).
 */
#include "rhi_d3d12_loader.h"

#include "../../runtime.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace rhi {

namespace {

D3D12Entries g_entries;
bool g_tried = false;
const char* g_why = nullptr;

template <typename T>
void resolve(HMODULE lib, const char* name, T& out) {
    out = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(lib, name)));
}

} // namespace

const D3D12Entries* d3d12_load_entries(const char** why) {
    if (!g_tried) {
        g_tried = true;
        /* LOAD_LIBRARY_SEARCH_SYSTEM32 rather than a bare name: these are
         * system libraries and must not be picked up out of the package
         * directory or the working directory. */
        HMODULE d3d12 = LoadLibraryExA("d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const DWORD d3d12_err = d3d12 ? 0 : GetLastError();
        HMODULE dxgi = LoadLibraryExA("dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const DWORD dxgi_err = dxgi ? 0 : GetLastError();
        /* Logged here as well as returned in `why`: the caller turns this
         * into a fatal, but a caller that only probes for the backend would
         * otherwise leave a run with nothing in the log saying which of the
         * two libraries is missing. */
        if (!d3d12) {
            g_why = "d3d12.dll is not present in the system directory";
            rt_log_error("rhi", "LoadLibrary(d3d12.dll) from the system directory failed "
                                "with Win32 error %lu; this system has no Direct3D 12",
                         (unsigned long)d3d12_err);
        } else if (!dxgi) {
            g_why = "dxgi.dll is not present in the system directory";
            rt_log_error("rhi", "LoadLibrary(dxgi.dll) from the system directory failed "
                                "with Win32 error %lu; d3d12.dll loaded, so this is not a "
                                "system without Direct3D 12",
                         (unsigned long)dxgi_err);
        } else {
            resolve(d3d12, "D3D12CreateDevice", g_entries.D3D12CreateDevice);
            resolve(d3d12, "D3D12GetDebugInterface", g_entries.D3D12GetDebugInterface);
            resolve(d3d12, "D3D12SerializeVersionedRootSignature",
                    g_entries.D3D12SerializeVersionedRootSignature);
            resolve(d3d12, "D3D12SerializeRootSignature", g_entries.D3D12SerializeRootSignature);
            resolve(dxgi, "CreateDXGIFactory2", g_entries.CreateDXGIFactory2);
            if (!g_entries.D3D12CreateDevice) {
                g_why = "d3d12.dll exports no D3D12CreateDevice";
                rt_log_error("rhi", "d3d12.dll loaded from the system directory but "
                                    "exports no D3D12CreateDevice");
            } else if (!g_entries.CreateDXGIFactory2) {
                g_why = "dxgi.dll exports no CreateDXGIFactory2, so this is a Windows "
                        "version older than 8.1";
                rt_log_error("rhi", "dxgi.dll loaded but exports no CreateDXGIFactory2, so "
                                    "this is a Windows version older than 8.1");
            } else {
                g_entries.loaded = true;
                /* The optional serializer, said out loud: a run that took
                 * the 1.0 fallback and a run with neither entry point fail in
                 * different places, and only this function can tell them
                 * apart. D3D12GetDebugInterface is not reported here; the
                 * device constructor names it, and only when the debug layer
                 * was actually asked for. */
                if (!g_entries.D3D12SerializeVersionedRootSignature) {
                    if (g_entries.D3D12SerializeRootSignature) {
                        rt_log_warn("rhi", "d3d12.dll exports no "
                                           "D3D12SerializeVersionedRootSignature; falling "
                                           "back to the 1.0 serializer, which this "
                                           "backend's root signature is described for "
                                           "anyway");
                    } else {
                        rt_log_error("rhi", "d3d12.dll exports neither "
                                            "D3D12SerializeVersionedRootSignature nor "
                                            "D3D12SerializeRootSignature; the root "
                                            "signature cannot be built and device creation "
                                            "will fail on it");
                    }
                }
            }
        }
    }
    if (why) *why = g_why;
    return g_entries.loaded ? &g_entries : nullptr;
}

namespace {

/* The directory holding the running executable, as a wide string with a
 * trailing separator, or empty when it cannot be determined. Not
 * host/portable.h's rt_exe_dir: that one converts to UTF-8, and everything
 * here goes straight back into a wide Win32 call, so the round trip would
 * only be a way to lose a character. */
std::wstring exe_directory() {
    std::wstring path(4096, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
    if (n == 0 || n >= path.size()) return std::wstring();
    path.resize(n);
    const size_t cut = path.find_last_of(L"\\/");
    if (cut == std::wstring::npos) return std::wstring();
    return path.substr(0, cut + 1);
}

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

/* Loads one DLL by full path first and by bare name second.
 *
 * LOAD_WITH_ALTERED_SEARCH_PATH is what makes the full-path form worth using:
 * it puts the DLL's own directory at the front of the search order for the
 * libraries that DLL imports, so dxcompiler.dll's neighbours resolve from
 * beside the executable. The previous form passed a bare name with
 * LOAD_LIBRARY_SEARCH_APPLICATION_DIR, which finds the file but does not help
 * when the executable is on a mapped network drive; the user's first D3D12
 * run failed there.
 *
 * `tried` collects the paths and the Win32 errors so the caller's message can
 * say which one failed and why. */
HMODULE load_named(const wchar_t* name, const std::wstring& dir, std::string& tried,
                   std::string& loaded_path) {
    if (!dir.empty()) {
        const std::wstring full = dir + name;
        HMODULE lib = LoadLibraryExW(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (lib) {
            loaded_path = to_utf8(full);
            return lib;
        }
        const DWORD err = GetLastError();
        char note[512];
        std::snprintf(note, sizeof(note), "%s failed with Win32 error %lu%s",
                      to_utf8(full).c_str(), (unsigned long)err,
                      err == ERROR_MOD_NOT_FOUND
                          ? " (ERROR_MOD_NOT_FOUND: the file is there but one of the DLLs it "
                            "imports is not; these DLLs need the Visual C++ redistributable, "
                            "MSVCP140.dll and VCRUNTIME140.dll)"
                          : "");
        if (!tried.empty()) tried += "; ";
        tried += note;
    }
    HMODULE lib = LoadLibraryW(name);
    if (lib) {
        wchar_t got[4096];
        const DWORD n = GetModuleFileNameW(lib, got, (DWORD)(sizeof(got) / sizeof(got[0])));
        loaded_path = n ? to_utf8(std::wstring(got, n)) : "(on the search path)";
        return lib;
    }
    char note[256];
    std::snprintf(note, sizeof(note), "the search path failed with Win32 error %lu",
                  (unsigned long)GetLastError());
    if (!tried.empty()) tried += "; ";
    tried += note;
    return nullptr;
}

std::string g_dxc_reason;
std::string g_dxcompiler_path;
std::string g_dxil_path;

} // namespace

DxcCreateInstanceFn d3d12_load_dxcompiler(const char** why) {
    static bool tried = false;
    static DxcCreateInstanceFn fn = nullptr;
    if (!tried) {
        tried = true;
        const std::wstring dir = exe_directory();

        if (dir.empty()) {
            rt_log_warn("rhi", "the directory holding ico.exe could not be determined, so "
                               "dxcompiler.dll and dxil.dll are looked for on the search "
                               "path only and not beside the executable");
        }

        /* dxil.dll first. It is the validator that signs what dxcompiler.dll
         * produces, dxcompiler.dll looks it up by base name, and a module
         * already in the process under that name is the one it finds. An
         * absent dxil.dll is not fatal here: the compile still succeeds and
         * rhi_d3d12_shaders.cpp checks the container's signature and says so.
         * It is still the reason a shader comes out unsigned and the driver
         * then refuses it with a bare E_INVALIDARG, so the paths that were
         * tried are named here, where they are still known. */
        std::string dxil_tried;
        if (!load_named(L"dxil.dll", dir, dxil_tried, g_dxil_path)) {
            rt_log_warn("rhi", "dxil.dll could not be loaded (%s); anything dxcompiler.dll "
                               "compiles in this run will be unsigned, and a driver outside "
                               "developer mode rejects unsigned DXIL",
                        dxil_tried.c_str());
        }

        std::string dxc_tried;
        HMODULE lib = load_named(L"dxcompiler.dll", dir, dxc_tried, g_dxcompiler_path);
        if (!lib) {
            g_dxc_reason = "dxcompiler.dll could not be loaded (" + dxc_tried + ")";
            /* Only a warn: this library is the fallback path, and a package
             * built with the DXIL compiled in never asks for it. The caller
             * turns it into a fatal when it is the path that was needed. */
            rt_log_warn("rhi", "%s", g_dxc_reason.c_str());
        } else {
            resolve(lib, "DxcCreateInstance", fn);
            if (!fn) {
                g_dxc_reason = "dxcompiler.dll exports no DxcCreateInstance";
                rt_log_warn("rhi", "%s (loaded from %s)", g_dxc_reason.c_str(),
                            g_dxcompiler_path.empty() ? "(path not recorded)"
                                                      : g_dxcompiler_path.c_str());
            }
        }
    }
    if (why) *why = g_dxc_reason.empty() ? nullptr : g_dxc_reason.c_str();
    return fn;
}

const char* d3d12_dxcompiler_path() {
    return g_dxcompiler_path.empty() ? nullptr : g_dxcompiler_path.c_str();
}

const char* d3d12_dxil_path() {
    return g_dxil_path.empty() ? nullptr : g_dxil_path.c_str();
}

} // namespace rhi
