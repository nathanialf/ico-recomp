/* rhi/metal/rhi_metal_device.mm: device, resources, pipelines, layer.
 *
 * Ours (MIT). See rhi_metal.h for the shape of the backend, rhi.h for the
 * interface it implements and rhi_metal_bindings.h for the argument index
 * convention the generated MSL and this file both obey.
 *
 * Nothing in this file has ever been compiled: there is no macOS toolchain on
 * the machines this project is developed on. docs/MACOS.md says so plainly,
 * and the macOS CI job is the first thing that will build it.
 */
#import "rhi_metal.h"
#import "rhi_metal_shaders.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include "../../runtime.h"
#include "../../host/portable.h"

#include <cfloat>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace rhi {

namespace {

const char* kind_name(BufferKind k) {
    switch (k) {
        case BufferKind::Upload:   return "upload";
        case BufferKind::Readback: return "readback";
        default:                   return "device local";
    }
}

/* The GPU families this backend accepts, highest first. Apple's families are
 * cumulative within a line, so a device that reports Apple9 also reports
 * Apple7; the first hit is therefore the highest the device supports and is
 * what gets logged. */
struct FamilyName {
    MTLGPUFamily family;
    const char* name;
};

const FamilyName* family_table(size_t* count) {
    static const FamilyName kTable[] = {
        { MTLGPUFamilyApple9, "Apple9" },
        { MTLGPUFamilyApple8, "Apple8" },
        { MTLGPUFamilyApple7, "Apple7" },
        { MTLGPUFamilyMac2,   "Mac2"   },
    };
    *count = sizeof(kTable) / sizeof(kTable[0]);
    return kTable;
}

/* 128 MiB of MSL, which nothing here approaches; the cap exists so a corrupt
 * cache file cannot be read into memory unbounded. */
constexpr uintmax_t kMaxArchiveBytes = 128u * 1024u * 1024u;

} // namespace

MTLPixelFormat to_mtl_format(Format f) {
    switch (f) {
        case Format::RGBA8Unorm: return MTLPixelFormatRGBA8Unorm;
        case Format::BGRA8Unorm: return MTLPixelFormatBGRA8Unorm;
        case Format::R32Uint:    return MTLPixelFormatR32Uint;
        default:                 return MTLPixelFormatInvalid;
    }
}

void MetalDevice::fatal(const char* fmt, ...) const {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    /* The device name is in every fatal for the reason the other two backends
     * give: the same message from two machines is the same bug only if it is
     * the same driver. */
    rt_fatal("rhi", nullptr, "%s (device: %s, %s)", msg, m_device_name.c_str(),
             m_api_version.empty() ? "no family yet" : m_api_version.c_str());
}

void MetalDevice::note_shader_path(const char* how) {
    if (m_logged_shader_path) return;
    m_logged_shader_path = true;
    rt_log_info("rhi", "Metal shaders: %s (binary archive: %s)", how,
                m_archive == nil ? "none this run"
                                 : (m_archive_loaded ? "loaded" : "starting empty"));
}

/* ---- device --------------------------------------------------------------- */

void MetalDevice::pick_device(const DeviceDesc& desc) {
    if (desc.prefer_software) {
        /* Never quietly a hardware device instead. Metal has no software
         * rasteriser (there is no WARP and no lavapipe here), so a caller
         * that asked for one has to be told rather than handed a GPU whose
         * results it would read as the reference. */
        rt_fatal("rhi", nullptr,
                 "a software device was requested and Metal has none: there is no "
                 "equivalent of D3D12's WARP or of a software Vulkan implementation on "
                 "macOS. Run without the software flag to use the system GPU.");
    }

    m_device = MTLCreateSystemDefaultDevice();
    if (!m_device) {
        rt_fatal("rhi", nullptr,
                 "MTLCreateSystemDefaultDevice returned nothing: this process has no Metal "
                 "device. On a Mac with a GPU that usually means the process has no window "
                 "server session, which is what a headless CI runner looks like.");
    }
    m_device_name = [[m_device name] UTF8String];

    size_t count = 0;
    const FamilyName* table = family_table(&count);
    const char* family = nullptr;
    for (size_t i = 0; i < count; ++i) {
        if ([m_device supportsFamily:table[i].family]) { family = table[i].name; break; }
    }
    if (!family) {
        m_api_version = "no accepted GPU family";
        fatal("this device reports neither MTLGPUFamilyApple7 nor MTLGPUFamilyMac2. "
              "rhi_metal.h says why those two are the floor.");
    }

    const bool metal3 = [m_device supportsFamily:MTLGPUFamilyMetal3];
    char version[128];
    std::snprintf(version, sizeof(version), "GPU family %s, %s, MSL %u.%u", family,
                  metal3 ? "Metal 3" : "pre-Metal 3", m_msl_version / 10000,
                  (m_msl_version / 100) % 100);
    m_api_version = version;

    /* What the renderer is allowed to ask this device for; see rhi::Limits.
     * Metal has no query for the largest threadgroup grid a dispatch may ask
     * for, and no documented per-dimension cap, so the three axes keep the
     * structure's default. That default is the floor Vulkan guarantees and
     * the number D3D12 states, which is the accuracy this backend can claim
     * rather than a value invented for it. */
    m_limits.frames_in_flight = kFrames;

    /* Metal's API validation layer is a loader setting and not something a
     * process can switch on for itself, unlike the Vulkan validation layer
     * and the D3D12 debug layer. Saying which environment variable does it is
     * the most this backend can do about desc.validation. */
    if (m_validation) {
        rt_log_info("rhi", "Metal has no in-process validation layer; run with "
                           "METAL_DEVICE_WRAPPER_TYPE=1 in the environment to get one");
    }

    rt_log_info("rhi", "Metal device: %s (%s), %u buffer arguments, %u textures, %u samplers",
                m_device_name.c_str(), m_api_version.c_str(),
                (unsigned)metal_bind::kMaxMetalBuffers,
                (unsigned)metal_bind::kMaxMetalTextures,
                (unsigned)metal_bind::kMaxMetalSamplers);
}

