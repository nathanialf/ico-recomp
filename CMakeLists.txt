# ico-recomp runtime build.
#
# Owns src/runtime/ only. The translator (tools/recomp, Rust) and the
# generated C in generated/ee/ (gitignored, produced locally at build time)
# are separate concerns; this file links against generated/ee when present
# and otherwise builds icorecomp-runtime in a stub mode (see src/runtime/main.cpp).
#
# third_party/parallel-gs (GS backend, P3): built as ONE shared library,
# libicorecomp-parallel-gs.so, that the runtime links dynamically. paraLLEl-GS
# is LGPLv3+; keeping every LGPL object inside the .so (and none in the MIT
# executable) is the license boundary, so the whole-archive/PIC dance below is
# deliberate. Sources are compiled unmodified from the pinned submodule.

cmake_minimum_required(VERSION 3.20)
project(icorecomp LANGUAGES C CXX)

# MSVC C runtime, chosen here so every scope agrees. paraLLEl-GS's own
# CMakeLists picks the static CRT when this is unset, and it does so inside
# add_subdirectory, so its archives came out /MT while our targets in this
# scope stayed on the default /MD: every paraLLEl-GS and Granite object then
# failed to link with LNK2038. Dynamic is the right side to settle on. The
# executable, icorecomp-parallel-gs.dll and SDL3.dll pass CRT state across
# the module boundary (the log sink's fd 2 redirect, the environment block
# the ICORECOMP_* toggles are read from), and that only works when they
# share one CRT instance. Has no effect on gcc, clang or the mingw cross
# build. CMP0091 is NEW at this cmake_minimum_required, so the value is
# honored per target rather than through CMAKE_CXX_FLAGS.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generated code (from the translator) is C11 and relies on type-punning
# through the rc_u128 union; both this project's own C and any generated C
# build with -fno-strict-aliasing.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---- which retail disc this build is for ---------------------------------
#
# One build serves one disc, because the generated C is a translation of one
# ELF: SCES_507.60, the European retail disc (SCES-50760). The boot ELF name,
# the SHA-1 pin and the target words are in src/runtime/target.h; the guest
# addresses the runtime reads are in src/runtime/guest/ico_syms.h. The
# translator writes generated/ee and generated/vu1; GENERATED_DIR and
# GENERATED_VU1_DIR override those for an out-of-tree translator run.

set(ICORECOMP_RUNTIME_SOURCES
    src/runtime/mem.cpp
    src/runtime/video_mode.cpp
    src/runtime/mmio.cpp
    src/runtime/hooks.cpp
    src/runtime/loader.cpp
    src/runtime/log.cpp
    src/runtime/host/run_state.cpp
    src/runtime/host/crash_report.cpp
    src/runtime/sha1.cpp
    src/runtime/main.cpp
    src/runtime/ee/sched.cpp
    src/runtime/ee/syscalls.cpp
    src/runtime/ee/intc.cpp
    src/runtime/ee/timers.cpp
    src/runtime/ee/alarms.cpp
    src/runtime/sif/sif.cpp
    src/runtime/sif/rpc.cpp
    src/runtime/sif/cdvd.cpp
    src/runtime/sif/sndn2.cpp
    src/runtime/sif/pad.cpp
    src/runtime/sif/mc.cpp
    src/runtime/guest/menu_nav.cpp
    src/runtime/guest/boot_trace.cpp
    src/runtime/guest/achievements.cpp
    src/runtime/guest/widescreen.cpp
    src/runtime/host/input.cpp
    src/runtime/host/mouse.cpp
    src/runtime/host/json.cpp
    src/runtime/host/settings.cpp
    src/runtime/host/settings_apply.cpp
    src/runtime/host/window.cpp
    src/runtime/host/window_service.cpp
    src/runtime/host/screenshot.cpp
    src/runtime/host/png_write.cpp
    src/runtime/snd/spu.cpp
    src/runtime/snd/engine.cpp
    src/runtime/snd/reverb.cpp
    src/runtime/host/audio.cpp
    src/runtime/iso/iso9660.cpp
    src/runtime/hw/dmac.cpp
    src/runtime/hw/ipu.cpp
    src/runtime/hw/vif1.cpp
    src/runtime/hw/gif.cpp
    src/runtime/hw/vu1rt.cpp
    src/runtime/hw/gspriv.cpp
    src/runtime/hw/geomcheck.cpp
    src/runtime/gs/gs_dumpwriter.cpp
    src/runtime/gs/gs_select.cpp
    src/runtime/gs/gs_threaded.cpp
)

add_executable(icorecomp-runtime ${ICORECOMP_RUNTIME_SOURCES})

# host/crash_report.cpp resolves the module and offset behind a faulting
# address with dladdr, which lives in libdl on the glibc versions that still
# have a separate one. Empty on Windows and on a glibc that merged it into
# libc, so this costs nothing where it is not needed.
target_link_libraries(icorecomp-runtime PRIVATE ${CMAKE_DL_LIBS})

# Ships as ico / ico.exe. The target keeps its name so CI (--target
# icorecomp-runtime) and every selftest reference stay valid.
set_target_properties(icorecomp-runtime PROPERTIES OUTPUT_NAME ico)

# Windows GUI subsystem. As a console-subsystem binary a double-clicked
# ico.exe opened a console window that died with the process, taking the
# only visible copy of a failure with it; as a GUI binary it opens no
# console at all, attaches to the shell's console when it was started from
# one, and allocates its own only when debug.console asks
# (src/runtime/log.cpp, rt_console_init).
#
# main() stays the entry point in both toolchains. mingw-w64's crtexe
# defines WinMainCRTStartup and mainCRTStartup as the same startup calling
# main, so -mwindows (what WIN32_EXECUTABLE passes) needs nothing further.
# MSVC's /SUBSYSTEM:WINDOWS looks for WinMain instead, so it is pointed
# back at the console startup, which calls main.
#
# What proves each half: the mingw half is linked by the runtime-ui-mingw CI
# job, which is a merge blocker. The MSVC half is linked only by
# runtime-stub-windows and runtime-pgs-windows, both continue-on-error, so a
# link failure from this option would be a green build. A change here has to
# be checked against an MSVC link by hand until one of those jobs blocks.
if(WIN32)
    set_target_properties(icorecomp-runtime PROPERTIES WIN32_EXECUTABLE ON)
    if(MSVC)
        target_link_options(icorecomp-runtime PRIVATE /ENTRY:mainCRTStartup)
    endif()
endif()

