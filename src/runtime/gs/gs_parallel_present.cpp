/* gs/gs_parallel_present.cpp: device, window and swapchain of the paraLLEl-GS
 * shim.
 *
 * Part of libicorecomp-parallel-gs; see gs_parallel_lib.cpp for the library
 * overview and gs_parallel_impl.h for the RtPgs type. Holds headless and
 * windowed device creation, present mode selection, the per-field present
 * (fit and filter of the scanout into the window backbuffer), and the window
 * control entry points behind rt_pgs_window_handle and friends.
 */
#include "gs_parallel_impl.h"

#include "gs_pgs_context.h"

/* Granite's thread-index table, for bind_consumer_thread below. */
#include "thread_id.hpp"

#include <cmath>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#ifdef ICORECOMP_PGS_SDL

namespace {

constexpr Vulkan::ContextCreationFlags kContextFlags =
    Vulkan::CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT |
    Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_HEAP_BIT |
    Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT;

/* Shared by init_windowed (the have_opts branch) and rt_pgs_set_present_mode:
 * RT_PGS_PRESENT_* -> the Vulkan::PresentMode Granite's WSI wants, plus the
 * log wording used at both call sites. */
Vulkan::PresentMode present_mode_from_rt(uint32_t rt_mode, const char** what) {
    switch (rt_mode) {
    case RT_PGS_PRESENT_FIFO:
        *what = "FIFO (blocks on the display refresh)";
        return Vulkan::PresentMode::SyncToVBlank;
    case RT_PGS_PRESENT_IMMEDIATE:
        *what = "immediate (may tear)";
        return Vulkan::PresentMode::UnlockedForceTearing;
    default: /* RT_PGS_PRESENT_MAILBOX */
        *what = "mailbox (non-blocking, no tearing)";
        return Vulkan::PresentMode::UnlockedNoTearing;
    }
}

const char* present_mode_name(Vulkan::PresentMode mode) {
    switch (mode) {
    case Vulkan::PresentMode::SyncToVBlank: return "FIFO";
    case Vulkan::PresentMode::UnlockedMaybeTear: return "mailbox/immediate (driver choice)";
    case Vulkan::PresentMode::UnlockedForceTearing: return "immediate";
    case Vulkan::PresentMode::UnlockedNoTearing: return "mailbox";
    }
    return "?";
}

} // namespace

#endif /* ICORECOMP_PGS_SDL */

/* ---- window control / event pump inversion (shim 3) ----------------------
 *
 * window_handle/notify_quit/notify_resize/surface_size are no-ops (NULL /
 * 0x0 / dropped) when headless, matching the doc comments in
 * gs_parallel_api.h. set_present_mode/set_presentation/set_render_scale are
 * declared reachable "between frames only" by that same header; the
 * m_in_frame check comes first so a caller that violates the contract gets a
 * loud fatal, headless or not.
 */

void* RtPgs::window_handle() {
#ifdef ICORECOMP_PGS_SDL
    if (m_wsi_active && m_platform) return (void*)m_platform->window();
#endif
    return nullptr;
}

void RtPgs::notify_quit() {
#ifdef ICORECOMP_PGS_SDL
    if (m_platform) m_platform->handle_quit();
#endif
    /* Recorded here as well as at the next present: the host polls this from
     * its own thread to decide when to exit, and a quit raised while the
     * consumer cannot present would otherwise never be seen. */
    m_window_closed.store(true, std::memory_order_release);
}

void RtPgs::notify_resize() {
#ifdef ICORECOMP_PGS_SDL
    if (m_platform) {
        /* Sample first, then raise the flag. The caller is the window's own
         * thread (see gs_parallel_api.h) and the consumer acts on the flag,
         * so raising it first would let the consumer rebuild the swapchain
         * against the size cache this call is about to replace. */
        m_platform->sample_state();
        m_platform->handle_resize();
    }
#endif
}

/* Creating thread only; the host calls it once per event pump. */
void RtPgs::sample_window_state() {
#ifdef ICORECOMP_PGS_SDL
    if (m_platform) m_platform->sample_state();
#endif
}