void MetalDevice::create_samplers() {
    /* The four immutable samplers, in the order rhi.h documents and with the
     * settings the Vulkan backend gives them one for one. */
    const struct {
        MTLSamplerMinMagFilter filter;
        MTLSamplerAddressMode address;
        const char* label;
    } kSpecs[kSamplerCount] = {
        { MTLSamplerMinMagFilterNearest, MTLSamplerAddressModeClampToEdge, "nearest/clamp"  },
        { MTLSamplerMinMagFilterLinear,  MTLSamplerAddressModeClampToEdge, "linear/clamp"   },
        { MTLSamplerMinMagFilterNearest, MTLSamplerAddressModeRepeat,      "nearest/repeat" },
        { MTLSamplerMinMagFilterLinear,  MTLSamplerAddressModeRepeat,      "linear/repeat"  },
    };
    for (uint32_t i = 0; i < kSamplerCount; ++i) {
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.minFilter = kSpecs[i].filter;
        sd.magFilter = kSpecs[i].filter;
        sd.mipFilter = MTLSamplerMipFilterNearest;
        sd.sAddressMode = kSpecs[i].address;
        sd.tAddressMode = kSpecs[i].address;
        sd.rAddressMode = kSpecs[i].address;
        sd.lodMinClamp = 0.0f;
        sd.lodMaxClamp = FLT_MAX;
        sd.label = [NSString stringWithUTF8String:kSpecs[i].label];
        m_samplers[i] = [m_device newSamplerStateWithDescriptor:sd];
        if (!m_samplers[i]) fatal("newSamplerStateWithDescriptor failed for %s", kSpecs[i].label);
    }
}

void MetalDevice::create_dummies() {
    /* Metal leaves an unbound argument slot undefined in the same way the
     * other two APIs do, and rhi.h's binding arrays are fixed size, so the
     * slots nothing bound take these. Unlike Vulkan they need no layout
     * transition, so there is no startup submit here. */
    BufferDesc bd;
    bd.size = 256;
    bd.kind = BufferKind::DeviceLocal;
    bd.usage = BufferUsage::Uniform | BufferUsage::Storage | BufferUsage::CopyDst;
    bd.debug_name = "rhi dummy buffer";
    m_dummy_buffer = create_buffer(bd);

    TextureDesc td;
    td.width = 1;
    td.height = 1;
    td.format = Format::RGBA8Unorm;
    td.usage = TextureUsage::Sampled | TextureUsage::CopyDst;
    td.debug_name = "rhi dummy texture";
    m_dummy_texture = create_texture(td);

    td.usage = TextureUsage::Storage;
    td.debug_name = "rhi dummy storage image";
    m_dummy_storage_image = create_texture(td);
}

MetalDevice::MetalDevice(const DeviceDesc& desc) {
    m_validation = desc.validation;
    /* The version the generator asked SPIRV-Cross for. Kept here rather than
     * in the shader loader so the log line that names the device also names
     * the language the shaders were written against. */
    m_msl_version = 20300;
    m_present_mode = desc.present_mode;

    pick_device(desc);

    m_queue = [m_device newCommandQueue];
    if (!m_queue) fatal("newCommandQueue returned nothing");
    m_queue.label = @"icorecomp rhi";

    m_timeline = [m_device newSharedEvent];
    if (!m_timeline) fatal("newSharedEvent returned nothing; the timeline needs one");
    m_timeline.label = @"icorecomp rhi timeline";

    m_libraries = [NSMutableDictionary dictionary];
    load_binary_archive();

    create_samplers();
    m_cmd = new MetalCommandList(this);
    create_dummies();

    if (!desc.headless) create_layer(desc);
}

