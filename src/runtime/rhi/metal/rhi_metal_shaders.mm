/* rhi/metal/rhi_metal_shaders.mm: see rhi_metal_shaders.h for the one path
 * and why there is no ahead-of-time .metallib.
 *
 * Ours (MIT).
 */
#import "rhi_metal_shaders.h"

#import "rhi_metal.h"
#import "../rhi_shaders.h"
#import "../../runtime.h"

/* The generated index is optional. A tree that has not run the generator with
 * spirv-cross available still builds, and the backend still says exactly
 * which command to run. */
#if defined(__has_include)
#  if __has_include("../rhi_shaders_msl.h")
#    include "../rhi_shaders_msl.h"
#    define ICORECOMP_HAVE_SHADER_MSL 1
#  endif
#endif

#include <cstring>
#include <string>

namespace rhi {

namespace {

/* The name of each SPIR-V array is rhi_shaders.h's shader_name_table(), which
 * the generator writes from the same list the arrays themselves come from.
 * The array address is the identity; rhi_metal_shaders.h says why. This file
 * used to keep a second copy of that table by hand, the copy was missing
 * shadow.comp, and the renderer fatalled at construction on this backend as a
 * result. Nothing is listed here now. */

/* The backend's own present blit. It has no GLSL counterpart because the
 * Vulkan backend blits with vkCmdBlitImage, which Metal has no equivalent of:
 * MTLBlitCommandEncoder's texture copies neither scale nor filter, so a
 * scaled present has to be a draw. One full-screen triangle from the vertex
 * id, no vertex buffer, placed by the viewport, with the destination
 * rectangle as the viewport and the scissor.
 *
 * Metal's clip space is D3D's and not Vulkan's: +Y is up and (-1, +1) is the
 * top left of the render target, so the same mapping the D3D12 blit uses is
 * the right one here, and it is the same reason the generator passes
 * --flip-vert-y for the overlay's vertex stage.
 *
 * The filter is a branch on a push constant rather than an index into the
 * sampler array, matching the D3D12 blit: a dynamic index into a sampler
 * array is an argument buffer feature, and this shader has no need of one.
 * The two samplers are the nearest/clamp and linear/clamp of rhi.h's
 * immutable set, at the indices rhi_metal_bindings.h assigns them. */
const char kBlitMsl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct BlitVaryings { float4 position [[position]]; float2 uv; };\n"
    "vertex BlitVaryings blit_vertex(uint vid [[vertex_id]])\n"
    "{\n"
    "    float2 uv = float2(float((vid << 1) & 2u), float(vid & 2u));\n"
    "    BlitVaryings out;\n"
    "    out.uv = uv;\n"
    "    out.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "    return out;\n"
    "}\n"
    "fragment float4 blit_fragment(BlitVaryings in [[stage_in]],\n"
    "                              constant uint4* pc [[buffer(20)]],\n"
    "                              array<texture2d<float>, 8> textures [[texture(0)]],\n"
    "                              array<sampler, 4> samplers [[sampler(0)]])\n"
    "{\n"
    "    if (pc[0].x != 0u) return textures[0].sample(samplers[1], in.uv);\n"
    "    return textures[0].sample(samplers[0], in.uv);\n"
    "}\n";

/* Compile options, and both settings are decisions rather than defaults.
 *
 * languageVersion is pinned to the version the generator asked SPIRV-Cross
 * for. Leaving it at the default would let a newer toolchain accept source
 * the pinned generator would not produce, so a mismatch between the two would
 * only show up as a shader that behaves differently on someone else's Mac.
 *
 * fastMathEnabled is turned off. It is on by default, and it lets the
 * compiler reassociate and contract float arithmetic. The rasteriser's
 * interpolation and the CRTC's colour maths are being checked against the
 * hardware bit for bit (docs/GS_RENDERER.md, the parity gate), so a
 * reassociation the other two backends do not perform would show up as this
 * backend disagreeing with them and nothing would say why.
 *
 * fastMathEnabled is the spelling the macOS 14 SDK has. macOS 15 replaced it
 * with the mathMode enum and deprecated this one, so a newer SDK compiles
 * this with a deprecation warning rather than an error. Switching to
 * mathMode would raise the SDK floor, which is not worth it for a property
 * that is set once. */
MTLCompileOptions* compile_options(uint32_t msl_version) {
    MTLCompileOptions* options = [MTLCompileOptions new];
    switch (msl_version) {
        case 20300: options.languageVersion = MTLLanguageVersion2_3; break;
        case 20400: options.languageVersion = MTLLanguageVersion2_4; break;
        default:    options.languageVersion = MTLLanguageVersion2_3; break;
    }
    options.fastMathEnabled = NO;
    return options;
}

id<MTLLibrary> library_from_source(MetalDevice* dev, const char* name, const char* source) {
    NSMutableDictionary<NSString*, id<MTLLibrary>>* cache = dev->library_cache();
    NSString* key = [NSString stringWithUTF8String:name];
    id<MTLLibrary> cached = cache[key];
    if (cached) return cached;

    NSError* error = nil;
    NSString* text = [NSString stringWithUTF8String:source];
    id<MTLLibrary> library = [dev->mtl() newLibraryWithSource:text
                                                      options:compile_options(dev->msl_version())
                                                        error:&error];
    if (!library) {
        dev->fatal("the Metal compiler rejected %s: %s. The MSL is generated by "
                   "tools/gen_gs_shaders.sh through SPIRV-Cross; rerun it and read the "
                   "diagnostic against src/runtime/gs/render/shaders/msl/%s.metal.",
                   name, error ? [[error localizedDescription] UTF8String] : "(no diagnostic)",
                   name);
    }
    cache[key] = library;
    return library;
}

#if defined(ICORECOMP_HAVE_SHADER_MSL)
const ShaderMsl* find_msl(const char* name) {
    size_t count = 0;
    const ShaderMsl* table = shader_msl_table(&count);
    for (size_t i = 0; i < count; ++i) {
        if (std::strcmp(table[i].name, name) == 0) return &table[i];
    }
    return nullptr;
}
#endif

} // namespace

MetalFunction metal_function_for_spirv(MetalDevice* dev, const uint32_t* spirv, size_t words,
                                       const char* stage, const char* debug_name) {
    /* Neither is needed to find the twin: the SPIR-V array address is the
     * identity, and the MSL table carries its own entry point name. */
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
        dev->fatal("pipeline %s was given a SPIR-V blob this backend has no MSL for. "
                   "The Metal backend identifies a shader by the generated array it came "
                   "from (rhi_shaders.h); a blob built anywhere else has no MSL twin.",
                   debug_name ? debug_name : "(unnamed)");
    }

#if defined(ICORECOMP_HAVE_SHADER_MSL)
    const ShaderMsl* msl = find_msl(name);
    if (!msl) {
        dev->fatal("src/runtime/rhi/rhi_shaders_msl.h has no entry for %s; rerun "
                   "tools/gen_gs_shaders.sh with spirv-cross available", name);
    }
    id<MTLLibrary> library = library_from_source(dev, name, msl->source);
    NSString* entry = [NSString stringWithUTF8String:msl->entry];
    id<MTLFunction> function = [library newFunctionWithName:entry];
    if (!function) {
        dev->fatal("the MSL for %s compiled but has no function named %s. SPIRV-Cross "
                   "renames the SPIR-V entry point, and the generator records the name it "
                   "chose; a mismatch means the two are out of step.", name, msl->entry);
    }
    function.label = [NSString stringWithUTF8String:name];
    dev->note_shader_path("msl");

    MetalFunction result;
    result.function = function;
    if (msl->local_size_x != 0) {
        result.threads_per_group = MTLSizeMake(msl->local_size_x, msl->local_size_y,
                                               msl->local_size_z);
    }
    return result;
#else
    dev->fatal("this build has no src/runtime/rhi/rhi_shaders_msl.h, so the Metal backend "
               "has no source for %s. Run tools/gen_gs_shaders.sh with spirv-cross "
               "available.", name);
#endif
}

MetalFunction metal_internal_function(MetalDevice* dev, const char* name) {
    id<MTLLibrary> library = library_from_source(dev, "blit", kBlitMsl);
    const bool vertex_stage = std::strcmp(name, "blit.vert") == 0;
    NSString* entry = vertex_stage ? @"blit_vertex" : @"blit_fragment";
    id<MTLFunction> function = [library newFunctionWithName:entry];
    if (!function) {
        dev->fatal("the present blit library has no function named %s",
                   [entry UTF8String]);
    }
    function.label = [NSString stringWithUTF8String:name];

    MetalFunction result;
    result.function = function;
    return result;
}

} // namespace rhi
