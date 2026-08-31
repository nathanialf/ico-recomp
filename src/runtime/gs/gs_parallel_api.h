/* gs/gs_parallel_api.h: the C ABI of libicorecomp-parallel-gs.
 *
 * This header is the whole boundary between the MIT runtime and the LGPLv3+
 * paraLLEl-GS shared library. Everything Granite/paraLLEl-GS (all C++) stays
 * inside the library, behind gs_parallel_lib.cpp; the executable side
 * (gs_parallel.cpp, gs_replay_main.cpp) sees only these opaque-handle C
 * functions. The narrow surface is deliberate:
 *   - license: no LGPL class layouts, inline code or vtables compile into
 *     the MIT executable;
 *   - linking: on MSVC a DLL with no exports produces no import library
 *     (LNK1181); explicit RT_GS_API exports make every toolchain emit one.
 *
 * Ours (MIT), like both files on either side of it.
 */
#ifndef ICORECOMP_GS_PARALLEL_API_H
#define ICORECOMP_GS_PARALLEL_API_H

#include <stdint.h>

#if defined(_WIN32)
#  if defined(ICORECOMP_PGS_BUILD_DLL)
#    define RT_GS_API __declspec(dllexport)
#  else
#    define RT_GS_API __declspec(dllimport)
#  endif
#else
#  define RT_GS_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

RT_GS_API const char* icorecomp_parallel_gs_shim_version(void);

/* Host services the library may call back into. Messages arrive fully
 * formatted; `fatal` must not return (the host terminates the process). */
typedef struct RtPgsHost {
    void (*log)(const char* component, const char* message);
    void (*fatal)(const char* component, const char* message);
} RtPgsHost;

/* Opaque live-backend instance (Vulkan device + optional SDL3 window +
 * ParallelGS::GSInterface, all library-side). */
typedef struct RtPgs RtPgs;

/* Creates the live backend. Never returns NULL: unrecoverable setup errors
 * (no Vulkan loader/device, GSInterface init failure) go through
 * host->fatal. `host` is copied; the pointed-to struct need not outlive the
 * call. */
RT_GS_API RtPgs* rt_pgs_create(const RtPgsHost* host);
RT_GS_API void rt_pgs_destroy(RtPgs* pgs);

/* Mirrors GsBackend (gs_backend.h): path 0..2, data is qwords*16 bytes. */
RT_GS_API void rt_pgs_submit_gif(RtPgs* pgs, int path, const uint8_t* data, uint32_t qwords);
RT_GS_API void rt_pgs_write_priv(RtPgs* pgs, uint32_t offset, uint64_t v);
RT_GS_API uint64_t rt_pgs_read_priv(RtPgs* pgs, uint32_t offset);

/* End of field: render, and present when a window is up. Returns a bitmask
 * of RT_PGS_VSYNC_* flags; WINDOW_CLOSED asks the host to exit cleanly (the
 * library never terminates the process itself). */
#define RT_PGS_VSYNC_PRESENTED     1u
#define RT_PGS_VSYNC_WINDOW_CLOSED 2u
RT_GS_API uint32_t rt_pgs_vsync(RtPgs* pgs, unsigned field);

RT_GS_API void rt_pgs_report_stats(RtPgs* pgs);

/* Headless replay of a raw dump (gs_dumpwriter.cpp format) through
 * paraLLEl-GS's own GSDumpParser; the consumer-side check of the dump
 * writer. Progress and errors go to stderr. Returns 0 on success, nonzero
 * otherwise (same contract as the icorecomp-gs-replay exit status).
 * screenshot_path may be NULL. */
RT_GS_API int rt_pgs_replay(const char* dump_path, const char* screenshot_path, int verbose);

#ifdef __cplusplus
}
#endif

#endif /* ICORECOMP_GS_PARALLEL_API_H */