MetalDevice::~MetalDevice() {
    wait_idle();
    store_binary_archive();

    for (BlitPipeline& bp : m_blit_pipelines) destroy_graphics_pipeline(bp.pipeline);
    m_blit_pipelines.clear();

    destroy_buffer(m_dummy_buffer);
    destroy_texture(m_dummy_texture);
    destroy_texture(m_dummy_storage_image);
    delete m_backbuffer;
    delete m_cmd;
    /* Everything else is an ARC strong reference and goes with the object. */
}

/* ---- buffers -------------------------------------------------------------- */

Buffer* MetalDevice::create_buffer(const BufferDesc& desc) {
    if (desc.size == 0) {
        fatal("create_buffer with size 0 (%s)", desc.debug_name ? desc.debug_name : "unnamed");
    }

    /* Shared for anything the CPU touches. rhi_metal.h says why there is no
     * Managed path: this port builds for arm64 macOS only, where Shared is
     * coherent and no didModifyRange call exists to forget. */
    MTLResourceOptions options = desc.kind == BufferKind::DeviceLocal
                               ? MTLResourceStorageModePrivate
                               : MTLResourceStorageModeShared;

    id<MTLBuffer> buffer = [m_device newBufferWithLength:(NSUInteger)desc.size options:options];
    if (!buffer) {
        fatal("newBufferWithLength failed for a %llu byte %s buffer (%s)",
              (unsigned long long)desc.size, kind_name(desc.kind),
              desc.debug_name ? desc.debug_name : "unnamed");
    }
    if (desc.debug_name) buffer.label = [NSString stringWithUTF8String:desc.debug_name];

    Buffer* b = new Buffer();
    b->buffer = buffer;
    b->size = desc.size;
    b->kind = desc.kind;
    /* Metal has no per-buffer usage flags: a buffer is a buffer, and what it
     * may be used for is decided by which argument slot it is bound to. The
     * field is kept so the fatal messages can name what the caller asked
     * for. */
    b->usage = desc.usage;
    if (desc.kind != BufferKind::DeviceLocal) b->mapped = [buffer contents];
    return b;
}

void MetalDevice::destroy_buffer(Buffer* b) {
    if (!b) return;
    b->buffer = nil;
    delete b;
}

void* MetalDevice::map(Buffer* b) {
    if (!b) fatal("map of a null buffer");
    if (!b->mapped) fatal("map of a device-local buffer; only Upload and Readback are mappable");
    return b->mapped;
}

/* ---- textures ------------------------------------------------------------- */

Texture* MetalDevice::create_texture(const TextureDesc& desc) {
    if (desc.width == 0 || desc.height == 0) {
        fatal("create_texture %ux%u (%s)", desc.width, desc.height,
              desc.debug_name ? desc.debug_name : "unnamed");
    }
    const MTLPixelFormat format = to_mtl_format(desc.format);
    if (format == MTLPixelFormatInvalid) {
        fatal("create_texture with a format this backend has no Metal spelling for (%s)",
              desc.debug_name ? desc.debug_name : "unnamed");
    }

    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                           width:desc.width
                                                          height:desc.height
                                                       mipmapped:NO];
    /* Copies need no usage bit in Metal, so CopySrc and CopyDst add nothing
     * here. The other three do. */
    MTLTextureUsage usage = MTLTextureUsageUnknown;
    if (has(desc.usage, TextureUsage::Sampled))     usage |= MTLTextureUsageShaderRead;
    if (has(desc.usage, TextureUsage::Storage))     usage |= MTLTextureUsageShaderWrite;
    if (has(desc.usage, TextureUsage::ColorTarget)) usage |= MTLTextureUsageRenderTarget;
    /* blit_texture is a draw and not a copy (see rhi_metal_cmd.mm), so a
     * texture this backend may have to blit into or out of also needs the two
     * bits that draw uses. Adding them here rather than making every caller
     * ask for them keeps rhi.h's usage set the same across the backends. */
    if (has(desc.usage, TextureUsage::CopySrc)) usage |= MTLTextureUsageShaderRead;
    if (has(desc.usage, TextureUsage::CopyDst)) usage |= MTLTextureUsageRenderTarget;
    td.usage = usage;
    td.storageMode = MTLStorageModePrivate;

    id<MTLTexture> texture = [m_device newTextureWithDescriptor:td];
    if (!texture) {
        fatal("newTextureWithDescriptor failed for a %ux%u texture (%s)", desc.width,
              desc.height, desc.debug_name ? desc.debug_name : "unnamed");
    }
    if (desc.debug_name) texture.label = [NSString stringWithUTF8String:desc.debug_name];

    Texture* t = new Texture();
    t->texture = texture;
    t->format = format;
    t->width = desc.width;
    t->height = desc.height;
    t->usage = desc.usage;
    return t;
}

