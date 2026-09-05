/* gs/render/gs_native.h: the clean-room GS backend's entry points.
 *
 * Ours (MIT). Built when ICORECOMP_NATIVE_GS is on; selected at run time by
 * ICORECOMP_GS_BACKEND naming one of the RHI backends, or by ICORECOMP_GS=native
 * (gs/gs_select.cpp).
 */
#ifndef ICORECOMP_GS_NATIVE_H
#define ICORECOMP_GS_NATIVE_H

#include "../gs_backend.h"

#include <cstdint>

/* Creates the renderer and its RHI device on `which`. Never returns null: a
 * device that cannot be created is a loud fatal naming the device and the
 * requirement.
 *
 * The window is the executable's (host/window_service.h) and must already
 * exist, created with the flags `which` needs: SDL_WINDOW_VULKAN for Vulkan,
 * none for D3D12 and Metal. With no window the device is created headless.
 *
 * `present_mode` is an RT_PGS_PRESENT_* value (gs/gs_parallel_api.h), the
 * swapchain mode the device is created with. It is resolved by the caller
 * rather than read from settings here, because this file also compiles into
 * icorecomp-gs-replay, which links no settings layer. */
GsBackend* rt_gs_make_native_backend(RtNativeRhi which, uint32_t present_mode);

/* Headless replay of a raw dump (gs/gs_dumpwriter.cpp format) through this
 * renderer, using our own parser (gs_dump_parse.h). Progress and errors go to
 * stderr. Returns 0 on success. `screenshot_path` may be null; when it is
 * given, the final field's scanout is written there as a P6 PPM, the same
 * shape the existing screenshot path writes. */
int rt_gs_native_replay(const char* dump_path, const char* screenshot_path, int verbose);

#endif /* ICORECOMP_GS_NATIVE_H */
