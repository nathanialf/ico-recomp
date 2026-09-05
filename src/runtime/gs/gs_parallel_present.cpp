/* gs/gs_parallel_present.cpp: device, window and swapchain of the paraLLEl-GS
 * shim.
 *
 * Part of libicorecomp-parallel-gs; see gs_parallel_lib.cpp for the library
 * overview and gs_parallel_impl.h for the RtPgs type. Holds headless and
 * windowed device creation, present mode selection, the per-field present
 * (fit and filter of the scanout into the window backbuffer), and the window
 * control entry points behind rt_pgs_notify_quit and friends.
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

void RtPgs::notify_quit() {
#ifdef ICORECOMP_PGS_SDL
    if (m_platform) m_platform->handle_quit();
#endif
    /* Recorded here as well as at the next present: the host polls this from
     * its own thread to decide when to exit, and a quit raised while the
     * consumer cannot present would otherwise never be seen. */
    const bool first = !m_window_closed.exchange(true, std::memory_order_acq_rel);
    /* This is the one entry that ends the run on purpose, and it is the only
     * thing that ever clears the platform's alive flag, so every other
     * "window closed" line in this shim is downstream of a call to this one.
     * Warn and not info: a run that ends has to say what ended it, and the
     * host calls this from its window sink on an SDL quit
     * (host/window_service.cpp), which is a fact no other line carries. */
    if (first) {
        warnf("paraLLEl-GS: the host notified a window quit (rt_pgs_notify_quit); the run is"
              " ending and no further field reaches the window");
    }
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
    /* Granite's logging interface is thread local, and from here on this is
     * the thread that runs every swapchain call, so its failures would
     * otherwise be the only ones not routed into the host log. */
    install_granite_log();
    logf("paraLLEl-GS: GS consumer thread bound (Granite thread index 0); GIF transfers,"
         " vsync and present run on it from here");
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

/* Swapchain texel layout for the screenshot converter and the boot trace
 * sample. Granite's WSI asked for BackbufferFormat::UNORM takes the first
 * of the five formats the driver reports that it accepts, in the driver's
 * own order: R8G8B8A8_UNORM, B8G8R8A8_UNORM, A2B10G10R10_UNORM_PACK32,
 * A2R10G10B10_UNORM_PACK32 and A8B8G8R8_UNORM_PACK32
 * (find_suitable_present_format, Granite/vulkan/wsi.cpp:2190-2199 at the
 * pinned revision). There is no preference in that loop; the one place a
 * format is forced is the DXGI interop presenter, which asks for
 * A2B10G10R10_UNORM_PACK32 outright (wsi.cpp:228) and which this port does
 * not reach (init_windowed sets extra usage flags, and
 * init_surface_swapchain_dxgi refuses those). The sRGB spellings below are
 * unreachable from BackbufferFormat::UNORM; they are kept so a future
 * BackbufferFormat::sRGB does not silently lose the screenshot.
 * Measured 2026-09-05 in the
 * user's log: "Created swapchain 2560 x 1920 (fmt: 64)", VkFormat 64 being
 * VK_FORMAT_A2B10G10R10_UNORM_PACK32; the earlier byte-order-only gate
 * refused it, which silently dropped every screenshot and the boot trace
 * sample on that machine. All eight spellings are four bytes a texel, which
 * is what the staging buffers are sized at. The 10-bit channels are published
 * as their top eight bits and the 2-bit alpha as 0, 85, 170 or 255; the
 * sRGB variants hold the same bytes as the UNORM ones, and a PNG has no
 * colour space field this port writes, so every case gives the file the
 * user was looking at. Anything else is refused rather than reinterpreted:
 * publishing float texels as if they were RGBA8 would write a wrong
 * picture with no way for anyone to tell. */
enum class ShotLayout { RGBA8, BGRA8, A2B10G10R10, A2R10G10B10 };

static bool shot_format_layout(VkFormat format, ShotLayout* layout) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    /* Same four bytes in the same order as R8G8B8A8 on a little-endian
     * host: the PACK32 name orders the components from the most significant
     * end, so byte 0 is R. Granite accepts this spelling for
     * BackbufferFormat::UNORM (wsi.cpp:2196), so a driver that reports only
     * it would otherwise cost every screenshot for no reason. */
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        *layout = ShotLayout::RGBA8;
        return true;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        *layout = ShotLayout::BGRA8;
        return true;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        *layout = ShotLayout::A2B10G10R10;
        return true;
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        *layout = ShotLayout::A2R10G10B10;
        return true;
    default:
        return false;
    }
}

/* One texel, four bytes in from the staging buffer, RGBA8 out. The packed
 * formats are one little-endian 32-bit word: for A2B10G10R10 red is bits
 * 0..9, green 10..19, blue 20..29 and alpha 30..31 (the Vulkan name lists
 * the components from the most significant end); A2R10G10B10 has red and
 * blue swapped. */