Format MetalDevice::texture_format(Texture* t) const {
    if (!t) return Format::Unknown;
    switch (t->format) {
        case MTLPixelFormatRGBA8Unorm: return Format::RGBA8Unorm;
        case MTLPixelFormatBGRA8Unorm: return Format::BGRA8Unorm;
        case MTLPixelFormatR32Uint:    return Format::R32Uint;
        default:                       return Format::Unknown;
    }
}

void MetalDevice::destroy_texture(Texture* t) {
    if (!t) return;
    t->texture = nil;
    delete t;
}

/* ---- pipelines ------------------------------------------------------------ */

ComputePipeline* MetalDevice::create_compute_pipeline(const uint32_t* spirv, size_t words,
                                                      const char* name) {
    const char* label = name ? name : "(unnamed)";
    MetalFunction fn = metal_function_for_spirv(this, spirv, words, "comp", label);

    MTLComputePipelineDescriptor* d = [MTLComputePipelineDescriptor new];
    d.computeFunction = fn.function;
    d.label = [NSString stringWithUTF8String:label];
    /* Metal wants to know that no threadgroup will be launched partly filled,
     * which is what lets it drop the bounds test in every dispatch. Every
     * dispatch this renderer issues covers whole threadgroups (the tile
     * dispatches are sized in tiles, the scanout in 8x8 blocks with the
     * shader testing the picture bounds itself), so this is a statement about
     * the caller and not an optimisation that changes a result. */
    d.threadGroupSizeIsMultipleOfThreadExecutionWidth = NO;
    if (m_archive) d.binaryArchives = @[ m_archive ];

    NSError* error = nil;
    id<MTLComputePipelineState> pso =
        [m_device newComputePipelineStateWithDescriptor:d
                                                options:MTLPipelineOptionNone
                                             reflection:nil
                                                  error:&error];
    if (!pso) {
        fatal("compute pipeline %s failed to build: %s", label,
              error ? [[error localizedDescription] UTF8String] : "(no diagnostic)");
    }
    if (fn.threads_per_group.width * fn.threads_per_group.height * fn.threads_per_group.depth
        > pso.maxTotalThreadsPerThreadgroup) {
        fatal("compute pipeline %s declares a threadgroup of %lux%lux%lu and this device "
              "allows at most %lu threads in one", label,
              (unsigned long)fn.threads_per_group.width,
              (unsigned long)fn.threads_per_group.height,
              (unsigned long)fn.threads_per_group.depth,
              (unsigned long)pso.maxTotalThreadsPerThreadgroup);
    }
    if (m_archive) {
        NSError* add_error = nil;
        if (![m_archive addComputePipelineFunctionsWithDescriptor:d error:&add_error]) {
            rt_log_warn("rhi", "the Metal binary archive would not take %s (%s); this run "
                               "compiles it every time", label,
                        add_error ? [[add_error localizedDescription] UTF8String] : "no reason");
        }
    }

    ComputePipeline* p = new ComputePipeline();
    p->pso = pso;
    p->threads_per_group = fn.threads_per_group;
    return p;
}

void MetalDevice::destroy_compute_pipeline(ComputePipeline* p) {
    if (!p) return;
    p->pso = nil;
    delete p;
}