/* Consumer thread only, once, before its first call into this instance. */
void RtPgs::bind_consumer_thread() {
    /* Granite keys its per-frame command pools on a thread index kept in
     * thread-local storage (Granite/util/thread_id.cpp), set for the creating
     * thread by Device::set_context. An unregistered thread logs an error and
     * falls back to index 0, so register index 0 explicitly: the device is
     * created with one thread index (init_context_from_platform(1, ...)), and
     * the creating thread and this one never run library calls at the same
     * time -- the host's command ring hands the work from one to the other
     * with its own happens-before edges -- so sharing the index is exactly
     * the arrangement Granite expects from a single-threaded submitter. */
    Util::register_thread_index(0);
    logf("paraLLEl-GS: GS consumer thread bound (Granite thread index 0); GIF transfers,"
         " vsync and present run on it from here");
}

void RtPgs::surface_size(uint32_t* width, uint32_t* height) {
    uint32_t w = 0, h = 0;
#ifdef ICORECOMP_PGS_SDL
    if (m_wsi_active && m_platform) {
        w = m_platform->get_surface_width();
        h = m_platform->get_surface_height();
    }
#endif
    if (width) *width = w;
    if (height) *height = h;
}

void RtPgs::present_rect(int32_t* x, int32_t* y, int32_t* w, int32_t* h,
                         int32_t* bb_w, int32_t* bb_h) {
    /* Under the mutex present_frame publishes them with, so all six describe
     * the same present: the writer is the host's GS consumer thread and this
     * reader is its EE thread (host/mouse.cpp, guest/menu_nav.cpp and
     * ui/ui_menu_cursor.cpp map a cursor position into the picture with
     * them). No frame guard: this reads state, it does not touch the
     * swapchain. */
    std::lock_guard<std::mutex> lk(m_present_rect_mu);
    if (x) *x = m_present_x;
    if (y) *y = m_present_y;
    if (w) *w = m_present_w;
    if (h) *h = m_present_h;
    if (bb_w) *bb_w = m_present_bb_w;
    if (bb_h) *bb_h = m_present_bb_h;
}

void RtPgs::set_present_mode(uint32_t mode) {
    if (m_in_frame) {
        fatalf("paraLLEl-GS: rt_pgs_set_present_mode called while a frame is in flight;"
               " settings must apply at the field boundary");
    }
#ifdef ICORECOMP_PGS_SDL
    if (!m_wsi_active) {
        logf("paraLLEl-GS: rt_pgs_set_present_mode ignored (headless, no window)");
        return;
    }
    const char* what = "mailbox (non-blocking, no tearing)";
    Vulkan::PresentMode vk_mode = present_mode_from_rt(mode, &what);
    m_opts.present_mode = mode;
    m_wsi->set_present_mode(vk_mode);
    /* set_present_mode only records the request; it takes effect on the
     * swapchain that gets rebuilt at the next begin_frame. Reading
     * get_present_mode() here reports the request just recorded, not
     * necessarily what the driver ends up honoring -- logging requested vs
     * what Granite is about to ask the driver for is still the useful
     * signal for "does this driver even support mailbox". */
    logf("paraLLEl-GS: present mode requested %s (%s, resolved by the host); "
         "driver reports %s once the swapchain rebuilds",
         what, present_mode_name(vk_mode), present_mode_name(m_wsi->get_present_mode()));
#else
    (void)mode;
    logf("paraLLEl-GS: rt_pgs_set_present_mode ignored (built without window support)");
#endif
}

void RtPgs::set_presentation(uint32_t fit, uint32_t filter) {
    if (m_in_frame) {
        fatalf("paraLLEl-GS: rt_pgs_set_presentation called while a frame is in flight;"
               " settings must apply at the field boundary");
    }
    /* Only stores; present_frame reads m_opts.fit/filter fresh every field,
     * so this takes effect at the next present with no further action. */
    m_opts.fit = fit;
    m_opts.filter = filter;
}

