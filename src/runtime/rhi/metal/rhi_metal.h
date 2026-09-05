/* rhi/metal/rhi_metal.h: internal types of the Metal backend.
 *
 * Ours (MIT). Objective-C++ only: every .mm in this directory includes it and
 * nothing else does. The renderer sees rhi.h alone. rhi_metal_bindings.h
 * holds the argument index convention and says why it is what it is.
 *
 * Objective-C++ and not metal-cpp. The window path needs Core Animation and
 * AppKit whatever the Metal binding is: SDL hands back an NSWindow, the layer
 * is a CAMetalLayer attached to its content view, and the backing scale comes
 * off NSWindow. metal-cpp would cover the MTL* half and leave the CA and NS
 * half in Objective-C anyway, so the whole backend is written in the one
 * language rather than split across two.
 *
 * ARC. The .mm files are compiled with -fobjc-arc (see the APPLE block in
 * CMakeLists.txt). Every id member below is a strong reference, which is what
 * keeps a texture alive for the life of its rhi::Texture without a retain and
 * release of our own.
 *
 * Device requirements. MTLGPUFamilyApple7 (Apple silicon, M1 and later) or
 * MTLGPUFamilyMac2, and the family is logged. Apple7 is the floor because
 * this port targets arm64 macOS 14 and nothing older; Mac2 is accepted as
 * well so that an Intel Mac with a discrete GPU reports a device rather than
 * a refusal, which is a better failure report than silence. A device below
 * both is a loud fatal naming the device.
 *
 * One command queue. Every dispatch, draw and copy the renderer issues is
 * already ordered against the one before it by rhi.h's single recording
 * thread and single command list, so a second queue would add cross-queue
 * events for work with no parallelism to find.
 *
 * Storage modes. arm64 macOS only (docs/MACOS.md: there is no x86_64 build),
 * so host visible memory is MTLStorageModeShared throughout and there is no
 * didModifyRange call anywhere: Shared is coherent on Apple silicon. An
 * Intel Mac reached through MTLGPUFamilyMac2 would need MTLStorageModeManaged
 * for the same buffers; that path is not written, and create_buffer says so
 * out loud rather than writing to memory the GPU will not see.
 */
#ifndef ICORECOMP_RHI_METAL_H
#define ICORECOMP_RHI_METAL_H

#include "../rhi.h"
#include "rhi_metal_bindings.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstring>
#include <string>
#include <vector>

namespace rhi {

/* ---- resources ------------------------------------------------------------ */

struct Buffer {
    id<MTLBuffer> buffer = nil;
    /* Shared-storage buffers are mapped for their whole life; contents is
     * stable, so this is resolved once at creation. Null for Private. */
    void* mapped = nullptr;
    uint64_t size = 0;
    BufferKind kind = BufferKind::DeviceLocal;
    BufferUsage usage = BufferUsage::None;
};

struct Texture {
    id<MTLTexture> texture = nil;
    MTLPixelFormat format = MTLPixelFormatInvalid;
    uint32_t width = 0;
    uint32_t height = 0;
    TextureUsage usage = TextureUsage::None;
    /* A drawable's texture belongs to the drawable, not to this object, and
     * is replaced every frame. The flag is also what tells the command list
     * that a submit touched the swapchain, the same as the Vulkan backend's
     * owns_image. */
    bool owns_texture = true;
};

struct ComputePipeline {
    id<MTLComputePipelineState> pso = nil;
    /* Metal dispatches take the threadgroup size at the call, where Vulkan
     * bakes it into the shader. The size comes from the shader's own
     * layout(local_size_*) line, carried through the generated MSL index
     * (rhi_shaders_msl.h) so that nothing here has to guess it. */
    MTLSize threads_per_group = MTLSizeMake(1, 1, 1);
};

struct GraphicsPipeline {
    id<MTLRenderPipelineState> pso = nil;
    MTLPixelFormat color_format = MTLPixelFormatInvalid;
};

class MetalDevice;

/* ---- command list ---------------------------------------------------------
 *
 * One MTLCommandBuffer per rhi.h submit. Metal has one encoder open at a
 * time and no way to interleave two kinds, so the list keeps the kind it has
 * open and closes it when an operation needs a different one. That is also
 * where most of the synchronisation comes from: Metal inserts the
 * dependencies between encoders itself for resources with hazard tracking on,
 * which is the default and which every resource here uses.
 *
 * Bindings stand until changed and are written into the encoder at each
 * dispatch and draw, exactly as the other two backends resolve them into a
 * descriptor set or table. Nothing is cached, so a binding changed between
 * two dispatches cannot leak into the first.
 */
class MetalCommandList final : public CommandList {
public:
    explicit MetalCommandList(MetalDevice* dev) : m_dev(dev) {}

