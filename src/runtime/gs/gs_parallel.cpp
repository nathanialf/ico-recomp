/* gs/gs_parallel.cpp: live GS backend on paraLLEl-GS
 * (third_party/parallel-gs, LGPLv3+, linked as libicorecomp-parallel-gs.so;
 * this file is ours and only talks to its public GSInterface).
 *
 * Wiring (mirrors the patterns in the submodule's own consumers,
 * tools/gs_dump_replayer.cpp for headless and tools/gs_stream_replayer.cpp
 * for presentation, without copying code):
 *   submit_gif  -> GSInterface::gif_transfer(path, data, bytes)
 *   write_priv  -> PrivRegisterState qword slots (same 16-byte-slot layout
 *                  the dump writer uses; see gs_dumpwriter.cpp)
 *   vsync       -> GSInterface::flush() + vsync(VSyncInfo{phase=field,...}),
 *                  then the ScanoutResult is blitted to a window swapchain
 *                  when one exists.
 *
 * Presentation modes, decided once at startup:
 *   windowed  SDL3 window + Granite WSI swapchain (built only when CMake
 *             found the vendored SDL3: ICORECOMP_PGS_SDL). Requires a
 *             display; the per-field cadence is: guest runs to the field
 *             boundary (ee/sched.cpp timeline), intc calls rt_gs_vsync_hook,
 *             and this backend renders + presents inside vsync(). Closing
 *             the window exits the run cleanly.
 *   headless  Vulkan device without a surface (no display, or
 *             ICORECOMP_GS_HEADLESS=1). Renders every field for real; with
 *             ICORECOMP_GS_SCREENSHOT=/path/out.ppm the latest scanout is
 *             written after each field so the file holds the final frame at
 *             exit (path must be outside the repo; scanout pixels are
 *             ROM-derived data).
 *
 * Validation layers: Granite auto-enables VK_LAYER_KHRONOS_validation when
 * present. We keep that off unless ICORECOMP_VVL=1 so normal runs match CI
 * machines without layers installed.
 *
 * Failure policy: no usable Vulkan loader/device, or a device missing
 * paraLLEl-GS's required features, is fatal with the reason logged (loud
 * failure beats silent wrongness).
 */
#include "gs_backend.h"

#ifdef ICORECOMP_HAVE_PARALLEL_GS

#include "runtime.h"

#include "gs_pgs_context.h"
#include "gs_readback.h"

#include "context.hpp"
#include "device.hpp"
#include "gs_interface.hpp"

#ifdef ICORECOMP_PGS_SDL
#include "wsi.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#endif

#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

bool env_is_1(const char* name) {
    const char* v = std::getenv(name);
    return v && std::strcmp(v, "1") == 0;
}

#ifdef ICORECOMP_PGS_SDL

constexpr Vulkan::ContextCreationFlags kContextFlags =
    Vulkan::CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT |
    Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_HEAP_BIT |
    Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT;

/* Minimal Vulkan::WSIPlatform on SDL3. Only what a fixed-function "blit the
 * scanout" presenter needs: surface creation, size queries, an alive flag
 * and event pumping. Input is not forwarded to the guest yet (P5). */
class SdlWsiPlatform final : public Vulkan::WSIPlatform {
public:
    bool init(unsigned width, unsigned height) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            rt_log("gs", "paraLLEl-GS: SDL_Init failed: %s", SDL_GetError());
            return false;
        }
        m_window = SDL_CreateWindow("ico-recomp", int(width), int(height),
                                    SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (!m_window) {
            rt_log("gs", "paraLLEl-GS: SDL_CreateWindow failed: %s", SDL_GetError());
            return false;
        }
        return true;
    }

    ~SdlWsiPlatform() override {
        if (m_window) SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }

    VkSurfaceKHR create_surface(VkInstance instance, VkPhysicalDevice) override {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(m_window, instance, nullptr, &surface)) {
            rt_log("gs", "paraLLEl-GS: SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
            return VK_NULL_HANDLE;
        }
        return surface;
    }

    std::vector<const char*> get_instance_extensions() override {
        Uint32 count = 0;
        const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&count);
        if (!exts) return {};
        return std::vector<const char*>(exts, exts + count);
    }

    uint32_t get_surface_width() override {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(m_window, &w, &h);
        return uint32_t(w > 0 ? w : 1);
    }

    uint32_t get_surface_height() override {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(m_window, &w, &h);
        return uint32_t(h > 0 ? h : 1);
    }

    bool alive(Vulkan::WSI&) override { return m_alive; }

    void poll_input() override {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_EVENT_QUIT:
                    m_alive = false;
                    break;
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                case SDL_EVENT_WINDOW_RESIZED:
                    resize = true;
                    break;
                default:
                    break;
            }
        }
    }

    void poll_input_async(Granite::InputTrackerHandler*) override { poll_input(); }

private:
    SDL_Window* m_window = nullptr;
    bool m_alive = true;
};

#endif /* ICORECOMP_PGS_SDL */