void RtPgs::set_raster(uint32_t raster) {
    if (m_in_frame) {
        fatalf("paraLLEl-GS: rt_pgs_set_raster called while a frame is in flight;"
               " settings must apply at the field boundary");
    }
    /* Only stores; RtPgs::vsync reads m_opts.raster fresh every field, so
     * this takes effect at the next vsync with no further action. */
    m_opts.raster = raster;
    logf("paraLLEl-GS: raster %s", rt_pgs_raster_log_text(raster));
}

void RtPgs::set_deinterlace(uint32_t deinterlace) {
    if (m_in_frame) {
        fatalf("paraLLEl-GS: rt_pgs_set_deinterlace called while a frame is in flight;"
               " settings must apply at the field boundary");
    }
    /* Only stores; RtPgs::vsync reads m_opts.deinterlace fresh every field,
     * so this takes effect at the next vsync with no further action. */
    m_opts.deinterlace = deinterlace;
    logf("paraLLEl-GS: deinterlace %s (display.deinterlace)",
         rt_pgs_deinterlace_name(deinterlace));
}

void RtPgs::set_render_scale(uint32_t factor) {
    if (m_in_frame) {
        fatalf("paraLLEl-GS: rt_pgs_set_render_scale called while a frame is in flight;"
               " settings must apply at the field boundary");
    }
    ParallelGS::SuperSampling ss;
    switch (factor) {
    case 1: ss = ParallelGS::SuperSampling::X1; break;
    case 4: ss = ParallelGS::SuperSampling::X4; break;
    case 8: ss = ParallelGS::SuperSampling::X8; break;
    case 16: ss = ParallelGS::SuperSampling::X16; break;
    default:
        /* The host validates render_scale against settings.json's allowed
         * set before this is ever called, so anything else is a programming
         * error, not user input (same reasoning as the constructor). */
        fatalf("paraLLEl-GS: rt_pgs_set_render_scale factor %u is not one of 1/4/8/16", factor);
    }
    /* ordered_super_sampling stays at the GSOptions default (true: an ordered
     * grid, so X4 is 2x2 and both axes carry extra samples, which is what
     * high-resolution scanout needs). super_sampled_textures follows the
     * factor for the reason spelled out in RtPgs::vsync: without it ICO's
     * display copy resolves the super-samples away before the CRTC ever sees
     * them. GSInterface forces it off for X1 by itself
     * (gs_interface.cpp:70-78), so the test here only mirrors that. */
    const bool ss_textures = factor >= 4;
    m_iface->set_super_sampling_rate(ss, true, ss_textures);
    m_opts.render_scale = factor;
    /* Twin of the create-time line; see the comment there about the
     * requested versus effective rate, and hires= on the scanout geometry
     * line for what the renderer did with the request. */
    logf("paraLLEl-GS: render scale applied live: %ux, super-sampled textures %s,"
         " high-resolution scanout %s",
         m_opts.render_scale, ss_textures ? "on" : "off",
         factor >= 4 ? "requested" : "off");
}

/* Pipeline cache file. The payload is whatever Granite's Device serializes
 * on this host, and there are two shapes of it (vulkan/device.cpp): the
 * legacy one, pipelineCacheUUID + a hash of the payload + the
 * vkGetPipelineCacheData blob, and, on a driver with VK_KHR_pipeline_binary,
 * PipelineCache's own format (vulkan/pipeline_cache.cpp). Both are checked
 * by Granite, not by the driver, and the two are not interchangeable, so a
 * file written by one shape and read by the other is rejected. The legacy
 * path rejects softly (it logs and creates a fresh cache); the binary path
 * rejects by returning false out of init_pipeline_cache, which is why the
 * load below retries with an empty payload rather than treating a failure as
 * "no cache this run". init_pipeline_cache is called even when there is no
 * file: with GRANITE_VULKAN_SYSTEM_HANDLES off the device otherwise never
 * creates a cache object at all, and there would be nothing to store at
 * exit. */
