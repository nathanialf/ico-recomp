/* rhi/d3d12/rhi_d3d12_loader.h: d3d12.dll, dxgi.dll and dxcompiler.dll found
 * at run time.
 *
 * Ours (MIT).
 *
 * Nothing here links an import library. The reasons are the same three the
 * Vulkan backend gives for volk: the same executable has to start on a
 * machine with no usable D3D12 and say so instead of failing to load; the
 * mingw cross build has the headers but not necessarily the import libraries;
 * and dxcompiler.dll is a developer-only fallback that must never be a
 * startup dependency.
 *
 * No Agility SDK. D3D12Core.dll and D3D12SDKVersion would pin a redistributed
 * runtime into the package, and every entry point this backend uses is in the
 * system d3d12.dll on Windows 10 1809 and later.
 */
#ifndef ICORECOMP_RHI_D3D12_LOADER_H
#define ICORECOMP_RHI_D3D12_LOADER_H

#include <d3d12.h>
#include <dxgi1_6.h>

namespace rhi {

/* The entry points this backend resolves. A null member is a function the
 * system libraries did not export; every caller checks the one it needs and
 * names it in the failure. */
struct D3D12Entries {
    /* d3d12.dll */
    HRESULT (WINAPI* D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**) = nullptr;
    HRESULT (WINAPI* D3D12GetDebugInterface)(REFIID, void**) = nullptr;
    /* Windows 10 1607 and later. Absent on older systems, where the 1.0
     * serializer below is the fallback. */
    HRESULT (WINAPI* D3D12SerializeVersionedRootSignature)(
        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**) = nullptr;
    HRESULT (WINAPI* D3D12SerializeRootSignature)(
        const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
        ID3DBlob**, ID3DBlob**) = nullptr;
    /* dxgi.dll */
    HRESULT (WINAPI* CreateDXGIFactory2)(UINT, REFIID, void**) = nullptr;

    bool loaded = false;
};

/* Loads d3d12.dll and dxgi.dll once and resolves the table. Returns null when
 * either library is missing, with the reason in `why` (a static string). */
const D3D12Entries* d3d12_load_entries(const char** why);

/* dxcompiler.dll, the fallback that compiles the committed HLSL at run time
 * when no DXIL is compiled in. Returns null when the library could not be
 * loaded, with the reason in `why`: the path that was tried and the Win32
 * error, because "not found" and "found but a dependency is missing" are the
 * same GetLastError-less message otherwise and the second is the common case.
 * Both DLLs are built against the Visual C++ runtime (MSVCP140.dll,
 * VCRUNTIME140.dll, VCRUNTIME140_1.dll), so a machine with no Visual C++
 * redistributable fails the load with ERROR_MOD_NOT_FOUND (126) even with
 * the files sitting next to the executable.
 *
 * dxil.dll is loaded first, from the same directory, because it is the
 * validator that signs a compiled blob and drivers reject unsigned DXIL
 * outside developer mode. dxcompiler.dll looks it up by base name, so having
 * it already in the process under that name is what makes the signing
 * happen. */
typedef HRESULT (WINAPI* DxcCreateInstanceFn)(REFCLSID, REFIID, void**);
DxcCreateInstanceFn d3d12_load_dxcompiler(const char** why);

/* Where the last d3d12_load_dxcompiler call found its two libraries, as UTF-8
 * for the log, or null when that library was not loaded. Valid only after
 * d3d12_load_dxcompiler has run. */
const char* d3d12_dxcompiler_path();
const char* d3d12_dxil_path();

} // namespace rhi

#endif /* ICORECOMP_RHI_D3D12_LOADER_H */
