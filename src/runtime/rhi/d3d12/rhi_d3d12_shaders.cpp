/* rhi/d3d12/rhi_d3d12_shaders.cpp: see rhi_d3d12_shaders.h for the two paths
 * and why the ahead-of-time one is the shipping path.
 *
 * Ours (MIT).
 */
#include "rhi_d3d12_shaders.h"

#include "rhi_d3d12.h"
#include "rhi_d3d12_loader.h"
#include "../rhi_shaders.h"
#include "../../runtime.h"

/* Both generated indexes are optional. Neither exists in a tree that has not
 * run its generator, and the renderer still builds and still says exactly
 * which command to run. */
#if defined(__has_include)
#  if __has_include("../rhi_shaders_dxil.h")
#    include "../rhi_shaders_dxil.h"
#    define ICORECOMP_HAVE_SHADER_DXIL 1
#  endif
#  if __has_include("../rhi_shaders_hlsl.h")
#    include "../rhi_shaders_hlsl.h"
#    define ICORECOMP_HAVE_SHADER_HLSL 1
#  endif
#endif

#include <dxcapi.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace rhi {

namespace {

/* The name of each SPIR-V array is rhi_shaders.h's shader_name_table(), which
 * the generator writes from the same list the arrays themselves come from.
 * The array address is the identity; rhi_d3d12_shaders.h says why. This file
 * used to keep a second copy of that table by hand, the copy was missing
 * shadow.comp, and the renderer fatalled at construction on this backend as a
 * result. Nothing is listed here now. */

/* The backend's own present blit. It has no GLSL counterpart because the
 * Vulkan backend blits with vkCmdBlitImage, which D3D12 has no equivalent of:
 * CopyTextureRegion cannot scale and cannot filter, so a scaled present has
 * to be a draw. One full-screen triangle from SV_VertexID, no vertex buffer,
 * placed by the viewport, with the destination rectangle as the viewport and
 * the scissor.
 *
 * The filter is a branch on a root constant rather than an index into the
 * sampler array, so that the blit needs no dynamic index at all. The two
 * samplers are the nearest/clamp and linear/clamp of rhi.h's immutable set,
 * at the registers rhi_d3d12_bindings.h assigns them; each is declared on
 * its own, which is a shader sampler range of one register, and the sampler
 * table covers both. */
const char kBlitHlsl[] =
    "cbuffer RootConstants : register(b4, space0) { uint4 g_blit_constants[8]; };\n"
    "Texture2D<float4> g_textures[8] : register(t0, space0);\n"
    "SamplerState g_sampler_nearest : register(s0, space0);\n"
    "SamplerState g_sampler_linear : register(s1, space0);\n"
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut main_vs(uint id : SV_VertexID) {\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    VSOut o;\n"
    "    o.uv = uv;\n"
    "    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "    return o;\n"
    "}\n"
    "float4 main_ps(VSOut i) : SV_Target {\n"
    "    if (g_blit_constants[0].x != 0u) return g_textures[0].Sample(g_sampler_linear, i.uv);\n"
    "    return g_textures[0].Sample(g_sampler_nearest, i.uv);\n"
    "}\n";

/* True when a DXIL container carries a signature.
 *
 * The layout is fixed: the four bytes 'D','X','B','C', then a sixteen byte
 * digest, then the version and the size. An unsigned container has that
 * digest all zero, which is exactly what a driver outside developer mode
 * refuses. Reading those twenty bytes is the whole check; nothing here parses
 * the parts. */
bool dxil_container_is_signed(const std::vector<uint8_t>& blob) {
    if (blob.size() < 20) return false;
    if (blob[0] != 'D' || blob[1] != 'X' || blob[2] != 'B' || blob[3] != 'C') return false;
    for (size_t i = 4; i < 20; ++i) {
        if (blob[i] != 0) return true;
    }
    return false;
}

/* Compiles HLSL through dxcompiler.dll. dxcompiler.dll signs what it compiles
 * in the same process, provided dxil.dll is in the process too; the loader
 * pulls that one in first from the same directory. Only reached when no DXIL
 * is compiled in. */
bool compile_with_dxc(D3D12Device* dev, const char* source, size_t source_bytes,
                      const char* entry, const char* target, const char* name,
                      std::vector<uint8_t>& out, std::string& error) {
    const char* why = nullptr;
    DxcCreateInstanceFn create = d3d12_load_dxcompiler(&why);
    if (!create) {
        error = why ? why : "dxcompiler.dll is not available";
        return false;
    }

    ComPtr<IDxcCompiler3> compiler;
    const HRESULT chr = create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3),
                               compiler.put_void());
    if (FAILED(chr) || !compiler) {
        char msg[192];
        std::snprintf(msg, sizeof(msg),
                      "DxcCreateInstance did not return an IDxcCompiler3 (HRESULT 0x%08lx, "
                      "%s); E_NOINTERFACE here means a dxcompiler.dll older than that "
                      "interface",
                      (unsigned long)chr, d3d12_hresult_name(chr));
        error = msg;
        return false;
    }

