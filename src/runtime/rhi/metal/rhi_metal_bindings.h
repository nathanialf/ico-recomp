/* rhi/metal/rhi_metal_bindings.h: the MSL binding index convention the Metal
 * backend and the MSL generator both obey.
 *
 * Ours (MIT). This header is the authority. tools/gen_gs_shaders.sh carries
 * the same table in its rewrite step and fails if a resource appears that
 * this table does not name; when the two disagree the shaders read whatever
 * happens to be at that index and nothing says so, which is why the script
 * errors on an unknown name rather than assigning one.
 *
 * Why a rewrite step at all. Vulkan has one descriptor namespace, so rhi.h's
 * five bindings are (set 0, binding 0..4). Metal has three independent
 * argument tables (buffer, texture, sampler) and SPIRV-Cross assigns an index
 * in each of them by walking the resources in order, so the indices it
 * chooses depend on which resources a shader happens to declare. Two shaders
 * that use different subsets of rhi.h's layout would then disagree about
 * where slot 0 of the storage buffers is. The generator therefore rewrites
 * every index to the table below, which is fixed for every pipeline exactly
 * as the one Vulkan descriptor set layout is.
 *
 * The mapping:
 *
 *   rhi.h binding                     MSL argument table      index
 *   ------------------------------------------------------------------------
 *   0  uniform buffer   [4]           buffer                  0..3
 *   1  storage buffer   [16]          buffer                  4..19
 *   2  sampled texture  [8]           texture                 0..7
 *   3  sampler          [4]           sampler                 0..3
 *   4  storage image    [4]           texture                 8..11
 *   push constants 128 bytes          buffer                  20
 *   the vertex buffer                 buffer                  21
 *
 * A storage buffer array costs sixteen buffer indices and not one. Metal has
 * no arrays of buffers, so SPIRV-Cross flattens `buffer Block { } g_buf[16]`
 * into sixteen entry point arguments at consecutive indices and rebuilds a
 * local pointer array from them (CompilerMSL::entry_point_args_discrete_
 * descriptors, and the reference output in SPIRV-Cross's own
 * reference/shaders-msl/vert/resource-arrays.ios.vert). Texture and sampler
 * arrays are native: `array<texture2d<float>, 8> g_textures [[texture(0)]]`
 * is one argument covering eight consecutive texture indices.
 *
 * Why no argument buffers. The whole layout is 22 of the 31 buffer indices,
 * 12 of the 128 texture indices and 4 of the 16 sampler indices a macOS
 * device provides, so every slot fits the direct binding limits. An argument
 * buffer would add an encoder, a residency call per submit and a second place
 * for the indices to be written down, and would buy nothing this layout
 * needs.
 *
 * The vertex buffer's index is ours to pick rather than SPIRV-Cross's: a
 * vertex stage takes its attributes through [[stage_in]], and which buffer
 * index feeds them is set by the MTLVertexDescriptor the pipeline is built
 * with. 21 is one past the push constants so the whole convention reads in
 * one run.
 *
 * Indices 22 and up are left free on purpose. SPIRV-Cross reserves the high
 * end of the buffer table for auxiliary buffers it can emit on its own
 * (swizzle constants at 30, buffer sizes at 25, tessellation and multiview
 * buffers between them; see CompilerMSL::Options in spirv_msl.hpp). None of
 * this project's shaders makes it emit one, and the generator's rewrite
 * errors on any argument it cannot name, so one appearing later is a build
 * failure rather than a collision.
 */
#ifndef ICORECOMP_RHI_METAL_BINDINGS_H
#define ICORECOMP_RHI_METAL_BINDINGS_H

#include <cstdint>

namespace rhi {
namespace metal_bind {

/* Base index of each binding array in its own argument table. */
enum : uint32_t {
    kUniformBufferBaseIndex  = 0,   /* buffer  0..3   */
    kStorageBufferBaseIndex  = 4,   /* buffer  4..19  */
    kPushConstantIndex       = 20,  /* buffer  20     */
    kVertexBufferIndex       = 21,  /* buffer  21     */

    kSampledTextureBaseIndex = 0,   /* texture 0..7   */
    kStorageImageBaseIndex   = 8,   /* texture 8..11  */

    kSamplerBaseIndex        = 0,   /* sampler 0..3   */
};

/* What a macOS device guarantees, for the assertion in the device's setup.
 * 31 buffers and 16 samplers per stage, and 128 textures per stage on
 * MTLGPUFamilyApple6 and later and on MTLGPUFamilyMac2, which are the two
 * families this backend accepts. */
enum : uint32_t {
    kMaxMetalBuffers  = 31,
    kMaxMetalTextures = 128,
    kMaxMetalSamplers = 16,
};

/* Attribute indices of the one vertex layout (rhi.h): float2 position,
 * float2 texture coordinate, one R8G8B8A8_UNORM colour, 20 bytes. These are
 * the SPIR-V location decorations, which SPIRV-Cross carries through to
 * [[attribute(N)]] unchanged. */
enum : uint32_t {
    kVertexAttrPosition = 0,
    kVertexAttrTexCoord = 1,
    kVertexAttrColor    = 2,
    kVertexStride       = 20,
};

} // namespace metal_bind
} // namespace rhi

#endif /* ICORECOMP_RHI_METAL_BINDINGS_H */
