/* gs/gs_replay_main.cpp: icorecomp-gs-replay, a headless consumer-side
 * check of the raw GS stream our dump writer emits.
 *
 * Feeds a raw dump (gs_dumpwriter.cpp output) through paraLLEl-GS's OWN
 * parser (GSDumpParser::open_raw) and full GSInterface rendering on a
 * headless Vulkan device, so the on-disk format and packet contents are
 * validated against the real implementation, not our reading of it. Works
 * without a display (lavapipe is enough), which makes it the CI-able
 * artifact for the GS transport.
 *
 * Usage: icorecomp-gs-replay <raw.gs> [--screenshot out.ppm] [--verbose]
 * Exit status: 0 when the whole stream parsed and rendered, 1 otherwise.
 * Screenshot output holds the final field's scanout (ROM-derived pixels;
 * keep it outside the repository).
 */
#include "gs_pgs_context.h"
#include "gs_readback.h"

#include "context.hpp"
#include "device.hpp"
#include "gs_dump_parser.hpp"
#include "gs_interface.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    const char* dump_path = nullptr;
    const char* screenshot_path = nullptr;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot_path = argv[++i];
        } else if (std::strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (!dump_path) {
            dump_path = argv[i];
        } else {
            std::fprintf(stderr, "usage: icorecomp-gs-replay <raw.gs> [--screenshot out.ppm] [--verbose]\n");
            return 1;
        }
    }
    if (!dump_path) {
        std::fprintf(stderr, "usage: icorecomp-gs-replay <raw.gs> [--screenshot out.ppm] [--verbose]\n");
        return 1;
    }

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
