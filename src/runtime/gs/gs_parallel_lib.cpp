/* gs/gs_parallel_lib.cpp: anchor translation unit for
 * libicorecomp-parallel-gs.so.
 *
 * The shared library exists purely as the LGPL boundary: it whole-archives
 * the paraLLEl-GS static libraries (and granite-vulkan) so every LGPLv3+
 * object lives in the .so and none in the MIT runtime executable. CMake
 * needs at least one source file on the target; this is it. Keep it free of
 * any real logic.
 */

extern "C" const char* icorecomp_parallel_gs_shim_version(void) {
    return "icorecomp-parallel-gs shim 1";
}