void RtPgs::pipeline_cache_load() {
    if (!m_device) return;
    std::vector<uint8_t> blob;
#ifdef ICORECOMP_PGS_SDL
    if (const char* base = SDL_GetBasePath()) {
        /* SDL3 owns this string; it is not freed (SDL_GetBasePath in SDL2
         * was the one that had to be). */
        m_pipeline_cache_path = std::string(base) + "cache/pipeline_cache.bin";
    }
#endif
    if (!m_pipeline_cache_path.empty()) {
        std::ifstream in(m_pipeline_cache_path, std::ios::binary);
        if (in) {
            blob.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    }
    bool ok = blob.empty() ? m_device->init_pipeline_cache(nullptr, 0)
                           : m_device->init_pipeline_cache(blob.data(), blob.size());
    if (!ok && !blob.empty()) {
        /* The stored payload was rejected outright: a file from a driver
         * with a different cache shape, or a truncated one. Start empty
         * rather than run with no cache object, and let the store at exit
         * replace the file. */
        logf("paraLLEl-GS: pipeline cache: %s rejected by Granite; starting from an empty cache "
             "and rewriting it at exit", m_pipeline_cache_path.c_str());
        blob.clear();
        ok = m_device->init_pipeline_cache(nullptr, 0);
    }
    if (!ok) {
        /* No cache object exists, so there is nothing to serialize either:
         * clear the path so the store at exit does not read an uninitialized
         * cache and overwrite a good file with it. */
        logf("paraLLEl-GS: pipeline cache: the device would not create one; pipelines compile "
             "uncached this run and no file is written");
        m_pipeline_cache_path.clear();
        return;
    }
    if (blob.empty()) {
        logf("paraLLEl-GS: pipeline cache: none at %s; every pipeline compiles from scratch this run "
             "and the cache is written at exit",
             m_pipeline_cache_path.empty() ? "(no base path)" : m_pipeline_cache_path.c_str());
    } else {
        logf("paraLLEl-GS: pipeline cache: %zu bytes read from %s (a UUID or hash mismatch is "
             "reported by Granite above and means a fresh cache)",
             blob.size(), m_pipeline_cache_path.c_str());
    }
}

void RtPgs::pipeline_cache_store() {
    if (!m_device || m_pipeline_cache_path.empty()) return;
    const size_t size = m_device->get_pipeline_cache_size();
    if (size == 0) {
        logf("paraLLEl-GS: pipeline cache: nothing to store");
        return;
    }
    std::vector<uint8_t> blob(size);
    if (!m_device->get_pipeline_cache_data(blob.data(), blob.size())) {
        logf("paraLLEl-GS: pipeline cache: vkGetPipelineCacheData failed; not stored");
        return;
    }
    std::error_code ec;
    const std::filesystem::path path(m_pipeline_cache_path);
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        logf("paraLLEl-GS: pipeline cache: cannot create %s (%s); not stored",
             path.parent_path().string().c_str(), ec.message().c_str());
        return;
    }
    /* Write beside, then rename over: a run killed mid-write leaves the
     * previous cache intact rather than a truncated file. */
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        /* Closed explicitly and checked after: an ofstream that fails while
         * flushing in its destructor reports nothing, and the partial file
         * would then be renamed over a good cache. Any failure removes the
         * temporary rather than leaving it beside the cache. */
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        bool wrote = bool(out);
        if (wrote) {
            out.write(reinterpret_cast<const char*>(blob.data()), (std::streamsize)blob.size());
            out.close();
            wrote = bool(out);
        }
        if (!wrote) {
            logf("paraLLEl-GS: pipeline cache: write to %s failed; not stored", tmp.string().c_str());
            std::filesystem::remove(tmp, ec);
            return;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        logf("paraLLEl-GS: pipeline cache: rename to %s failed (%s); not stored",
             path.string().c_str(), ec.message().c_str());
        std::filesystem::remove(tmp, ec);
        return;
    }
    logf("paraLLEl-GS: pipeline cache: %zu bytes written to %s", blob.size(), path.string().c_str());
}

