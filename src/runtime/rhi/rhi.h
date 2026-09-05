/* rhi/rhi.h: the render hardware interface the native GS renderer sits on.
 *
 * Ours (MIT). Deliberately a floor and not a framework: it exposes what the
 * renderer's milestones (a) to (g) are known to need and nothing more, so a
 * second and a third backend (D3D12, then Metal) have a small surface to
 * implement rather than a Vulkan shape to emulate.
 *
 * What is deliberately absent, because relying on any of it would make a
 * D3D12 or Metal backend a rewrite rather than a port: buffer device address,
 * descriptor indexing beyond the fixed arrays below, 8- and 16-bit storage,
 * subgroup size control and subgroup operations. If a milestone needs one of
 * them, it is added here first, with the fallback for the backends that lack
 * it written at the same time.
 *
 * Binding model. One descriptor set layout, the same for every pipeline:
 *
 *   set 0 binding 0   uniform buffer   [4]
 *   set 0 binding 1   storage buffer   [16]
 *   set 0 binding 2   sampled texture  [8]
 *   set 0 binding 3   sampler          [4], immutable
 *   set 0 binding 4   storage image    [4]
 *
 * plus 128 bytes of push constants visible to every stage. The four samplers
 * are fixed for the life of the device: 0 nearest/clamp, 1 linear/clamp,
 * 2 nearest/repeat, 3 linear/repeat. A shader indexes them by constant, so
 * no sampler is ever created or bound per draw.
 *
 * Threading. One device, one recording thread. The renderer runs on the GS
 * command ring's worker thread (see gs/gs_backend.h) and nothing else touches
 * the device, so nothing here is internally synchronised.
 *
 * Errors. Per CLAUDE.md: a lost device, a missing feature and a pipeline that
 * fails to build are loud rt_fatal calls naming the device and the thing that
 * was missing. Nothing here returns a null handle for a condition the caller
 * cannot do anything about.
 */
#ifndef ICORECOMP_RHI_H
#define ICORECOMP_RHI_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rhi {

/* ---- enums ---------------------------------------------------------------- */

enum class Format : uint32_t {
    Unknown = 0,
    RGBA8Unorm,   /* the renderer's own images */
    BGRA8Unorm,   /* what a swapchain usually hands back */
    R32Uint,      /* single channel integer images */
};

enum class BufferKind : uint32_t {
    /* GPU-only storage. Written by copies from an Upload buffer. */
    DeviceLocal,
    /* Host visible and host coherent, written by the CPU, read by copies or
     * read directly by the GPU. */
    Upload,
    /* Host visible and cached, written by the GPU, read by the CPU after the
     * submit that filled it has been waited on. */
    Readback,
};

enum class BufferUsage : uint32_t {
    None       = 0,
    Storage    = 1 << 0,
    Uniform    = 1 << 1,
    Vertex     = 1 << 2,
    Index      = 1 << 3,
    Indirect   = 1 << 4,
    CopySrc    = 1 << 5,
    CopyDst    = 1 << 6,
};

inline BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return (BufferUsage)((uint32_t)a | (uint32_t)b);
}
inline bool has(BufferUsage set, BufferUsage bit) {
    return ((uint32_t)set & (uint32_t)bit) != 0;
}

enum class TextureUsage : uint32_t {
    None         = 0,
    Sampled      = 1 << 0,
    Storage      = 1 << 1,
    ColorTarget  = 1 << 2,
    CopySrc      = 1 << 3,
    CopyDst      = 1 << 4,
};

inline TextureUsage operator|(TextureUsage a, TextureUsage b) {
    return (TextureUsage)((uint32_t)a | (uint32_t)b);
}
inline bool has(TextureUsage set, TextureUsage bit) {
    return ((uint32_t)set & (uint32_t)bit) != 0;
}

/* The four immutable samplers, by their binding index. */
enum : uint32_t {
    kSamplerNearestClamp  = 0,
    kSamplerLinearClamp   = 1,
    kSamplerNearestRepeat = 2,
    kSamplerLinearRepeat  = 3,
    kSamplerCount         = 4,
};

/* Fixed binding array sizes; a slot outside them is a programming error and
 * is fatal, not clamped. */
