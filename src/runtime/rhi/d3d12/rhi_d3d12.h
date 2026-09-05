/* rhi/d3d12/rhi_d3d12.h: internal types of the D3D12 backend.
 *
 * Ours (MIT). Only the three rhi_d3d12_*.cpp files include this; the renderer
 * sees rhi.h alone. rhi_d3d12_bindings.h holds the register convention and
 * says why it is what it is.
 *
 * Feature level 12_0 is the floor. That is what guarantees resource binding
 * tier 2, and this backend needs twenty UAVs in one table (sixteen storage
 * buffers plus four storage images) where tier 1 on a feature level 11.0
 * device guarantees only eight. A device below 12_0 is skipped during
 * selection with a log line naming it; no usable adapter at all is fatal.
 *
 * What is deliberately not required, so that the requirement list stays the
 * same as the Vulkan backend's: wave (subgroup) operations, 64-bit shader
 * atomics, typed UAV loads of anything but the guaranteed formats, and
 * anything from the Agility SDK. The one optional feature that is checked and
 * only logged is TypedUAVLoadAdditionalFormats: the storage images are
 * written and never read, and a typed UAV store to R8G8B8A8_UNORM is
 * guaranteed at every feature level this backend accepts.
 *
 * One direct queue, no separate compute or copy queue. Every dispatch, draw
 * and copy the renderer issues is already ordered against the one before it
 * by rhi.h's single recording thread and single command list, so a second
 * queue would add ownership transfers and a second timeline for work that has
 * no parallelism to find.
 *
 * Memory is one committed resource per buffer and per texture, matching the
 * Vulkan backend's one allocation per resource and for the same reason: the
 * renderer holds a handful of them.
 */
#ifndef ICORECOMP_RHI_D3D12_H
#define ICORECOMP_RHI_D3D12_H

#include "../rhi.h"
#include "rhi_d3d12_bindings.h"
#include "rhi_d3d12_loader.h"
#include "rhi_d3d12_shaders.h"

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace rhi {

/* ---- a minimal COM pointer ------------------------------------------------
 *
 * Not Microsoft::WRL::ComPtr: wrl/client.h is a Windows SDK header and the
 * mingw cross build does not have a usable one. Release on destruction and on
 * reassignment is all this backend asks of it. */
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept : m_p(o.m_p) { o.m_p = nullptr; }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) { reset(); m_p = o.m_p; o.m_p = nullptr; }
        return *this;
    }
    ~ComPtr() { reset(); }

    T* get() const { return m_p; }
    T* operator->() const { return m_p; }
    explicit operator bool() const { return m_p != nullptr; }
    /* For the out parameter of a Create* call: releases what was held first,
     * so a second call on the same holder cannot leak. */
    T** put() { reset(); return &m_p; }
    void** put_void() { reset(); return reinterpret_cast<void**>(&m_p); }
    void reset() { if (m_p) { m_p->Release(); m_p = nullptr; } }
    T* detach() { T* p = m_p; m_p = nullptr; return p; }

private:
    T* m_p = nullptr;
};

/* ---- resources ------------------------------------------------------------ */

struct Buffer {
    ID3D12Resource* resource = nullptr;
    /* The mapped upload resource behind a host-visible buffer. For an Upload
     * or Readback buffer this is `resource` itself; for an Upload buffer that
     * also has to be readable by a shader it is a second resource, because a
     * D3D12 upload heap cannot carry ALLOW_UNORDERED_ACCESS and the storage
     * buffers are UAVs (rhi_d3d12_bindings.h says why they are UAVs). In that
     * case `resource` is a default-heap shadow and bind_storage_buffer copies
     * the bound range into it. */
    ID3D12Resource* staging = nullptr;
    void* mapped = nullptr;
    uint64_t size = 0;
    BufferKind kind = BufferKind::DeviceLocal;
    BufferUsage usage = BufferUsage::None;
    /* Default-heap resources are tracked; upload and readback resources are
     * fixed at GENERIC_READ and COPY_DEST for their whole life, which D3D12
     * requires, so their state never moves. */
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    bool tracked = false;
    bool shadowed = false;
};