static inline void shot_convert_texel(ShotLayout layout, const uint8_t* px, uint8_t out[4]) {
    switch (layout) {
    case ShotLayout::RGBA8:
        out[0] = px[0]; out[1] = px[1]; out[2] = px[2]; out[3] = px[3];
        return;
    case ShotLayout::BGRA8:
        out[0] = px[2]; out[1] = px[1]; out[2] = px[0]; out[3] = px[3];
        return;
    case ShotLayout::A2B10G10R10:
    case ShotLayout::A2R10G10B10: {
        uint32_t w;
        std::memcpy(&w, px, 4);
        const uint8_t lo = uint8_t((w >> 2) & 0xff);
        const uint8_t mid = uint8_t((w >> 12) & 0xff);
        const uint8_t hi = uint8_t((w >> 22) & 0xff);
        const uint8_t a = uint8_t(((w >> 30) & 3u) * 85u);
        if (layout == ShotLayout::A2B10G10R10) { out[0] = lo; out[2] = hi; }
        else { out[0] = hi; out[2] = lo; }
        out[1] = mid; out[3] = a;
        return;
    }
    }
}

void RtPgs::request_screenshot(uint32_t slots) {
    /* Consumer thread: the host routes this through its GS command ring, so
     * it arrives in order with the GIF and vsync traffic and the arm lands on
     * the field the user pressed the key on. No frame guard: it writes one
     * plain member and touches neither the swapchain nor the GS interface,
     * which is what lets a request enqueued from the menu apply on the very
     * next present. */
    if (slots != 1 && slots != RT_PGS_SHOT_SLOTS) {
        warnf("paraLLEl-GS: rt_pgs_request_screenshot slots = %u is not 1 or %u; arming 1",
              slots, RT_PGS_SHOT_SLOTS);
        slots = 1;
    }
    /* A new arm supersedes any image still sitting in the slots. Such an
     * image belongs to an earlier arm the host gave up on: host/screenshot.cpp
     * disarms after two seconds while the library's arm stays live, so the
     * next field with a picture publishes into a slot nobody is waiting for.
     * Left there it would be handed to this arm's first take and written under
     * a fresh timestamp, showing a scene from minutes ago with nothing in the
     * log to say so. Dropped here, and named, so the discard is visible. */
    {
        std::lock_guard<std::mutex> lk(m_shot_mu);
        for (uint32_t slot = 0; slot < RT_PGS_SHOT_SLOTS; ++slot) {
            if (m_shot_ready[slot].rgba.empty()) continue;
            warnf("paraLLEl-GS: rt_pgs_request_screenshot: slot %u still held a %ux%u image from an"
                  " arm the host abandoned; dropped rather than served as this capture",
                  slot, m_shot_ready[slot].width, m_shot_ready[slot].height);
            m_shot_ready[slot] = ShotReady{};
        }
    }
    m_shot_slots = slots;
}

size_t RtPgs::take_screenshot(uint32_t slot, uint32_t* w, uint32_t* h,
                              uint8_t* dst, size_t dst_bytes) {
    if (slot >= RT_PGS_SHOT_SLOTS) return 0;
    /* Same arrangement as present_rect above: published under this mutex by
     * the consumer thread, read here from the host's EE thread, so a caller
     * never gets a size from one capture and rows from another. */
    std::lock_guard<std::mutex> lk(m_shot_mu);
    ShotReady& r = m_shot_ready[slot];
    if (r.rgba.empty()) return 0;
    if (w) *w = r.width;
    if (h) *h = r.height;
    if (!dst) return r.rgba.size();          /* size query, slot kept */
    if (dst_bytes < r.rgba.size()) {
        warnf("paraLLEl-GS: rt_pgs_take_screenshot slot %u needs %zu bytes, the caller offered"
              " %zu; the image is kept", slot, r.rgba.size(), dst_bytes);
        return 0;
    }
    const size_t bytes = r.rgba.size();
    std::memcpy(dst, r.rgba.data(), bytes);
    r.rgba.clear();
    r.rgba.shrink_to_fit();
    r.width = 0;
    r.height = 0;
    return bytes;
}

/* One warn when the window stops being fed and one info when it starts
 * again, with the number of fields that never reached it in between. The
 * trigger for this shape: a run that ended with an error on screen had
 * nothing in the log at the default level saying what stopped it, and a
 * line a field is not the alternative (a minimized window skips sixty a
 * second). The reason is compared by text rather than by pointer because
 * the phrases come from three translation units. */
void RtPgs::note_present_skipped(const char* reason) {
    ++m_present_skipped;
    ++m_present_skipped_total;
    if (m_present_skip_reason && std::strcmp(m_present_skip_reason, reason) == 0) return;
    m_present_skip_reason = reason;
    m_present_skipped = 1;
    warnf("paraLLEl-GS: nothing is reaching the window: %s. Further skipped fields are"
          " counted, not logged.", reason);
}

void RtPgs::note_present_resumed() {
    if (!m_present_skip_reason) return;
    logf("paraLLEl-GS: presenting again after %llu field(s) that did not reach the window (%s)",
         (unsigned long long)m_present_skipped, m_present_skip_reason);
    m_present_skip_reason = nullptr;
    m_present_skipped = 0;
}