enum : uint32_t {
    kMaxUniformBuffers = 4,
    kMaxStorageBuffers = 16,
    kMaxSampledTextures = 8,
    kMaxStorageImages = 4,
    kPushConstantBytes = 128,
};

enum class PresentMode : uint32_t { Fifo, Mailbox, Immediate };

/* Which backend a device is created on. Auto is what a caller that has no
 * opinion asks for: the first backend built into this binary that can create
 * a device, in the order the create_device dispatcher documents. */
enum class Backend : uint32_t { Auto, Vulkan, D3D12, Metal };

/* Where a resource is being used, for the barriers the caller asks for. Kept
 * coarse on purpose: the renderer's dependencies are whole-pass sized, and a
 * finer model would be a Vulkan model that the other backends would have to
 * imitate. */
enum class Stage : uint32_t {
    Compute,
    Graphics,
    Copy,
    Host,
    Present,
};

enum class Access : uint32_t {
    Read,
    Write,
    ReadWrite,
};

/* ---- descriptors ---------------------------------------------------------- */

struct BufferDesc {
    uint64_t size = 0;
    BufferKind kind = BufferKind::DeviceLocal;
    BufferUsage usage = BufferUsage::None;
    const char* debug_name = nullptr;
};

struct TextureDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    Format format = Format::RGBA8Unorm;
    TextureUsage usage = TextureUsage::None;
    const char* debug_name = nullptr;
};

/* The one graphics pipeline kind: a single colour attachment, triangle list,
 * dynamic viewport and scissor, optional alpha blend, and the fixed vertex
 * layout below.
 *
 * The vertex layout is the overlay's (gs/gs_parallel_api.h
 * RtPgsOverlayVertex): float2 position in surface pixels, float2 texture
 * coordinate, and one R8G8B8A8_UNORM colour. The present blit uses the same
 * layout with a white colour, so one pipeline kind covers both users. */
struct GraphicsPipelineDesc {
    const uint32_t* vertex_spirv = nullptr;
    size_t vertex_spirv_words = 0;
    const uint32_t* fragment_spirv = nullptr;
    size_t fragment_spirv_words = 0;
    Format color_format = Format::RGBA8Unorm;
    bool blend = false;
    /* Blend the source as premultiplied alpha (ONE, ONE_MINUS_SRC_ALPHA)
     * rather than straight (SRC_ALPHA, ONE_MINUS_SRC_ALPHA). The UI emits
     * premultiplied geometry; see RT_PGS_OVERLAY_PREMULTIPLIED. */
    bool premultiplied = false;
    const char* debug_name = nullptr;
};

/* What the device this run got can actually do, read once when it is created
 * and constant for its life. A caller that derives a dispatch grid or a set
 * of per-frame buffers from a guest register has to check it against the
 * device rather than against a number written into its own source, because
 * the number in the source is a floor from a specification and the device is
 * the thing that will be asked. */
struct Limits {
    /* The largest compute dispatch grid, in workgroups, on each axis.
     * Vulkan reports it in VkPhysicalDeviceLimits::maxComputeWorkGroupCount
     * and guarantees at least 65535 on every axis. D3D12 states 65535 on
     * every axis for every feature level this port targets and offers no
     * query for it, so that backend fills these three in with 65535. Going
     * past the limit is undefined rather than an error the driver reports.
     */
    uint32_t max_workgroup_count[3] = { 65535u, 65535u, 65535u };
    /* How many frames the backend keeps in flight, which is how many sets of
     * per-frame upload buffers a caller needs for one to be free by the time
     * it comes round again. */
    uint32_t frames_in_flight = 2;
};

struct DeviceDesc {
    /* Which backend to create on. See rhi::Backend and create_device. */
    Backend backend = Backend::Auto;
    /* No window: the device is created for offscreen work only, which is what
     * the replay tool and CI use. */
    bool headless = true;
    /* Turn on the validation layer and the debug messenger. Set from a debug
     * flag by the caller, never by default. */
    bool validation = false;
    /* Create the device on the software rasteriser rather than on a GPU: WARP
     * on D3D12, and a software Vulkan implementation where the loader offers
     * one. A debug and CI flag only. It is never a fallback: a machine with a
     * GPU driver that failed must say so, because a software rasteriser
     * running at a frame a second looks like a hang rather than a missing
     * driver. */
    bool prefer_software = false;