struct Texture {
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    uint32_t width = 0;
    uint32_t height = 0;
    TextureUsage usage = TextureUsage::None;
    /* Index into the device's render target view heap, or UINT32_MAX when the
     * texture is not a colour target. */
    uint32_t rtv = UINT32_MAX;
    /* Whether destroy_texture releases the resource. True for a backbuffer
     * too: IDXGISwapChain::GetBuffer hands out a reference of its own. */
    bool owns_resource = true;
    /* A backbuffer. Distinct from owns_resource, which on this backend is
     * true for a backbuffer as well; the two were one flag, so nothing ever
     * set touched_swapchain, the backbuffer timeline was never recorded and
     * acquire_backbuffer never waited for the submit that last drew into the
     * image it was handing out. */
    bool swapchain = false;
};

struct ComputePipeline {
    ID3D12PipelineState* pso = nullptr;
};

struct GraphicsPipeline {
    ID3D12PipelineState* pso = nullptr;
};

class D3D12Device;

/* ---- command list ---------------------------------------------------------
 *
 * The same shape as the Vulkan backend's: bindings stand until changed and a
 * fresh descriptor table is written for every dispatch and draw, out of a
 * per-frame ring in the one shader-visible heap. Nothing is cached, so a
 * binding changed between two dispatches cannot leak into the first.
 *
 * Resource states are moved inside the operations that need them, from the
 * state each resource is tracked as being in, for the reason the Vulkan
 * backend gives for layouts: making every caller barrier by hand is the most
 * common way to get this subtly wrong. buffer_barrier and texture_barrier
 * stay public for the dependencies only the caller knows about, and a barrier
 * between two shader accesses of the same resource becomes a UAV barrier
 * rather than a transition, which is the D3D12 spelling of the same edge.
 */
class D3D12CommandList final : public CommandList {
public:
    explicit D3D12CommandList(D3D12Device* dev) : m_dev(dev) {}

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
    void reset(ID3D12GraphicsCommandList* list);
    ID3D12GraphicsCommandList* handle() const { return m_list; }
    bool touched_swapchain() const { return m_touched_swapchain; }

    /* Used by the device's own recording (the dummy resource setup). */
    void transition(Texture* t, D3D12_RESOURCE_STATES to);
    void transition(Buffer* b, D3D12_RESOURCE_STATES to);

private:
    struct BufferBinding {
        Buffer* buffer = nullptr;
        uint64_t offset = 0;
        uint64_t range = 0;
    };

    /* Writes the 32 descriptors of one set into the frame's ring and returns
     * the GPU handle of its first descriptor. */
    D3D12_GPU_DESCRIPTOR_HANDLE build_descriptor_table();
    void uav_barrier(ID3D12Resource* res);

    D3D12Device* m_dev = nullptr;
    ID3D12GraphicsCommandList* m_list = nullptr;

    BufferBinding m_ubo[kMaxUniformBuffers];
    BufferBinding m_ssbo[kMaxStorageBuffers];
    Texture* m_tex[kMaxSampledTextures] = {};
    Texture* m_image[kMaxStorageImages] = {};
    uint8_t m_push[kPushConstantBytes] = {};
    bool m_in_render_pass = false;
    bool m_touched_swapchain = false;
};

/* ---- device --------------------------------------------------------------- */

class D3D12Device final : public Device {
public:
    explicit D3D12Device(const DeviceDesc& desc);
    ~D3D12Device() override;

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

    /* Whether this device presents, which is a property of the window it was
     * created with and not of the swapchain object standing at this instant.
     * A minimised window has no swapchain until it is restored, and a caller
     * that read this as "no window" would stop presenting for the rest of the
     * run and say nothing. acquire_backbuffer rebuilds and reports the frames
     * it cannot serve. */
    bool has_swapchain() const override { return m_hwnd != nullptr; }
    void set_present_mode(PresentMode mode) override;
    void notify_resize(uint32_t width, uint32_t height) override;
    bool acquire_backbuffer(Texture** out) override;
    void present() override;
    uint32_t surface_width() const override { return m_surface_width; }
    uint32_t surface_height() const override { return m_surface_height; }

    bool read_texture(Texture* t, std::vector<uint8_t>& out,
                      uint32_t* width, uint32_t* height) override;