void RtPgs::drain_screenshots() {
    for (uint32_t slot = 0; slot < RT_PGS_SHOT_SLOTS; ++slot) {
        ShotPending& p = m_shot[slot];
        if (!p.buffer) continue;
        /* The wait the capture deliberately did not do. By the time this
         * runs the submit that filled the buffer is a whole present old, so
         * it returns without blocking; doing it where the copy was recorded
         * would have stalled the GS worker on the field being displayed. */
        if (p.fence) p.fence->wait();

        /* capture_backbuffer refused any format this does not know, so the
         * answer here is only ever the layout. */
        ShotLayout layout = ShotLayout::RGBA8;
        if (!shot_format_layout(p.format, &layout)) {
            warnf("paraLLEl-GS: screenshot: slot %u holds VkFormat %d, which capture_backbuffer"
                  " should never have copied; dropped", slot, int(p.format));
            p = ShotPending{};
            continue;
        }

        const uint8_t* px = static_cast<const uint8_t*>(
            m_device->map_host_buffer(*p.buffer, Vulkan::MEMORY_ACCESS_READ_BIT));
        if (!px) {
            warnf("paraLLEl-GS: screenshot: the staging buffer would not map; capture dropped");
            p = ShotPending{};
            continue;
        }
        /* copy_image_to_buffer was given row_length 0, so the rows already
         * arrive tightly packed from the top; the only work left is the
         * texel layout. */
        const size_t texels = size_t(p.width) * size_t(p.height);
        std::vector<uint8_t> rgba(texels * 4);
        if (layout == ShotLayout::RGBA8) {
            std::memcpy(rgba.data(), px, rgba.size());
        } else {
            for (size_t i = 0; i < texels; ++i) {
                shot_convert_texel(layout, px + i * 4, rgba.data() + i * 4);
            }
        }
        m_device->unmap_host_buffer(*p.buffer, Vulkan::MEMORY_ACCESS_READ_BIT);

        {
            std::lock_guard<std::mutex> lk(m_shot_mu);
            m_shot_ready[slot].rgba = std::move(rgba);
            m_shot_ready[slot].width = p.width;
            m_shot_ready[slot].height = p.height;
        }
        p = ShotPending{};
    }
}

