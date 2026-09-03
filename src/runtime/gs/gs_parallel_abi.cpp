/* gs/gs_parallel_abi.cpp: the C ABI of libicorecomp-parallel-gs.
 *
 * The rt_pgs_* entry points declared in gs_parallel_api.h, each forwarding to
 * the RtPgs method of the same name (gs_parallel_impl.h), plus the shim
 * version string and the headless replay entry point the gs-replay executable
 * calls. See gs_parallel_lib.cpp for the library overview.
 */
#include "gs_parallel_impl.h"

#include "gs_pgs_context.h"
#include "gs_readback.h"

#include "context.hpp"
#include "device.hpp"
#include "gs_dump_parser.hpp"
#include "gs_interface.hpp"

#include <cstdio>

extern "C" const char* icorecomp_parallel_gs_shim_version(void) {
    return "icorecomp-parallel-gs shim 3 (C ABI, see gs_parallel_api.h)";
}

/* ---- C ABI ---------------------------------------------------------------- */

extern "C" {

RtPgs* rt_pgs_create(const RtPgsHost* host, const RtPgsCreateOptions* opts) {
    return new RtPgs(*host, opts);
}

void rt_pgs_destroy(RtPgs* pgs) {
    delete pgs;
}

void rt_pgs_submit_gif(RtPgs* pgs, int path, const uint8_t* data, uint32_t qwords) {
    pgs->submit_gif(path, data, qwords);
}

void rt_pgs_write_priv(RtPgs* pgs, uint32_t offset, uint64_t v) {
    pgs->write_priv(offset, v);
}

uint64_t rt_pgs_read_priv(RtPgs* pgs, uint32_t offset) {
    return pgs->read_priv(offset);
}

uint32_t rt_pgs_vsync(RtPgs* pgs, unsigned field) {
    return pgs->vsync(field);
}

void rt_pgs_report_stats(RtPgs* pgs) {
    pgs->report_stats();
}

void* rt_pgs_window_handle(RtPgs* pgs) {
    return pgs->window_handle();
}

void rt_pgs_notify_quit(RtPgs* pgs) {
    pgs->notify_quit();
}

void rt_pgs_notify_resize(RtPgs* pgs) {
    pgs->notify_resize();
}

void rt_pgs_surface_size(RtPgs* pgs, uint32_t* width, uint32_t* height) {
    pgs->surface_size(width, height);
}

void rt_pgs_set_present_mode(RtPgs* pgs, uint32_t mode) {
    pgs->set_present_mode(mode);
}

void rt_pgs_set_presentation(RtPgs* pgs, uint32_t fit, uint32_t filter) {
    pgs->set_presentation(fit, filter);
}

void rt_pgs_set_raster(RtPgs* pgs, uint32_t raster) {
    pgs->set_raster(raster);
}

void rt_pgs_set_deinterlace(RtPgs* pgs, uint32_t deinterlace) {
    pgs->set_deinterlace(deinterlace);
}

void rt_pgs_set_render_scale(RtPgs* pgs, uint32_t factor) {
    pgs->set_render_scale(factor);
}

uint32_t rt_pgs_overlay_texture_create(RtPgs* pgs, const uint8_t* rgba8, uint32_t width, uint32_t height) {
    return pgs->overlay_texture_create(rgba8, width, height);
}

void rt_pgs_overlay_texture_destroy(RtPgs* pgs, uint32_t texture) {
    pgs->overlay_texture_destroy(texture);
}

void rt_pgs_overlay_set_frame(RtPgs* pgs, const RtPgsOverlayFrame* frame) {
    pgs->overlay_set_frame(frame);
}

uint32_t rt_pgs_present_ui(RtPgs* pgs) {
    return pgs->present_ui();
}

/* Body moved intact from the pre-C-ABI gs_replay_main.cpp so the replay
 * executable needs no Granite/paraLLEl-GS classes. stderr output keeps the
 * "gs-replay:" prefix that tooling greps for. */
int rt_pgs_replay(const char* dump_path, const char* screenshot_path, int verbose) {
    if (!Vulkan::Context::init_loader(nullptr)) {
        std::fprintf(stderr, "gs-replay: Vulkan loader initialization failed\n");
        return 1;
    }

    RtGsContextResult ctx = rt_gs_make_pgs_context();
    if (!ctx.context) {
        std::fprintf(stderr, "gs-replay: no usable Vulkan device\n");
        return 1;
    }
    if (ctx.descriptor_buffer_disabled) {
        std::fprintf(stderr, "gs-replay: descriptor-buffer path disabled (see gs_pgs_context.h)\n");
    }

    Vulkan::Device device;
    device.set_context(*ctx.context);
    device.init_frame_contexts(4);
    std::fprintf(stderr, "gs-replay: device \"%s\"\n", device.get_gpu_properties().deviceName);

    ParallelGS::GSInterface iface;
    ParallelGS::GSOptions opts = {};
    if (!iface.init(&device, opts)) {
        std::fprintf(stderr, "gs-replay: GSInterface::init failed; device lacks required features (see log)\n");
        return 1;
    }

    ParallelGS::GSDumpParser parser;
    if (!parser.open_raw(dump_path, 4 * 1024 * 1024, &iface)) {
        std::fprintf(stderr, "gs-replay: could not open %s\n", dump_path);
        return 1;
    }

    /* iterate_until_vsync returns at each vsync that had GIF traffic since
     * the previous return, and false at end of stream. The parser consumes
     * trailing traffic-free vsyncs before reporting eof, so "vsync groups"
     * below counts frames with actual transfers, not raw vsync packets. */
    unsigned groups = 0;
    ParallelGS::ScanoutResult last = {};
    while (parser.iterate_until_vsync(false, false)) {
        ++groups;
        ParallelGS::ScanoutResult res = parser.consume_vsync_result();
        if (verbose) {
            std::fprintf(stderr,
                         "gs-replay: vsync group %u: image=%s internal=%ux%u mode=%ux%u\n",
                         groups, res.image ? "yes" : "no",
                         res.internal_width, res.internal_height,
                         res.mode_width, res.mode_height);
        }
        if (res.image) last = res;
    }

    iface.flush();
    device.wait_idle();

    if (groups == 0) {
        std::fprintf(stderr, "gs-replay: FAILED: no vsync with GIF traffic found in %s\n", dump_path);
        return 1;
    }

    std::fprintf(stderr, "gs-replay: OK: %u vsync group(s) rendered from %s\n", groups, dump_path);
    if (last.image) {
        std::fprintf(stderr, "gs-replay: final scanout %ux%u (mode %ux%u)\n",
                     last.image->get_width(), last.image->get_height(),
                     last.mode_width, last.mode_height);
    }

    if (screenshot_path) {
        if (!last.image) {
            std::fprintf(stderr, "gs-replay: no scanout image to screenshot\n");
            return 1;
        }
        /* The parser requests READ_ONLY_OPTIMAL as the scanout layout. */
        if (!rt_gs_write_scanout_ppm(device, *last.image, screenshot_path,
                                     VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL)) {
            std::fprintf(stderr, "gs-replay: screenshot write failed\n");
            return 1;
        }
        std::fprintf(stderr, "gs-replay: wrote %s\n", screenshot_path);
    }
    return 0;
}

} /* extern "C" */