GraphicsPipeline* MetalDevice::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
    const char* label = desc.debug_name ? desc.debug_name : "(unnamed)";
    MetalFunction vs = metal_function_for_spirv(this, desc.vertex_spirv,
                                                desc.vertex_spirv_words, "vert", label);
    MetalFunction fs = metal_function_for_spirv(this, desc.fragment_spirv,
                                                desc.fragment_spirv_words, "frag", label);

    /* The one vertex layout, documented in rhi.h: float2 position, float2
     * texture coordinate, one packed RGBA8 colour. 20 bytes. */
    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[metal_bind::kVertexAttrPosition].format = MTLVertexFormatFloat2;
    vd.attributes[metal_bind::kVertexAttrPosition].offset = 0;
    vd.attributes[metal_bind::kVertexAttrPosition].bufferIndex = metal_bind::kVertexBufferIndex;
    vd.attributes[metal_bind::kVertexAttrTexCoord].format = MTLVertexFormatFloat2;
    vd.attributes[metal_bind::kVertexAttrTexCoord].offset = 8;
    vd.attributes[metal_bind::kVertexAttrTexCoord].bufferIndex = metal_bind::kVertexBufferIndex;
    vd.attributes[metal_bind::kVertexAttrColor].format = MTLVertexFormatUChar4Normalized;
    vd.attributes[metal_bind::kVertexAttrColor].offset = 16;
    vd.attributes[metal_bind::kVertexAttrColor].bufferIndex = metal_bind::kVertexBufferIndex;
    vd.layouts[metal_bind::kVertexBufferIndex].stride = metal_bind::kVertexStride;
    vd.layouts[metal_bind::kVertexBufferIndex].stepFunction = MTLVertexStepFunctionPerVertex;
    vd.layouts[metal_bind::kVertexBufferIndex].stepRate = 1;

    MTLRenderPipelineDescriptor* rd = [MTLRenderPipelineDescriptor new];
    rd.label = [NSString stringWithUTF8String:label];
    rd.vertexFunction = vs.function;
    rd.fragmentFunction = fs.function;
    rd.vertexDescriptor = vd;
    rd.rasterSampleCount = 1;

    const MTLPixelFormat color = to_mtl_format(desc.color_format);
    if (color == MTLPixelFormatInvalid) {
        fatal("graphics pipeline %s asks for a colour format this backend has no Metal "
              "spelling for", label);
    }
    MTLRenderPipelineColorAttachmentDescriptor* att = rd.colorAttachments[0];
    att.pixelFormat = color;
    att.writeMask = MTLColorWriteMaskAll;
    att.blendingEnabled = desc.blend ? YES : NO;
    /* Premultiplied geometry blends with ONE; straight alpha with SRC_ALPHA.
     * The UI emits the former; see RT_PGS_OVERLAY_PREMULTIPLIED. Same table
     * as the Vulkan backend's. */
    att.rgbBlendOperation = MTLBlendOperationAdd;
    att.sourceRGBBlendFactor = desc.premultiplied ? MTLBlendFactorOne
                                                  : MTLBlendFactorSourceAlpha;
    att.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    att.alphaBlendOperation = MTLBlendOperationAdd;
    att.sourceAlphaBlendFactor = MTLBlendFactorOne;
    att.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

    if (m_archive) rd.binaryArchives = @[ m_archive ];

    NSError* error = nil;
    id<MTLRenderPipelineState> pso = [m_device newRenderPipelineStateWithDescriptor:rd
                                                                             error:&error];
    if (!pso) {
        fatal("graphics pipeline %s failed to build: %s", label,
              error ? [[error localizedDescription] UTF8String] : "(no diagnostic)");
    }
    if (m_archive) {
        NSError* add_error = nil;
        if (![m_archive addRenderPipelineFunctionsWithDescriptor:rd error:&add_error]) {
            rt_log_warn("rhi", "the Metal binary archive would not take %s (%s); this run "
                               "compiles it every time", label,
                        add_error ? [[add_error localizedDescription] UTF8String] : "no reason");
        }
    }

    GraphicsPipeline* p = new GraphicsPipeline();
    p->pso = pso;
    p->color_format = color;
    return p;
}

void MetalDevice::destroy_graphics_pipeline(GraphicsPipeline* p) {
    if (!p) return;
    p->pso = nil;
    delete p;
}

GraphicsPipeline* MetalDevice::blit_pipeline(MTLPixelFormat format) {
    for (BlitPipeline& bp : m_blit_pipelines) {
        if (bp.format == format) return bp.pipeline;
    }

    MetalFunction vs = metal_internal_function(this, "blit.vert");
    MetalFunction fs = metal_internal_function(this, "blit.frag");

    MTLRenderPipelineDescriptor* rd = [MTLRenderPipelineDescriptor new];
    rd.label = @"rhi present blit";
    rd.vertexFunction = vs.function;
    rd.fragmentFunction = fs.function;
    /* No vertex descriptor: the triangle comes from the vertex id. */
    rd.rasterSampleCount = 1;
    rd.colorAttachments[0].pixelFormat = format;
    rd.colorAttachments[0].writeMask = MTLColorWriteMaskAll;
    rd.colorAttachments[0].blendingEnabled = NO;
    if (m_archive) rd.binaryArchives = @[ m_archive ];

    NSError* error = nil;
    id<MTLRenderPipelineState> pso = [m_device newRenderPipelineStateWithDescriptor:rd
                                                                             error:&error];
    if (!pso) {
        fatal("the present blit pipeline failed to build for pixel format %lu: %s",
              (unsigned long)format,
              error ? [[error localizedDescription] UTF8String] : "(no diagnostic)");
    }

    GraphicsPipeline* p = new GraphicsPipeline();
    p->pso = pso;
    p->color_format = format;
    m_blit_pipelines.push_back(BlitPipeline{ format, p });
    return p;
}