void RtPgs::set_present_mode(uint32_t mode) {
    if (m_in_frame) {
        fatalf("paraLLEl-GS: rt_pgs_set_present_mode called while a frame is in flight;"
               " settings must apply at the field boundary");
    }
#ifdef ICORECOMP_PGS_SDL
    if (!m_wsi_active) {
        warnf("paraLLEl-GS: rt_pgs_set_present_mode ignored (headless, no window)");
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
    warnf("paraLLEl-GS: rt_pgs_set_present_mode ignored (built without window support)");
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
    logf("paraLLEl-GS: deinterlace %s (compiled in; the display.deinterlace key"
         " was retired 2026-09-04)",
         rt_pgs_deinterlace_name(deinterlace));
}

void RtPgs::set_widescreen_aspect(double aspect) {
    if (m_in_frame) {
        fatalf("paraLLEl-GS: rt_pgs_set_widescreen_aspect called while a frame is in flight;"
               " settings must apply at the field boundary");
    }
    /* Only stores; RtPgs::vsync reads it fresh every field. Not validated
     * here: the host derives it from the window or from 16/9 and a value
     * this library decided to correct would be a divergence from what the
     * host asked for. A non-finite or non-positive one is treated as off,
     * which is what 0 means. */
    m_widescreen_aspect = aspect;
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
        warnf("paraLLEl-GS: pipeline cache: %s rejected by Granite; starting from an empty cache "
              "and rewriting it at exit", m_pipeline_cache_path.c_str());
        blob.clear();
        ok = m_device->init_pipeline_cache(nullptr, 0);
    }
    if (!ok) {
        /* No cache object exists, so there is nothing to serialize either:
         * clear the path so the store at exit does not read an uninitialized
         * cache and overwrite a good file with it. */
        warnf("paraLLEl-GS: pipeline cache: the device would not create one; pipelines compile "
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
        warnf("paraLLEl-GS: pipeline cache: vkGetPipelineCacheData failed; not stored");
        return;
    }
    std::error_code ec;
    const std::filesystem::path path(m_pipeline_cache_path);
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        warnf("paraLLEl-GS: pipeline cache: cannot create %s (%s); not stored",
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
            warnf("paraLLEl-GS: pipeline cache: write to %s failed; not stored", tmp.string().c_str());
            std::filesystem::remove(tmp, ec);
            return;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        warnf("paraLLEl-GS: pipeline cache: rename to %s failed (%s); not stored",
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
    m_descbuf_disabled = ctx.descriptor_buffer_disabled;
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
    logf("paraLLEl-GS: headless Vulkan device (the host passed no window); "
         "set ICORECOMP_GS_SCREENSHOT=/path/out.ppm to capture the scanout");
}

/* See rt_pgs_present_pump in gs_parallel_api.h.
 *
 * The whole of the decoupling is here: rt_pgs_vsync latches a finished field
 * and this decides when it reaches the window. Nothing about the guest's
 * timing passes through it. It is called from the GS command ring's consumer
 * (gs/gs_threaded.cpp) right after every vsync record and again from the
 * consumer's park loop while the ring is empty, so a repeat runs on that
 * thread while the EE is asleep in the audio pacer.
 *
 * Headless, or before the first field, there is nothing to present and the
 * call is a cheap no-op. */
uint32_t RtPgs::present_pump(double max_hz, uint64_t* serial) {
#ifdef ICORECOMP_PGS_SDL
    if (serial) *serial = m_latest_serial;
    /* Neither of these two is a failure: headless has no window to present
     * into (init_headless said so at startup), and serial 0 is the window
     * between the first pump and the first finished field. Nothing is
     * skipped, so nothing is counted. */
    if (!m_wsi_active || m_latest_serial == 0) return 0;
    if (m_in_frame) {
        fatalf("paraLLEl-GS: rt_pgs_present_pump called while a frame is in flight;"
               " it may not be called reentrantly (e.g. from pump_events)");
    }
    const auto now = std::chrono::steady_clock::now();
    bool repeat = false;
    if (m_presented_serial != m_latest_serial) {
        /* A new field. Presented whatever the rate is: the rate is a floor on
         * how often the window is refreshed, never a ceiling that would drop
         * a picture the guest produced. */
    } else if (max_hz > 0.0) {
        const double elapsed = std::chrono::duration<double>(now - m_last_present_at).count();
        /* The rate gate. Not a skip and not logged: the picture on the window
         * is already this serial, so nothing is lost, and a line here would
         * be one per pump. What the rate actually achieved is in the present
         * counters (rt_pgs_present_timings) and the `present` verbose
         * channel. */
        if (elapsed < 1.0 / max_hz) return 0;
        repeat = true;
    } else {
        /* max_hz 0: one present per new serial and no repeats, which is what
         * every field did before this entry point existed. Same as above,
         * the window already holds this serial. */
        return 0;
    }

    const auto t_present = std::chrono::steady_clock::now();
    const bool presented = present(m_latest_scanout, m_latest_aspect);
    m_present_ns.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - t_present).count(),
                           std::memory_order_relaxed);
    /* Nothing reached the swapchain: the window is not presentable, or
     * begin_frame failed. The serial, the pacing clock and the counters all
     * stay where they were, so a minimized window neither reports presents it
     * did not make nor owes a burst of them when it comes back (the first
     * present after the restore is a new one, timed from that moment). This
     * is what keeps the `present` verbose channel and the profiler's present
     * line honest, which is the measurement display.present_rate is judged
     * on. */
    if (!presented) return 0;
    m_presented_serial = m_latest_serial;
    /* Stamped after the present, not before it: the interval this paces is
     * between one picture appearing and the next, and a present that took
     * longer than the interval must not immediately owe another. */
    m_last_present_at = std::chrono::steady_clock::now();
    m_presents.fetch_add(1, std::memory_order_relaxed);
    if (repeat) m_present_repeats.fetch_add(1, std::memory_order_relaxed);
    return RT_PGS_PUMP_PRESENTED | (repeat ? RT_PGS_PUMP_REPEAT : 0u);
#else
    (void)max_hz;
    if (serial) *serial = 0;
    return 0;
#endif
}

#ifdef ICORECOMP_PGS_SDL

void RtPgs::init_windowed() {
    /* The window is the host's. It was created with SDL_WINDOW_VULKAN at its
     * configured size before this instance existed (host/window_service.cpp),
     * and this platform adopts it: no SDL_Init, no SDL_CreateWindow, and no
     * size fallback here, because nothing here decides a size any more. The
     * window_width/height options are now only what the host sized its own
     * window with, kept in the struct so a log can say what was asked for. */
    auto platform = std::make_unique<SdlWsiPlatform>(*this);
    if (!platform->adopt((SDL_Window*)m_opts.host_window)) {
        /* The only way adopt fails is a null window, which the caller has
         * already tested for, so this is a programming error rather than a
         * missing video driver. Silence here used to leave a run that asked
         * for a window running headless with nothing in the log. */
        errorf("paraLLEl-GS: the host window could not be adopted by the WSI platform;"
               " falling back to headless and nothing reaches the screen");
        return;
    }

    auto wsi = std::make_unique<Vulkan::WSI>();
    wsi->set_platform(platform.get());
    wsi->set_backbuffer_format(Vulkan::BackbufferFormat::UNORM);

    /* The present path does three things to the swapchain image that a plain
     * colour attachment may not do: vkCmdClearColorImage on the letterbox
     * border, vkCmdBlitImage of the scanout picture into it, and
     * vkCmdCopyImageToBuffer for the screenshot and the boot trace's sample.
     * The first two need TRANSFER_DST, the third needs TRANSFER_SRC.
     * Granite creates the swapchain with
     * "info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | current_extra_usage"
     * (Granite/vulkan/wsi.cpp:2748), and current_extra_usage is whatever
     * WSI::set_extra_usage_flags last stored (wsi.cpp:2000), so this call has
     * to happen before the swapchain is built. Granite masks off anything the
     * surface does not support and logs it (wsi.cpp:2579-2583), and it turns
     * prerotate off when extra usage is set (wsi.cpp:2573-2577), which this
     * port does not use. Without this the clear, the blit and the copy are
     * all usage violations that no validation layer sees, because the shim
     * sets GRANITE_VULKAN_NO_VALIDATION=1 unless ICORECOMP_VVL=1.
     *
     * One knock-on, stated because it is not obvious: Granite's DXGI interop
     * presenter refuses any swapchain with extra usage
     * (init_surface_swapchain_dxgi, wsi.cpp:216-217), so setting these bits
     * takes that path out on a build that has it. The shipped Windows build
     * is the mingw cross build, which already forces
     * GRANITE_VULKAN_DXGI_INTEROP off (CMakeLists.txt), so nothing shipped
     * changes; an MSVC Windows build now takes the plain Vulkan swapchain
     * instead of the interop one. */
    wsi->set_extra_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

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
            logf("paraLLEl-GS: present mode %s (compiled in as mailbox; the display.present"
                 " key was retired 2026-09-04 and ICORECOMP_GS_PRESENT still selects another"
                 " one, resolved by the host)", what);
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
        m_descbuf_disabled = true;
        logf("paraLLEl-GS: descriptor-buffer path disabled (ICORECOMP_GS_NO_DESCBUF=1;"
             " see gs_pgs_context.h)");
    }
    if (!wsi->init_context_from_platform(1, {}, flags)) {
        errorf("paraLLEl-GS: WSI context init failed (no Vulkan device for this surface),"
               " falling back to headless and nothing reaches the screen");
        return;
    }
    if (!wsi->init_device() || !wsi->init_surface_swapchain()) {
        errorf("paraLLEl-GS: WSI device/swapchain init failed for a %ux%u surface,"
               " falling back to headless and nothing reaches the screen",
               platform->get_surface_width(), platform->get_surface_height());
        return;
    }
    m_platform = std::move(platform);
    m_wsi = std::move(wsi);
    m_device = &m_wsi->get_device();
    m_wsi_active = true;
    pipeline_cache_load();
}