    /* The native window, read out of SDL by the caller with
     * SDL_GetWindowProperties so that nothing in rhi/ links SDL:
     *
     *   Windows   SDL_PROP_WINDOW_WIN32_HWND_POINTER            -> win32_hwnd
     *             SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER        -> win32_hinstance
     *   X11       SDL_PROP_WINDOW_X11_DISPLAY_POINTER           -> x11_display
     *             SDL_PROP_WINDOW_X11_WINDOW_NUMBER             -> x11_window
     *   Wayland   SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER       -> wl_display
     *             SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER       -> wl_surface
     *   macOS     SDL_PROP_WINDOW_COCOA_WINDOW_POINTER          -> cocoa_window
     *
     * Exactly one platform's pair is filled in; the rest stay null. macOS
     * takes a single pointer rather than a pair because the Metal backend
     * attaches a CAMetalLayer to the NSWindow's own content view, so the
     * window is all it needs. */
    void* win32_hwnd = nullptr;
    void* win32_hinstance = nullptr;
    void* x11_display = nullptr;
    uint64_t x11_window = 0;
    void* wl_display = nullptr;
    void* wl_surface = nullptr;
    void* cocoa_window = nullptr;

    uint32_t surface_width = 0;
    uint32_t surface_height = 0;
    PresentMode present_mode = PresentMode::Fifo;
};

/* ---- opaque resources ------------------------------------------------------
 *
 * Backend-defined. The renderer only ever holds pointers to them. */
struct Buffer;
struct Texture;
struct ComputePipeline;
struct GraphicsPipeline;

class CommandList {
public:
    virtual ~CommandList() = default;

    /* Bindings persist until changed, and are resolved into a descriptor set
     * at each dispatch or draw. A slot beyond the fixed array size is fatal. */
    virtual void bind_uniform_buffer(uint32_t slot, Buffer* b, uint64_t offset, uint64_t range) = 0;
    virtual void bind_storage_buffer(uint32_t slot, Buffer* b, uint64_t offset, uint64_t range) = 0;
    virtual void bind_texture(uint32_t slot, Texture* t) = 0;
    virtual void bind_storage_image(uint32_t slot, Texture* t) = 0;
    virtual void push_constants(const void* data, size_t bytes) = 0;

    virtual void bind_compute_pipeline(ComputePipeline* p) = 0;
    virtual void dispatch(uint32_t groups_x, uint32_t groups_y, uint32_t groups_z) = 0;
    virtual void dispatch_indirect(Buffer* args, uint64_t offset) = 0;

    /* One colour attachment, no depth. The pass ends before any copy or
     * dispatch may touch the attachment. */
    virtual void begin_render_pass(Texture* color, bool clear,
                                   float r, float g, float b, float a) = 0;
    virtual void end_render_pass() = 0;
    virtual void bind_graphics_pipeline(GraphicsPipeline* p) = 0;
    virtual void bind_vertex_buffer(Buffer* b, uint64_t offset) = 0;
    virtual void bind_index_buffer(Buffer* b, uint64_t offset) = 0;
    virtual void set_viewport(float x, float y, float w, float h) = 0;
    virtual void set_scissor(int32_t x, int32_t y, uint32_t w, uint32_t h) = 0;
    virtual void draw_indexed(uint32_t index_count, uint32_t first_index, int32_t vertex_offset) = 0;

    virtual void copy_buffer(Buffer* dst, uint64_t dst_offset,
                             Buffer* src, uint64_t src_offset, uint64_t bytes) = 0;
    virtual void copy_buffer_to_texture(Texture* dst, Buffer* src, uint64_t src_offset) = 0;
    virtual void copy_texture_to_buffer(Buffer* dst, uint64_t dst_offset, Texture* src) = 0;
    /* Whole-image blit with either filter, used by the present path. */
    virtual void blit_texture(Texture* dst, int32_t dx, int32_t dy, uint32_t dw, uint32_t dh,
                              Texture* src, bool linear_filter) = 0;

    virtual void buffer_barrier(Buffer* b, Stage from, Access from_access,
                                Stage to, Access to_access) = 0;
    virtual void texture_barrier(Texture* t, Stage from, Access from_access,
                                 Stage to, Access to_access) = 0;
};