    void bind_uniform_buffer(uint32_t slot, Buffer* b, uint64_t offset, uint64_t range) override;
    void bind_storage_buffer(uint32_t slot, Buffer* b, uint64_t offset, uint64_t range) override;
    void bind_texture(uint32_t slot, Texture* t) override;
    void bind_storage_image(uint32_t slot, Texture* t) override;
    void push_constants(const void* data, size_t bytes) override;

    void bind_compute_pipeline(ComputePipeline* p) override;
    void dispatch(uint32_t gx, uint32_t gy, uint32_t gz) override;
    void dispatch_indirect(Buffer* args, uint64_t offset) override;

    void begin_render_pass(Texture* color, bool clear, float r, float g, float b, float a) override;
    void end_render_pass() override;
    void bind_graphics_pipeline(GraphicsPipeline* p) override;
    void bind_vertex_buffer(Buffer* b, uint64_t offset) override;
    void bind_index_buffer(Buffer* b, uint64_t offset) override;
    void set_viewport(float x, float y, float w, float h) override;
    void set_scissor(int32_t x, int32_t y, uint32_t w, uint32_t h) override;
    void draw_indexed(uint32_t index_count, uint32_t first_index, int32_t vertex_offset) override;

    void copy_buffer(Buffer* dst, uint64_t dst_offset,
                     Buffer* src, uint64_t src_offset, uint64_t bytes) override;
    void copy_buffer_to_texture(Texture* dst, Buffer* src, uint64_t src_offset) override;
    void copy_texture_to_buffer(Buffer* dst, uint64_t dst_offset, Texture* src) override;
    void blit_texture(Texture* dst, int32_t dx, int32_t dy, uint32_t dw, uint32_t dh,
                      Texture* src, bool linear_filter) override;

    void buffer_barrier(Buffer* b, Stage from, Access fa, Stage to, Access ta) override;
    void texture_barrier(Texture* t, Stage from, Access fa, Stage to, Access ta) override;

    /* Called by the device around recording. */
    void reset(id<MTLCommandBuffer> cb);
    id<MTLCommandBuffer> handle() const { return m_cb; }
    bool touched_swapchain() const { return m_touched_swapchain; }
    /* Closes whatever encoder is open. The device calls this before it ends
     * the command buffer. */
    void end_encoder();

private:
    struct BufferBinding {
        Buffer* buffer = nullptr;
        uint64_t offset = 0;
        uint64_t range = 0;
    };

    id<MTLComputeCommandEncoder> compute_encoder();
    id<MTLBlitCommandEncoder> blit_encoder();
    /* Writes the standing bindings and the immutable samplers into whichever
     * encoder is open. */
    void apply_compute_bindings(id<MTLComputeCommandEncoder> enc);
    void apply_render_bindings(id<MTLRenderCommandEncoder> enc);

    enum class Encoder { None, Compute, Blit, Render };

    MetalDevice* m_dev = nullptr;
    id<MTLCommandBuffer> m_cb = nil;
    id<MTLComputeCommandEncoder> m_compute = nil;
    id<MTLBlitCommandEncoder> m_blit = nil;
    id<MTLRenderCommandEncoder> m_render = nil;
    Encoder m_encoder = Encoder::None;

    BufferBinding m_ubo[kMaxUniformBuffers];
    BufferBinding m_ssbo[kMaxStorageBuffers];
    Texture* m_tex[kMaxSampledTextures] = {};
    Texture* m_image[kMaxStorageImages] = {};
    uint8_t m_push[kPushConstantBytes] = {};

    ComputePipeline* m_compute_pipeline = nullptr;
    Buffer* m_index_buffer = nullptr;
    uint64_t m_index_offset = 0;
    /* The attachment a render pass is drawing into, for the scissor clamp
     * that Metal needs and Vulkan does not. */
    uint32_t m_render_width = 0;
    uint32_t m_render_height = 0;
    bool m_touched_swapchain = false;
};

/* ---- device --------------------------------------------------------------- */

class MetalDevice final : public Device {
public:
    explicit MetalDevice(const DeviceDesc& desc);
    ~MetalDevice() override;

    const char* device_name() const override { return m_device_name.c_str(); }
    const char* api_version() const override { return m_api_version.c_str(); }
    const Limits& limits() const override { return m_limits; }

    Buffer* create_buffer(const BufferDesc& desc) override;
    void destroy_buffer(Buffer* b) override;
    void* map(Buffer* b) override;
    uint64_t buffer_size(Buffer* b) const override { return b ? b->size : 0; }

    Texture* create_texture(const TextureDesc& desc) override;
    void destroy_texture(Texture* t) override;
    uint32_t texture_width(Texture* t) const override { return t ? t->width : 0; }
    uint32_t texture_height(Texture* t) const override { return t ? t->height : 0; }
    Format texture_format(Texture* t) const override;