/* Granite hands both swapchain calls to the caller as a bare bool. What
 * each of them means, read off Granite/vulkan/wsi.cpp:
 *
 *   begin_frame  false when no swapchain image could be acquired: the
 *                surface is unusable or the swapchain could not be rebuilt
 *                at the size the platform reports.
 *   end_frame    false only when vkQueuePresentKHR returned a negative
 *                VkResult, and by then Granite has already torn the
 *                swapchain down (it is rebuilt at the next begin_frame).
 *
 * Neither exposes the VkResult itself. Granite's own line for it carries
 * that, and install_granite_log routes it into this log at error level, so
 * it sits beside these lines rather than on a stderr this process may not
 * own. What is left for these two to report is the state the call was made
 * in, which is what separates a resize from a minimize from a lost device.
 * One line each per run, then a count: both are per-field calls. */
void RtPgs::note_begin_frame_failed(const char* where) {
    ++m_begin_frame_failures;
    if (!m_begin_frame_logged) {
        m_begin_frame_logged = true;
        int32_t bb_w = 0, bb_h = 0;
        {
            std::lock_guard<std::mutex> lk(m_present_rect_mu);
            bb_w = m_present_bb_w;
            bb_h = m_present_bb_h;
        }
        const char* why = m_platform->not_presentable_reason();
        warnf("paraLLEl-GS: WSI begin_frame failed: no swapchain image was acquired, so %s is"
              " dropped. Surface asked for %ux%u, last swapchain backbuffer %dx%d,"
              " present mode %s, window %s, resize %s. Granite's own line above or below"
              " carries the driver's reason. Further failures are counted, not logged.",
              where, m_platform->get_surface_width(), m_platform->get_surface_height(),
              bb_w, bb_h, present_mode_name(m_wsi->get_present_mode()),
              why ? why : "presentable",
              m_platform->should_resize() ? "pending" : "not pending");
    }
    note_present_skipped("WSI begin_frame acquired no swapchain image");
}

void RtPgs::note_end_frame_failed(const char* where) {
    ++m_end_frame_failures;
    if (!m_end_frame_logged) {
        m_end_frame_logged = true;
        const char* why = m_platform->not_presentable_reason();
        warnf("paraLLEl-GS: WSI end_frame failed: vkQueuePresentKHR returned an error, so %s was"
              " composed but did not reach the display, and Granite has torn the swapchain"
              " down; the next field rebuilds it. Surface %ux%u, present mode %s, window %s,"
              " resize %s. Granite's own line above or below carries the VkResult."
              " Further failures are counted, not logged.",
              where, m_platform->get_surface_width(), m_platform->get_surface_height(),
              present_mode_name(m_wsi->get_present_mode()),
              why ? why : "presentable",
              m_platform->should_resize() ? "pending" : "not pending");
    }
}

/* The store that ends the run. hw/gspriv.cpp exits on this flag at the next
 * field boundary, so a silent store is a process that stops with nothing in
 * the log to say why, which is exactly what a user saw.
 *
 * What alive() consults, read off SdlWsiPlatform above: one atomic bool,
 * m_alive, initialised true and cleared in exactly one place,
 * handle_quit(), whose only caller is RtPgs::notify_quit, whose only caller
 * is rt_pgs_notify_quit from the host's window sink. So a line from here
 * with no "the host notified a window quit" line before it means the flag
 * was cleared by something this shim does not know about, and the message
 * says so rather than implying the player closed the window. */