class ParallelBackend final : public GsBackend {
public:
    ParallelBackend() {
        if (!env_is_1("ICORECOMP_VVL")) {
            /* Granite auto-enables the validation layer when installed;
             * keep runs reproducible unless explicitly requested. */
            setenv("GRANITE_VULKAN_NO_VALIDATION", "1", 0);
        } else {
            rt_log("gs", "paraLLEl-GS: ICORECOMP_VVL=1, Vulkan validation enabled if the layer is installed");
        }

        if (!Vulkan::Context::init_loader(nullptr)) {
            rt_fatal("gs", nullptr, "paraLLEl-GS: Vulkan loader initialization failed (no libvulkan or no ICD)");
        }

        m_screenshot_path = std::getenv("ICORECOMP_GS_SCREENSHOT");

#ifdef ICORECOMP_PGS_SDL
        const bool display_available =
            (std::getenv("DISPLAY") && *std::getenv("DISPLAY")) ||
            (std::getenv("WAYLAND_DISPLAY") && *std::getenv("WAYLAND_DISPLAY"));
        if (display_available && !env_is_1("ICORECOMP_GS_HEADLESS")) {
            init_windowed();
        }
#endif
        if (!m_device) init_headless();

        m_iface = std::make_unique<ParallelGS::GSInterface>();
        ParallelGS::GSOptions opts = {};
        if (!m_iface->init(m_device, opts)) {
            rt_fatal("gs", nullptr,
                     "paraLLEl-GS: GSInterface::init failed; the Vulkan device does not meet its "
                     "requirements (see the log above for the missing features)");
        }

        const auto& gpu_props = m_device->get_gpu_properties();
        rt_log("gs", "paraLLEl-GS: live backend up on \"%s\" (Vulkan %u.%u, %s)",
               gpu_props.deviceName,
               VK_API_VERSION_MAJOR(gpu_props.apiVersion), VK_API_VERSION_MINOR(gpu_props.apiVersion),
               m_wsi_active ? "windowed" : "headless");
    }

    ~ParallelBackend() override {
        if (m_device) m_device->wait_idle();
        m_iface.reset();
#ifdef ICORECOMP_PGS_SDL
        m_wsi.reset();
        m_platform.reset();
#endif
        m_headless_device.reset();
        m_headless_context.reset();
    }

    void submit_gif(int path, const uint8_t* data, uint32_t qwords) override {
        if (path < 0 || path > 2 || qwords == 0) return;
        m_iface->gif_transfer(uint32_t(path), data, size_t(qwords) * 16);
        m_transfer_since_vsync = true;
    }

    void write_priv(uint32_t offset, uint64_t v) override {
        offset &= 0x1FFF;
        auto& priv = m_iface->get_priv_register_state();
        if (offset < 0x1000) {
            priv.qwords_lo[(offset >> 4) * 2] = v;
        } else {
            priv.qwords_hi[((offset - 0x1000) >> 4) * 2] = v;
        }
    }

    uint64_t read_priv(uint32_t offset) override {
        offset &= 0x1FFF;
        const auto& priv = m_iface->get_priv_register_state();
        if (offset < 0x1000) return priv.qwords_lo[(offset >> 4) * 2];
        return priv.qwords_hi[((offset - 0x1000) >> 4) * 2];
    }

    bool vsync(unsigned field) override {
        ++m_vsyncs;
        const bool presented = m_transfer_since_vsync;
        m_transfer_since_vsync = false;

        m_iface->flush();

        ParallelGS::VSyncInfo info = {};
        info.phase = field & 1;
        info.force_progressive = true;
        info.anti_blur = true;
        info.adapt_to_internal_horizontal_resolution = true;
        /* Both consumers of the scanout image here (swapchain blit,
         * screenshot readback) want a transfer source. */
        info.dst_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        info.dst_stage = VK_PIPELINE_STAGE_2_BLIT_BIT;
        info.dst_access = VK_ACCESS_2_TRANSFER_READ_BIT;

        ParallelGS::ScanoutResult scanout = m_iface->vsync(info);

#ifdef ICORECOMP_PGS_SDL
        if (m_wsi_active) present(scanout);
#endif
        if (m_screenshot_path && scanout.image) {
            if (!rt_gs_write_scanout_ppm(*m_device, *scanout.image, m_screenshot_path)) {
                rt_log("gs", "paraLLEl-GS: screenshot write to %s failed", m_screenshot_path);
                m_screenshot_path = nullptr; /* do not spam every field */
            }
        }
        return presented;
    }