    /* DXC takes its arguments as wide strings. The names here are all ASCII
     * literals of this file's own making, so a widening copy is enough. */
    auto widen = [](const char* s) {
        std::wstring w;
        for (; *s; ++s) w.push_back((wchar_t)(unsigned char)*s);
        return w;
    };
    const std::wstring w_entry = widen(entry);
    const std::wstring w_target = widen(target);
    const std::wstring w_name = widen(name);
    const wchar_t* args[] = {
        w_name.c_str(),
        L"-E", w_entry.c_str(),
        L"-T", w_target.c_str(),
        /* Column-major matrices: SPIRV-Cross emits the same convention the
         * GLSL declared, and the overlay's transform arrives column-major
         * with column vectors (see overlay.vert). */
        L"-Zpc",
        L"-O3",
    };

    DxcBuffer src{};
    src.Ptr = source;
    src.Size = source_bytes;
    src.Encoding = DXC_CP_UTF8;

    ComPtr<IDxcResult> result;
    const HRESULT rhr = compiler->Compile(&src, args,
                                          (UINT32)(sizeof(args) / sizeof(args[0])), nullptr,
                                          __uuidof(IDxcResult), result.put_void());
    if (FAILED(rhr) || !result) {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "IDxcCompiler3::Compile failed to run (HRESULT 0x%08lx, %s)",
                      (unsigned long)rhr, d3d12_hresult_name(rhr));
        error = msg;
        return false;
    }

    /* status stays E_FAIL when GetStatus itself fails, which takes the branch
     * below and reports whatever diagnostic DXC left. The failure to read the
     * status is its own line, because otherwise a compile that actually
     * succeeded would be reported as one that failed with no message. */
    HRESULT status = E_FAIL;
    const HRESULT shr = result->GetStatus(&status);
    if (FAILED(shr)) {
        rt_log_error("rhi", "IDxcResult::GetStatus failed with HRESULT 0x%08lx (%s) while "
                            "compiling %s; the compile is treated as failed",
                     (unsigned long)shr, d3d12_hresult_name(shr), name);
    }
    if (FAILED(status)) {
        ComPtr<IDxcBlobUtf8> errors;
        if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8),
                                        errors.put_void(), nullptr))
            && errors && errors->GetStringLength() > 0) {
            error.assign(errors->GetStringPointer(), errors->GetStringLength());
        } else {
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                          "the compile failed with HRESULT 0x%08lx (%s) and DXC produced no "
                          "diagnostic", (unsigned long)status, d3d12_hresult_name(status));
            error = msg;
        }
        return false;
    }

    ComPtr<IDxcBlob> object;
    const HRESULT ohr = result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob),
                                          object.put_void(), nullptr);
    if (FAILED(ohr) || !object || object->GetBufferSize() == 0) {
        char msg[192];
        std::snprintf(msg, sizeof(msg),
                      "the compile succeeded but produced no object (GetOutput HRESULT "
                      "0x%08lx, %s, %llu bytes)", (unsigned long)ohr,
                      d3d12_hresult_name(ohr),
                      object ? (unsigned long long)object->GetBufferSize() : 0ull);
        error = msg;
        return false;
    }
    out.resize(object->GetBufferSize());
    std::memcpy(out.data(), object->GetBufferPointer(), out.size());

    /* Which libraries were used, and whether the blob came out signed, once
     * per run. A driver outside developer mode rejects an unsigned container,
     * and the failure it gives back is CreateComputePipelineState returning
     * E_INVALIDARG with nothing said about why, so the state is logged here
     * where the cause is still visible. */
    static bool logged = false;
    if (!logged) {
        logged = true;
        const char* dxc_path = d3d12_dxcompiler_path();
        const char* dxil_path = d3d12_dxil_path();
        rt_log_info("rhi", "dxcompiler.dll: %s", dxc_path ? dxc_path : "(not recorded)");
        rt_log_info("rhi", "dxil.dll (the validator that signs): %s",
                    dxil_path ? dxil_path : "not loaded");
    }
    if (!dxil_container_is_signed(out)) {
        /* Not fatal here: the pipeline creation that follows is where the
         * driver decides, and a machine in developer mode accepts it. Loud,
         * because this is the one thing that makes an otherwise correct
         * shader fail to build. */
        static bool warned = false;
        if (!warned) {
            warned = true;
            rt_log_warn("rhi", "the DXIL dxcompiler.dll produced is unsigned (its container "
                               "hash is all zero). Put dxil.dll next to ico.exe beside "
                               "dxcompiler.dll; without it a driver rejects the shader "
                               "unless Windows is in developer mode.");
        }
    }
    (void)dev;
    return true;
}

#if defined(ICORECOMP_HAVE_SHADER_DXIL)
bool find_dxil(const char* name, std::vector<uint8_t>& out) {
    size_t count = 0;
    const ShaderDxil* table = shader_dxil_table(&count);
    for (size_t i = 0; i < count; ++i) {
        if (std::strcmp(table[i].name, name) != 0) continue;
        out.assign(table[i].bytes, table[i].bytes + table[i].size);
        return true;
    }
    return false;
}
#endif