void RtPgs::note_window_closed(const char* site) {
    if (m_window_closed.exchange(true, std::memory_order_acq_rel)) return;
    warnf("paraLLEl-GS: %s found the window gone (the WSI platform's alive flag is false) and"
          " the run ends at the next field boundary. That flag is only ever cleared by"
          " rt_pgs_notify_quit, so unless a window-quit line precedes this one, nothing the"
          " player did asked for it. Surface %ux%u, window %s.",
          site, m_platform->get_surface_width(), m_platform->get_surface_height(),
          m_platform->not_presentable_reason() ? m_platform->not_presentable_reason()
                                               : "presentable");
}

bool RtPgs::present(const ParallelGS::ScanoutResult& scanout, double aspect) {
    const bool presented = present_frame(scanout, aspect);
    /* Runs on every path out of present_frame, including its early returns.
     * A window closed while the swapchain was unusable still has to reach the
     * host: the flag below and RT_PGS_VSYNC_WINDOW_CLOSED are the only
     * signals hw/gspriv.cpp exits on, so missing it leaves the process
     * running with no window and no way to quit. */
    if (!m_platform->alive(*m_wsi)) note_window_closed("the guest field present");
    return presented;
}

/* Boot trace, presented-colour stream: record the copy. The shape of
 * capture_backbuffer below (barriers, CachedHost staging, fence deferred to
 * the next present), for a 16x16 block at the backbuffer centre. */
void RtPgs::sample_backbuffer(Vulkan::CommandBuffer& cmd, const Vulkan::Image& backbuffer,
                              VkImageLayout layout, int bb_w, int bb_h, bool have_image) {
    constexpr int kSide = 16;
    if (m_boot_sample.buffer) return; /* the previous one has not drained */
    if (bb_w < kSide || bb_h < kSide) return;
    ShotLayout texel_layout = ShotLayout::RGBA8;
    if (!shot_format_layout(backbuffer.get_format(), &texel_layout)) {
        if (!m_sample_format_logged) {
            m_sample_format_logged = true;
            warnf("paraLLEl-GS: boot trace: the swapchain format is VkFormat %d, which the"
                  " converter has no texel layout for; the presented-colour sample is refused",
                  int(backbuffer.get_format()));
        }
        return;
    }

    Vulkan::BufferCreateInfo bi = {};
    bi.domain = Vulkan::BufferDomain::CachedHost;
    bi.size = VkDeviceSize(kSide) * VkDeviceSize(kSide) * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    Vulkan::BufferHandle buf = m_device->create_buffer(bi, nullptr);
    if (!buf) return;

    const int32_t x = int32_t(bb_w / 2 - kSide / 2);
    const int32_t y = int32_t(bb_h / 2 - kSide / 2);
    cmd.image_barrier(backbuffer, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                      VK_ACCESS_2_MEMORY_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    const VkImageSubresourceLayers layers = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    cmd.copy_image_to_buffer(*buf, backbuffer, 0, { x, y, 0 },
                             { uint32_t(kSide), uint32_t(kSide), 1 }, 0, 0, layers);
    cmd.image_barrier(backbuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout,
                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                      VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    cmd.barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);

    m_boot_sample.buffer = std::move(buf);
    m_boot_sample.fence = {};
    m_boot_sample.field = m_vsyncs;
    m_boot_sample.image = have_image;
    m_boot_sample.format = backbuffer.get_format();
}

/* Boot trace, presented-colour stream: read the copy back and log it when
 * its colour class changes. Class thresholds: every channel above 240 is
 * white, every channel below 16 is black, anything else is a picture. */
void RtPgs::drain_boot_sample() {
    BootSample& p = m_boot_sample;
    if (!p.buffer) return;
    if (p.fence) p.fence->wait();
    /* Same guard, and the same reasoning, as drain_screenshots above:
     * sample_backbuffer refused any format this does not know, so a failure
     * here means a format changed between the copy and the drain. Drop the
     * sample with a line rather than converting it as if it were RGBA8. */
    ShotLayout layout = ShotLayout::RGBA8;
    if (!shot_format_layout(p.format, &layout)) {
        warnf("paraLLEl-GS: boot trace: field %llu was sampled as VkFormat %d, which"
              " sample_backbuffer should never have copied; dropped",
              (unsigned long long)p.field, int(p.format));
        p = BootSample{};
        return;
    }
    const uint8_t* px = static_cast<const uint8_t*>(
        m_device->map_host_buffer(*p.buffer, Vulkan::MEMORY_ACCESS_READ_BIT));
    if (!px) {
        warnf("paraLLEl-GS: boot trace: the sample staging buffer would not map; field %llu"
              " has no presented-colour line", (unsigned long long)p.field);
        p = BootSample{};
        return;
    }
    uint32_t sum[3] = { 0, 0, 0 };
    uint8_t lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 };
    for (size_t i = 0; i < 256; ++i) {
        uint8_t c[4];
        shot_convert_texel(layout, px + i * 4, c);
        for (int k = 0; k < 3; ++k) {
            sum[k] += c[k];
            if (c[k] < lo[k]) lo[k] = c[k];
            if (c[k] > hi[k]) hi[k] = c[k];
        }
    }
    m_device->unmap_host_buffer(*p.buffer, Vulkan::MEMORY_ACCESS_READ_BIT);
    const uint32_t mean[3] = { sum[0] / 256, sum[1] / 256, sum[2] / 256 };
    int cls = 2;
    if (mean[0] < 16 && mean[1] < 16 && mean[2] < 16) cls = 0;
    else if (mean[0] > 240 && mean[1] > 240 && mean[2] > 240) cls = 1;
    static const char* const kClass[3] = { "black", "white", "neither black nor white" };
    if (cls != m_boot_class) {
        m_boot_class = cls;
        logf("paraLLEl-GS: boot trace: field %llu presented %s at the window centre"
             " (16x16 mean rgb %u,%u,%u, min %u,%u,%u, max %u,%u,%u; %s)",
             (unsigned long long)p.field, kClass[cls], mean[0], mean[1], mean[2],
             lo[0], lo[1], lo[2], hi[0], hi[1], hi[2],
             p.image ? "scanout image blitted" : "no scanout image, the clear");
    }
    if (p.field >= kBootTraceFields) {
        logf("paraLLEl-GS: boot trace: presented-colour trace ends at field %llu",
             (unsigned long long)p.field);
    }
    p = BootSample{};
}