    /* ---- used by D3D12CommandList and the shader loader ---- */
    ID3D12Device* d3d() const { return m_device.get(); }
    ID3D12RootSignature* root_signature() const { return m_root_signature.get(); }
    ID3D12CommandSignature* dispatch_signature() const { return m_dispatch_signature.get(); }
    ID3D12DescriptorHeap* descriptor_heap() const { return m_descriptor_heap.get(); }
    ID3D12DescriptorHeap* sampler_heap() const { return m_sampler_heap.get(); }
    Buffer* dummy_buffer() const { return m_dummy_buffer; }
    Buffer* dummy_uniform_buffer() const { return m_dummy_uniform_buffer; }
    Texture* dummy_texture() const { return m_dummy_texture; }
    Texture* dummy_storage_image() const { return m_dummy_storage_image; }
    /* One set of 32 descriptors out of the current frame's ring. */
    void allocate_descriptor_set(D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                                 D3D12_GPU_DESCRIPTOR_HANDLE* gpu);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle(uint32_t index) const;
    uint32_t descriptor_size() const { return m_descriptor_size; }
    /* A default-heap scratch used by the copies that D3D12's 256-byte row
     * pitch and 512-byte placement rules force through a second buffer. The
     * state is the device's because the scratch is shared by every copy in a
     * command list and has no Buffer wrapper to track it. */
    ID3D12Resource* scratch(uint64_t bytes);
    D3D12_RESOURCE_STATES scratch_state() const { return m_scratch_state; }
    void set_scratch_state(D3D12_RESOURCE_STATES s) { m_scratch_state = s; }
    /* Copies the written part of a host-visible storage buffer into its
     * default-heap shadow. See Buffer::staging for why the shadow exists. */
    void flush_storage_shadow(ID3D12GraphicsCommandList* list, Buffer* b,
                              uint64_t offset, uint64_t bytes);
    /* Drains the debug layer's message queue into the runtime log. Called
     * after every submit while validation is on, which is this backend's
     * equivalent of the Vulkan debug messenger. */
    void drain_debug_messages();
    /* Everything CreateGraphicsPipelineState and CreateComputePipelineState
     * were given, printed when one of them fails, then the debug layer's
     * messages if it is on, then a fatal. E_INVALIDARG from either is the
     * whole diagnostic the runtime offers by itself, so the description has
     * to come from here. */
    [[noreturn]] void report_graphics_pipeline_failure(
        const char* name, HRESULT hr, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& pd,
        const ShaderBytecode& vs, const ShaderBytecode& ps);
    [[noreturn]] void report_compute_pipeline_failure(
        const char* name, HRESULT hr, const D3D12_COMPUTE_PIPELINE_STATE_DESC& pd,
        const ShaderBytecode& cs);
    /* The pipeline the present blit draws with, for the destination format. */
    GraphicsPipeline* blit_pipeline(DXGI_FORMAT format);

    [[noreturn]] void fatal(const char* fmt, ...) const;
    void check(HRESULT hr, const char* what) const;
    /* A call that came back with the device gone. Names the failing call, its
     * HRESULT and what GetDeviceRemovedReason says, because the second is the
     * one that carries the cause. Nothing recovers from a removed device, so
     * this ends the run rather than letting it keep drawing into it. */
    [[noreturn]] void fatal_device_removed(const char* what, HRESULT hr) const;

private:
    /* The presentation path skips a field rather than failing when there is
     * no swapchain or no acquired backbuffer. One warn opens a run of skips
     * and one info closes it, carrying the count, so a window that shows
     * nothing says so without writing a line a field. */
    void note_present_skipped(const char* why);
    void note_present_resumed();
    void pick_adapter(const DeviceDesc& desc);
    void create_device(const DeviceDesc& desc);
    void create_root_signature();
    void create_frames();
    void create_dummies();
    void create_swapchain(uint32_t width, uint32_t height);
    void destroy_swapchain();
    uint32_t alloc_rtv();
    void free_rtv(uint32_t index);

    static constexpr uint32_t kFrames = 2;
    /* Descriptor sets one frame may use. The renderer's worst frame is a
     * scanout dispatch, the raster dispatches of one field, a present blit
     * and one draw per overlay command. A ring that runs out is fatal rather
     * than grown, for the reason the Vulkan pool gives: growing it silently
     * would hide a leak in the caller. */
    static constexpr uint32_t kSetsPerFrame = 1024;
    static constexpr uint32_t kRtvHeapSize = 64;

