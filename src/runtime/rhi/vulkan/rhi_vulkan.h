/* rhi/vulkan/rhi_vulkan.h: internal types of the Vulkan backend.
 *
 * Ours (MIT). Only rhi_vulkan_device.cpp and rhi_vulkan_cmd.cpp include this;
 * the renderer sees rhi.h alone.
 *
 * The loader is volk (third_party/volk, MIT) against the Khronos headers
 * (third_party/Vulkan-Headers). Both are pinned submodules. Nothing here
 * links a Vulkan library at build time: volkInitialize finds the loader at
 * run time, which is what lets the same binary run on a machine with no
 * Vulkan installed and report that clearly instead of failing to start.
 *
 * Vulkan 1.3 is required, and a device below it is a loud fatal naming the
 * device and its version. 1.3 is what makes dynamic rendering and
 * synchronization2 core, and building the pre-1.3 paths for both would double
 * the backend for hardware this port does not target. The requirement is
 * recorded in docs/GS_RENDERER.md next to the rest of what runs where.
 *
 * Memory is one allocation per resource. The renderer holds a handful of
 * buffers and images (4 MiB of local memory, a scanout image, a frame image,
 * overlay geometry and a few textures), so a suballocator would be code with
 * nothing to do.
 */
#ifndef ICORECOMP_RHI_VULKAN_H
#define ICORECOMP_RHI_VULKAN_H

#include "../rhi.h"

/* Platform surface support is compiled in only where CMake found the headers
 * for it (see the ICORECOMP_RHI_* options). Headless needs none of them, and
 * a build host without X11 or Wayland development headers still builds the
 * renderer and the replay tool. */
/* The build already defines these alongside the ICORECOMP_RHI_* names, so
 * volk sees them too; the guards keep this header usable on its own. */
#if defined(ICORECOMP_RHI_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#  define VK_USE_PLATFORM_WIN32_KHR
#endif
#if defined(ICORECOMP_RHI_XLIB) && !defined(VK_USE_PLATFORM_XLIB_KHR)
#  define VK_USE_PLATFORM_XLIB_KHR
#endif
#if defined(ICORECOMP_RHI_WAYLAND) && !defined(VK_USE_PLATFORM_WAYLAND_KHR)
#  define VK_USE_PLATFORM_WAYLAND_KHR
#endif

#include <volk.h>

/* Xlib's headers are C from before namespaces and define these as macros.
 * rhi.h is included above so its enumerators are already parsed, but any
 * later use of TextureUsage::None or a VkResult named Success would break, so
 * the macros go. */
#if defined(ICORECOMP_RHI_XLIB)
#  undef None
#  undef Status
#  undef Bool
#  undef Success
#  undef Always
#  undef True
#  undef False
#endif

#include <cstring>
#include <string>
#include <vector>

namespace rhi {

/* ---- resources ------------------------------------------------------------ */

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    uint64_t size = 0;
    BufferKind kind = BufferKind::DeviceLocal;
};

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    /* Swapchain images are owned by the swapchain, not by this object. */
    bool owns_image = true;
};

struct ComputePipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
};

struct GraphicsPipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
};

class VulkanDevice;

/* ---- command list ---------------------------------------------------------
 *
 * One per frame in flight, reused. The bindings below are resolved into a
 * fresh descriptor set at every dispatch and draw, which is why there is no
 * "bind set" call: a caller that changes one slot between two dispatches gets
 * two sets and no aliasing. */
class VulkanCommandList final : public CommandList {
public:
    VulkanCommandList(VulkanDevice* dev) : m_dev(dev) {}

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
    void reset(VkCommandBuffer cb);
    VkCommandBuffer handle() const { return m_cb; }
    bool touched_swapchain() const { return m_touched_swapchain; }

private:
    struct BufferBinding {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        VkDeviceSize range = 0;
    };

    /* Moves a texture to the layout an operation needs, using the layout it
     * is tracked as being in. Keeping this inside the copy, blit and render
     * pass calls rather than making every caller barrier by hand removes the
     * single most common way to get a Vulkan renderer subtly wrong. */
    void transition(Texture* t, VkImageLayout to, VkPipelineStageFlags2 stage,
                    VkAccessFlags2 access);
    VkDescriptorSet build_descriptor_set();

    VulkanDevice* m_dev = nullptr;
    VkCommandBuffer m_cb = VK_NULL_HANDLE;

    BufferBinding m_ubo[kMaxUniformBuffers];
    BufferBinding m_ssbo[kMaxStorageBuffers];
    Texture* m_tex[kMaxSampledTextures] = {};
    Texture* m_image[kMaxStorageImages] = {};
    uint8_t m_push[kPushConstantBytes] = {};
    bool m_in_render_pass = false;
    bool m_touched_swapchain = false;
};

/* ---- device --------------------------------------------------------------- */

class VulkanDevice final : public Device {
public:
    explicit VulkanDevice(const DeviceDesc& desc);
    ~VulkanDevice() override;

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

    /* Whether this device presents, which is a property of the surface it was
     * created with and not of the swapchain object standing at this instant.
     * A minimised window has no swapchain until it is restored, and a caller
     * that read this as "no window" would stop presenting for the rest of the
     * run and say nothing. acquire_backbuffer rebuilds and reports the frames
     * it cannot serve. */
    bool has_swapchain() const override { return m_surface != VK_NULL_HANDLE; }
    void set_present_mode(PresentMode mode) override;
    void notify_resize(uint32_t width, uint32_t height) override;
    bool acquire_backbuffer(Texture** out) override;
    void present() override;
    uint32_t surface_width() const override { return m_surface_width; }
    uint32_t surface_height() const override { return m_surface_height; }