void RtPgs::capture_backbuffer(Vulkan::CommandBuffer& cmd, const Vulkan::Image& backbuffer,
                               VkImageLayout layout, uint32_t slot,
                               int32_t x, int32_t y, int32_t w, int32_t h) {
    if (slot >= RT_PGS_SHOT_SLOTS || w <= 0 || h <= 0) return;
    ShotPending& p = m_shot[slot];
    /* A slot still holding an undrained copy means the previous capture has
     * not been read back yet. Nothing is queued behind it: the host arms one
     * field at a time, and drain_screenshots at the top of this present has
     * already emptied anything from an earlier one. */
    if (p.buffer) return;

    /* The format gate, before the copy and not after it. The staging buffer
     * below is sized at four bytes a texel, so a swapchain format wider than
     * that would have the GPU write past its end; and a format the converter
     * has no byte order for could only be published as if it were RGBA8,
     * which would be a wrong picture with no way for anyone to tell. Granite
     * is asked for BackbufferFormat::UNORM, so neither should happen, and
     * this refuses the capture rather than reporting on one that already
     * ran. drain_screenshots then only ever converts a format this knows. */
    ShotLayout texel_layout = ShotLayout::RGBA8;
    if (!shot_format_layout(backbuffer.get_format(), &texel_layout)) {
        if (!m_shot_format_logged) {
            m_shot_format_logged = true;
            warnf("paraLLEl-GS: screenshot: the swapchain format is VkFormat %d, which this"
                  " converter has no texel layout for; the capture is refused rather than copied"
                  " out and written as if it were RGBA8", int(backbuffer.get_format()));
        }
        return;
    }

    /* Same shape as rt_gs_write_scanout_ppm (gs_readback.h) minus its fence
     * wait: this runs on the GS worker thread inside the field the user is
     * looking at, so the wait is deferred to drain_screenshots. */
    Vulkan::BufferCreateInfo bi = {};
    bi.domain = Vulkan::BufferDomain::CachedHost;
    bi.size = VkDeviceSize(w) * VkDeviceSize(h) * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    Vulkan::BufferHandle buf = m_device->create_buffer(bi, nullptr);
    if (!buf) {
        warnf("paraLLEl-GS: screenshot: a %dx%d staging buffer could not be allocated;"
              " this capture is dropped", w, h);
        return;
    }

    /* The image is handed back in the layout it came in with, so everything
     * after this point in present_frame is unchanged by a capture being
     * armed. ALL_COMMANDS on the outer side of each barrier because this is
     * called from two places with two different layouts and producers (the
     * scanout blit before the overlay pass, the overlay pass itself after
     * it); the copy is one region per field, so the coarse stage costs
     * nothing measurable and cannot be wrong. */
    cmd.image_barrier(backbuffer, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                      VK_ACCESS_2_MEMORY_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    const VkImageSubresourceLayers layers = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    cmd.copy_image_to_buffer(*buf, backbuffer, 0, { x, y, 0 },
                             { uint32_t(w), uint32_t(h), 1 }, 0, 0, layers);
    cmd.image_barrier(backbuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout,
                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                      VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    cmd.barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);

    p.buffer = std::move(buf);
    p.width = uint32_t(w);
    p.height = uint32_t(h);
    p.format = backbuffer.get_format();
}

bool RtPgs::present_frame(const ParallelGS::ScanoutResult& scanout, double aspect) {
    /* Finishes the previous present's screenshot copy, if there was one,
     * before anything this field can queue behind it. See drain_screenshots:
     * the fence it waits on is a whole present old by now. */
    drain_screenshots();
    /* The boot trace's centre sample from the previous present, same
     * reasoning: its fence is a whole present old. */
    drain_boot_sample();
    /* Takes the host's resize notification, if any, into Granite's own flag.
     * On this thread, before begin_frame reads it. */
    m_platform->sync_from_host();
    if (const char* why = m_platform->not_presentable_reason()) {
        /* begin_frame() would park this thread here; see presentable().
         * Pumping is a no-op off the window's own thread, where the host's
         * per-field pump is what delivers the restore and close events; it
         * still matters in the launcher phase, where this is that thread. */
        m_platform->poll_input();
        note_present_skipped(why);
        return false;
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
        note_begin_frame_failed("this guest field");
        return false;
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
                    warnf("paraLLEl-GS: display.fit=integer has no room for a 1x copy"
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
                /* Only the two fits whose width was derived from the aspect
                 * re-derive it. display.fit=stretch set dst_w = bb_w by
                 * construction, so recomputing it from the shrunk height
                 * would pillarbox a stretched picture the moment a raw movie
                 * field arrived: at 2560x1440 the destination would go from
                 * 2560x1440 to 1917x1438. Stretch keeps the full width and
                 * gives up the two rows only. */
                if (fit != RT_PGS_FIT_STRETCH && aspect > 0.0)
                    dst_w = int(std::lround(dst_h * aspect));
                x0 = (bb_w - dst_w) / 2;
                y0 = (bb_h - dst_h) / 2;
                static bool logged_bob_room = false;
                if (!logged_bob_room) {
                    logged_bob_room = true;
                    warnf("paraLLEl-GS: deinterlace=bob: destination %dx%d fills the %dx%d window"
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

    /* Boot trace, presented-colour stream: the backbuffer centre as it is
     * about to go to the window, scanout blit or clear, before the overlay
     * (see gs_parallel_impl.h next to kBootTraceFields). */
    if (m_vsyncs <= kBootTraceFields) {
        sample_backbuffer(*cmd, backbuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          bb_w, bb_h, bool(scanout.image));
    }

    /* The user-facing screenshot (rt_pgs_request_screenshot). Taken here, out
     * of the backbuffer over exactly the rectangle just published, which is
     * the presented picture at presented size with the letterbox bars
     * excluded and the display aspect already applied by the blit above. It
     * is before the overlay branch below, so the settings menu, the launcher,
     * the drawn pointer and the fps readout are never in the file.
     *
     * An empty rectangle means this field presented no scanout image
     * (the clear, and possibly the overlay). There is no picture to capture
     * and the arm is kept for the next field that has one, rather than
     * writing a black rectangle the user did not ask for. */
    if (m_shot_slots && rect_w > 0 && rect_h > 0) {
        capture_backbuffer(*cmd, backbuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           RT_PGS_SHOT_PRE, rect_x, rect_y, rect_w, rect_h);
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
    /* The diagnostic second copy: the same field, the same rectangle, after
     * the overlay pass. Both branches above leave the image in
     * PRESENT_SRC_KHR, so one call covers the overlay and the no-overlay
     * path, which is the point: with the menu closed this file and the pre
     * one must be byte identical, and with the menu open they must differ.
     * That is what proves the pre copy is taken where the comment above says
     * it is. Only armed when the host asked for two slots. */
    if (m_shot_slots >= RT_PGS_SHOT_SLOTS && rect_w > 0 && rect_h > 0) {
        capture_backbuffer(*cmd, backbuffer, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                           RT_PGS_SHOT_POST, rect_x, rect_y, rect_w, rect_h);
    }
    /* The arm is one shot and is consumed by the field that had a picture. */
    if (rect_w > 0 && rect_h > 0) m_shot_slots = 0;

    /* Fenced only when this field recorded a copy: drain_screenshots waits on
     * it at the top of the next present. Every slot holding a buffer was
     * filled by this submit, since the drain above emptied the rest. */
    Vulkan::Fence shot_fence;
    bool shot_submitted = false;
    for (const ShotPending& p : m_shot) {
        if (p.buffer) shot_submitted = true;
    }
    /* The boot trace sample rides the same fence: a buffer with no fence yet
     * was recorded by this command buffer (drain_boot_sample emptied any
     * older one at the top of this present). */
    const bool sample_submitted = m_boot_sample.buffer && !m_boot_sample.fence;
    device.submit(cmd, (shot_submitted || sample_submitted) ? &shot_fence : nullptr);
    if (shot_submitted) {
        for (ShotPending& p : m_shot) {
            if (p.buffer) p.fence = shot_fence;
        }
    }
    if (sample_submitted) m_boot_sample.fence = shot_fence;

    if (!m_wsi->end_frame()) note_end_frame_failed("this guest field");
    m_in_frame = false;
    note_present_resumed();
    /* The frame went to the swapchain. end_frame failing above is a failure
     * of the queue present, not of the composition, and it is already logged;
     * everything this function is measured on (the blit, the overlay pass and
     * the screenshot copies) happened. */
    return true;
}

#endif /* ICORECOMP_PGS_SDL */