/* ---- submission ----------------------------------------------------------- */

CommandList* MetalDevice::begin_command_list() {
    if (m_recording) fatal("begin_command_list while a command list is still open");
    /* Two frames in flight, the same back pressure the other two backends
     * apply. Metal needs it less than they do (a command queue blocks once
     * its own buffer count is reached, and there is no per-frame pool to
     * recycle), but a renderer that paces the same way on all three is one
     * fewer difference when a frame time is being compared. */
    wait(m_frame_timeline[m_frame_index]);

    id<MTLCommandBuffer> cb = [m_queue commandBuffer];
    if (!cb) fatal("the command queue would not hand out a command buffer");
    m_cmd->reset(cb);
    m_recording = true;
    return m_cmd;
}

uint64_t MetalDevice::submit(CommandList* cmd) {
    if (!m_recording || cmd != m_cmd) fatal("submit of a command list that is not open");
    m_cmd->end_encoder();
    m_recording = false;

    const uint64_t value = ++m_timeline_value;
    id<MTLCommandBuffer> cb = m_cmd->handle();
    [cb encodeSignalEvent:m_timeline value:value];
    [cb commit];

    m_frame_timeline[m_frame_index] = value;
    m_frame_index = (m_frame_index + 1) % kFrames;
    return value;
}

void MetalDevice::wait(uint64_t value) {
    if (value == 0) return;
    if (m_timeline.signaledValue >= value) return;
    /* Ten seconds. Long enough that no frame this renderer draws can reach it
     * and short enough that a hung GPU is reported rather than hanging the
     * process for good. A timeout is a fatal because there is nothing a
     * caller could do with the news. */
    const uint32_t kTimeoutMs = 10000;
    if (![m_timeline waitUntilSignaledValue:value timeoutMS:kTimeoutMs]) {
        fatal("submitted work did not complete within %u ms (waiting for timeline value "
              "%llu, the device has signalled %llu)", kTimeoutMs,
              (unsigned long long)value, (unsigned long long)m_timeline.signaledValue);
    }
}

void MetalDevice::wait_idle() {
    wait(m_timeline_value);
}

/* ---- the layer ------------------------------------------------------------ */

void MetalDevice::create_layer(const DeviceDesc& desc) {
    if (!desc.cocoa_window) {
        rt_log_warn("rhi", "a windowed Metal device was asked for and no NSWindow was "
                           "supplied; running headless");
        return;
    }

    NSWindow* window = (__bridge NSWindow*)desc.cocoa_window;
    NSView* view = [window contentView];
    if (!view) fatal("the NSWindow has no content view to attach a layer to");

    m_layer = [CAMetalLayer layer];
    m_layer.device = m_device;
    /* BGRA8Unorm, which is the only format every macOS display path takes
     * without a conversion. rhi.h's Format::BGRA8Unorm is the same order, and
     * read_texture puts the channels right for a caller that wants RGBA. */
    m_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    /* NO, not the default YES: the screenshot path reads a backbuffer back
     * (Device::read_texture), and a framebufferOnly drawable texture can be
     * an attachment and nothing else. */
    m_layer.framebufferOnly = NO;

    m_backing_scale = (double)[window backingScaleFactor];
    if (m_backing_scale <= 0.0) m_backing_scale = 1.0;

    /* Layer-backed rather than layer-hosting, and mutated inside an explicit
     * transaction: the renderer runs on the GS worker thread (rhi.h,
     * Threading), and Core Animation batches property changes into the
     * transaction of whichever thread makes them. On the main thread the run
     * loop commits that transaction; on any other thread nothing does, so the
     * change would sit unpublished until something else happened to commit.
     * Doing it explicitly is what makes the geometry take effect off the main
     * thread. Whether this is enough on a real Mac is untested; it is the
     * first thing to check if the picture is the wrong size or does not
     * appear. */
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    view.wantsLayer = YES;
    view.layer = m_layer;
    m_layer.contentsScale = m_backing_scale;
    [CATransaction commit];

    uint32_t width = desc.surface_width;
    uint32_t height = desc.surface_height;
    if (width == 0 || height == 0) {
        /* The caller gave no size. The layer's own bounds are in points, so
         * the backing scale is what turns them into the pixels a drawable is
         * measured in. */
        const CGRect bounds = [view bounds];
        width = (uint32_t)(bounds.size.width * m_backing_scale);
        height = (uint32_t)(bounds.size.height * m_backing_scale);
    }
    resize_layer(width, height);
    apply_present_mode();

    rt_log_info("rhi", "Metal layer: %ux%u pixels at backing scale %.2f, %s",
                m_surface_width, m_surface_height, m_backing_scale,
                m_present_mode == PresentMode::Fifo ? "display sync on" : "display sync off");
}

