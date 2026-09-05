/* rhi/d3d12/rhi_d3d12_bindings.h: the register convention the D3D12 backend
 * and the HLSL generator both obey.
 *
 * Ours (MIT). This header is the authority. tools/gen_gs_shaders.sh carries
 * the same table in its rewrite step and fails if a resource appears that
 * this table does not name; when the two disagree the shaders bind to the
 * wrong registers and nothing says so, which is why the script errors on an
 * unknown name rather than assigning one.
 *
 * Why a rewrite step at all. Vulkan has one descriptor namespace, so rhi.h's
 * five bindings are (set 0, binding 0..4). HLSL has four register classes
 * (b, t, u, s) with independent numbering, and SPIRV-Cross derives a register
 * index from the SPIR-V binding decoration, so binding 1 (sixteen storage
 * buffers) would land on u1..u16 and collide with binding 4 (four storage
 * images) at u4. The generator therefore rewrites each declaration's register
 * to the table below.
 *
 * The mapping, all in space0:
 *
 *   rhi.h binding                     HLSL                    table offset
 *   ------------------------------------------------------------------------
 *   0  uniform buffer   [4]           b0..b3    CBV             0
 *   1  storage buffer   [16]          u0..u15   UAV (raw)       4
 *   2  sampled texture  [8]           t0..t7    SRV             20
 *   3  sampler          [4]           s0..s3    sampler table   (its own heap)
 *   4  storage image    [4]           u16..u19  UAV (typed)     28
 *   push constants 128 bytes          b4        root constants  (parameter 0)
 *
 * The storage buffers are UAVs and not SRVs even in the passes that only read
 * them, because raster.comp declares all sixteen slots as one GLSL block
 * array (an array of blocks cannot mix member layouts) and writes through
 * atomics into slot 0. One array in HLSL is one register class, so the whole
 * array is RWByteAddressBuffer.
 *
 * The four samplers are a descriptor table of their own over a four
 * descriptor SAMPLER heap, written once at device creation and never
 * rewritten, which is what rhi.h means by immutable.
 *
 * They are not four static samplers, which is what this backend tried first
 * and what made every graphics pipeline fail to build with E_INVALIDARG. The
 * generated HLSL declares one array, `SamplerState g_samplers[4] :
 * register(s0)`, and a shader resource range of more than one register is
 * matched against descriptor table ranges only: D3D12 does not merge adjacent
 * static samplers into a range that can bind it. The compiler's own root
 * signature checker (dxc, which runs the same validation the runtime does at
 * CreateGraphicsPipelineState) rejects the pair with
 *
 *   Shader sampler descriptor range (RegisterSpace=0, NumDescriptors=4,
 *   BaseShaderRegister=0) is not fully bound in root signature.
 *
 * and accepts it once the four static samplers are one table range. A single
 * `SamplerState s : register(s0)` is a range of one and a static sampler does
 * bind it, which is why the compute shaders, which declare no sampler at all,
 * and the present blit, which declares two single samplers, never showed it.
 *
 * The sampler heap is separate from the CBV/SRV/UAV heap because D3D12 has no
 * heap that holds both; both are bound for the life of a command list.
 *
 * Push constants are at b4 rather than b0 so that the root constants and the
 * uniform-buffer CBVs never share a register, which keeps one root signature
 * valid for every pipeline the way the single Vulkan pipeline layout is.
 */
#ifndef ICORECOMP_RHI_D3D12_BINDINGS_H
#define ICORECOMP_RHI_D3D12_BINDINGS_H

#include <cstdint>

namespace rhi {
namespace d3d12_bind {

/* Root parameters, in the order they are declared. */
enum : uint32_t {
    kRootParamConstants = 0,
    kRootParamTable     = 1,
    kRootParamSamplers  = 2,
    kRootParamCount     = 3,
};

/* Root constants: 128 bytes is 32 DWORDs, which is half the 64 DWORD root
 * signature budget. Each of the two descriptor tables costs one more DWORD,
 * so the signature is 34 of 64. */
enum : uint32_t {
    kRootConstantRegister = 4,   /* b4 */
    kRootConstantSpace    = 0,
    kRootConstantDwords   = 32,
};

/* Base HLSL register of each binding array. */
enum : uint32_t {
    kUniformBufferBaseRegister = 0,   /* b0  */
    kStorageBufferBaseRegister = 0,   /* u0  */
    kSampledTextureBaseRegister = 0,  /* t0  */
    kSamplerBaseRegister = 0,         /* s0  */
    kStorageImageBaseRegister = 16,   /* u16 */
    kRegisterSpace = 0,
};

/* Offsets of each range inside one descriptor table, in descriptors. The
 * table is one contiguous run in the shader-visible CBV/SRV/UAV heap, so a
 * set is copied or written as one block of kDescriptorsPerSet. */
enum : uint32_t {
    kTableUniformBuffers = 0,
    kTableStorageBuffers = 4,
    kTableSampledTextures = 20,
    kTableStorageImages = 28,
    kDescriptorsPerSet = 32,
};

} // namespace d3d12_bind
} // namespace rhi

#endif /* ICORECOMP_RHI_D3D12_BINDINGS_H */