    void report_stats() override {
        rt_log("gs", "paraLLEl-GS: %llu vsyncs rendered (%s)",
               (unsigned long long)m_vsyncs, m_wsi_active ? "windowed" : "headless");
    }

private:
    void init_headless() {
        RtGsContextResult ctx = rt_gs_make_pgs_context();
        if (!ctx.context) {
            rt_fatal("gs", nullptr, "paraLLEl-GS: no usable Vulkan device (instance/device creation failed)");
        }
        if (ctx.descriptor_buffer_disabled) {
            rt_log("gs", "paraLLEl-GS: descriptor-buffer path disabled (CPU device or "
                         "ICORECOMP_GS_NO_DESCBUF=1; see gs_pgs_context.h)");
        }
        m_headless_context = std::move(ctx.context);
        m_headless_device = std::make_unique<Vulkan::Device>();
        m_headless_device->set_context(*m_headless_context);
        m_headless_device->init_frame_contexts(4);
        m_device = m_headless_device.get();
        rt_log("gs", "paraLLEl-GS: headless Vulkan device (no display or ICORECOMP_GS_HEADLESS=1); "
                     "set ICORECOMP_GS_SCREENSHOT=/path/out.ppm to capture the scanout");
    }

#ifdef ICORECOMP_PGS_SDL
    void init_windowed() {
        auto platform = std::make_unique<SdlWsiPlatform>();
        /* 640x448: the NTSC full-height frame this game scans out. */
        if (!platform->init(640, 448)) return;

        auto wsi = std::make_unique<Vulkan::WSI>();
        wsi->set_platform(platform.get());
        wsi->set_backbuffer_format(Vulkan::BackbufferFormat::UNORM);
        /* WSI owns context creation here, so the CPU-device auto-fallback in
         * gs_pgs_context.h does not apply; a windowed run on a software
         * device needs ICORECOMP_GS_NO_DESCBUF=1 by hand. */
        Vulkan::ContextCreationFlags flags = kContextFlags;
        if (std::getenv("ICORECOMP_GS_NO_DESCBUF")) {
            flags &= ~Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT;
        }
        if (!wsi->init_context_from_platform(1, {}, flags)) {
            rt_log("gs", "paraLLEl-GS: WSI context init failed, falling back to headless");
            return;
        }
        if (!wsi->init_device() || !wsi->init_surface_swapchain()) {
            rt_log("gs", "paraLLEl-GS: WSI device/swapchain init failed, falling back to headless");
            return;
        }
        m_platform = std::move(platform);
        m_wsi = std::move(wsi);
        m_device = &m_wsi->get_device();
        m_wsi_active = true;
    }

    void present(const ParallelGS::ScanoutResult& scanout) {
        if (!m_wsi->begin_frame()) {
            rt_log("gs", "paraLLEl-GS: WSI begin_frame failed");
            return;
        }

        auto& device = m_wsi->get_device();
        auto cmd = device.request_command_buffer();
        auto& backbuffer = device.get_swapchain_view().get_image();

        cmd->swapchain_touch_in_stages(VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT);
        cmd->image_barrier(backbuffer,
                           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, 0,
                           VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT);

        VkClearValue clear = {};
        cmd->clear_image(backbuffer, clear);

        if (scanout.image) {
            /* Letterbox: preserve the mode aspect ratio inside the window. */
            const int bb_w = int(backbuffer.get_width());
            const int bb_h = int(backbuffer.get_height());
            const int mode_w = int(scanout.mode_width ? scanout.mode_width : scanout.internal_width);
            const int mode_h = int(scanout.mode_height ? scanout.mode_height : scanout.internal_height);
            int dst_w = bb_w, dst_h = bb_h;
            if (mode_w > 0 && mode_h > 0) {
                if (int64_t(bb_w) * mode_h > int64_t(bb_h) * mode_w) {
                    dst_w = int(int64_t(bb_h) * mode_w / mode_h);
                } else {
                    dst_h = int(int64_t(bb_w) * mode_h / mode_w);
                }
            }
            const int x0 = (bb_w - dst_w) / 2;
            const int y0 = (bb_h - dst_h) / 2;
            cmd->blit_image(backbuffer, *scanout.image,
                            { x0, y0, 0 }, { dst_w, dst_h, 1 },
                            { 0, 0, 0 },
                            { int(scanout.image->get_width()), int(scanout.image->get_height()), 1 },
                            0, 0, 0, 0, 1, VK_FILTER_LINEAR);
        }

        cmd->image_barrier(backbuffer,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                           VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_NONE, 0);
        device.submit(cmd);

        if (!m_wsi->end_frame()) {
            rt_log("gs", "paraLLEl-GS: WSI end_frame failed");
        }
        if (!m_platform->alive(*m_wsi)) {
            rt_log("gs", "paraLLEl-GS: window closed, exiting");
            std::exit(0);
        }
    }
#endif /* ICORECOMP_PGS_SDL */

    std::unique_ptr<Vulkan::Context> m_headless_context;
    std::unique_ptr<Vulkan::Device> m_headless_device;
#ifdef ICORECOMP_PGS_SDL
    std::unique_ptr<SdlWsiPlatform> m_platform;
    std::unique_ptr<Vulkan::WSI> m_wsi;
#endif
    Vulkan::Device* m_device = nullptr; /* whichever of the above is live */
    std::unique_ptr<ParallelGS::GSInterface> m_iface;
    const char* m_screenshot_path = nullptr;
    uint64_t m_vsyncs = 0;
    bool m_transfer_since_vsync = false;
    bool m_wsi_active = false;
};

} // namespace

GsBackend* rt_gs_make_parallel_backend() {
    return new ParallelBackend();
}

#endif /* ICORECOMP_HAVE_PARALLEL_GS */