void MetalDevice::resize_layer(uint32_t width, uint32_t height) {
    m_surface_width = width;
    m_surface_height = height;
    if (!m_layer) return;
    /* A minimised or zero-sized window. Not an error: acquire_backbuffer
     * reports there is nothing to present into and the caller skips the
     * frame. The layer's drawable size is left alone, because Metal rejects a
     * zero one. */
    if (width == 0 || height == 0) return;

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    m_layer.drawableSize = CGSizeMake((CGFloat)width, (CGFloat)height);
    [CATransaction commit];
}

void MetalDevice::apply_present_mode() {
    if (!m_layer) return;
    /* Metal has no tearing mode, so rhi.h's three become two settings of one
     * property plus a drawable count. What each one actually does is written
     * out in docs/GS_RENDERER.md; the short form:
     *
     *   fifo       displaySyncEnabled YES. nextDrawable blocks until a
     *              drawable frees up and each present waits for a vertical
     *              blank. This is Vulkan's FIFO.
     *   mailbox    displaySyncEnabled NO with three drawables. The compositor
     *              takes the most recent completed present. Close to Vulkan's
     *              mailbox, and the difference is that Metal still hands the
     *              picture over at a vertical blank.
     *   immediate  the same as mailbox. Metal exposes nothing that tears, so
     *              this is not immediate's Vulkan meaning and the log says so
     *              rather than letting a setting claim an effect it has not
     *              got. */
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    m_layer.displaySyncEnabled = m_present_mode == PresentMode::Fifo ? YES : NO;
    m_layer.maximumDrawableCount = 3;
    [CATransaction commit];

    if (m_present_mode == PresentMode::Immediate) {
        rt_log_info("rhi", "present mode immediate on Metal is mailbox: there is no tearing "
                           "mode in Core Animation, so nothing here can tear");
    }
}

void MetalDevice::set_present_mode(PresentMode mode) {
    if (mode == m_present_mode) return;
    m_present_mode = mode;
    /* No layer rebuild, the same as the D3D12 backend and unlike the Vulkan
     * one, where the mode is baked into the swapchain object. */
    apply_present_mode();
}

void MetalDevice::notify_resize(uint32_t width, uint32_t height) {
    if (width == m_surface_width && height == m_surface_height) return;
    resize_layer(width, height);
}

bool MetalDevice::acquire_backbuffer(Texture** out) {
    if (out) *out = nullptr;
    m_drawable = nil;
    if (!m_layer) return false;
    if (m_surface_width == 0 || m_surface_height == 0) return false;

    /* nextDrawable returns nil when every drawable is still in use and the
     * wait timed out. That is not an error: the caller skips the frame. */
    m_drawable = [m_layer nextDrawable];
    if (!m_drawable) return false;

    if (!m_backbuffer) m_backbuffer = new Texture();
    m_backbuffer->texture = [m_drawable texture];
    m_backbuffer->format = m_layer.pixelFormat;
    m_backbuffer->width = (uint32_t)[m_backbuffer->texture width];
    m_backbuffer->height = (uint32_t)[m_backbuffer->texture height];
    m_backbuffer->usage = TextureUsage::ColorTarget | TextureUsage::CopySrc;
    m_backbuffer->owns_texture = false;
    if (out) *out = m_backbuffer;
    return true;
}

void MetalDevice::present() {
    if (!m_layer || !m_drawable) return;
    /* Metal schedules a present on a command buffer, and rhi.h calls present
     * after submit rather than as part of it, so the present goes on a
     * command buffer of its own with nothing else in it. Command buffers run
     * in the order they were committed on one queue, so this one lands after
     * the submit that drew the frame. The cost is one empty command buffer
     * per frame; the alternative, attaching the present inside submit, would
     * make submit and present mean something different here than in the other
     * two backends. */
    id<MTLCommandBuffer> cb = [m_queue commandBuffer];
    cb.label = @"rhi present";
    [cb presentDrawable:m_drawable];
    [cb commit];
    m_drawable = nil;
    if (m_backbuffer) m_backbuffer->texture = nil;
}

/* ---- readback ------------------------------------------------------------- */

