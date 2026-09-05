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
 * The rendering itself lives inside libicorecomp-parallel-gs
 * (gs_parallel_abi.cpp, rt_pgs_replay); this file is only the argument
 * parser, so the executable stays on the MIT side of the LGPL boundary
 * (see gs_parallel_api.h).
 *
 * Usage: icorecomp-gs-replay <raw.gs> [--backend=parallel|native]
 *                             [--screenshot out.ppm] [--verbose]
 *        icorecomp-gs-replay --probe
 *
 * --backend picks which renderer consumes the stream. parallel is the default
 * so every existing invocation behaves exactly as it did: paraLLEl-GS's own
 * parser and renderer, which is the second opinion on our dump format. native
 * runs the clean-room renderer (gs/render/) through our own parser
 * (gs/render/gs_dump_parse.h) on a headless RHI device; that is the pair that
 * has to agree for the parity gate in docs/GS_RENDERER.md.
 * Exit status: 0 when the whole stream parsed and rendered, 1 otherwise.
 * Screenshot output holds the final field's scanout (ROM-derived pixels;
 * keep it outside the repository).
 *
 * --probe takes no dump: it creates the headless Vulkan device on its own
 * and prints the device it found plus one PASS/FAIL line per requirement
 * paraLLEl-GS makes of it. It exists for platforms this project ships
 * without having run, macOS first among them: the probe output is the one
 * thing a user or a CI runner on such a machine can send back that says
 * whether the renderer can work there at all. Exit status is 0 whenever a
 * device enumerated, whether or not it passed (a device that fails is a
 * report, not a tool failure), and 2 when no device did.
 */
#ifdef ICORECOMP_HAVE_PARALLEL_GS
#include "gs_parallel_api.h"
#include "gs_probe_api.h"
#endif
#ifdef ICORECOMP_NATIVE_GS
#include "render/gs_native.h"
#endif

#include <cstdio>
#include <cstring>

namespace {

#ifdef ICORECOMP_HAVE_PARALLEL_GS
const char* pass_text(int32_t ok) { return ok ? "PASS" : "FAIL"; }

/* Prints the probe report. Same text on every platform, so a log from a Mac
 * and a log from Linux line up when they are read side by side. */
int run_probe() {
    RtPgsProbe p;
    if (!rt_pgs_probe(&p) || !p.have_device) {
        std::printf("no Vulkan device\n");
        return 2;
    }

    std::printf("device: %s\n", p.device_name);
    std::printf("device type: %u, vendor 0x%04x, device 0x%04x\n",
        p.device_type, p.vendor_id, p.device_id);
    /* Vulkan does not define the encoding of driverVersion, so the raw
     * value is what gets reported rather than a decode that would be wrong
     * for some vendor. */
    std::printf("driver version: 0x%08x (raw; the encoding is vendor specific)\n",
        p.driver_version);
    std::printf("Vulkan API version: %u.%u.%u\n",
        (p.api_version >> 22) & 0x7fu, (p.api_version >> 12) & 0x3ffu,
        p.api_version & 0xfffu);
    if (p.descriptor_buffer_disabled) {
        std::printf("note: the context dropped VK_EXT_descriptor_buffer\n");
    }

    std::printf("%s descriptorIndexing\n", pass_text(p.descriptor_indexing));
    std::printf("%s timelineSemaphore\n", pass_text(p.timeline_semaphore));
    std::printf("%s bufferDeviceAddress\n", pass_text(p.buffer_device_address));
    std::printf("%s storageBuffer8BitAccess\n", pass_text(p.storage_buffer_8bit));
    std::printf("%s storageBuffer16BitAccess\n", pass_text(p.storage_buffer_16bit));
    std::printf("%s shaderInt16\n", pass_text(p.shader_int16));
    std::printf("%s scalarBlockLayout\n", pass_text(p.scalar_block_layout));
    std::printf("%s subgroup arithmetic/shuffle/vote/ballot/basic (supported 0x%08x)\n",
        pass_text(p.subgroup_ops), p.subgroup_supported_ops);
    std::printf("%s subgroup size control, 4 to 64 invocations\n",
        pass_text(p.subgroup_size_control));
    std::printf("%s 32 KiB compute shared memory (limit %u bytes)\n",
        pass_text(p.compute_shared_memory), p.max_compute_shared_memory);
    std::printf("result: %s\n",
        p.all_pass ? "this device meets paraLLEl-GS's requirements"
                   : "this device does NOT meet paraLLEl-GS's requirements");
    return 0;
}
#endif /* ICORECOMP_HAVE_PARALLEL_GS */

void usage() {
    std::fprintf(stderr,
        "usage: icorecomp-gs-replay <raw.gs> [--backend=parallel|native]\n"
        "                           [--screenshot out.ppm] [--verbose]\n"
        "       icorecomp-gs-replay --probe\n");
}

} // namespace

int main(int argc, char** argv) {
    const char* dump_path = nullptr;
    const char* screenshot_path = nullptr;
    bool verbose = false;
    bool probe = false;
    /* Default parallel, so nothing that already runs this tool changes. */
    bool native = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--probe") == 0) {
            probe = true;
        } else if (std::strncmp(argv[i], "--backend=", 10) == 0) {
            const char* which = argv[i] + 10;
            if (std::strcmp(which, "native") == 0) {
                native = true;
            } else if (std::strcmp(which, "parallel") == 0) {
                native = false;
            } else {
                std::fprintf(stderr, "icorecomp-gs-replay: unknown backend '%s' "
                                     "(expected parallel or native)\n", which);
                return 1;
            }
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot_path = argv[++i];
        } else if (std::strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (!dump_path) {
            dump_path = argv[i];
        } else {
            usage();
            return 1;
        }
    }

    if (probe) {
        if (dump_path) {
            std::fprintf(stderr, "icorecomp-gs-replay: --probe takes no dump file\n");
            return 1;
        }
#ifdef ICORECOMP_HAVE_PARALLEL_GS
        return run_probe();
#else
        std::fprintf(stderr, "icorecomp-gs-replay: --probe reports on paraLLEl-GS's "
                             "requirements and this build has no paraLLEl-GS\n");
        return 1;
#endif
    }

    if (!dump_path) {
        usage();
        return 1;
    }

    if (native) {
#ifdef ICORECOMP_NATIVE_GS
        return rt_gs_native_replay(dump_path, screenshot_path, verbose ? 1 : 0);
#else
        std::fprintf(stderr, "icorecomp-gs-replay: this build has no native GS renderer "
                             "(configure with -DICORECOMP_NATIVE_GS=ON)\n");
        return 1;
#endif
    }
#ifdef ICORECOMP_HAVE_PARALLEL_GS
    return rt_pgs_replay(dump_path, screenshot_path, verbose ? 1 : 0);
#else
    std::fprintf(stderr, "icorecomp-gs-replay: this build has no paraLLEl-GS backend; "
                         "use --backend=native\n");
    return 1;
#endif
}