void RtPgs::init_headless() {
    RtGsContextResult ctx = rt_gs_make_pgs_context();
    if (!ctx.context) {
        fatalf("paraLLEl-GS: no usable Vulkan device (instance/device creation failed)");
    }
    if (ctx.descriptor_buffer_disabled) {
        logf("paraLLEl-GS: descriptor-buffer path disabled (CPU device or "
             "ICORECOMP_GS_NO_DESCBUF=1; see gs_pgs_context.h)");
    }
    m_headless_context = std::move(ctx.context);
    m_headless_device = std::make_unique<Vulkan::Device>();
    m_headless_device->set_context(*m_headless_context);
    m_headless_device->init_frame_contexts(4);
    m_device = m_headless_device.get();
    pipeline_cache_load();
    logf("paraLLEl-GS: headless Vulkan device (no display or ICORECOMP_GS_HEADLESS=1); "
         "set ICORECOMP_GS_SCREENSHOT=/path/out.ppm to capture the scanout");
}

#ifdef ICORECOMP_PGS_SDL

void RtPgs::init_windowed() {
    auto platform = std::make_unique<SdlWsiPlatform>(*this);
    /* 640x480: the 4:3 this backend presents at (scanout_display_aspect), so
     * the window opens with no letterbox. Not the scanout's pixel dimensions:
     * ICO scans out 512x224 in the mode domain, which is not its shape on a
     * TV. This is only the fallback for a caller that passes 0 in either
     * field (see RtPgsCreateOptions in gs_parallel_api.h); the host always
     * passes display.window_width/height, which default to 1280x960, the
     * same 4:3 shape at twice the size. */
    const unsigned window_w = (m_have_opts && m_opts.window_width) ? m_opts.window_width : 640;
    const unsigned window_h = (m_have_opts && m_opts.window_height) ? m_opts.window_height : 480;
    if (!platform->init(window_w, window_h)) return;

    auto wsi = std::make_unique<Vulkan::WSI>();
    wsi->set_platform(platform.get());
    wsi->set_backbuffer_format(Vulkan::BackbufferFormat::UNORM);

    /* Present pacing. Granite defaults to FIFO, which blocks each present
     * until the display has scanned out the previous one. The guest clock
     * here is advanced by guest execution, not by the presenter, so a
     * blocking present does not pace the game, it only stalls the thread
     * that is trying to run it: miss a 60 Hz deadline once and the
     * swapchain settles at 30, and the game runs at half speed while still
     * looking smooth. MAILBOX presents without blocking and without
     * tearing. ICORECOMP_GS_PRESENT=vsync restores the old behavior,
     * =tear forces IMMEDIATE. */
    {
        Vulkan::PresentMode mode = Vulkan::PresentMode::UnlockedNoTearing;
        const char* what = "mailbox (non-blocking, no tearing)";
        if (m_have_opts) {
            /* The host already resolved settings.json vs ICORECOMP_GS_PRESENT
             * (gs_parallel.cpp); this reads only the result. */
            mode = present_mode_from_rt(m_opts.present_mode, &what);
            logf("paraLLEl-GS: present mode %s (display.present, resolved by the host)", what);
        } else {
            /* opts == NULL: the pre-settings default path documented on
             * rt_pgs_create. ICORECOMP_GS_PRESENT is read here, and only
             * here, in this one case. */
            const char* pm = std::getenv("ICORECOMP_GS_PRESENT");
            if (pm && (std::strcmp(pm, "vsync") == 0 || std::strcmp(pm, "fifo") == 0)) {
                mode = Vulkan::PresentMode::SyncToVBlank;
                what = "FIFO (blocks on the display refresh)";
            } else if (pm && (std::strcmp(pm, "tear") == 0 || std::strcmp(pm, "immediate") == 0)) {
                mode = Vulkan::PresentMode::UnlockedForceTearing;
                what = "immediate (may tear)";
            }
            logf("paraLLEl-GS: present mode %s (ICORECOMP_GS_PRESENT=vsync|mailbox|tear)", what);
        }
        wsi->set_present_mode(mode);
    }
    /* WSI owns context creation here, so the CPU-device auto-fallback in
     * gs_pgs_context.h does not apply; a windowed run on a software device
     * needs ICORECOMP_GS_NO_DESCBUF=1 by hand. */
    Vulkan::ContextCreationFlags flags = kContextFlags;
    if (std::getenv("ICORECOMP_GS_NO_DESCBUF")) {
        flags &= ~Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT;
    }
    if (!wsi->init_context_from_platform(1, {}, flags)) {
        logf("paraLLEl-GS: WSI context init failed, falling back to headless");
        return;
    }
    if (!wsi->init_device() || !wsi->init_surface_swapchain()) {
        logf("paraLLEl-GS: WSI device/swapchain init failed, falling back to headless");
        return;
    }
    m_platform = std::move(platform);
    m_wsi = std::move(wsi);
    m_device = &m_wsi->get_device();
    m_wsi_active = true;
    pipeline_cache_load();
}