bool MetalDevice::read_texture(Texture* t, std::vector<uint8_t>& out,
                               uint32_t* width, uint32_t* height) {
    if (!t || t->width == 0 || t->height == 0) return false;
    if (t->format != MTLPixelFormatRGBA8Unorm && t->format != MTLPixelFormatBGRA8Unorm) {
        fatal("read_texture of a format this path does not pack (MTLPixelFormat %lu)",
              (unsigned long)t->format);
    }
    const uint64_t bytes = (uint64_t)t->width * t->height * 4;

    BufferDesc bd;
    bd.size = bytes;
    bd.kind = BufferKind::Readback;
    bd.usage = BufferUsage::CopyDst;
    bd.debug_name = "rhi readback";
    Buffer* staging = create_buffer(bd);

    CommandList* cmd = begin_command_list();
    cmd->copy_texture_to_buffer(staging, 0, t);
    wait(submit(cmd));

    out.resize((size_t)bytes);
    std::memcpy(out.data(), staging->mapped, (size_t)bytes);
    /* A drawable comes back as BGRA, and rhi.h's contract is RGBA, so the
     * channels are put right here rather than every caller having to ask what
     * format the layer picked. Same as the Vulkan backend. */
    if (t->format == MTLPixelFormatBGRA8Unorm) {
        for (size_t i = 0; i + 3 < out.size(); i += 4) {
            const uint8_t b = out[i];
            out[i] = out[i + 2];
            out[i + 2] = b;
        }
    }
    if (width) *width = t->width;
    if (height) *height = t->height;
    destroy_buffer(staging);
    return true;
}

/* ---- the binary archive ---------------------------------------------------
 *
 * A cache and not a build product. rhi_metal_shaders.h says what it does and
 * does not save. Every failure here is a log line and a run without it: the
 * rule the Vulkan pipeline cache in gs/gs_parallel_present.cpp follows, for
 * the same reason, which is that a cache that can stop a run is worse than no
 * cache.
 */

void MetalDevice::load_binary_archive() {
    m_archive_path = rt_exe_dir() + "/cache/rhi_metal_pipelines.metallib";

    MTLBinaryArchiveDescriptor* d = [MTLBinaryArchiveDescriptor new];
    std::error_code ec;
    const std::filesystem::path path(m_archive_path);
    const uintmax_t size = std::filesystem::file_size(path, ec);
    const bool have_file = !ec && size > 0 && size <= kMaxArchiveBytes;
    if (have_file) {
        d.url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:m_archive_path.c_str()]];
    }

    NSError* error = nil;
    m_archive = [m_device newBinaryArchiveWithDescriptor:d error:&error];
    if (!m_archive && have_file) {
        /* A file written by another Metal version, or a truncated one. Start
         * from empty and rewrite it at exit, which is what the Vulkan cache
         * does with a UUID mismatch. */
        rt_log_info("rhi", "Metal binary archive: %s rejected (%s); starting from an empty "
                           "archive and rewriting it at exit", m_archive_path.c_str(),
                    error ? [[error localizedDescription] UTF8String] : "no reason given");
        d.url = nil;
        error = nil;
        m_archive = [m_device newBinaryArchiveWithDescriptor:d error:&error];
    }
    if (!m_archive) {
        rt_log_info("rhi", "Metal binary archive: the device would not create one (%s); "
                           "pipelines compile from source every run",
                    error ? [[error localizedDescription] UTF8String] : "no reason given");
        m_archive_path.clear();
        return;
    }
    m_archive_loaded = have_file;
    m_archive.label = @"icorecomp rhi pipelines";
}

void MetalDevice::store_binary_archive() {
    if (!m_archive || m_archive_path.empty()) return;

    const std::filesystem::path path(m_archive_path);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        rt_log_info("rhi", "Metal binary archive: cannot create %s (%s); not stored",
                    path.parent_path().string().c_str(), ec.message().c_str());
        return;
    }

    /* Written beside the real file and renamed, so a run that dies mid-write
     * cannot leave a half archive that the next run would then reject. */
    const std::filesystem::path tmp = path.string() + ".tmp";
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:tmp.string().c_str()]];
    NSError* error = nil;
    if (![m_archive serializeToURL:url error:&error]) {
        rt_log_info("rhi", "Metal binary archive: serializeToURL failed (%s); not stored",
                    error ? [[error localizedDescription] UTF8String] : "no reason given");
        return;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        rt_log_info("rhi", "Metal binary archive: rename to %s failed (%s); not stored",
                    path.string().c_str(), ec.message().c_str());
        std::filesystem::remove(tmp, ec);
        return;
    }
    rt_log_info("rhi", "Metal binary archive: written to %s", path.string().c_str());
}

Device* create_metal_device(const DeviceDesc& desc) {
    return new MetalDevice(desc);
}

} // namespace rhi