    ComputePipeline* create_compute_pipeline(const uint32_t* spirv, size_t words,
                                             const char* name) override;
    void destroy_compute_pipeline(ComputePipeline* p) override;
    GraphicsPipeline* create_graphics_pipeline(const GraphicsPipelineDesc& desc) override;
    void destroy_graphics_pipeline(GraphicsPipeline* p) override;

    CommandList* begin_command_list() override;
    uint64_t submit(CommandList* cmd) override;
    void wait(uint64_t timeline_value) override;
    void wait_idle() override;

    bool has_swapchain() const override { return m_layer != nil; }
    void set_present_mode(PresentMode mode) override;
    void notify_resize(uint32_t width, uint32_t height) override;
    bool acquire_backbuffer(Texture** out) override;
    void present() override;
    uint32_t surface_width() const override { return m_surface_width; }
    uint32_t surface_height() const override { return m_surface_height; }

    bool read_texture(Texture* t, std::vector<uint8_t>& out,
                      uint32_t* width, uint32_t* height) override;

    /* ---- used by MetalCommandList and the shader loader ---- */
    id<MTLDevice> mtl() const { return m_device; }
    id<MTLSamplerState> sampler(uint32_t i) const { return m_samplers[i]; }
    Buffer* dummy_buffer() const { return m_dummy_buffer; }
    Texture* dummy_texture() const { return m_dummy_texture; }
    Texture* dummy_storage_image() const { return m_dummy_storage_image; }
    /* The pipeline the present blit draws with, one per destination format,
     * built on first use. blit_texture says why a blit is a draw here. */
    GraphicsPipeline* blit_pipeline(MTLPixelFormat format);
    /* The MSL version this device's shaders were generated for and are
     * compiled against, as SPIRV-Cross spells it (20300 for MSL 2.3).
     * rhi_metal_shaders.mm pins MTLCompileOptions to it. */
    uint32_t msl_version() const { return m_msl_version; }
    void note_shader_path(const char* how);
    NSMutableDictionary<NSString*, id<MTLLibrary>>* library_cache() const { return m_libraries; }
    id<MTLBinaryArchive> binary_archive() const { return m_archive; }

    [[noreturn]] void fatal(const char* fmt, ...) const;

private:
    void pick_device(const DeviceDesc& desc);
    void create_samplers();
    void create_dummies();
    void create_layer(const DeviceDesc& desc);
    void resize_layer(uint32_t width, uint32_t height);
    void apply_present_mode();
    void load_binary_archive();
    void store_binary_archive();

    static constexpr uint32_t kFrames = 2;

    id<MTLDevice> m_device = nil;
    id<MTLCommandQueue> m_queue = nil;
    id<MTLSharedEvent> m_timeline = nil;
    id<MTLSamplerState> m_samplers[kSamplerCount] = {};
    /* One MTLLibrary per generated MSL source, built once and kept, so a
     * second pipeline over the same shader does not recompile it. Keyed by
     * the shader's generated name ("raster.comp"). */
    NSMutableDictionary<NSString*, id<MTLLibrary>>* m_libraries = nil;
    id<MTLBinaryArchive> m_archive = nil;
    std::string m_archive_path;
    bool m_archive_loaded = false;

    /* Filled in where the device is created, from what the device reports.
     * See rhi::Limits. */
    Limits m_limits;
    std::string m_device_name = "(no device)";
    std::string m_api_version;
    uint32_t m_msl_version = 0;
    bool m_validation = false;
    bool m_logged_shader_path = false;

    uint64_t m_timeline_value = 0;
    /* The submit each frame slot last produced, so begin_command_list can
     * wait for the slot to be free the way the other backends do. */
    uint64_t m_frame_timeline[kFrames] = {};
    uint32_t m_frame_index = 0;

    MetalCommandList* m_cmd = nullptr;
    bool m_recording = false;

    Buffer* m_dummy_buffer = nullptr;
    Texture* m_dummy_texture = nullptr;
    Texture* m_dummy_storage_image = nullptr;

    CAMetalLayer* m_layer = nil;
    id<CAMetalDrawable> m_drawable = nil;
    Texture* m_backbuffer = nullptr;
    uint32_t m_surface_width = 0;
    uint32_t m_surface_height = 0;
    double m_backing_scale = 1.0;
    PresentMode m_present_mode = PresentMode::Fifo;

    struct BlitPipeline {
        MTLPixelFormat format = MTLPixelFormatInvalid;
        GraphicsPipeline* pipeline = nullptr;
    };
    std::vector<BlitPipeline> m_blit_pipelines;

    friend class MetalCommandList;
};

/* Format translation, shared by both translation units. */
MTLPixelFormat to_mtl_format(Format f);

} // namespace rhi

#endif /* ICORECOMP_RHI_METAL_H */