# Standalone mcserv protocol exercise (sif/mc_selftest.cpp): links mc.cpp
# against stubbed runtime services; run with ICORECOMP_SAVES_DIR set.
add_executable(icorecomp-mc-selftest
    src/runtime/sif/mc.cpp
    src/runtime/sif/mc_selftest.cpp
)
target_include_directories(icorecomp-mc-selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/runtime
)
target_compile_definitions(icorecomp-mc-selftest PRIVATE
    ICORECOMP_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(icorecomp-mc-selftest PRIVATE -Wall -Wextra -fno-strict-aliasing)
endif()

# Standalone sifcmd handshake exercise (sif/sif_selftest.cpp): links sif.cpp
# and rpc.cpp against stubbed runtime services and drives SCES_507.60's boot
# handshake (INIT_CMD opt 0 and opt 1, the RESET_CMD IOP reboot, and the
# SMFLAG spins on either side of it).
add_executable(icorecomp-sif-selftest
    src/runtime/sif/sif.cpp
    src/runtime/sif/rpc.cpp
    src/runtime/sif/sif_selftest.cpp
)
target_include_directories(icorecomp-sif-selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/runtime
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(icorecomp-sif-selftest PRIVATE -Wall -Wextra -fno-strict-aliasing)
endif()

# ---- log level selftest ---------------------------------------------------
#
# Standalone exercise of the level filter in log.cpp: the compiled-in
# default, the ICORECOMP_LOG_LEVEL override, the name parser, what each
# level lets through, and the verbose channel that passes its debug lines
# whatever the level is. Points the sink at a file under
# ICORECOMP_LOG_SELFTEST_DIR through ICORECOMP_LOG and reads back what
# actually landed there.
#
# host/run_state.cpp comes with it, and is the second thing this target
# covers: the end-of-run summary at both of its levels, and the synchronous
# crash write path. run_state.cpp calls nothing outside runtime.h's log
# entry points, which is what lets these two link on their own.
add_executable(icorecomp-log-selftest
    src/runtime/log.cpp
    src/runtime/host/run_state.cpp
    src/runtime/log_selftest.cpp
)
target_include_directories(icorecomp-log-selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/runtime
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(icorecomp-log-selftest PRIVATE -Wall -Wextra -fno-strict-aliasing)
endif()

# Standalone exercise of the JSON DOM (host/json.cpp) and the typed settings
# model (host/settings.cpp): host/settings_selftest.cpp against stub
# rt_log/rt_base_dir (settings.cpp's only two runtime.h externs), not
# against log.cpp, so this never pulls in the log sink or the rest of the
# runtime. Run with ICORECOMP_SETTINGS_SELFTEST_DIR set to a scratch dir.
add_executable(icorecomp-settings-selftest
    src/runtime/host/json.cpp
    src/runtime/host/settings.cpp
    src/runtime/host/settings_selftest.cpp
)
target_include_directories(icorecomp-settings-selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/runtime
)
target_compile_definitions(icorecomp-settings-selftest PRIVATE
    ICORECOMP_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(icorecomp-settings-selftest PRIVATE -Wall -Wextra -fno-strict-aliasing)
endif()

# Standalone exercise of the menu pointer (guest/menu_nav.cpp): the screen
# mapping derived from the game's own scene objects, the "is a menu up"
# predicate and the press planner, against a stub rt_log and a g_pages array
# this file's harness maps the two guest tables into. Built without
# ICORECOMP_HAVE_SDL and without ICORECOMP_HAVE_PARALLEL_GS, so no SDL, no
# window and no GS library: ICORECOMP_MENU_NAV_TEST compiles the cursor
# injection hook in their place.
#
add_executable(icorecomp-menu-nav-selftest
    src/runtime/guest/menu_nav.cpp
    src/runtime/guest/menu_nav_selftest.cpp
)
target_include_directories(icorecomp-menu-nav-selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/runtime
)
target_compile_definitions(icorecomp-menu-nav-selftest PRIVATE
    ICORECOMP_MENU_NAV_TEST
    ICORECOMP_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(icorecomp-menu-nav-selftest PRIVATE -Wall -Wextra -fno-strict-aliasing)
endif()

# Standalone exercise of the achievement observer (guest/achievements.cpp):
# the 0 to 1 edge detection on the game's progress bit array, the
# whole-array replacement rule that keeps a load and a new game from
# unlocking everything, the counters, the store round trip, the
# unparsable-file rule and the derived unlocks. Links achievements.cpp and
# host/json.cpp against a stub rt_log and rt_base_dir and against the
# g_pages array the harness maps the guest words into, the same arrangement
# the menu-nav selftest uses. No SDL, no window, no GS library and no disc
# image. ICORECOMP_ACHIEVEMENTS_TEST compiles in the seams the harness needs
# to install a trophy bit table of its own, because the shipped one is
# deliberately empty (guest/ico_syms.h).
#
# Run with a scratch directory: it writes achievements.json there.
add_executable(icorecomp-achievements-selftest
    src/runtime/guest/achievements.cpp
    src/runtime/guest/achievements_selftest.cpp
    src/runtime/host/json.cpp
    src/runtime/video_mode.cpp
)
target_include_directories(icorecomp-achievements-selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/runtime
)
target_compile_definitions(icorecomp-achievements-selftest PRIVATE
    ICORECOMP_ACHIEVEMENTS_TEST
    ICORECOMP_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(icorecomp-achievements-selftest PRIVATE -Wall -Wextra -fno-strict-aliasing)
endif()

# ---- video mode / field timeline selftest --------------------------------
#
# Standalone exercise of the field timeline (src/runtime/video_mode.cpp):
# the NTSC numbers this runtime has always used, the PAL numbers derived
# from the analog standard, and the switch between them that SetGsCrt makes.
# Links video_mode.cpp against a stub rt_log and nothing else.
add_executable(icorecomp-video-mode-selftest
    src/runtime/video_mode.cpp
    src/runtime/video_mode_selftest.cpp
)
target_include_directories(icorecomp-video-mode-selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/runtime
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(icorecomp-video-mode-selftest PRIVATE -Wall -Wextra -fno-strict-aliasing)
endif()

# ---- OSD configuration word selftest --------------------------------------
#
# Standalone exercise of the console OSD configuration word (ee/osd_config.h)
# and of the vendor library's own readers of it. This is a PAL-only surface:
# the PAL ELF reaches GetOsdConfigParam at boot through sceScfGetLanguage and
# the US ELF never calls it. The subject is header only, so this links
# nothing: no settings model, no guest memory, no runtime services.
add_executable(icorecomp-osd-config-selftest
    src/runtime/ee/osd_config_selftest.cpp
)
target_include_directories(icorecomp-osd-config-selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/runtime
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(icorecomp-osd-config-selftest PRIVATE -Wall -Wextra -fno-strict-aliasing)
endif()

# Standalone exercise of the SgStPcm ring geometry (snd/pcm_stream.h), the
# interleave the attract movie's audio streams out of the IOP buffer. The
# subject is header only, so this links nothing: no SDL, no runtime services.
# Its numbers are the ones the retail game sends.
add_executable(icorecomp-snd-pcm-selftest
    src/runtime/snd/tests/pcm_stream_selftest.cpp
)
target_include_directories(icorecomp-snd-pcm-selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/runtime
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(icorecomp-snd-pcm-selftest PRIVATE -Wall -Wextra -fno-strict-aliasing)
endif()

# The PNG writer host/screenshot.cpp, the diagnostic dumps and the icon tool
# all write their files with (host/png_write.h). Pure over a pixel buffer,
# so this needs no window, no GS backend and no disc. It links
# host/inflate.cpp as well, and inflates its own output: the round trip is
# through a decoder that shares no code with the writer.
add_executable(icorecomp-png-selftest
    src/runtime/host/png_selftest.cpp
    src/runtime/host/png_write.cpp
    src/runtime/host/inflate.cpp
)
target_include_directories(icorecomp-png-selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/runtime
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(icorecomp-png-selftest PRIVATE -Wall -Wextra -fno-strict-aliasing)
endif()

# Standalone exercise of the left-stick reshaping behind
# gameplay.run_any_direction (host/stick_shape.h). The subject is header
# only, so this links nothing: no SDL, no settings model, no runtime
# services. Its angle sweep is the evidence that a full tilt reaches the
# game's run threshold at every angle.
add_executable(icorecomp-stick-shape-selftest
    src/runtime/host/stick_shape_selftest.cpp
)
target_include_directories(icorecomp-stick-shape-selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/runtime
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(icorecomp-stick-shape-selftest PRIVATE -Wall -Wextra -fno-strict-aliasing)
endif()

# Standalone exercise of the mouse-to-right-stick mapping behind
# input.mouse_look (host/mouse_look.h). Header only, so this links nothing
# either. Its sweep is the evidence that every nonzero mouse delta turns the
# camera and that a full-rate delta saturates at every angle.
add_executable(icorecomp-mouse-look-selftest
    src/runtime/host/mouse_look_selftest.cpp
)
target_include_directories(icorecomp-mouse-look-selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/runtime
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(icorecomp-mouse-look-selftest PRIVATE -Wall -Wextra -fno-strict-aliasing)
endif()

target_include_directories(icorecomp-runtime PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/runtime
    ${CMAKE_SOURCE_DIR}/third_party/minicoro
)

target_compile_definitions(icorecomp-runtime PRIVATE
    ICORECOMP_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    # Gates the rt_settings() read in prof.h (header-only; see its comment).
    # Only targets that link host/settings.cpp may define this: icorecomp-
    # ipu-selftest also includes prof.h transitively (hw/ipu.cpp,
    # iso/iso9660.cpp) but does not link settings.cpp, so it must not.
    ICORECOMP_HAVE_SETTINGS
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(icorecomp-runtime PRIVATE -Wall -Wextra -fno-strict-aliasing)
endif()

# ---- IPU selftest ---------------------------------------------------------
#
# Standalone check of the IPU model (hw/ipu.cpp) against the real FMV
# bitstream: mounts the disc image, demuxes the video elementary stream
# EE-side (as the game does), drives the IPU over its MMIO interface
# (SETIQ/SETTH/BCLR/FDEC/VDEC/BDEC/CSC), and writes the decoded first I
# frame to an untracked BMP for eyeball verification. See
# src/runtime/hw/ipu_selftest.cpp.
# host/run_state.cpp comes with log.cpp wherever it is linked: rt_fatal ends
# a run by naming the exit reason and printing the summary, both of which
# live there. It calls nothing outside runtime.h's own log entry points.
add_executable(icorecomp-ipu-selftest
    src/runtime/hw/ipu.cpp
    src/runtime/hw/ipu_selftest.cpp
    src/runtime/iso/iso9660.cpp
    src/runtime/loader.cpp
    src/runtime/log.cpp
    src/runtime/host/run_state.cpp
    src/runtime/sha1.cpp
)
target_include_directories(icorecomp-ipu-selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/runtime
)
target_compile_definitions(icorecomp-ipu-selftest PRIVATE
    ICORECOMP_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(icorecomp-ipu-selftest PRIVATE -Wall -Wextra -fno-strict-aliasing)
endif()

# ---- VIF1 UNPACK selftest -------------------------------------------------
#
# Differential test of the UNPACK rewrite in hw/vif1.cpp. That file keeps
# the implementation it replaced as exec_unpack_reference, compiled in only
# under ICORECOMP_VIF1_SELFTEST, and this binary runs randomised UNPACK
# commands through both from identical state and compares VU1 data memory
# byte for byte. It links vif1.cpp against stubs for the runtime services
# (log, VU1 state, GIF, DMAC diagnostics), so no scheduler, no GS backend
# and no disc image. See src/runtime/hw/vif1_selftest.cpp.
add_executable(icorecomp-vif1-selftest
    src/runtime/hw/vif1.cpp
    src/runtime/hw/vif1_selftest.cpp
)
target_include_directories(icorecomp-vif1-selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/runtime
)
# ICORECOMP_HAVE_SETTINGS is deliberately absent: this target includes
# prof.h transitively but does not link settings.cpp, same as the IPU
# selftest above.
target_compile_definitions(icorecomp-vif1-selftest PRIVATE ICORECOMP_VIF1_SELFTEST)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(icorecomp-vif1-selftest PRIVATE -Wall -Wextra -fno-strict-aliasing)
endif()

# ---- widescreen 2D transform selftest -------------------------------------
#
# display.widescreen scales 2D X coordinates back about the GS window centre
# in hw/gif.cpp, so that widening the 3D projection does not stretch the
# menus, the subtitles and the fades with it. This binary runs hand-built GIF
# packets through the real rt_gif_submit and checks the 12.4 coordinates the
# backend receives, rounding included, plus every case the classification
# rule is supposed to leave alone. It links gif.cpp against stubs for the
# runtime services, so no GS library, no scheduler and no disc image. See
# src/runtime/hw/gif_widescreen_selftest.cpp.
add_executable(icorecomp-gif-widescreen-selftest
    src/runtime/hw/gif.cpp
    src/runtime/hw/gif_widescreen_selftest.cpp
)
target_include_directories(icorecomp-gif-widescreen-selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/runtime
)
# ICORECOMP_HAVE_SETTINGS is deliberately absent, for the reason the VIF1
# selftest above states: prof.h comes in transitively, settings.cpp does not.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(icorecomp-gif-widescreen-selftest PRIVATE -Wall -Wextra -fno-strict-aliasing)
endif()

# ---- GS command ring selftest ---------------------------------------------
#
# Round-trip check of gs/gs_threaded.cpp's ThreadedBackend: the same
# randomised GsBackend call sequence is played into a recording backend
# directly and into a second one through the ring, and the two recordings
# must match byte for byte. That is the check behind "an ICORECOMP_GS=dump
# run writes identical output with and without the wrapper", which cannot be
# run here (it needs a disc, a GPU and a display). Links gs_threaded.cpp
# against stubs for rt_log/rt_fatal, so no GS library, no SDL and no runtime
# services. See src/runtime/gs/gs_ring_selftest.cpp.
add_executable(icorecomp-gsring-selftest
    src/runtime/gs/gs_threaded.cpp
    src/runtime/gs/gs_ring_selftest.cpp
)
target_include_directories(icorecomp-gsring-selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/runtime
)
# ICORECOMP_HAVE_SETTINGS is deliberately absent, same as the IPU and VIF1
# selftests: nothing here links host/settings.cpp.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(icorecomp-gsring-selftest PRIVATE -Wall -Wextra -fno-strict-aliasing)
endif()

# ---- title logo selftest --------------------------------------------------
#
# Standalone check of the launcher's title image (ui/title_logo.cpp): mounts
# the disc image, pulls the three letter meshes and the title animation out of
# DFDATAS/DATA.DF, reduces them to the wordmark's geometry, rasterises it to a
# PNG for eyeball verification, and then reads the result back through the
# cache. It also checks ui/cursor_image.cpp, the menu cursor cut out of that
# same image, against synthetic glyphs first and the real wordmark after.
# Links no RmlUi, no SDL and no GS. The output path is a required argument and
# must be outside the repository: the PNG it writes is game-derived.
add_executable(icorecomp-title-logo-selftest
    src/runtime/ui/title_logo.cpp
    src/runtime/ui/cursor_image.cpp
    src/runtime/ui/title_logo_selftest.cpp
    src/runtime/ui/data_df.cpp
    src/runtime/host/inflate.cpp
    src/runtime/host/png_write.cpp
    src/runtime/iso/iso9660.cpp
    src/runtime/loader.cpp
    src/runtime/log.cpp
    src/runtime/host/run_state.cpp
    src/runtime/sha1.cpp
)
target_include_directories(icorecomp-title-logo-selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/runtime
)
target_compile_definitions(icorecomp-title-logo-selftest PRIVATE
    ICORECOMP_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(icorecomp-title-logo-selftest PRIVATE -Wall -Wextra -fno-strict-aliasing)
endif()

# ---- packaged save icon (host tool, optional .rc embed) -------------------
#
# icorecomp-icon-extract (ours): reads the mounted disc's own memory-card
# save icon (DATA.DF's icon.sys and the view icon it names, boy_blk.ico on
# the retail disc) and renders it the way the PS2 browser draws it: a black
# silhouette on the icon.sys background, at the sizes a Windows .ico wants.
# A plain host tool -- no SDL, no RmlUi, no GS -- run at build time (the
# `icon` target below) or by hand; never at game run time (the runtime's own
# render is ui/save_icon.cpp, called from ui_launcher.cpp for the window
# icon). See ui/icon_extract.cpp for the usage and the .ico layout it writes.
add_executable(icorecomp-icon-extract
    src/runtime/ui/icon_extract.cpp
    src/runtime/ui/ps2_icon.cpp
    src/runtime/ui/ps2_icon_render.cpp
    src/runtime/ui/save_icon.cpp
    src/runtime/ui/data_df.cpp
    src/runtime/host/png_write.cpp
    src/runtime/iso/iso9660.cpp
    src/runtime/loader.cpp
    src/runtime/log.cpp
    src/runtime/host/run_state.cpp
    src/runtime/sha1.cpp
)
target_include_directories(icorecomp-icon-extract PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/runtime
)
target_compile_definitions(icorecomp-icon-extract PRIVATE
    ICORECOMP_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(icorecomp-icon-extract PRIVATE -Wall -Wextra -fno-strict-aliasing)
endif()

# ICORECOMP_DISC: the disc image the `icon` target renders the save icon
# from. Never committed (CLAUDE.md); empty by default, which configures and
# builds exactly as before: the exe then carries no icon resource, and
# Windows shows it with its own default application icon.
set(ICORECOMP_DISC "" CACHE FILEPATH
    "Disc image the icon target renders the save icon from; never committed")
# ICORECOMP_ICON_FILE: the rendered ico.ico to embed as the Windows
# executable's icon. Ordinarily the `icon` target's own output; set by hand
# when cross-compiling, since icorecomp-icon-extract cannot run on the build
# host in that configuration (see the CMAKE_CROSSCOMPILING branch below).
set(ICORECOMP_ICON_FILE "" CACHE FILEPATH
    "Rendered ico.ico to embed as the Windows executable icon; empty builds without one")
# ICORECOMP_ICNS_FILE: the same thing for the macOS bundle icon. Ordinarily
# the `icon` target's own output, filled in below; set by hand when the
# rendering was done on another machine.
set(ICORECOMP_ICNS_FILE "" CACHE FILEPATH
    "Rendered icorecomp.icns for the macOS app bundle; empty builds without one")

if(ICORECOMP_DISC AND NOT CMAKE_CROSSCOMPILING)
    set(ICORECOMP_ICON_DIR "${CMAKE_BINARY_DIR}/icon")
    # On macOS the same run also writes the bundle's .icns, so one `icon`
    # build produces both containers from one set of renders. Neither file
    # is ever committed; both land in the build directory.
    set(ICORECOMP_ICON_OUTPUTS "${ICORECOMP_ICON_DIR}/ico.ico")
    set(ICORECOMP_ICON_EXTRA_ARGS)
    if(APPLE)
        set(_icorecomp_icns_out "${ICORECOMP_ICON_DIR}/icorecomp.icns")
        list(APPEND ICORECOMP_ICON_OUTPUTS "${_icorecomp_icns_out}")
        list(APPEND ICORECOMP_ICON_EXTRA_ARGS --icns "${_icorecomp_icns_out}")
        # A hand-set ICORECOMP_ICNS_FILE wins: the render is then only for
        # this build tree and the packaged bundle keeps the file the user
        # named.
        if(NOT ICORECOMP_ICNS_FILE)
            set(ICORECOMP_ICNS_FILE "${_icorecomp_icns_out}")
        endif()
    endif()
    add_custom_command(
        OUTPUT ${ICORECOMP_ICON_OUTPUTS}
        COMMAND "$<TARGET_FILE:icorecomp-icon-extract>"
                "${ICORECOMP_DISC}" "${ICORECOMP_ICON_DIR}/ico.ico"
                --png "${ICORECOMP_ICON_DIR}"
                ${ICORECOMP_ICON_EXTRA_ARGS}
        DEPENDS icorecomp-icon-extract "${ICORECOMP_DISC}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "icorecomp: rendering the save icon from ${ICORECOMP_DISC}"
        VERBATIM
    )
    add_custom_target(icon DEPENDS ${ICORECOMP_ICON_OUTPUTS})
elseif(ICORECOMP_DISC AND CMAKE_CROSSCOMPILING)
    message(STATUS "icorecomp: ICORECOMP_DISC is set but this is a cross build; "
                   "icorecomp-icon-extract cannot run here. Configure and build "
                   "--target icon natively, then pass its output as "
                   "-DICORECOMP_ICON_FILE=<path>/ico.ico to this build.")
endif()

if(WIN32 AND ICORECOMP_ICON_FILE)
    enable_language(RC)
    get_filename_component(_icorecomp_icon_abs "${ICORECOMP_ICON_FILE}" ABSOLUTE)
    if(NOT EXISTS "${_icorecomp_icon_abs}")
        # Not fatal: the documented flow is to configure with this set and
        # build --target icon in the same tree, so the file can legitimately
        # arrive after this line. Say so now rather than leaving the resource
        # compiler to report it.
        message(WARNING "icorecomp-runtime: ICORECOMP_ICON_FILE ${_icorecomp_icon_abs} does not "
                        "exist yet; the resource compile will fail unless it is there by then")
    endif()
    file(TO_CMAKE_PATH "${_icorecomp_icon_abs}" ICORECOMP_ICON_FILE_RC)
    configure_file("${CMAKE_SOURCE_DIR}/packaging/ico.rc.in" "${CMAKE_BINARY_DIR}/icon/ico.rc" @ONLY)
    target_sources(icorecomp-runtime PRIVATE "${CMAKE_BINARY_DIR}/icon/ico.rc")
    # The .rc names the .ico by path, so nothing in the generated build files
    # ties the compiled resource to the icon's own bytes: without this, a
    # re-rendered ico.ico leaves the exe carrying the previous icon. Applied
    # by the Makefile and Ninja generators.
    set_source_files_properties("${CMAKE_BINARY_DIR}/icon/ico.rc" PROPERTIES
        OBJECT_DEPENDS "${_icorecomp_icon_abs}")
    message(STATUS "icorecomp-runtime: embedding icon ${ICORECOMP_ICON_FILE}")
elseif(WIN32 AND CMAKE_CROSSCOMPILING)
    # Loud, because this is how the shipped ico.exe lost its icon once
    # already: icorecomp-icon-extract cannot run on the build host in a cross
    # configure, so nothing here can produce the file and the resource is
    # simply absent unless it was rendered natively first and named on the
    # command line. docs/WINDOWS.md carries the two-step flow.
    message(WARNING
        "icorecomp-runtime: cross-compiling for Windows with an empty "
        "ICORECOMP_ICON_FILE, so ico.exe will ship with no icon resource. "
        "Render one natively first (cmake --preset linux-gcc-release, build "
        "icorecomp-icon-extract, run it on the disc image to a directory "
        "outside this repository), then reconfigure this build with "
        "-DICORECOMP_ICON_FILE=<that path>/ico.ico. See docs/WINDOWS.md.")
elseif(WIN32)
    message(STATUS "icorecomp-runtime: no ICORECOMP_ICON_FILE; the exe ships with the default icon")
elseif(ICORECOMP_ICON_FILE)
    # An icon resource is a Windows executable's, so there is nothing to do
    # with the file here. Say it rather than ignoring the variable silently.
    message(STATUS "icorecomp: ICORECOMP_ICON_FILE is set but this is not a Windows target; "
                   "the icon is embedded only in ico.exe, so it is ignored here")
endif()

# ---- optional generated/ee link ------------------------------------------
#
# GENERATED_DIR defaults to <repo>/generated/ee (the translator's normal
# output location, gitignored: see CLAUDE.md's ROM-derived-data rule) but can
# be overridden, e.g. `cmake -DGENERATED_DIR=/path/to/ee ...`, for out-of-tree
# translator runs.
if(NOT DEFINED GENERATED_DIR)
    set(GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated/ee")
endif()

# Every generated .c file starts with #include "ee/funcs.h". That path is
# the translator's, and it resolves only where the generated directory is
# itself named `ee`: the default is, but -DGENERATED_DIR can name anything.
# Rather than depend on a directory's name, the build writes one forwarding
# header into its own tree and puts that on the include path in place of
# ${GENERATED_DIR}/.. . file(GENERATE) leaves the file alone when the
# content has not changed, so this does not re-trigger a rebuild of the
# generated objects on every configure.
set(GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated-include")
file(GENERATE OUTPUT "${GENERATED_INCLUDE_DIR}/ee/funcs.h"
    CONTENT "/* Written by CMake. Forwards the translator's \"ee/funcs.h\" to the\n * generated directory this build was configured with. */\n#include \"${GENERATED_DIR}/funcs.h\"\n")

if(EXISTS "${GENERATED_DIR}")
    file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS "${GENERATED_DIR}/*.c")
    if(GENERATED_SOURCES)
        add_library(generated OBJECT ${GENERATED_SOURCES})
        set_target_properties(generated PROPERTIES
            C_STANDARD 11
            C_STANDARD_REQUIRED ON
        )
        target_include_directories(generated PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${GENERATED_DIR}
            ${GENERATED_INCLUDE_DIR}
        )
        # -ffp-contract=off: the VU0 macro-mode FMAC helpers this code
        # calls (rc_vu_fmac kinds madd and msub, rc_vu_opmula and
        # rc_vu_opmsub in recomp_ops.h) are written as a separate multiply
        # and add because that is what the hardware does, rounding the
        # product to single precision before the add. Letting the host
        # contract them into an FMA keeps the product at full width and
        # changes the result. The COP1 multiply-add family (madd.s and
        # friends) is not in this binary's census, so it is not a reason
        # here. Same flag as the VU1 object library below; remove it only
        # together with an ISA change that makes fusion correct.
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(generated PRIVATE -fno-strict-aliasing -ffp-contract=off)
        elseif(MSVC)
            # /fp:precise still permits FMA contraction on recent MSVC;
            # /fp:contract- (VS2022+) forbids it, matching -ffp-contract=off.
            target_compile_options(generated PRIVATE /fp:precise /fp:contract-)
        endif()

        target_link_libraries(icorecomp-runtime PRIVATE generated)
        target_include_directories(icorecomp-runtime PRIVATE ${GENERATED_DIR} ${GENERATED_INCLUDE_DIR})
        target_compile_definitions(icorecomp-runtime PRIVATE ICORECOMP_HAVE_GENERATED)
        message(STATUS "icorecomp-runtime: linking generated code from ${GENERATED_DIR} (${GENERATED_DIR})")
    else()
        message(STATUS "icorecomp-runtime: ${GENERATED_DIR} exists but has no .c files; building in stub mode")
    endif()
else()
    message(STATUS "icorecomp-runtime: ${GENERATED_DIR} not found; building in stub mode (see src/runtime/main.cpp)")
endif()

# ---- optional generated/vu1 link -----------------------------------------
#
# Recompiled VU1 microprograms (icorecomp vu1 --out generated/vu1). The
# runtime's VIF1 MSCAL path resolves uploads against the registry these
# files fill via rt_vu1_register_all(). -ffp-contract=off: the VU1 FMAC
# model requires separately rounded multiply and add.
if(NOT DEFINED GENERATED_VU1_DIR)
    set(GENERATED_VU1_DIR "${CMAKE_SOURCE_DIR}/generated/vu1")
endif()

if(EXISTS "${GENERATED_VU1_DIR}")
    file(GLOB GENERATED_VU1_SOURCES CONFIGURE_DEPENDS "${GENERATED_VU1_DIR}/*.c")
    if(GENERATED_VU1_SOURCES)
        add_library(generated_vu1 OBJECT ${GENERATED_VU1_SOURCES})
        set_target_properties(generated_vu1 PROPERTIES
            C_STANDARD 11
            C_STANDARD_REQUIRED ON
        )
        target_include_directories(generated_vu1 PRIVATE ${CMAKE_SOURCE_DIR}/include)
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(generated_vu1 PRIVATE -fno-strict-aliasing -ffp-contract=off)
        elseif(MSVC)
            # /fp:precise still permits FMA contraction on recent MSVC;
            # /fp:contract- (VS2022+) forbids it, matching -ffp-contract=off.
            target_compile_options(generated_vu1 PRIVATE /fp:precise /fp:contract-)
        endif()

        target_link_libraries(icorecomp-runtime PRIVATE generated_vu1)
        target_compile_definitions(icorecomp-runtime PRIVATE ICORECOMP_HAVE_VU1_GENERATED)
        message(STATUS "icorecomp-runtime: linking generated VU1 code from ${GENERATED_VU1_DIR}")
    else()
        message(STATUS "icorecomp-runtime: ${GENERATED_VU1_DIR} exists but has no .c files; MSCAL will loud-skip")
    endif()
else()
    message(STATUS "icorecomp-runtime: ${GENERATED_VU1_DIR} not found; MSCAL will loud-skip")
endif()

# ---- SDL3 (third_party/SDL) ----------------------------------------------
#
# SDL is a direct dependency of this project, not something borrowed out of a
# submodule of a submodule. The executable owns the window
# (src/runtime/host/window_service.cpp): it creates it, pumps its events, and
# hands the handle, the native platform handles or a Vulkan surface to
# whichever GS backend is presenting. That makes SDL the window library of the
# port, so removing third_party/parallel-gs must not take it away with it.
#
# third_party/SDL is pinned to the commit Granite vendors
# (release-3.4.0-1161-g8586f63d2), so a tree with both submodules compiles one
# SDL rather than two revisions of it. Granite's copy is the fallback for a
# tree that has parallel-gs checked out and not third_party/SDL, which is what
# an existing build directory looks like before the submodule is initialised.
#
# SHARED on purpose, and for a sharper reason than before: the window is
# created in the executable and libicorecomp-parallel-gs presents into a
# surface made from it, so the two must be the same SDL instance. Two static
# copies would leave the library building a swapchain on a window the exe's
# SDL does not know about.
#
# Requires CMake >= 3.25 for add_subdirectory(SYSTEM), the same floor the
# paraLLEl-GS backend below has.
option(ICORECOMP_WINDOW "Build the SDL3 window service and windowed presentation" ON)

set(ICORECOMP_HAVE_SDL OFF)
if(ICORECOMP_WINDOW AND CMAKE_VERSION VERSION_LESS 3.25)
    message(STATUS "icorecomp: CMake ${CMAKE_VERSION} < 3.25; the SDL3 window service is disabled")
    set(ICORECOMP_WINDOW OFF)
endif()
if(ICORECOMP_WINDOW)
    set(ICORECOMP_SDL_DIR "")
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/SDL/CMakeLists.txt")
        set(ICORECOMP_SDL_DIR "${CMAKE_SOURCE_DIR}/third_party/SDL")
        set(ICORECOMP_SDL_WHICH "third_party/SDL")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/third_party/parallel-gs/Granite/third_party/sdl3/CMakeLists.txt")
        set(ICORECOMP_SDL_DIR "${CMAKE_SOURCE_DIR}/third_party/parallel-gs/Granite/third_party/sdl3")
        set(ICORECOMP_SDL_WHICH "Granite's vendored copy; third_party/SDL is not checked out")
    endif()

    if(ICORECOMP_SDL_DIR)
        set(SDL_SHARED ON CACHE BOOL "" FORCE)
        set(SDL_STATIC OFF CACHE BOOL "" FORCE)
        set(SDL_TIMERS ON CACHE BOOL "" FORCE)
        set(SDL_LIBC ON CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
        set(SDL_ATOMIC OFF CACHE BOOL "" FORCE)
        # Audio ON: host/audio.cpp plays the sound engine's mix through an
        # SDL3 audio stream when a playback device exists (falls back to
        # silent/WAV-capture operation otherwise).
        set(SDL_AUDIO ON CACHE BOOL "" FORCE)
        set(SDL_RENDER OFF CACHE BOOL "" FORCE)
        set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
        set(SDL_POWER OFF CACHE BOOL "" FORCE)
        set(SDL_FILE OFF CACHE BOOL "" FORCE)
        set(SDL_CPUINFO OFF CACHE BOOL "" FORCE)
        set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
        set(SDL_LOCALE OFF CACHE BOOL "" FORCE)
        # The launcher's disc picker (src/runtime/ui/ui_launcher.cpp) needs
        # SDL_ShowOpenFileDialog, and its footer credit link needs SDL_OpenURL.
        # Only SDL_DIALOG is an option in this SDL revision (its CMakeLists
        # declares the Dialog subsystem through define_sdl_subsystem, default
        # ON); it is set here so a future default flip cannot quietly remove
        # the file dialog. SDL_FILESYSTEM and SDL_MISC are not options at all
        # any more: those two are selected per platform (HAVE_SDL_FILESYSTEM /
        # HAVE_SDL_MISC), so the lines below document the requirement and
        # would take effect again if upstream reintroduced the options. The
        # OFF values they replaced never disabled anything.
        set(SDL_DIALOG ON CACHE BOOL "" FORCE)
        set(SDL_FILESYSTEM ON CACHE BOOL "" FORCE)
        set(SDL_MISC ON CACHE BOOL "" FORCE)
        # Headless build hosts (no X11/Wayland dev headers) may still build
        # SDL; window creation then fails at run time and the window service
        # reports no window, which sends every GS backend headless. Video
        # drivers are still auto-detected and built where the headers exist.
        set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
        add_subdirectory("${ICORECOMP_SDL_DIR}"
                         "${CMAKE_BINARY_DIR}/icorecomp-sdl3" EXCLUDE_FROM_ALL SYSTEM)
        set(ICORECOMP_HAVE_SDL ON)
        message(STATUS "icorecomp: SDL3 from ${ICORECOMP_SDL_WHICH}")
    else()
        message(STATUS "icorecomp: no SDL3 checkout "
                       "(git submodule update --init third_party/SDL); "
                       "the window service is disabled and every GS backend runs headless")
    endif()
endif()

if(ICORECOMP_HAVE_SDL)
    # The window service (host/window_service.cpp) is in the unconditional
    # source list above, because it compiles to no-op stubs without SDL and
    # its callers are unconditional. This is what turns it on.
    target_compile_definitions(icorecomp-runtime PRIVATE ICORECOMP_HAVE_SDL ICORECOMP_SND_SDL)
    target_link_libraries(icorecomp-runtime PRIVATE SDL3::SDL3)
endif()

# ---- paraLLEl-GS live backend (third_party/parallel-gs) -------------------
#
# Layout (see also third_party/README.md and CLAUDE.md license rules):
#   libicorecomp-parallel-gs.so  ALL LGPLv3+ paraLLEl-GS objects plus the
#                                Granite pieces they need, whole-archived so
#                                the .so is complete. This is the LGPL
#                                boundary; the runtime and tools link it
#                                dynamically and contain no LGPL objects.
#   icorecomp-runtime            gains gs/gs_parallel.cpp (ours, MIT), a thin
#                                GsBackend adapter over the shared library's
#                                C ABI (gs/gs_parallel_api.h), selected at
#                                runtime by ICORECOMP_GS. The real backend
#                                lives inside the library
#                                (gs/gs_parallel_lib.cpp and the other
#                                gs/gs_parallel_*.cpp units it names).
#   icorecomp-gs-replay          small headless tool (ours) that feeds a raw
#                                dump through paraLLEl-GS's own GSDumpParser
#                                via the same C ABI; validates the dump writer
#                                against the real consumer without a display.
#
# Requires CMake >= 3.25 for add_subdirectory(SYSTEM) and
# $<LINK_LIBRARY:WHOLE_ARCHIVE>; older CMake just disables the live backend.
option(ICORECOMP_PARALLEL_GS "Build the paraLLEl-GS live GS backend" ON)

# ---- submodule patches (third_party/patches) ------------------------------
#
# Submodule sources are never edited in place: LGPL compliance for
# parallel-gs wants the pinned sources to stay pristine and replaceable, so
# local changes are carried as patch files and applied to the working tree
# here, at configure time. See third_party/patches/README.md.
#
# Idempotent: a patch that reverse-applies cleanly is already in the tree and
# is skipped, so re-running cmake in an existing build directory is a no-op.
# Anything else is fatal. A submodule that silently lacks a patch would build
# and run with the bug the patch exists to fix, which is worse than not
# building.
find_package(Git QUIET)

function(icorecomp_apply_submodule_patches submodule_name submodule_dir)
    file(GLOB _patches
         "${CMAKE_SOURCE_DIR}/third_party/patches/${submodule_name}-[0-9][0-9][0-9][0-9]-*.patch")
    list(SORT _patches)
    if(NOT _patches)
        return()
    endif()

    # A patch file edit must re-run configure, or the tree keeps the old one.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_patches})

    if(NOT GIT_FOUND)
        message(FATAL_ERROR
                "icorecomp: third_party/patches holds patches for ${submodule_name} "
                "but git was not found; install git or apply them by hand "
                "(git apply, from ${submodule_dir})")
    endif()

    foreach(_patch IN LISTS _patches)
        get_filename_component(_patch_name "${_patch}" NAME)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${_patch}"
            WORKING_DIRECTORY "${submodule_dir}"
            RESULT_VARIABLE _reverse_ok
            OUTPUT_QUIET ERROR_QUIET)
        if(_reverse_ok EQUAL 0)
            message(STATUS "icorecomp: ${_patch_name} already applied to ${submodule_name}")
            continue()
        endif()

        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply "${_patch}"
            WORKING_DIRECTORY "${submodule_dir}"
            RESULT_VARIABLE _apply_rc
            ERROR_VARIABLE _apply_err)
        if(NOT _apply_rc EQUAL 0)
            message(FATAL_ERROR
                    "icorecomp: could not apply ${_patch_name} to ${submodule_dir}: "
                    "${_apply_err}"
                    "The submodule is either not at the pinned commit or already "
                    "carries conflicting local edits. Reset it "
                    "(git -C ${submodule_dir} reset --hard && "
                    "git submodule update --init --recursive) and configure again.")
        endif()
        message(STATUS "icorecomp: applied ${_patch_name} to ${submodule_name}")
    endforeach()
endfunction()

set(ICORECOMP_PGS_DIR "${CMAKE_SOURCE_DIR}/third_party/parallel-gs")
if(ICORECOMP_PARALLEL_GS AND CMAKE_VERSION VERSION_LESS 3.25)
    message(STATUS "icorecomp: CMake ${CMAKE_VERSION} < 3.25; paraLLEl-GS backend disabled")
    set(ICORECOMP_PARALLEL_GS OFF)
endif()
if(ICORECOMP_PARALLEL_GS AND NOT EXISTS "${ICORECOMP_PGS_DIR}/Granite/CMakeLists.txt")
    message(STATUS "icorecomp: parallel-gs submodule not initialized "
                   "(git submodule update --init --recursive); paraLLEl-GS backend disabled")
    set(ICORECOMP_PARALLEL_GS OFF)
endif()

if(ICORECOMP_PARALLEL_GS)
    # Static Granite/paraLLEl-GS archives must be PIC to land in the .so.
    # Granite decides PIC itself (its CMakeLists overrides the inherited
    # CMAKE_POSITION_INDEPENDENT_CODE); GRANITE_POSITION_INDEPENDENT is its
    # supported knob. The plain set() covers parallel-gs's own targets.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(GRANITE_POSITION_INDEPENDENT ON CACHE BOOL "" FORCE)
    # Standalone mode: what upstream's PCSX2 integration uses. Embedded
    # (slangmosh) shaders, no runtime shader compiler, no Granite app
    # framework or renderer. GRANITE_PLATFORM=null; we do WSI ourselves.
    set(PARALLEL_GS_STANDALONE ON CACHE BOOL "" FORCE)
    if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        # mingw cross build: Granite's DXGI-interop presenter (wsi_dxgi.cpp)
        # targets the MSVC SDK headers; the plain Vulkan swapchain path is
        # used instead. Everything is loaded dynamically either way.
        set(GRANITE_VULKAN_DXGI_INTEROP OFF CACHE BOOL "" FORCE)
    endif()
    icorecomp_apply_submodule_patches(parallel-gs "${ICORECOMP_PGS_DIR}")
    add_subdirectory("${ICORECOMP_PGS_DIR}" EXCLUDE_FROM_ALL SYSTEM)

    # The shared library. Whole-archive the LGPL libs (parallel-gs,
    # parallel-gs-dump) and granite-vulkan (Context/Device/WSI, which the
    # library-internal backend uses). Remaining granite static deps resolve
    # symbol-by-symbol via the normal transitive link. The executables see
    # only the C ABI in gs_parallel_api.h (RT_GS_API exports; explicit
    # exports also make MSVC emit the import library). The gs_parallel_*.cpp
    # sources below are ours (MIT) but compile into the library, so they
    # alone may use the Granite/paraLLEl-GS C++ interfaces. They are one
    # shim split by unit; gs_parallel_impl.h is the RtPgs type they share.
    add_library(icorecomp-parallel-gs SHARED
        src/runtime/gs/gs_parallel_lib.cpp
        src/runtime/gs/gs_parallel_scanout.cpp
        src/runtime/gs/gs_parallel_present.cpp
        src/runtime/gs/gs_parallel_overlay.cpp
        src/runtime/gs/gs_parallel_abi.cpp
        src/runtime/gs/gs_probe_lib.cpp
    )
    target_compile_definitions(icorecomp-parallel-gs PRIVATE ICORECOMP_PGS_BUILD_DLL)
    target_include_directories(icorecomp-parallel-gs PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
    target_link_libraries(icorecomp-parallel-gs PRIVATE
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,parallel-gs>"
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,parallel-gs-dump>"
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,granite-vulkan>"
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(icorecomp-parallel-gs PRIVATE -Wall -Wextra -fno-strict-aliasing)
        # ld64 spells this "-undefined error", and it is already the default
        # there, so the GNU spelling is only passed where GNU ld or lld
        # reads it. Apple's linker rejects the option outright.
        if(NOT APPLE)
            target_link_options(icorecomp-parallel-gs PRIVATE -Wl,--no-undefined)
        endif()
    endif()
    if(APPLE)
        # The install tree puts the dylib next to the executable, whose
        # rpath is @loader_path (see the packaging section). Without this
        # the install_name would keep naming the build tree.
        set_target_properties(icorecomp-parallel-gs PROPERTIES INSTALL_NAME_DIR "@rpath")
    endif()
    if(ICORECOMP_HAVE_SDL)
        # The library no longer creates a window: the exe's window service
        # does, and gs_parallel.cpp passes the SDL_Window* in through
        # RtPgsCreateOptions::host_window. SDL is still linked here because
        # the WSI platform inside the library queries the adopted window's
        # Vulkan instance extensions. Same shared SDL as the executable.
        target_compile_definitions(icorecomp-parallel-gs PRIVATE ICORECOMP_PGS_SDL)
        target_link_libraries(icorecomp-parallel-gs PRIVATE SDL3::SDL3)
    endif()

    # Runtime: the thin GsBackend adapter over the C ABI (ours).
    target_sources(icorecomp-runtime PRIVATE src/runtime/gs/gs_parallel.cpp)
    target_compile_definitions(icorecomp-runtime PRIVATE ICORECOMP_HAVE_PARALLEL_GS)
    target_link_libraries(icorecomp-runtime PRIVATE icorecomp-parallel-gs)

    # Headless raw-dump replay tool (ours): the CI-able consumer-side check
    # of the dump writer format. Only the C ABI; no Granite headers.
    add_executable(icorecomp-gs-replay src/runtime/gs/gs_replay_main.cpp)
    target_include_directories(icorecomp-gs-replay PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
    target_link_libraries(icorecomp-gs-replay PRIVATE icorecomp-parallel-gs)
    # --backend=parallel and --probe are compiled in only when there is a
    # paraLLEl-GS to reach; the tool also builds with the native renderer
    # alone (see the native GS renderer block below).
    target_compile_definitions(icorecomp-gs-replay PRIVATE ICORECOMP_HAVE_PARALLEL_GS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(icorecomp-gs-replay PRIVATE -Wall -Wextra -fno-strict-aliasing)
    endif()

    message(STATUS "icorecomp: paraLLEl-GS backend enabled (window service: ${ICORECOMP_HAVE_SDL})")
else()
    message(STATUS "icorecomp: paraLLEl-GS backend disabled; ICORECOMP_GS=parallel will fail loudly")
endif()

# ---- native GS renderer (src/runtime/rhi, src/runtime/gs/render) -----------
#
# The clean-room replacement for the paraLLEl-GS backend: our own Vulkan RHI
# (src/runtime/rhi, MIT) and our own GS implementation (src/runtime/gs/render,
# MIT) with no LGPL code anywhere in the chain. It builds straight into
# icorecomp-runtime, unlike paraLLEl-GS, because there is no license boundary
# to keep it behind.
#
# Independent of ICORECOMP_PARALLEL_GS: both can be on, and ICORECOMP_GS picks
# between them at run time (gs/gs_select.cpp). See docs/GS_RENDERER.md.
#
# The shaders are compiled ahead of time and committed. When the generated
# index header is absent the renderer is switched off with the command to
# produce it, rather than failing the configure: a tree that has not run the
# generator still builds everything else.
option(ICORECOMP_NATIVE_GS "Build the clean-room native GS renderer" ON)

if(ICORECOMP_NATIVE_GS)
    set(ICORECOMP_GSR_MISSING "")
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/volk/volk.c")
        list(APPEND ICORECOMP_GSR_MISSING
             "third_party/volk (git submodule update --init third_party/volk)")
    endif()
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers/include/vulkan/vulkan.h")
        list(APPEND ICORECOMP_GSR_MISSING
             "third_party/Vulkan-Headers (git submodule update --init third_party/Vulkan-Headers)")
    endif()
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/src/runtime/rhi/rhi_shaders.h")
        list(APPEND ICORECOMP_GSR_MISSING
             "src/runtime/rhi/rhi_shaders.h (run tools/gen_gs_shaders.sh)")
    endif()
    if(ICORECOMP_GSR_MISSING)
        message(STATUS "icorecomp: native GS renderer disabled, missing: ${ICORECOMP_GSR_MISSING}")
        set(ICORECOMP_NATIVE_GS OFF)
    endif()
endif()

if(ICORECOMP_NATIVE_GS)
    # Platform surface support, compiled in only where the headers exist. A
    # headless build (the replay tool, CI on lavapipe) needs none of them.
    include(CheckIncludeFile)
    set(ICORECOMP_RHI_PLATFORM_DEFS "")
    if(WIN32)
        list(APPEND ICORECOMP_RHI_PLATFORM_DEFS ICORECOMP_RHI_WIN32 VK_USE_PLATFORM_WIN32_KHR)
    else()
        check_include_file("X11/Xlib.h" ICORECOMP_HAVE_XLIB_H)
        check_include_file("wayland-client.h" ICORECOMP_HAVE_WAYLAND_H)
        if(ICORECOMP_HAVE_XLIB_H)
            list(APPEND ICORECOMP_RHI_PLATFORM_DEFS ICORECOMP_RHI_XLIB VK_USE_PLATFORM_XLIB_KHR)
        endif()
        if(ICORECOMP_HAVE_WAYLAND_H)
            list(APPEND ICORECOMP_RHI_PLATFORM_DEFS ICORECOMP_RHI_WAYLAND VK_USE_PLATFORM_WAYLAND_KHR)
        endif()
    endif()

    # ---- D3D12 backend (src/runtime/rhi/d3d12) -----------------------------
    #
    # Windows only, and only where the headers are actually there: the mingw
    # cross build gets them from the mingw-w64 header package, and a host
    # without that package still builds everything else. Nothing is linked
    # against d3d12 or dxgi. The backend resolves both through LoadLibrary at
    # run time (rhi/d3d12/rhi_d3d12_loader.h says why), so one executable
    # starts on a machine with no D3D12 and reports that instead of failing to
    # load.
    #
    # dxcapi.h is in the required set because the backend includes it for the
    # developer fallback that compiles the committed HLSL at run time. Nothing
    # links dxcompiler.dll; only the declarations are needed.
    set(ICORECOMP_D3D12 OFF)
    if(WIN32)
        include(CheckIncludeFileCXX)
        include(CheckCXXSourceCompiles)
        check_include_file_cxx("d3d12.h" ICORECOMP_HAVE_D3D12_H)
        check_include_file_cxx("dxgi1_6.h" ICORECOMP_HAVE_DXGI1_6_H)
        check_include_file_cxx("d3d12sdklayers.h" ICORECOMP_HAVE_D3D12SDKLAYERS_H)
        # dxcapi.h includes no Windows header of its own and does not compile
        # on its own (HRESULT, UINT32 and IUnknown are all undeclared), so it
        # is checked the way the backend actually includes it: after one.
        check_cxx_source_compiles(
            "#include <windows.h>\n#include <dxcapi.h>\nint main() { return 0; }"
            ICORECOMP_HAVE_DXCAPI_H)
        set(ICORECOMP_D3D12_MISSING "")
        if(NOT ICORECOMP_HAVE_D3D12_H)
            list(APPEND ICORECOMP_D3D12_MISSING "d3d12.h")
        endif()
        if(NOT ICORECOMP_HAVE_DXGI1_6_H)
            list(APPEND ICORECOMP_D3D12_MISSING "dxgi1_6.h")
        endif()
        if(NOT ICORECOMP_HAVE_D3D12SDKLAYERS_H)
            list(APPEND ICORECOMP_D3D12_MISSING "d3d12sdklayers.h")
        endif()
        if(NOT ICORECOMP_HAVE_DXCAPI_H)
            list(APPEND ICORECOMP_D3D12_MISSING "dxcapi.h")
        endif()
        if(ICORECOMP_D3D12_MISSING)
            message(STATUS "icorecomp: D3D12 backend disabled, missing headers: "
                           "${ICORECOMP_D3D12_MISSING}")
        else()
            set(ICORECOMP_D3D12 ON)
            list(APPEND ICORECOMP_RHI_PLATFORM_DEFS ICORECOMP_RHI_D3D12)
            message(STATUS "icorecomp: D3D12 backend enabled")
        endif()
    endif()

    # ---- the DirectX Shader Compiler DLLs, for the run-time HLSL path ------
    #
    # Nothing here. The build stopped installing dxcompiler.dll and dxil.dll
    # beside ico.exe on 2026-09-05, so the discovery that used to fill
    # ICORECOMP_DXC_FILES had no consumer left; it is removed rather than left
    # printing a status line about a fallback that is not shipped. The
    # candidate list it searched also carried one developer's absolute
    # checkout path, which does not belong in a committed build file.
    #
    # What ships is the compiled-in DXIL (src/runtime/rhi/rhi_shaders_dxil.h,
    # checked against its HLSL below). A developer who wants the run-time HLSL
    # path instead copies dxcompiler.dll and dxil.dll from a DirectX Shader
    # Compiler release next to ico.exe by hand; the loader looks for them on
    # the module search path and says so when they are missing
    # (src/runtime/rhi/d3d12/rhi_d3d12_loader.cpp). Both DLLs import
    # MSVCP140.dll, VCRUNTIME140.dll and VCRUNTIME140_1.dll, so a machine with
    # no Visual C++ redistributable fails their load with ERROR_MOD_NOT_FOUND
    # (126), which reads as "file not found" in every message that does not
    # print the error number. That is why the compiled-in DXIL is the shipping
    # path.

    # ---- the committed shader blobs against their sources ------------------
    #
    # Two links, one script. First, the GLSL against the SPIR-V, HLSL and MSL
    # built from it: all three come out of one run of tools/gen_gs_shaders.sh,
    # so a shader edited without rerunning it leaves all three stale and
    # mutually consistent, which nothing else in the tree can see. The
    # generator records the SHA-1 of every source it read in rhi_shaders.h.
    # Second, the HLSL against the DXIL: nothing at run time ties a DXIL
    # container to the source it was built from, because the backend matches a
    # shader by SPIR-V content and then looks the DXIL up by name, so
    # rerunning only gen_gs_shaders.sh would leave Vulkan on the new shader
    # and D3D12 on the old one with no line in the log. Those digests are in
    # rhi_shaders_dxil.h.
    #
    # This recomputes both on every build of the runtime and fails when one
    # has drifted. Cheap: a dozen SHA-1s of small text files. It runs wherever
    # the native renderer is built, not only where D3D12 is, because the first
    # link has nothing to do with D3D12.
    #
    # Exit 2 is "no index header at all", which is the state of a checkout
    # that has never generated anything, and is not a failure.
    if(EXISTS "${CMAKE_SOURCE_DIR}/src/runtime/rhi/rhi_shaders.h")
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_custom_target(icorecomp-shaders-fresh ALL
                COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_SOURCE_DIR}/tools/check_shaders_fresh.py"
                COMMENT "Checking the committed shader blobs against their sources"
                VERBATIM)
            add_dependencies(icorecomp-runtime icorecomp-shaders-fresh)
        else()
            message(STATUS "icorecomp: no Python 3 interpreter, so the committed shader "
                           "blobs are not checked against their sources this build; the "
                           "shader-blobs CI job still does it")
        endif()
    endif()

    # ---- Metal backend (src/runtime/rhi/metal) -----------------------------
    #
    # Apple only. Written in Objective-C++ rather than against metal-cpp,
    # because the window path needs Core Animation and AppKit whatever the
    # Metal binding is; src/runtime/rhi/metal/rhi_metal.h says so at more
    # length. The .mm files are compiled with ARC, set per source file below
    # rather than on the target so the C++ translation units beside them keep
    # their own flags.
    #
    # Four frameworks and nothing loaded at run time: Metal and QuartzCore are
    # present on every macOS this port targets (14 and later), unlike Vulkan,
    # which arrives through MoltenVK and so has to be found with volk. There
    # is nothing here that can be absent, so there is no indirection.
    #
    # OBJCXX is enabled here and not in the project() call, so a non-Apple
    # configure never goes looking for an Objective-C++ compiler.
    set(ICORECOMP_METAL OFF)
    if(APPLE)
        enable_language(OBJCXX)
        find_library(ICORECOMP_FRAMEWORK_METAL Metal)
        find_library(ICORECOMP_FRAMEWORK_QUARTZCORE QuartzCore)
        find_library(ICORECOMP_FRAMEWORK_COCOA Cocoa)
        find_library(ICORECOMP_FRAMEWORK_FOUNDATION Foundation)
        set(ICORECOMP_METAL_MISSING "")
        foreach(fw METAL QUARTZCORE COCOA FOUNDATION)
            if(NOT ICORECOMP_FRAMEWORK_${fw})
                list(APPEND ICORECOMP_METAL_MISSING "${fw}")
            endif()
        endforeach()
        if(ICORECOMP_METAL_MISSING)
            message(STATUS "icorecomp: Metal backend disabled, frameworks not found: "
                           "${ICORECOMP_METAL_MISSING}")
        else()
            set(ICORECOMP_METAL ON)
            list(APPEND ICORECOMP_RHI_PLATFORM_DEFS ICORECOMP_RHI_METAL)
            message(STATUS "icorecomp: Metal backend enabled")
        endif()
    endif()

    # volk (MIT) as a static library with hidden visibility. Hidden matters:
    # libicorecomp-parallel-gs carries Granite's own copy of volk, and two
    # copies of the same global function pointer table must not interpose on
    # each other. Hidden visibility binds our objects to our own copy at
    # static link time and leaves the library's alone.
    add_library(icorecomp-volk STATIC third_party/volk/volk.c)
    target_include_directories(icorecomp-volk PUBLIC
        ${CMAKE_SOURCE_DIR}/third_party/volk
        ${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers/include)
    target_compile_definitions(icorecomp-volk PUBLIC ${ICORECOMP_RHI_PLATFORM_DEFS})
    set_target_properties(icorecomp-volk PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        C_VISIBILITY_PRESET hidden)
    if(NOT WIN32)
        # volk dlopen()s the loader; on Linux that needs libdl on older libc.
        target_link_libraries(icorecomp-volk PUBLIC ${CMAKE_DL_LIBS})
    endif()

    set(ICORECOMP_GSR_SOURCES
        src/runtime/rhi/vulkan/rhi_vulkan_device.cpp
        src/runtime/rhi/vulkan/rhi_vulkan_cmd.cpp
        src/runtime/gs/render/gs_vram.cpp
        src/runtime/gs/render/gs_clut.cpp
        src/runtime/gs/render/gs_crtc.cpp
        src/runtime/gs/render/gs_dump_parse.cpp
        src/runtime/gs/render/gs_draw.cpp
        src/runtime/gs/render/gs_native.cpp
    )
    if(ICORECOMP_D3D12)
        list(APPEND ICORECOMP_GSR_SOURCES
            src/runtime/rhi/d3d12/rhi_d3d12_loader.cpp
            src/runtime/rhi/d3d12/rhi_d3d12_shaders.cpp
            src/runtime/rhi/d3d12/rhi_d3d12_device.cpp
            src/runtime/rhi/d3d12/rhi_d3d12_cmd.cpp
        )
    endif()
    if(ICORECOMP_METAL)
        list(APPEND ICORECOMP_GSR_SOURCES
            src/runtime/rhi/metal/rhi_metal_shaders.mm
            src/runtime/rhi/metal/rhi_metal_device.mm
            src/runtime/rhi/metal/rhi_metal_cmd.mm
        )
        # ARC per source file. Every id member in rhi_metal.h is a strong
        # reference and the backend does no retain or release of its own, so
        # these three cannot be compiled without it.
        set_source_files_properties(
            src/runtime/rhi/metal/rhi_metal_shaders.mm
            src/runtime/rhi/metal/rhi_metal_device.mm
            src/runtime/rhi/metal/rhi_metal_cmd.mm
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    endif()

    target_sources(icorecomp-runtime PRIVATE ${ICORECOMP_GSR_SOURCES})
    target_compile_definitions(icorecomp-runtime PRIVATE ICORECOMP_NATIVE_GS)
    target_link_libraries(icorecomp-runtime PRIVATE icorecomp-volk)

    # The headless replay tool. It exists already when the paraLLEl-GS backend
    # is built; otherwise it is created here, so a build with only the native
    # renderer still has the CI-able consumer of the dump format.
    if(NOT TARGET icorecomp-gs-replay)
        add_executable(icorecomp-gs-replay src/runtime/gs/gs_replay_main.cpp)
        target_include_directories(icorecomp-gs-replay PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(icorecomp-gs-replay PRIVATE -Wall -Wextra -fno-strict-aliasing)
        endif()
    endif()
    # The renderer's own logging goes through runtime.h's rt_log_* and
    # rt_fatal. The runtime executable has log.cpp; this tool does not, and
    # pulling log.cpp in would drag the console, the state directory and the
    # register dumper with it. It links the stub translation unit instead,
    # the same arrangement the VIF1 and GS ring selftests use.
    target_sources(icorecomp-gs-replay PRIVATE
        ${ICORECOMP_GSR_SOURCES}
        src/runtime/gs/render/gs_render_log_stubs.cpp
        # gs_native.cpp asks the window service for the window it should
        # present into. This tool has none and is never given ICORECOMP_HAVE_SDL,
        # so the service compiles to its stub half (host/window_service.cpp)
        # and every answer is "no window", which is what sends the device
        # down its headless path.
        src/runtime/host/window_service.cpp)
    # runtime.h pulls in include/recomp_api.h for the R5900Context rt_fatal
    # takes; the tool needs that directory even though it runs no guest code.
    target_include_directories(icorecomp-gs-replay PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_compile_definitions(icorecomp-gs-replay PRIVATE ICORECOMP_NATIVE_GS)
    target_link_libraries(icorecomp-gs-replay PRIVATE icorecomp-volk)

    if(ICORECOMP_METAL)
        # Both consumers of ICORECOMP_GSR_SOURCES need the frameworks, so the
        # link is done once here where both targets exist.
        foreach(tgt icorecomp-runtime icorecomp-gs-replay)
            target_link_libraries(${tgt} PRIVATE
                ${ICORECOMP_FRAMEWORK_METAL}
                ${ICORECOMP_FRAMEWORK_QUARTZCORE}
                ${ICORECOMP_FRAMEWORK_COCOA}
                ${ICORECOMP_FRAMEWORK_FOUNDATION})
        endforeach()
    endif()

    # ---- swizzle selftest --------------------------------------------------
    #
    # Round-trips gs/render/gs_swizzle.h, the header the C++ renderer and the
    # GLSL shaders share, against gs_swizzle_reference.cpp, which is written
    # from the manual's literal column and page tables. Header only plus that
    # one reference, so it links no runtime services and needs no GPU.
    add_executable(icorecomp-gs-swizzle-selftest
        src/runtime/gs/render/gs_swizzle_reference.cpp
        src/runtime/gs/render/gs_swizzle_selftest.cpp
    )
    target_include_directories(icorecomp-gs-swizzle-selftest PRIVATE
        ${CMAKE_SOURCE_DIR}/src/runtime)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(icorecomp-gs-swizzle-selftest PRIVATE
            -Wall -Wextra -fno-strict-aliasing)
    endif()

    # ---- GIF decode selftest ----------------------------------------------
    #
    # Builds packets and checks the register stream gs/render/gif_decode.h
    # produces, including the Q latch and a packet split across submissions.
    # Header only; nothing else is linked.
    add_executable(icorecomp-gif-decode-selftest
        src/runtime/gs/render/gif_decode_selftest.cpp
    )
    target_include_directories(icorecomp-gif-decode-selftest PRIVATE
        ${CMAKE_SOURCE_DIR}/src/runtime)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(icorecomp-gif-decode-selftest PRIVATE
            -Wall -Wextra -fno-strict-aliasing)
    endif()

    # ---- rasteriser selftest -----------------------------------------------
    #
    # Coverage masks written out by hand from the rule in gs/render/gs_prim.h,
    # the vertex queue's retention rules for every primitive kind, PACKED
    # against REGLIST assembly, and the Z plane solve. Links the assembler and
    # the log stubs; no GPU, no Vulkan, no disc.
    add_executable(icorecomp-gs-raster-selftest
        src/runtime/gs/render/gs_clut.cpp
        src/runtime/gs/render/gs_draw.cpp
        src/runtime/gs/render/gs_raster_selftest.cpp
        src/runtime/gs/render/gs_render_log_stubs.cpp
    )
    target_include_directories(icorecomp-gs-raster-selftest PRIVATE
        ${CMAKE_SOURCE_DIR}/src/runtime
        ${CMAKE_SOURCE_DIR}/include)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(icorecomp-gs-raster-selftest PRIVATE
            -Wall -Wextra -fno-strict-aliasing)
    endif()

    # ---- texture selftest --------------------------------------------------
    #
    # Milestone (c)'s CPU-checkable half: the CLAMP formulas, the CSM1 CLUT
    # arrangement, TEXA, the TFX table, the CLD load rules as a state machine
    # against a real palette in a LocalMemory, the page tracker, the mip base
    # rules and the LOD formula. Expected values are written out by hand from
    # the manual. Links the CLUT cache, the assembler (for the page tracker)
    # and the log stubs; no GPU, no Vulkan, no disc.
    add_executable(icorecomp-gs-texture-selftest
        src/runtime/gs/render/gs_clut.cpp
        src/runtime/gs/render/gs_draw.cpp
        src/runtime/gs/render/gs_texture_selftest.cpp
        src/runtime/gs/render/gs_render_log_stubs.cpp
    )
    target_include_directories(icorecomp-gs-texture-selftest PRIVATE
        ${CMAKE_SOURCE_DIR}/src/runtime
        ${CMAKE_SOURCE_DIR}/include)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(icorecomp-gs-texture-selftest PRIVATE
            -Wall -Wextra -fno-strict-aliasing)
    endif()

    # ---- synthetic dump generator -----------------------------------------
    #
    # Writes GS streams in our own raw format from content this repository
    # generates, so the renderer and the replay tool can be exercised with no
    # disc and no capture. The generator is committed; its output is not.
    add_executable(icorecomp-gs-gen-dump tools/gs_gen_dump/main.cpp)
    target_include_directories(icorecomp-gs-gen-dump PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(icorecomp-gs-gen-dump PRIVATE
            -Wall -Wextra -fno-strict-aliasing)
    endif()

    message(STATUS "icorecomp: native GS renderer enabled (surface support: ${ICORECOMP_RHI_PLATFORM_DEFS})")
else()
    message(STATUS "icorecomp: native GS renderer disabled; ICORECOMP_GS=native will fail loudly")
endif()

# ---- RmlUi settings/launcher UI (third_party/rmlui, third_party/freetype) --
#
# The UI lives entirely in the MIT executable: RmlUi (MIT) and FreeType (FTL,
# MIT-compatible) are linked statically into icorecomp-runtime, and the only
# thing that crosses the LGPL boundary is the plain POD overlay draw list in
# gs/gs_parallel_api.h. See src/runtime/ui/ui.h.
#
# Independent of ICORECOMP_PARALLEL_GS on purpose: with the live backend off
# the UI sources still compile and link (that is the cheap CI gate on both
# Linux and the mingw cross build), and at run time rt_ui_init reports there
# is no window and disables itself.
option(ICORECOMP_UI "Build the RmlUi settings/launcher UI" ON)

if(ICORECOMP_UI AND NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/rmlui/CMakeLists.txt")
    message(STATUS "icorecomp: rmlui submodule not initialized "
                   "(git submodule update --init third_party/rmlui); UI disabled")
    set(ICORECOMP_UI OFF)
endif()
if(ICORECOMP_UI AND NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/freetype/CMakeLists.txt")
    message(STATUS "icorecomp: freetype submodule not initialized "
                   "(git submodule update --init third_party/freetype); UI disabled")
    set(ICORECOMP_UI OFF)
endif()

if(ICORECOMP_UI)
    # Both dependencies static into the executable. FreeType's
    # cmake_minimum_required is 3.0...3.5, so CMP0077 is OLD in its scope and
    # its option() calls overwrite plain variables: every FT_* knob below has
    # to be a cache entry to survive. RmlUi's range reaches 3.27, where
    # CMP0077 is NEW, so a normal variable is enough there.
    set(BUILD_SHARED_LIBS OFF)
    set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
    # FreeType honors this (its CMakeLists guards every install() with it):
    # nothing of FreeType belongs in the packaged folder, only the linked
    # objects inside the executable.
    set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/freetype"
                     "${CMAKE_BINARY_DIR}/icorecomp-freetype" EXCLUDE_FROM_ALL SYSTEM)
    # FreeType only sets EXPORT_NAME Freetype::Freetype on an interface
    # target; there is no such alias in the build tree, and RmlUi checks for
    # the target by name.
    if(NOT TARGET Freetype::Freetype)
        add_library(Freetype::Freetype ALIAS freetype)
    endif()

    # cmake/FindFreetype.cmake answers RmlUi's find_package(Freetype) with the
    # target above; see the comment in that file.
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

    set(RMLUI_FONT_ENGINE "freetype" CACHE STRING "" FORCE)
    set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
    set(RMLUI_LUA_BINDINGS OFF CACHE BOOL "" FORCE)
    set(RMLUI_SVG_PLUGIN OFF CACHE BOOL "" FORCE)
    set(RMLUI_LOTTIE_PLUGIN OFF CACHE BOOL "" FORCE)
    set(RMLUI_HARFBUZZ_SAMPLE OFF CACHE BOOL "" FORCE)
    set(RMLUI_TRACY_PROFILING OFF CACHE BOOL "" FORCE)
    # RMLUI_BACKEND is left alone: "none" is not one of its accepted values
    # (CMake/OptionsLists.cmake) and the setting only picks a backend for the
    # samples, which are off. RMLUI_INSTALL_RUNTIME_DEPENDENCIES likewise:
    # RmlUi only declares that option on WIN32 with a shared build.
    add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/rmlui"
                     "${CMAKE_BINARY_DIR}/icorecomp-rmlui" EXCLUDE_FROM_ALL SYSTEM)

    # ui_events.cpp and ui_rebind.cpp are empty without SDL, exactly like
    # host/window.cpp: there are no events to translate or capture.
    target_sources(icorecomp-runtime PRIVATE
        src/runtime/ui/ui.cpp
        src/runtime/ui/ui_render.cpp
        src/runtime/ui/ui_system.cpp
        src/runtime/ui/ui_events.cpp
        src/runtime/ui/ui_rebind.cpp
        src/runtime/ui/ui_settings_model.cpp
        src/runtime/ui/ui_menu_cursor.cpp
        src/runtime/ui/ui_achievements_model.cpp
        src/runtime/ui/ui_launcher.cpp
        src/runtime/ui/title_logo.cpp
        src/runtime/ui/cursor_image.cpp
        src/runtime/ui/data_df.cpp
        src/runtime/ui/ps2_icon.cpp
        src/runtime/ui/ps2_icon_render.cpp
        src/runtime/ui/save_icon.cpp
        src/runtime/host/inflate.cpp
    )
    target_compile_definitions(icorecomp-runtime PRIVATE ICORECOMP_UI)
    # RmlUi::Core only. The debugger is a separate target that nothing here
    # links, so EXCLUDE_FROM_ALL means it is never built.
    target_link_libraries(icorecomp-runtime PRIVATE RmlUi::Core)

    message(STATUS "icorecomp: RmlUi UI enabled (static RmlUi 6.3 + FreeType 2.13.3)")
else()
    message(STATUS "icorecomp: RmlUi UI disabled; the settings menu and launcher are not built")
endif()

# ---- packaging -------------------------------------------------------------
#
# `cmake --install <build> --prefix <dir>` lays down a self-contained,
# runnable folder: the executable, the two shared libraries it loads, and a
# README. Nothing else. The runtime resolves its config, saves, log and disc
# probe against the executable's own directory (rt_base_dir in
# src/runtime/loader.cpp), so the folder works wherever it is unzipped and
# however it is launched, and the user's disc image is the only thing they
# add to it.
#
# No config/ is installed on purpose: a packaged run falls back to the
# compiled-in pins (loader.cpp) and creates saves/mc0 on first use.
#
# macOS is the one platform where the layout is not flat at the top level:
# the install prefix holds "ICO Recomp.app", and everything that would sit
# beside the executable elsewhere sits inside Contents/MacOS instead. That
# keeps rt_base_dir's contract intact (ui/, saves/, screenshots/ and
# settings.json are still the executable's own siblings), while giving
# Finder the bundle it needs to launch the thing at all. The user's disc
# image goes in Contents/MacOS as well; see docs/MACOS.md.

if(WIN32)
    set(ICORECOMP_PACKAGE_PLATFORM "for Windows (x86_64)")
elseif(APPLE)
    set(ICORECOMP_PACKAGE_PLATFORM "for macOS (Apple Silicon)")
else()
    set(ICORECOMP_PACKAGE_PLATFORM "for Linux (x86_64)")
endif()
set(ICORECOMP_PACKAGE_EXE "$<TARGET_FILE_NAME:icorecomp-runtime>")

# Where the executable, the shared libraries and ui/ go, relative to the
# install prefix. Everything below installs through these two variables so
# the bundle layout is stated once.
if(APPLE)
    set(ICORECOMP_BUNDLE_NAME "ICO Recomp.app")
    set(ICORECOMP_INSTALL_BIN "${ICORECOMP_BUNDLE_NAME}/Contents/MacOS")
    set(ICORECOMP_INSTALL_UI "${ICORECOMP_INSTALL_BIN}/ui")
else()
    set(ICORECOMP_INSTALL_BIN ".")
    set(ICORECOMP_INSTALL_UI "ui")
endif()

if(APPLE)
    # Mach-O's spelling of $ORIGIN. Same intent as the Linux branch: the
    # installed executable finds libicorecomp-parallel-gs.dylib and
    # libSDL3.dylib beside itself, inside Contents/MacOS.
    set_target_properties(icorecomp-runtime PROPERTIES
        INSTALL_RPATH "@loader_path"
        BUILD_WITH_INSTALL_RPATH FALSE)
elseif(NOT WIN32)
    # Flat layout: the installed executable finds its libraries beside
    # itself. Windows already searches the exe's own directory first.
    set_target_properties(icorecomp-runtime PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH FALSE)
endif()

if(WIN32)
    # timeBeginPeriod, for the frame limiter's sleep resolution. Outside the
    # generated-code branch: portable.h calls it in every configuration,
    # including stub mode.
    target_link_libraries(icorecomp-runtime PRIVATE winmm)
endif()

install(TARGETS icorecomp-runtime RUNTIME DESTINATION "${ICORECOMP_INSTALL_BIN}")

# dxcompiler.dll and dxil.dll beside the executable, when a DXC release was
# found at configure time. PROGRAMS and not FILES, for the reason SDL3.dll is
# installed that way below: FILES installs 644, and a DLL without the execute
# bit fails to load once the mode bits survive the trip to the target machine.
#
# They have to be beside ico.exe and not in a subfolder: the backend loads
# dxcompiler.dll by full path from the executable's own directory, and
# dxcompiler.dll then finds dxil.dll by base name in that same directory.
# See src/runtime/rhi/d3d12/rhi_d3d12_loader.cpp.
# Not installed since 2026-09-05: the native D3D12 backend is the only user
# of dxcompiler.dll and dxil.dll, and the native renderers were withdrawn
# from the player's build (docs/SETTINGS.md, "Renderer backends"). The
# files are still found at configure time for the replay tool and CI.

if(ICORECOMP_UI)
    # The UI's documents, stylesheets and fonts, found at run time under
    # rt_base_dir()/ui (src/runtime/ui/ui.cpp). Each font's OFL notice ships
    # beside it (PlayfairDisplay-OFL.txt, JetBrainsMono-OFL.txt), which is
    # what the license requires.
    install(DIRECTORY ui/ DESTINATION "${ICORECOMP_INSTALL_UI}")
endif()

# The renderer, and the licence sentence that goes with it. The two are
# written together because they have to agree: a build with
# ICORECOMP_PARALLEL_GS=OFF ships no LGPL code at all, and a README that
# claims LGPL obligations over this repository's own MIT renderer is a
# licence statement about code that is not there.
if(ICORECOMP_PARALLEL_GS)
    # RUNTIME catches the Windows DLL, LIBRARY the Linux .so and the macOS
    # .dylib.
    install(TARGETS icorecomp-parallel-gs
        RUNTIME DESTINATION "${ICORECOMP_INSTALL_BIN}"
        LIBRARY DESTINATION "${ICORECOMP_INSTALL_BIN}")
    set(ICORECOMP_PACKAGE_GSLIB "$<TARGET_FILE_NAME:icorecomp-parallel-gs>")
    set(ICORECOMP_PACKAGE_LIBS "    ${ICORECOMP_PACKAGE_GSLIB}")
    set(ICORECOMP_PACKAGE_GS_LICENSE
        "It loads ${ICORECOMP_PACKAGE_GSLIB}, which is paraLLEl-GS under LGPLv3+, and
SDL3 under the zlib license, both as separate shared libraries.")
    set(ICORECOMP_PACKAGE_LIBS_NOTE
        "The two libraries are the renderer (paraLLEl-GS) and SDL3, which provides
the window, controller input and audio. Both must stay next to the
executable.")
elseif(ICORECOMP_NATIVE_GS)
    set(ICORECOMP_PACKAGE_GSLIB "the native renderer, linked into the executable")
    set(ICORECOMP_PACKAGE_LIBS "")
    set(ICORECOMP_PACKAGE_GS_LICENSE
        "The renderer is this port's own code, under the same MIT license, linked
into the executable. SDL3 ships beside it as a separate shared library
under the zlib license.")
    set(ICORECOMP_PACKAGE_LIBS_NOTE
        "The renderer is linked into the executable. The one library beside it is
SDL3, which provides the window, controller input and audio; it must stay
next to the executable.")
else()
    set(ICORECOMP_PACKAGE_GSLIB "no renderer (this build has none)")
    set(ICORECOMP_PACKAGE_LIBS "    (no shared libraries in this build)")
    set(ICORECOMP_PACKAGE_GS_LICENSE
        "This build carries no renderer at all. SDL3, where it ships, is under the
zlib license.")
    set(ICORECOMP_PACKAGE_LIBS_NOTE
        "This build carries no renderer, so it cannot draw a picture.")
endif()

# SDL ships whenever the window service is built, which no longer depends on
# the paraLLEl-GS library being there: the executable links SDL itself.
if(ICORECOMP_HAVE_SDL)
    # SDL3 comes from an EXCLUDE_FROM_ALL subdirectory, so install its
    # file rather than its target. PROGRAMS, not FILES: FILES installs
    # 644, and a DLL without the execute bit fails to load with
    # STATUS_ACCESS_DENIED (0xC0000022) whenever the mode bits survive
    # the trip to the target machine, which they do over a share or a
    # permission-preserving unzip. On ELF platforms the soname symlink
    # has to come along too, since that is the name the exe links. On
    # macOS that is the versioned dylib name in SDL3's install_name.
    #
    # install(FILES) on the soname copies the link, not the file it points
    # at: measured on Linux, where the installed libSDL3.so.0 is a 16-byte
    # symlink to libSDL3.so.0.5.0 beside it, so the package carries one
    # copy of SDL and one link, not two copies.
    install(PROGRAMS $<TARGET_FILE:SDL3-shared> DESTINATION "${ICORECOMP_INSTALL_BIN}")
    if(NOT WIN32)
        install(FILES $<TARGET_SONAME_FILE:SDL3-shared>
                DESTINATION "${ICORECOMP_INSTALL_BIN}")
    endif()
    if(ICORECOMP_PACKAGE_LIBS STREQUAL "")
        set(ICORECOMP_PACKAGE_LIBS "    $<TARGET_FILE_NAME:SDL3-shared>")
    else()
        string(APPEND ICORECOMP_PACKAGE_LIBS "\n    $<TARGET_FILE_NAME:SDL3-shared>")
    endif()
elseif(ICORECOMP_PACKAGE_LIBS STREQUAL "")
    set(ICORECOMP_PACKAGE_LIBS "    (no shared libraries in this build)")
endif()

# The ui/ folder ships only when the UI is built, so the README's folder
# listing names it the same way it names the libraries: through a variable,
# not a fixed line.
if(ICORECOMP_UI)
    set(ICORECOMP_PACKAGE_UI "\n    ui/")
    set(ICORECOMP_PACKAGE_UI_NOTE
        "\n\nui/ holds the settings menu's documents, stylesheet and fonts, plus those\nfonts' licenses. It stays next to the executable as well.")
    # The FreeType License requires a binary distribution to say in its
    # documentation that the software is based in part on the FreeType
    # Team's work; the two OFL fonts ship their notices beside them.
    set(ICORECOMP_PACKAGE_UI_LICENSE
        "\n\nThe settings menu and launcher are built with RmlUi under the MIT license\nand FreeType under the FreeType License. Portions of this software are\ncopyright (c) 2024 The FreeType Project (https://www.freetype.org). All\nrights reserved.\n\nThe two fonts in ui/ are licensed under the SIL Open Font License, Version\n1.1: JetBrains Mono, copyright 2020 The JetBrains Mono Project Authors, and\nPlayfair Display, copyright 2017 The Playfair Display Project Authors, with\nthe Reserved Font Name Playfair Display. Each font's full notice ships beside\nit in ui/fonts.")
else()
    set(ICORECOMP_PACKAGE_UI "")
    set(ICORECOMP_PACKAGE_UI_NOTE "")
    set(ICORECOMP_PACKAGE_UI_LICENSE "")
endif()

# Where the files the two sections above list actually sit. Flat beside the
# executable everywhere but macOS, where the install puts them inside the
# bundle and the README is installed next to it; the macOS section further
# down repeats the detail, and this is what stops the opening contradicting
# it.
if(APPLE)
    set(ICORECOMP_PACKAGE_LAYOUT_NOTE
        "\n\nOn macOS the executable and everything listed below live inside the bundle,\nin ICO Recomp.app/Contents/MacOS, not in this folder. See the macOS section\nbelow, which is where the disc image goes as well.")
else()
    set(ICORECOMP_PACKAGE_LAYOUT_NOTE "")
endif()

# Which retail disc this package runs, in the words a player would use.
set(ICORECOMP_PACKAGE_REGION "PAL")
set(ICORECOMP_PACKAGE_BOOT_ELF "SCES_507.60")

# Rendered at install time because the library file names are generator
# expressions; configure_file alone cannot resolve them.
install(CODE "
    set(ICORECOMP_PACKAGE_PLATFORM \"${ICORECOMP_PACKAGE_PLATFORM}\")
    set(ICORECOMP_PACKAGE_EXE \"${ICORECOMP_PACKAGE_EXE}\")
    set(ICORECOMP_PACKAGE_GSLIB \"${ICORECOMP_PACKAGE_GSLIB}\")
    set(ICORECOMP_PACKAGE_LIBS \"${ICORECOMP_PACKAGE_LIBS}\")
    set(ICORECOMP_PACKAGE_LIBS_NOTE \"${ICORECOMP_PACKAGE_LIBS_NOTE}\")
    set(ICORECOMP_PACKAGE_GS_LICENSE \"${ICORECOMP_PACKAGE_GS_LICENSE}\")
    set(ICORECOMP_PACKAGE_LAYOUT_NOTE \"${ICORECOMP_PACKAGE_LAYOUT_NOTE}\")
    set(ICORECOMP_PACKAGE_UI \"${ICORECOMP_PACKAGE_UI}\")
    set(ICORECOMP_PACKAGE_UI_NOTE \"${ICORECOMP_PACKAGE_UI_NOTE}\")
    set(ICORECOMP_PACKAGE_UI_LICENSE \"${ICORECOMP_PACKAGE_UI_LICENSE}\")
    set(ICORECOMP_PACKAGE_REGION \"${ICORECOMP_PACKAGE_REGION}\")
    set(ICORECOMP_PACKAGE_BOOT_ELF \"${ICORECOMP_PACKAGE_BOOT_ELF}\")
    configure_file(\"${CMAKE_SOURCE_DIR}/packaging/README.txt.in\"
                   \"\${CMAKE_INSTALL_PREFIX}/README.txt\" @ONLY)
")

# ---- macOS app bundle ------------------------------------------------------
#
# Everything from here down is Apple-only and has never been run: nobody on
# this project has a Mac. It is written from Apple's documented rules, and
# docs/MACOS.md says plainly which parts CI can prove and which it cannot.
#
# The bundle is hand-assembled rather than built with MACOSX_BUNDLE. The
# runtime resolves ui/, saves/, screenshots/, settings.json and the disc
# image against the executable's own directory (rt_base_dir), so the
# executable has to sit in the same folder as those, and MACOSX_BUNDLE's
# Resources split would put them somewhere else. Contents/MacOS holds the
# lot; Contents/Resources holds only what the runtime never resolves against
# rt_base_dir, which is the icon Finder reads, the Vulkan ICD manifest the
# loader reads, and MoltenVK's license.
if(APPLE)
    # MoltenVK is loaded at run time, not linked: Granite dlopens
    # libvulkan.1.dylib and then libMoltenVK.dylib
    # (third_party/parallel-gs/Granite/vulkan/context.cpp:220-244) and volk
    # does the same on the native renderer's Vulkan path, so this build needs
    # no Vulkan SDK at link time.
    #
    # Setting the three variables below copies one MoltenVK release into the
    # bundle and installs the launcher that makes those dlopen calls find it,
    # which together are what "self-contained" means here. docs/MACOS.md,
    # "MoltenVK in the bundle", is the whole account, including why a leaf
    # name dlopen does not see a dylib sitting beside the executable and why
    # Info.plist's LSEnvironment cannot fix that. Leaving them empty ships the
    # bundle without a driver and without a launcher: the user then supplies
    # the Vulkan SDK or a Homebrew molten-vk themselves.
    set(ICORECOMP_MOLTENVK_DYLIB "" CACHE FILEPATH
        "libMoltenVK.dylib to ship inside the bundle; empty means the user provides one")
    # The Vulkan ICD manifest from the same MoltenVK release. It is rewritten
    # rather than written from scratch: its api_version is a property of that
    # build of MoltenVK, and inventing one here would be substituting a
    # plausible value for a measured one.
    set(ICORECOMP_MOLTENVK_ICD "" CACHE FILEPATH
        "MoltenVK_icd.json from the same MoltenVK release; empty means look beside the dylib")
    # Apache-2.0 requires the license text to travel with a redistributed
    # binary, and libMoltenVK.dylib is redistributed the moment it is copied
    # into the bundle.
    set(ICORECOMP_MOLTENVK_LICENSE "" CACHE FILEPATH
        "MoltenVK's LICENSE file, installed into Contents/Resources beside the ICD")

    set(ICORECOMP_BUNDLE_IDENTIFIER "org.icorecomp.ico" CACHE STRING
        "CFBundleIdentifier of the macOS app bundle")
    # Finder reads this without an extension; the file installed below is
    # icorecomp.icns. Left empty when no .icns will be installed, so the
    # plist does not name a resource that is not in the bundle: a default or
    # CI bundle then falls back to the generic app icon, which it did
    # anyway, without the dangling reference to explain in a bug report.
    if(ICORECOMP_ICNS_FILE)
        set(ICORECOMP_BUNDLE_ICON_NAME "icorecomp")
    else()
        set(ICORECOMP_BUNDLE_ICON_NAME "")
    endif()
    set(ICORECOMP_BUNDLE_BINARY "$<TARGET_FILE_NAME:icorecomp-runtime>")
    # Overwritten below when the launcher ships. Finder execs whatever this
    # names, and the launcher execs the binary in turn.
    set(ICORECOMP_BUNDLE_EXECUTABLE "${ICORECOMP_BUNDLE_BINARY}")
    set(ICORECOMP_BUNDLE_LAUNCHER "ico-launch")

    if(ICORECOMP_MOLTENVK_DYLIB)
        install(PROGRAMS "${ICORECOMP_MOLTENVK_DYLIB}"
                DESTINATION "${ICORECOMP_INSTALL_BIN}")
        message(STATUS "icorecomp: bundling MoltenVK from ${ICORECOMP_MOLTENVK_DYLIB}")

        # The launcher. It exists only to put the bundle's own directory in
        # front of the two dlopen calls that look for a Vulkan library by
        # leaf name; packaging/macos-launch.sh.in carries the full reason,
        # and there is nothing for it to do in a build that bundles no
        # MoltenVK, which is why it is installed inside this branch.
        #
        # Two steps because the file names a generator expression:
        # configure_file substitutes @ICORECOMP_BUNDLE_BINARY@ and
        # file(GENERATE) resolves what it expanded to.
        configure_file("${CMAKE_SOURCE_DIR}/packaging/macos-launch.sh.in"
                       "${CMAKE_BINARY_DIR}/macos-launch.sh.configured" @ONLY)
        file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/${ICORECOMP_BUNDLE_LAUNCHER}"
             INPUT "${CMAKE_BINARY_DIR}/macos-launch.sh.configured")
        install(PROGRAMS "${CMAKE_BINARY_DIR}/${ICORECOMP_BUNDLE_LAUNCHER}"
                DESTINATION "${ICORECOMP_INSTALL_BIN}")
        set(ICORECOMP_BUNDLE_EXECUTABLE "${ICORECOMP_BUNDLE_LAUNCHER}")

        # The ICD manifest, with its library_path pointed at the copy in
        # Contents/MacOS. The Vulkan loader resolves a relative library_path
        # against the manifest's own directory, so this holds wherever the
        # user moves the bundle: Contents/Resources/vulkan/icd.d up three is
        # Contents, and MacOS/libMoltenVK.dylib from there.
        set(_mvk_icd "${ICORECOMP_MOLTENVK_ICD}")
        if(NOT _mvk_icd)
            get_filename_component(_mvk_dir "${ICORECOMP_MOLTENVK_DYLIB}" DIRECTORY)
            foreach(_cand "${_mvk_dir}/../share/vulkan/icd.d/MoltenVK_icd.json"
                          "${_mvk_dir}/MoltenVK_icd.json")
                if(EXISTS "${_cand}")
                    set(_mvk_icd "${_cand}")
                    break()
                endif()
            endforeach()
        endif()
        if(_mvk_icd)
            file(READ "${_mvk_icd}" _mvk_icd_text)
            string(REGEX REPLACE "\"library_path\"[^\"]*\"[^\"]*\""
                   "\"library_path\": \"../../../MacOS/libMoltenVK.dylib\""
                   _mvk_icd_text "${_mvk_icd_text}")
            if(NOT _mvk_icd_text MATCHES "\\.\\./\\.\\./\\.\\./MacOS/libMoltenVK\\.dylib")
                # Loud rather than quiet: an unrewritten manifest would point
                # at the build machine's own MoltenVK and the bundle would
                # work only there.
                message(FATAL_ERROR
                    "icorecomp: ${_mvk_icd} has no library_path this build recognises, so the "
                    "bundled ICD manifest cannot be pointed at Contents/MacOS. Fix the "
                    "rewrite in CMakeLists.txt or clear ICORECOMP_MOLTENVK_ICD.")
            endif()
            file(WRITE "${CMAKE_BINARY_DIR}/MoltenVK_icd.json" "${_mvk_icd_text}")
            install(FILES "${CMAKE_BINARY_DIR}/MoltenVK_icd.json"
                    DESTINATION "${ICORECOMP_BUNDLE_NAME}/Contents/Resources/vulkan/icd.d")
            message(STATUS "icorecomp: bundling the Vulkan ICD manifest from ${_mvk_icd}")
        else()
            message(STATUS "icorecomp: no MoltenVK_icd.json found; set "
                           "-DICORECOMP_MOLTENVK_ICD=<path>. Without it a machine that has a "
                           "Vulkan loader installed enumerates its own drivers and not the "
                           "one in this bundle; the paraLLEl-GS backend still reaches the "
                           "bundled dylib directly.")
        endif()

        if(ICORECOMP_MOLTENVK_LICENSE)
            install(FILES "${ICORECOMP_MOLTENVK_LICENSE}"
                    DESTINATION "${ICORECOMP_BUNDLE_NAME}/Contents/Resources"
                    RENAME "MoltenVK-LICENSE.txt")
        else()
            message(WARNING "icorecomp: bundling libMoltenVK.dylib without its license. "
                            "MoltenVK is Apache-2.0 and the text has to travel with the "
                            "binary; set -DICORECOMP_MOLTENVK_LICENSE=<path to LICENSE>.")
        endif()
    else()
        message(STATUS "icorecomp: no ICORECOMP_MOLTENVK_DYLIB; the bundle relies on the "
                       "user's Vulkan SDK or Homebrew molten-vk, and ships no launcher")
    endif()

    if(ICORECOMP_ICNS_FILE)
        # OPTIONAL, for the same reason the Windows icon path warns instead
        # of failing: the default value of ICORECOMP_ICNS_FILE names a file
        # only the `icon` target produces, and `icon` is not in ALL. Without
        # this, the three commands docs/MACOS.md gives abort inside
        # `cmake --install` before the codesign block runs, leaving a
        # half-populated unsigned bundle. Missing an icon is not worth that.
        install(FILES "${ICORECOMP_ICNS_FILE}"
                DESTINATION "${ICORECOMP_BUNDLE_NAME}/Contents/Resources"
                RENAME "${ICORECOMP_BUNDLE_ICON_NAME}.icns"
                OPTIONAL)
        if(NOT EXISTS "${ICORECOMP_ICNS_FILE}")
            message(STATUS "icorecomp: ICORECOMP_ICNS_FILE ${ICORECOMP_ICNS_FILE} does not "
                           "exist yet; the install skips it unless it is there by then "
                           "(build --target icon with -DICORECOMP_DISC=<image> to render it)")
        endif()
    else()
        message(STATUS "icorecomp: no ICORECOMP_ICNS_FILE; the bundle ships without an icon "
                       "(build --target icon with -DICORECOMP_DISC=<image> to render one)")
    endif()

    # Written at install time for the same reason the README is: the
    # executable name is a generator expression.
    install(CODE "
        set(ICORECOMP_BUNDLE_IDENTIFIER \"${ICORECOMP_BUNDLE_IDENTIFIER}\")
        set(ICORECOMP_BUNDLE_EXECUTABLE \"${ICORECOMP_BUNDLE_EXECUTABLE}\")
        set(ICORECOMP_BUNDLE_ICON_NAME \"${ICORECOMP_BUNDLE_ICON_NAME}\")
        configure_file(\"${CMAKE_SOURCE_DIR}/packaging/Info.plist.in\"
                       \"\${CMAKE_INSTALL_PREFIX}/${ICORECOMP_BUNDLE_NAME}/Contents/Info.plist\"
                       @ONLY)
    ")

    # Ad-hoc signature. The arm64 kernel refuses to run an unsigned Mach-O
    # at all, so this is not a distribution nicety: without it the bundle
    # does not start. Each dylib is signed before the bundle, because
    # signing the bundle seals what its contents are and a later change to
    # one of them invalidates the seal. Ordered last in this file so every
    # install() above has already put its file in place. A codesign failure
    # is not fatal here: it is reported, and the bundle is still on disk for
    # the user to sign by hand, which is better than a half-installed tree.
    #
    # The runtime binary is signed explicitly and not left to the bundle
    # signature. When the launcher ships, CFBundleExecutable is a shell
    # script, so the binary is a nested Mach-O that the bundle pass would
    # seal as a resource without signing, and the kernel would refuse to
    # exec it. Signing it twice when it is the main executable is harmless.
    #
    # No --options runtime anywhere here. The hardened runtime makes dyld
    # ignore every DYLD_ variable, which is exactly what the launcher sets.
    install(CODE "
        find_program(ICORECOMP_CODESIGN codesign)
        if(NOT ICORECOMP_CODESIGN)
            message(WARNING \"icorecomp: codesign not found; the bundle is unsigned and will \"
                            \"not launch on Apple Silicon until it is signed by hand\")
        else()
            set(_bundle \"\${CMAKE_INSTALL_PREFIX}/${ICORECOMP_BUNDLE_NAME}\")
            file(GLOB _dylibs \"\${_bundle}/Contents/MacOS/*.dylib\")
            list(APPEND _dylibs \"\${_bundle}/Contents/MacOS/${ICORECOMP_BUNDLE_BINARY}\")
            foreach(_lib \${_dylibs})
                execute_process(COMMAND \"\${ICORECOMP_CODESIGN}\" --force --sign - \"\${_lib}\"
                                RESULT_VARIABLE _rc)
                if(NOT _rc EQUAL 0)
                    message(WARNING \"icorecomp: codesign failed on \${_lib} (\${_rc})\")
                endif()
            endforeach()
            execute_process(COMMAND \"\${ICORECOMP_CODESIGN}\" --force --sign - \"\${_bundle}\"
                            RESULT_VARIABLE _rc)
            if(NOT _rc EQUAL 0)
                message(WARNING \"icorecomp: codesign failed on \${_bundle} (\${_rc})\")
            else()
                message(STATUS \"icorecomp: ad-hoc signed \${_bundle}\")
                # Says whether the seal actually covers what is in the
                # bundle. A bundle whose main executable is a script is
                # signed into Contents/_CodeSignature rather than embedded,
                # and this is the only statement anyone gets about that
                # without a Mac to launch it on.
                execute_process(COMMAND \"\${ICORECOMP_CODESIGN}\" --verify --deep --strict
                                        --verbose=2 \"\${_bundle}\"
                                RESULT_VARIABLE _rc)
                if(NOT _rc EQUAL 0)
                    message(WARNING \"icorecomp: codesign --verify rejected \${_bundle} \"
                                    \"(\${_rc}); it will not launch as it stands\")
                endif()
            endif()
        endif()
    ")
endif()