class Device {
public:
    virtual ~Device() = default;

    /* For logs and for the fatal messages. */
    virtual const char* device_name() const = 0;
    virtual const char* api_version() const = 0;
    /* What this device can be asked for. Filled in when the device is
     * created; see rhi::Limits. */
    virtual const Limits& limits() const = 0;

    virtual Buffer* create_buffer(const BufferDesc& desc) = 0;
    virtual void destroy_buffer(Buffer* b) = 0;
    /* Upload and Readback buffers only; a DeviceLocal buffer is fatal. The
     * mapping lives for the life of the buffer, so this is cheap to call. */
    virtual void* map(Buffer* b) = 0;
    virtual uint64_t buffer_size(Buffer* b) const = 0;

    virtual Texture* create_texture(const TextureDesc& desc) = 0;
    virtual void destroy_texture(Texture* t) = 0;
    virtual uint32_t texture_width(Texture* t) const = 0;
    virtual uint32_t texture_height(Texture* t) const = 0;
    /* The format a texture actually has. A backbuffer's is chosen by the
     * swapchain, and a graphics pipeline has to be built for the format of
     * the attachment it draws into, so callers need to be able to ask. */
    virtual Format texture_format(Texture* t) const = 0;

    virtual ComputePipeline* create_compute_pipeline(const uint32_t* spirv, size_t words,
                                                     const char* debug_name) = 0;
    virtual void destroy_compute_pipeline(ComputePipeline* p) = 0;
    virtual GraphicsPipeline* create_graphics_pipeline(const GraphicsPipelineDesc& desc) = 0;
    virtual void destroy_graphics_pipeline(GraphicsPipeline* p) = 0;

    /* One command list at a time. submit() returns the timeline value the
     * work will have reached when it completes. */
    virtual CommandList* begin_command_list() = 0;
    virtual uint64_t submit(CommandList* cmd) = 0;
    virtual void wait(uint64_t timeline_value) = 0;
    virtual void wait_idle() = 0;

    /* ---- presentation. All no-ops on a headless device. ---- */
    virtual bool has_swapchain() const = 0;
    virtual void set_present_mode(PresentMode mode) = 0;
    virtual void notify_resize(uint32_t width, uint32_t height) = 0;
    /* Acquires the next backbuffer. False when there is nothing to present
     * into this frame (a minimised window, or a swapchain being rebuilt),
     * which is not an error. */
    virtual bool acquire_backbuffer(Texture** out) = 0;
    virtual void present() = 0;
    virtual uint32_t surface_width() const = 0;
    virtual uint32_t surface_height() const = 0;

    /* Synchronous readback of an RGBA8 texture into tightly packed bytes.
     * Submits, waits and copies, so callers keep it off per-field paths. */
    virtual bool read_texture(Texture* t, std::vector<uint8_t>& out,
                              uint32_t* width, uint32_t* height) = 0;
};

/* Creates the Vulkan device. Never returns null: every failure this can
 * reach is fatal and says which device and which requirement. */
Device* create_vulkan_device(const DeviceDesc& desc);

#if defined(ICORECOMP_RHI_D3D12)
/* The same for D3D12, built only where CMake found the Windows headers.
 * Feature level 12_0 is the floor and src/runtime/rhi/d3d12/rhi_d3d12.h says
 * why. There is no dispatcher here on purpose: which backend to create is the
 * caller's decision from desc.backend, and putting the choice behind a
 * function in rhi.h would give it a default that hides a backend that failed
 * to start behind one that did. */
Device* create_d3d12_device(const DeviceDesc& desc);
#endif

#if defined(ICORECOMP_RHI_METAL)
/* The same for Metal, built only on Apple platforms. MTLGPUFamilyApple7 or
 * MTLGPUFamilyMac2 is the floor and src/runtime/rhi/metal/rhi_metal.h says
 * why. Implemented in Objective-C++; the declaration is plain C++ so nothing
 * outside rhi/metal has to be. There is no dispatcher here, for the reason
 * given above D3D12. */
Device* create_metal_device(const DeviceDesc& desc);
#endif

} // namespace rhi

#endif /* ICORECOMP_RHI_H */
