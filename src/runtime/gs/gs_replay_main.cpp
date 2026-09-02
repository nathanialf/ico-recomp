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
 * Usage: icorecomp-gs-replay <raw.gs> [--screenshot out.ppm] [--verbose]
 * Exit status: 0 when the whole stream parsed and rendered, 1 otherwise.
 * Screenshot output holds the final field's scanout (ROM-derived pixels;
 * keep it outside the repository).
 */
#include "gs_parallel_api.h"

#include <cstdio>
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

    return rt_pgs_replay(dump_path, screenshot_path, verbose ? 1 : 0);
}
