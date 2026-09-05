/* gs/gs_probe_api.h: the device-capability probe half of
 * libicorecomp-parallel-gs's C ABI.
 *
 * Same boundary rules as gs_parallel_api.h: the executable side sees only C
 * types and RT_GS_API-exported functions, and every Granite/paraLLEl-GS C++
 * type stays inside the library (gs_probe_lib.cpp). It is a separate header
 * because the probe is the one part of the ABI a caller uses without an
 * RtPgs instance: gs_select.cpp and the launcher ask it what the machine has
 * before deciding whether to create one, and gs_replay_main.cpp --probe uses
 * it alone. The RT_GS_API macro below is the same definition, guarded so
 * including both headers is harmless.
 *
 * What it is for: on a machine nobody involved in this port owns, the first
 * question is not "does it render" but "does a Vulkan device exist here at
 * all, and does it meet paraLLEl-GS's requirements". That list is
 * third_party/parallel-gs/gs/gs_renderer.cpp:800-831, and RtPgsProbe is one
 * field per line of it, so a failing machine reports which requirement it
 * misses instead of "GSInterface::init failed".
 *
 * Ours (MIT).
 */
#ifndef ICORECOMP_GS_PROBE_API_H
#define ICORECOMP_GS_PROBE_API_H

#include <stdint.h>

#ifndef RT_GS_API
#if defined(_WIN32)
#  if defined(ICORECOMP_PGS_BUILD_DLL)
#    define RT_GS_API __declspec(dllexport)
#  else
#    define RT_GS_API __declspec(dllimport)
#  endif
#else
#  define RT_GS_API __attribute__((visibility("default")))
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Plain POD, no pointers: the settings menu's later "Feature support" line
 * can hold one of these and read it without calling back into the library.
 * Every feature field is 1 for pass and 0 for fail, in the order
 * gs_renderer.cpp lists them. */
typedef struct RtPgsProbe {
    int32_t have_device;              /* 0: no Vulkan device enumerated; the
                                       * rest of the struct is then zero */
    char device_name[256];            /* VkPhysicalDeviceProperties.deviceName */
    uint32_t api_version;             /* VK_API_VERSION_* packs this */
    uint32_t driver_version;          /* vendor-encoded, printed raw */
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t device_type;             /* VkPhysicalDeviceType as an integer */

    int32_t descriptor_indexing;
    int32_t timeline_semaphore;
    int32_t buffer_device_address;
    int32_t storage_buffer_8bit;
    int32_t storage_buffer_16bit;
    int32_t shader_int16;
    int32_t scalar_block_layout;
    int32_t subgroup_ops;             /* arithmetic + shuffle + vote + ballot + basic */
    int32_t subgroup_size_control;    /* 4 to 64 invocations a subgroup */
    int32_t compute_shared_memory;    /* at least 32 KiB */

    uint32_t max_compute_shared_memory;  /* the measured limit, in bytes */
    uint32_t subgroup_supported_ops;     /* VkSubgroupFeatureFlags, raw */

    int32_t descriptor_buffer_disabled;  /* the context dropped the bit */
    int32_t all_pass;                    /* every feature field above is 1 */
} RtPgsProbe;

/* Creates a headless Vulkan device exactly the way the live backend does,
 * fills `out`, and destroys it again. Returns 1 when a device enumerated
 * (whether or not it passed), 0 when none did. Never fatal: the whole point
 * is to report a machine that cannot run the renderer, so a failure here is
 * an answer, not an error. Safe to call before rt_pgs_create and safe to
 * call when no display exists. */
RT_GS_API int32_t rt_pgs_probe(RtPgsProbe* out);

/* The same struct, filled from the device a live instance already created,
 * with no second device made and nothing destroyed. This is what the Display
 * tab's two read-only lines are built from now: the old arrangement ran
 * rt_pgs_probe at startup, which created and destroyed a Vulkan device on
 * every launch and reported a device that need not be the one the run went
 * on to use.
 *
 * `pgs` is the opaque instance from rt_pgs_create (gs_parallel_api.h);
 * declared void* here so this header keeps standing on its own. Returns 1
 * when the struct was filled, 0 when `pgs` or `out` is NULL or the instance
 * has no device. Readable from either thread: it reads immutable device
 * properties settled at creation. */
RT_GS_API int32_t rt_pgs_live_probe(void* pgs, RtPgsProbe* out);

#ifdef __cplusplus
}
#endif

#endif /* ICORECOMP_GS_PROBE_API_H */