#if defined(ICORECOMP_HAVE_SHADER_HLSL)
const ShaderHlsl* find_hlsl(const char* name) {
    size_t count = 0;
    const ShaderHlsl* table = shader_hlsl_table(&count);
    for (size_t i = 0; i < count; ++i) {
        if (std::strcmp(table[i].name, name) == 0) return &table[i];
    }
    return nullptr;
}
#endif

const char* target_for(const char* stage) {
    if (std::strcmp(stage, "cs") == 0) return "cs_6_0";
    if (std::strcmp(stage, "vs") == 0) return "vs_6_0";
    return "ps_6_0";
}

} // namespace

ShaderBytecode d3d12_shader_for_spirv(D3D12Device* dev, const uint32_t* spirv,
                                      size_t words, const char* stage,
                                      const char* debug_name) {
    /* The blob is matched word for word against the generated arrays. An
     * address comparison is what the first D3D12 run tried and it failed:
     * the arrays live in a header, so before they were made inline every
     * translation unit held its own copy at its own address. Content
     * identity does not depend on that either way. The HLSL table carries
     * its own compile target, so the stage is not needed here. */
    (void)stage;
    size_t count = 0;
    const ShaderName* table = shader_name_table(&count);
    const char* name = nullptr;
    for (size_t i = 0; i < count; ++i) {
        if (table[i].words == words
            && std::memcmp(table[i].spirv, spirv, words * sizeof(uint32_t)) == 0) {
            name = table[i].name;
            break;
        }
    }
    if (!name) {
        dev->fatal("pipeline %s was given a SPIR-V blob this backend has no HLSL for. "
                   "The D3D12 backend identifies a shader by the generated array it came "
                   "from (rhi_shaders.h); a blob built anywhere else has no DXIL twin.",
                   debug_name ? debug_name : "(unnamed)");
    }

    ShaderBytecode result;
    result.name = name;
#if defined(ICORECOMP_HAVE_SHADER_DXIL)
    if (find_dxil(name, result.bytes)) {
        result.from_dxil = true;
        return result;
    }
#endif

#if defined(ICORECOMP_HAVE_SHADER_HLSL)
    if (const ShaderHlsl* hlsl = find_hlsl(name)) {
        std::string error;
        if (compile_with_dxc(dev, hlsl->source, std::strlen(hlsl->source), hlsl->entry,
                             hlsl->target, name, result.bytes, error)) {
            return result;
        }
        dev->fatal("no DXIL is compiled in for %s and the committed HLSL could not be "
                   "compiled at run time: %s.\n"
                   "  Copy dxcompiler.dll and dxil.dll from a DirectX Shader Compiler "
                   "release (github.com/microsoft/DirectXShaderCompiler, bin/x64 of the "
                   "Windows zip) into the folder holding ico.exe. Both also need the "
                   "Visual C++ redistributable (aka.ms/vs/17/release/vc_redist.x64.exe); "
                   "a Win32 error 126 above means that is what is missing, not the file.\n"
                   "  A package built with src/runtime/rhi/rhi_shaders_dxil.h present does "
                   "not need either DLL: run tools/gen_gs_shaders_dxil.sh (Linux) or "
                   "tools/gen_gs_shaders_dxil.ps1 (Windows) and rebuild.",
                   name, error.c_str());
    }
    dev->fatal("src/runtime/rhi/rhi_shaders_hlsl.h has no entry for %s; rerun "
               "tools/gen_gs_shaders.sh with spirv-cross available", name);
#else
    dev->fatal("this build has neither src/runtime/rhi/rhi_shaders_dxil.h nor "
               "rhi_shaders_hlsl.h, so the D3D12 backend has no bytecode for %s. Run "
               "tools/gen_gs_shaders.sh (HLSL) and tools/gen_gs_shaders_dxil.sh on Linux "
               "or tools/gen_gs_shaders_dxil.ps1 on Windows (DXIL).",
               name);
#endif
}

ShaderBytecode d3d12_internal_shader(D3D12Device* dev, const char* name, const char* stage) {
    ShaderBytecode result;
    result.name = name;
#if defined(ICORECOMP_HAVE_SHADER_DXIL)
    if (find_dxil(name, result.bytes)) {
        result.from_dxil = true;
        return result;
    }
#endif
    const char* entry = std::strcmp(stage, "vs") == 0 ? "main_vs" : "main_ps";
    std::string error;
    if (compile_with_dxc(dev, kBlitHlsl, sizeof(kBlitHlsl) - 1, entry, target_for(stage),
                         name, result.bytes, error)) {
        return result;
    }
    dev->fatal("the present blit shader %s has no compiled-in DXIL and it could not be "
               "compiled at run time: %s.\n"
               "  Copy dxcompiler.dll and dxil.dll from a DirectX Shader Compiler release "
               "into the folder holding ico.exe, or rebuild with "
               "src/runtime/rhi/rhi_shaders_dxil.h present "
               "(tools/gen_gs_shaders_dxil.sh on Linux, .ps1 on Windows).",
               name, error.c_str());
}

} // namespace rhi
