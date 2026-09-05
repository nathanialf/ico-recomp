/* rhi/d3d12/rhi_d3d12_shaders.h: where the D3D12 backend gets its bytecode.
 *
 * Ours (MIT).
 *
 * Two paths, and the one that was taken is logged at info the first time a
 * pipeline is built:
 *
 *   dxil     Committed, signed DXIL compiled ahead of time into
 *            src/runtime/rhi/rhi_shaders_dxil.h by
 *            tools/gen_gs_shaders_dxil.sh (Linux) or .ps1 (Windows). This is
 *            the shipping path, and the only one that needs nothing on the
 *            player's machine. Ahead of time because drivers reject unsigned
 *            DXIL outside developer mode; both generators check the
 *            container hash of what they wrote and fail on a zero one.
 *
 *   hlsl     The committed HLSL (rhi_shaders_hlsl.h, produced from the same
 *            SPIR-V by tools/gen_gs_shaders.sh) compiled at run time through
 *            dxcompiler.dll, with dxil.dll loaded from the same directory to
 *            sign the result. A fallback: those two DLLs are 32 MB and need
 *            the Visual C++ redistributable, and a machine without it fails
 *            their load with an error that reads as a missing file.
 *
 * Neither header exists until its generator has been run, so both are pulled
 * in with __has_include and their absence is a fatal at pipeline creation
 * naming the command to run, not a build error.
 *
 * A shader is identified by the CONTENT of the SPIR-V the caller passed,
 * compared word for word against the generated arrays in rhi_shaders.h.
 * Not by the array's address: those arrays live in a header, so before they
 * were made inline every translation unit held its own copy at its own
 * address, and the first D3D12 run failed on exactly that. rhi.h's
 * create_compute_pipeline and GraphicsPipelineDesc carry SPIR-V and nothing
 * else, so content identity costs no change to the interface. A blob that
 * matches none of them is fatal and names the pipeline, rather than
 * silently picking a shader.
 *
 * Which DXIL container goes with that name is settled at build time, not
 * here: the generator records the SHA-1 of each HLSL source in
 * rhi_shaders_dxil.h and tools/check_shaders_fresh.py, wired to the runtime
 * target, fails the build when a source has been edited without the DXIL
 * being regenerated.
 */
#ifndef ICORECOMP_RHI_D3D12_SHADERS_H
#define ICORECOMP_RHI_D3D12_SHADERS_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rhi {

class D3D12Device;

/* One stage's bytecode, owned by the caller. */
struct ShaderBytecode {
    std::vector<uint8_t> bytes;
    bool from_dxil = false;   /* false means it came out of dxcompiler.dll */
    /* The generated shader's own name ("overlay.vert", "raster.comp",
     * "blit.frag"), which is what the pipeline diagnostics print. A pointer
     * into a static table or a literal, so it outlives every use. */
    const char* name = "(unresolved)";
};

/* Resolves a SPIR-V blob to DXIL. `stage` is "cs", "vs" or "ps" and is used
 * for the run-time compile target and for the messages. Fatal on failure. */
ShaderBytecode d3d12_shader_for_spirv(D3D12Device* dev, const uint32_t* spirv,
                                      size_t words, const char* stage,
                                      const char* debug_name);

/* Resolves one of the backend's own internal shaders by name ("blit.vert" or
 * "blit.frag"), which have no SPIR-V because Vulkan does the same work with
 * vkCmdBlitImage. Fatal on failure. */
ShaderBytecode d3d12_internal_shader(D3D12Device* dev, const char* name,
                                     const char* stage);

} // namespace rhi

#endif /* ICORECOMP_RHI_D3D12_SHADERS_H */