void RtPgs::present(const ParallelGS::ScanoutResult& scanout, double aspect) {
    present_frame(scanout, aspect);
    /* Runs on every path out of present_frame, including its early returns.
     * A window closed while the swapchain was unusable still has to reach the
     * host: the flag below and RT_PGS_VSYNC_WINDOW_CLOSED are the only
     * signals hw/gspriv.cpp exits on, so missing it leaves the process
     * running with no window and no way to quit. */
    if (!m_platform->alive(*m_wsi)) m_window_closed.store(true, std::memory_order_release);
}

void RtPgs::present_frame(const ParallelGS::ScanoutResult& scanout, double aspect) {
    /* Takes the host's resize notification, if any, into Granite's own flag.
     * On this thread, before begin_frame reads it. */
    m_platform->sync_from_host();
    if (!m_platform->presentable()) {
        /* begin_frame() would park this thread here; see presentable().
         * Pumping is a no-op off the window's own thread, where the host's
         * per-field pump is what delivers the restore and close events; it
         * still matters in the launcher phase, where this is that thread. */
        m_platform->poll_input();
        return;
    }

    /* Set before the call, not after it returns: Granite's WSI::begin_frame
     * (wsi.cpp) calls platform->poll_input() -- and so pump_events -- itself,
     * after acquiring the swapchain image but before begin_frame() returns
     * ("Poll after acquire as well for optimal latency"). The guard has to
     * be armed across that reentrant call, which is exactly why
     * pump_events may only queue events and call notify_quit/notify_resize;
     * anything else it calls lands here fatal. */
    m_in_frame = true;
    if (!m_wsi->begin_frame()) {
        m_in_frame = false;
        logf("paraLLEl-GS: WSI begin_frame failed");
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

    const int bb_w = int(backbuffer.get_width());
    const int bb_h = int(backbuffer.get_height());

    /* The rectangle rt_pgs_present_rect will report, published once at the
     * end of the two branches below so a reader on the other thread never
     * sees half of one present and half of the next. */
    int32_t rect_x = 0, rect_y = 0, rect_w = 0, rect_h = 0;

    if (scanout.image) {
        /* Presentation of the already-rendered scanout only: fit and filter
         * decide how it is scaled and sampled into the window backbuffer.
         * Nothing below this point can change what the game rendered. */
        int dst_w = bb_w, dst_h = bb_h;
        uint32_t fit = m_opts.fit;

        if (fit == RT_PGS_FIT_STRETCH) {
            dst_w = bb_w;
            dst_h = bb_h;
        } else if (fit == RT_PGS_FIT_INTEGER) {
            /* Largest integer n whose n * (the scanout image's own pixel
             * height) copy fits the backbuffer in BOTH dimensions; width
             * follows from the SAME display aspect the letterbox path below
             * derives from, so a tall narrow window must shrink n rather
             * than let the derived width spill past the backbuffer edge
             * (a negative blit offset is not a valid Vulkan region). */
            /* A raw field under display.deinterlace bob is half the frame
             * height; the integer step is still the frame's, so the step is
             * doubled for it and the picture keeps the same set of sizes in
             * every mode. */
            const int image_h = int(scanout.image->get_height());
            const bool raw_field = scanout.interlaced && image_h == int(scanout.internal_height);
            const int scanout_h = raw_field ? 2 * image_h : image_h;
            int n = scanout_h > 0 ? bb_h / scanout_h : 0;
            if (aspect > 0.0) {
                while (n >= 1 && std::lround(double(n * scanout_h) * aspect) > bb_w) --n;
            }
            if (n >= 1 && aspect > 0.0) {
                dst_h = n * scanout_h;
                dst_w = int(std::lround(double(dst_h) * aspect));
            } else {
                static bool warned_no_integer_fit = false;
                if (!warned_no_integer_fit) {
                    warned_no_integer_fit = true;
                    logf("paraLLEl-GS: display.fit=integer has no room for a 1x copy"
                         " (window %dx%d, scanout height %d); falling back to letterbox",
                         bb_w, bb_h, scanout_h);
                }
                fit = RT_PGS_FIT_LETTERBOX;
            }
        }
        if (fit == RT_PGS_FIT_LETTERBOX) {
            /* Letterbox: fit the scanout's display aspect inside the window.
             * scanout_display_aspect() explains why that is not the image's
             * own width:height. aspect <= 0 means the renderer reported no
             * mode, which vsync() has already logged; fill the window rather
             * than invent a ratio, as the pre-aspect code did whenever the
             * mode was zero. */
            dst_w = bb_w;
            dst_h = bb_h;
            if (aspect > 0.0) {
                if (double(bb_w) > double(bb_h) * aspect) {
                    dst_w = int(std::lround(double(bb_h) * aspect));
                } else {
                    dst_h = int(std::lround(double(bb_w) / aspect));
                }
            }
        }

        int x0 = (bb_w - dst_w) / 2;
        int y0 = (bb_h - dst_h) / 2;

        /* Bob (display.deinterlace bob): the renderer returned the field
         * itself instead of a composition, so this blit is the bob. The
         * vertical stretch to dst_h with the linear filter is the line
         * doubling; what is left is where the field sits on the raster.
         *
         * A CRT draws the odd field one raster line below the even one, which
         * is the same convention weave.frag uses when it composes: rows where
         * (y & 1) == phase come from the current field. So phase 1 moves down
         * half a frame line and phase 0 up half a line, in output pixels
         * dst_h / (4 * field rows). Without it both fields land on the same
         * rows and bob is a 30 Hz vertical judder of the whole picture.
         *
         * The test is the image height: a composed frame is twice
         * internal_height (fastmad_deinterlace, gs_renderer.cpp), a
         * high-resolution scanout is twice it as well, and only the raw field
         * skip_deinterlace returns is equal to it. At 960 output lines the
         * shift is one or two pixels.
         *
         * The rect has to stay inside the backbuffer (a negative blit offset
         * is not a valid Vulkan region). A picture that already fills the
         * window vertically, which is the default 4:3 window and every
         * display.fit=stretch window, has no room for the offset, so the
         * picture gives up one shifted row's worth of height at the top and
         * the bottom (two or three pixels at 960 lines, kept at the same
         * aspect) rather than drawing both fields on the same rows, which is
         * a 30 Hz judder of the whole picture. Logged once with the numbers. */
        if (scanout.interlaced && scanout.image->get_height() == scanout.internal_height) {
            const int shift = int(std::lround((double(scanout.interlace_phase) - 0.5) * double(dst_h)
                                              / (2.0 * double(scanout.image->get_height()))));
            const int room = shift < 0 ? -shift : shift;
            if (!(y0 - room >= 0 && y0 + room + dst_h <= bb_h) && room > 0) {
                const int old_w = dst_w, old_h = dst_h;
                dst_h -= 2 * room;
                if (aspect > 0.0) dst_w = int(std::lround(dst_h * aspect));
                x0 = (bb_w - dst_w) / 2;
                y0 = (bb_h - dst_h) / 2;
                static bool logged_bob_room = false;
                if (!logged_bob_room) {
                    logged_bob_room = true;
                    logf("paraLLEl-GS: display.deinterlace=bob: destination %dx%d fills the %dx%d window"
                         " vertically; shrunk to %dx%d so the half-line field offset of %d pixel(s) fits",
                         old_w, old_h, bb_w, bb_h, dst_w, dst_h, room);
                }
            }
            y0 += shift;
        }

        const VkFilter filter = m_opts.filter == RT_PGS_FILTER_NEAREST
            ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        cmd->blit_image(backbuffer, *scanout.image,
                        { x0, y0, 0 }, { dst_w, dst_h, 1 },
                        { 0, 0, 0 },
                        { int(scanout.image->get_width()), int(scanout.image->get_height()), 1 },
                        0, 0, 0, 0, 1, filter);

        /* Record what the blit actually used, for rt_pgs_present_rect. Taken
         * after the fit has been resolved, so a letterbox fallback from the
         * integer path is reflected rather than the requested fit. */
        rect_x = int32_t(x0);
        rect_y = int32_t(y0);
        rect_w = int32_t(dst_w);
        rect_h = int32_t(dst_h);
    } else {
        /* No scanout this field: the backbuffer holds the clear and,
         * possibly, the overlay. Nothing maps window pixels to guest pixels,
         * so the rectangle is reported empty. Not the previous field's
         * rectangle, which no longer describes what is on screen, and not the
         * whole backbuffer either: a caller mapping a cursor into that would
         * get a position on a picture the guest never drew. An empty
         * rectangle is what every reader already treats as "no picture"
         * (guest/menu_nav.cpp, ui/ui_menu_cursor.cpp both test w and h). The
         * rect_* locals are already zero. */
    }
    {
        std::lock_guard<std::mutex> lk(m_present_rect_mu);
        m_present_x = rect_x;
        m_present_y = rect_y;
        m_present_w = rect_w;
        m_present_h = rect_h;
        m_present_bb_w = int32_t(bb_w);
        m_present_bb_h = int32_t(bb_h);
    }

    if (!m_overlay_cmds.empty()) {
        /* Overlay pass: draw the retained frame on top of what was just
         * blitted, instead of presenting straight from TRANSFER_DST.
         *
         * The render pass declares its own layouts for a swapchain
         * attachment and we do not get to pick them (Granite
         * vulkan/render_pass.cpp lines 238-248): with loadOp LOAD the
         * initialLayout is the image's swapchain layout, and the finalLayout
         * is always that same swapchain layout. Granite's WSI records the
         * swapchain layout as VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
         * (Device::init_swapchain's default layout argument, device.hpp),
         * so the pass wants PRESENT_SRC_KHR going in and leaves the image in
         * PRESENT_SRC_KHR coming out. Hand it PRESENT_SRC_KHR here and let
         * the pass be the last transition: no post-pass barrier, because
         * there is nothing left to transition from COLOR_ATTACHMENT_OPTIMAL,
         * the image was never in it. */
        cmd->image_barrier(backbuffer,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                           VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        Vulkan::RenderPassInfo rp = device.get_swapchain_render_pass(Vulkan::SwapchainRenderPass::ColorOnly);
        /* get_swapchain_render_pass defaults to clear_attachments = ~0u
         * (clear-all); left as-is that would erase the scanout blit just
         * written above. MANDATORY: load what's there, don't clear it. */
        rp.clear_attachments = 0;
        rp.load_attachments = 1u << 0;
        rp.store_attachments = 1u << 0;
        cmd->begin_render_pass(rp);
        draw_overlay(*cmd);
        cmd->end_render_pass();
    } else {
        cmd->image_barrier(backbuffer,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                           VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_NONE, 0);
    }
    device.submit(cmd);

    if (!m_wsi->end_frame()) {
        logf("paraLLEl-GS: WSI end_frame failed");
    }
    m_in_frame = false;
}

#endif /* ICORECOMP_PGS_SDL */