    const D3D12Entries* m_entries = nullptr;
    ComPtr<IDXGIFactory2> m_factory;
    ComPtr<IDXGIAdapter1> m_adapter;
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<ID3D12RootSignature> m_root_signature;
    ComPtr<ID3D12CommandSignature> m_dispatch_signature;
    ComPtr<ID3D12DescriptorHeap> m_descriptor_heap;
    /* rhi.h's four immutable samplers, written once at device creation. See
     * rhi_d3d12_bindings.h for why they are a table and not static
     * samplers. */
    ComPtr<ID3D12DescriptorHeap> m_sampler_heap;
    ComPtr<ID3D12DescriptorHeap> m_rtv_heap;
    ComPtr<ID3D12Fence> m_fence;
    ComPtr<ID3D12Resource> m_scratch;
    /* Scratch buffers a growth replaced. A command list still being recorded
     * may already reference the old one, so it is kept alive until wait_idle
     * proves nothing on the queue can be using it. */
    std::vector<ComPtr<ID3D12Resource>> m_retired;
    ComPtr<ID3D12InfoQueue> m_info_queue;
    uint64_t m_scratch_bytes = 0;
    D3D12_RESOURCE_STATES m_scratch_state = D3D12_RESOURCE_STATE_COMMON;
    HANDLE m_fence_event = nullptr;

    /* Filled in where the device is created, from what the device reports.
     * See rhi::Limits. */
    Limits m_limits;
    std::string m_device_name = "(no device)";
    std::string m_api_version;
    bool m_validation = false;
    bool m_software = false;
    bool m_allow_tearing = false;
    bool m_logged_shader_path = false;

    uint32_t m_descriptor_size = 0;
    uint32_t m_rtv_size = 0;
    std::vector<uint32_t> m_rtv_free;
    uint32_t m_rtv_next = 0;

    struct Frame {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        uint32_t descriptor_next = 0;
        uint64_t timeline = 0;
    };
    Frame m_frames[kFrames];
    uint32_t m_frame_index = 0;
    uint64_t m_timeline_value = 0;

    D3D12CommandList* m_cmd = nullptr;
    bool m_recording = false;

    Buffer* m_dummy_buffer = nullptr;
    /* Two dummies and not one: the same resource cannot be in
     * VERTEX_AND_CONSTANT_BUFFER for the CBV slots and UNORDERED_ACCESS for
     * the raw UAV slots of the same descriptor table. */
    Buffer* m_dummy_uniform_buffer = nullptr;
    Texture* m_dummy_texture = nullptr;
    Texture* m_dummy_storage_image = nullptr;

    ComPtr<IDXGISwapChain3> m_swapchain;
    HANDLE m_frame_latency = nullptr;
    std::vector<Texture*> m_backbuffers;
    std::vector<uint64_t> m_backbuffer_timeline;
    uint32_t m_backbuffer_index = UINT32_MAX;
    uint32_t m_surface_width = 0;
    uint32_t m_surface_height = 0;
    DXGI_FORMAT m_swapchain_format = DXGI_FORMAT_UNKNOWN;
    PresentMode m_present_mode = PresentMode::Fifo;
    bool m_swapchain_dirty = false;
    bool m_said_no_client_area = false;
    /* State of the skipped-field reporting above: whether a run of skips is
     * open, whether its warn has been written, and how many fields it has
     * eaten so far. Reset by note_present_resumed so a second stall is
     * reported as loudly as the first. */
    bool m_present_stalled = false;
    bool m_said_present_stalled = false;
    uint64_t m_skipped_fields = 0;
    void* m_hwnd = nullptr;

    /* One blit pipeline per destination format, built on first use. */
    struct BlitPipeline {
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        GraphicsPipeline* pipeline = nullptr;
    };
    std::vector<BlitPipeline> m_blit_pipelines;

    friend class D3D12CommandList;
};

/* Format translation, shared by both translation units. */
DXGI_FORMAT to_dxgi_format(Format f);

/* The name of an HRESULT this backend can actually be handed, so every
 * failure line carries the code and what it means. Only codes that occur
 * here are named; anything else comes back as "unrecognized HRESULT",
 * because a guessed name is worse than a bare number. */
const char* d3d12_hresult_name(HRESULT hr);

/* True for the codes that mean the device is gone. Nothing recovers from
 * one: every later call fails the same way, so the run ends at the first
 * one rather than continuing on a dead device. */
bool d3d12_device_lost(HRESULT hr);

} // namespace rhi

#endif /* ICORECOMP_RHI_D3D12_H */