    bool read_texture(Texture* t, std::vector<uint8_t>& out,
                      uint32_t* width, uint32_t* height) override;

    /* Used by VulkanCommandList. */
    VkDevice vk() const { return m_device; }
    VkPipelineLayout pipeline_layout() const { return m_pipeline_layout; }
    VkDescriptorSet allocate_descriptor_set();
    Buffer* dummy_buffer() const { return m_dummy_buffer; }
    Texture* dummy_texture() const { return m_dummy_texture; }
    Texture* dummy_storage_image() const { return m_dummy_storage_image; }
    [[noreturn]] void fatal(const char* fmt, ...) const;
    void check(VkResult r, const char* what) const;

private:
    void create_instance(const DeviceDesc& desc);
    void create_surface(const DeviceDesc& desc);
    void pick_physical_device(bool prefer_software);
    void create_device();
    void create_frames();
    void create_layout_and_samplers();
    void create_dummies();
    void create_swapchain(uint32_t width, uint32_t height);
    /* retire=true when a new swapchain is about to be created from this one:
     * the handle and its release semaphores are kept alive one generation
     * longer instead of being destroyed here. See the definition. */
    void destroy_swapchain(bool retire = false);
    void free_retired_swapchain();
    void drain_acquire();
    /* A field that reaches no screen, and the field that ends that. One warn
     * carries the reason at the start of a stall and one info carries the
     * count at the end of it; the fields in between are counted only, because
     * a minimised window is one line per field otherwise. */
    void note_present_stall(const char* fmt, ...);
    void note_presentation_resumed();
    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags props) const;

    static constexpr uint32_t kFrames = 2;

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_gpu = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties m_mem_props{};
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queue_family = UINT32_MAX;
    /* Filled in where the device is created, from what the device reports.
     * See rhi::Limits. */
    Limits m_limits;
    std::string m_device_name = "(no device)";
    std::string m_api_version;
    bool m_validation = false;
    bool m_software = false;  /* the device picked is a CPU implementation */

    VkDescriptorSetLayout m_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;
    VkSampler m_samplers[kSamplerCount] = {};
    VkPipelineCache m_pipeline_cache = VK_NULL_HANDLE;

    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkDescriptorPool descriptors = VK_NULL_HANDLE;
        /* Signalled by vkAcquireNextImageKHR, waited by the submit that draws
         * into the acquired image. Reused only once the frame's timeline says
         * the submit that waited on it has completed, and drained by an empty
         * submit when an acquire was never consumed. acquire_backbuffer does
         * both, and says why. */
        VkSemaphore acquire = VK_NULL_HANDLE;
        uint64_t timeline = 0;
    };
    Frame m_frames[kFrames];
    uint32_t m_frame_index = 0;
    VkSemaphore m_timeline = VK_NULL_HANDLE;
    uint64_t m_timeline_value = 0;

    /* The acquire semaphore of the image that is acquired and not yet
     * submitted, or null. Held rather than re-derived from m_frame_index,
     * because submit() advances that index: any submit between the acquire
     * and the presenting one would otherwise leave the present waiting on a
     * semaphore nothing signalled. */
    VkSemaphore m_acquired = VK_NULL_HANDLE;

    VulkanCommandList* m_cmd = nullptr;
    bool m_recording = false;

    Buffer* m_dummy_buffer = nullptr;
    Texture* m_dummy_texture = nullptr;
    Texture* m_dummy_storage_image = nullptr;

    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    std::vector<Texture*> m_backbuffers;
    /* One release semaphore per swapchain image, not per frame in flight.
     * The presentation engine says nothing about when it has finished waiting
     * on one, so the only thing that makes a re-signal safe is that the image
     * itself came back out of vkAcquireNextImageKHR; a per-frame semaphore
     * with three images has no such guarantee. */
    std::vector<VkSemaphore> m_image_release;
    /* The previous generation, kept alive across one swapchain recreate.
     * vkDeviceWaitIdle says nothing about the presentation engine having
     * finished waiting on a semaphore handed to vkQueuePresentKHR; only
     * reacquiring the image does. So a resize hands the old handle to
     * vkCreateSwapchainKHR as oldSwapchain and leaves these to the next
     * recreate or to the destructor, both of which are a full swapchain
     * lifetime later. */
    VkSwapchainKHR m_retired_swapchain = VK_NULL_HANDLE;
    std::vector<VkSemaphore> m_retired_release;
    uint32_t m_backbuffer_index = UINT32_MAX;
    /* The image the most recent submit signalled a release for, or
     * UINT32_MAX. present() waits on that image's semaphore, and presents
     * nothing when no submit signalled one. */
    uint32_t m_present_image = UINT32_MAX;
    uint32_t m_surface_width = 0;
    uint32_t m_surface_height = 0;
    VkFormat m_swapchain_format = VK_FORMAT_UNDEFINED;
    PresentMode m_present_mode = PresentMode::Fifo;
    bool m_swapchain_dirty = false;
    bool m_said_unused_acquire = false;
    bool m_said_present_without_draw = false;
    /* Both of these are re-armed by a successful create_swapchain: they
     * describe one swapchain generation, not the whole run. */
    bool m_said_no_extent = false;
    bool m_said_suboptimal_present = false;
    /* Set while nothing is reaching the screen, with the number of fields
     * that have not. See note_present_stall. */
    bool m_present_stalled = false;
    uint64_t m_stalled_fields = 0;
};

/* Format translation, shared by both translation units. */
VkFormat to_vk_format(Format f);

} // namespace rhi

#endif /* ICORECOMP_RHI_VULKAN_H */
