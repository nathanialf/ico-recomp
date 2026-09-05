/* rhi/metal/rhi_metal_shaders.h: where the Metal backend gets its functions.
 *
 * Ours (MIT).
 *
 * One path, and it is logged at info the first time a pipeline is built:
 *
 *   msl      The committed MSL (src/runtime/rhi/rhi_shaders_msl.h, produced
 *            from the same SPIR-V as the other two backends by
 *            tools/gen_gs_shaders.sh), compiled at run time with
 *            newLibraryWithSource:options:error:.
 *
 * There is no ahead-of-time path and no committed .metallib, which is the one
 * place this backend differs from D3D12's. Two reasons, both structural.
 * Producing a .metallib needs Xcode's metal compiler, which runs on macOS
 * only and cannot be redistributed through this repository, so nothing on the
 * machines this project is developed on could build it. And a .metallib is
 * bound to the Metal toolchain that wrote it, where DXIL is bound only to
 * being signed. What replaces it is the binary archive below, which is a
 * cache and not a build product.
 *
 * The binary archive. MTLBinaryArchive caches compiled pipeline states in a
 * file under the executable's cache directory, mirroring where the
 * paraLLEl-GS backend keeps its Vulkan pipeline cache
 * (gs/gs_parallel_present.cpp resolves that as the base directory plus
 * "cache/"). It saves the pipeline compile, not the MSL to AIR compile: the
 * source still goes through newLibraryWithSource on every run. That is worth
 * saying plainly rather than calling it a shader cache, because it means a
 * cold start still pays for parsing four MSL translation units. A missing,
 * unreadable or rejected archive is a log line and an empty archive, never a
 * failure: the same rule the Vulkan cache follows.
 *
 * The index header does not exist until tools/gen_gs_shaders.sh has been run
 * with spirv-cross available, so it is pulled in with __has_include and its
 * absence is a fatal at pipeline creation naming the command to run, not a
 * build error.
 *
 * A shader is identified by the SPIR-V array the caller passed, exactly as
 * the D3D12 backend identifies one: rhi.h's create_compute_pipeline and
 * GraphicsPipelineDesc carry SPIR-V and nothing else, and every one of those
 * pointers comes from an accessor in the generated rhi_shaders.h, so the
 * array address is a stable identity that costs no change to the interface.
 * A pointer that is not one of them is fatal and names the pipeline.
 */
#ifndef ICORECOMP_RHI_METAL_SHADERS_H
#define ICORECOMP_RHI_METAL_SHADERS_H

#import <Metal/Metal.h>

#include <cstddef>
#include <cstdint>

namespace rhi {

class MetalDevice;

/* One resolved stage. `threads_per_group` is the shader's own
 * layout(local_size_*) for a compute stage and (1,1,1) otherwise; Metal takes
 * the threadgroup size at the dispatch rather than from the function, so it
 * has to travel with it. */
struct MetalFunction {
    id<MTLFunction> function = nil;
    MTLSize threads_per_group = MTLSizeMake(1, 1, 1);
};

/* Resolves a SPIR-V blob to a Metal function. `stage` is "comp", "vert" or
 * "frag" and is used only for the messages. Fatal on failure. */
MetalFunction metal_function_for_spirv(MetalDevice* dev, const uint32_t* spirv, size_t words,
                                       const char* stage, const char* debug_name);

/* Resolves one of the backend's own internal shaders by name ("blit.vert" or
 * "blit.frag"), which have no SPIR-V because Vulkan does the same work with
 * vkCmdBlitImage. Fatal on failure. */
MetalFunction metal_internal_function(MetalDevice* dev, const char* name);

} // namespace rhi

#endif /* ICORECOMP_RHI_METAL_SHADERS_H */
