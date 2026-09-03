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

set(ICORECOMP_RUNTIME_SOURCES
    src/runtime/mem.cpp
    src/runtime/mmio.cpp
    src/runtime/hooks.cpp
    src/runtime/loader.cpp
    src/runtime/log.cpp
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
    src/runtime/host/input.cpp
    src/runtime/host/mouse.cpp
    src/runtime/host/json.cpp
    src/runtime/host/settings.cpp
    src/runtime/host/settings_apply.cpp
    src/runtime/host/window.cpp
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

# Ships as ico / ico.exe. The target keeps its name so CI (--target
# icorecomp-runtime) and every selftest reference stay valid.
set_target_properties(icorecomp-runtime PROPERTIES OUTPUT_NAME ico)

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
# ICORECOMP_PGS_SDL and without ICORECOMP_HAVE_PARALLEL_GS, so no SDL, no
# window and no GS library: ICORECOMP_MENU_NAV_TEST compiles the cursor
# injection hook in their place.
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
add_executable(icorecomp-ipu-selftest
    src/runtime/hw/ipu.cpp
    src/runtime/hw/ipu_selftest.cpp
    src/runtime/iso/iso9660.cpp
    src/runtime/loader.cpp
    src/runtime/log.cpp
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

if(ICORECOMP_DISC AND NOT CMAKE_CROSSCOMPILING)
    set(ICORECOMP_ICON_DIR "${CMAKE_BINARY_DIR}/icon")
    add_custom_command(
        OUTPUT "${ICORECOMP_ICON_DIR}/ico.ico"
        COMMAND "$<TARGET_FILE:icorecomp-icon-extract>"
                "${ICORECOMP_DISC}" "${ICORECOMP_ICON_DIR}/ico.ico"
                --png "${ICORECOMP_ICON_DIR}"
        DEPENDS icorecomp-icon-extract "${ICORECOMP_DISC}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "icorecomp: rendering the save icon from ${ICORECOMP_DISC}"
        VERBATIM
    )
    add_custom_target(icon DEPENDS "${ICORECOMP_ICON_DIR}/ico.ico")
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
            ${GENERATED_DIR}/..
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
        target_include_directories(icorecomp-runtime PRIVATE ${GENERATED_DIR} ${GENERATED_DIR}/..)
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
option(ICORECOMP_GS_WINDOW "Build SDL3 window presentation for the live GS backend" ON)

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

    # Optional SDL3 window presentation. Standalone parallel-gs does not
    # build Granite's vendored SDL3, so configure it here the same way
    # Granite's third_party/CMakeLists.txt does for its SDL platform.
    # SDL is a SHARED library on purpose: the window/WSI lives inside
    # libicorecomp-parallel-gs (gs_parallel_present.cpp) while keyboard/gamepad
    # polling and audio live in the runtime executable (host/input.cpp,
    # host/audio.cpp), and both must talk to the SAME SDL instance. Two
    # static copies would give the exe a keyboard-state view of an SDL that
    # owns no window.
    set(ICORECOMP_PGS_HAVE_SDL OFF)
    if(ICORECOMP_GS_WINDOW AND EXISTS "${ICORECOMP_PGS_DIR}/Granite/third_party/sdl3/CMakeLists.txt")
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
        # SDL; window creation then fails at runtime and the backend falls
        # back to headless rendering. Video drivers are still auto-detected
        # and built where the headers exist.
        set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
        add_subdirectory("${ICORECOMP_PGS_DIR}/Granite/third_party/sdl3"
                         "${CMAKE_BINARY_DIR}/icorecomp-sdl3" EXCLUDE_FROM_ALL SYSTEM)
        set(ICORECOMP_PGS_HAVE_SDL ON)
    endif()

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
        target_link_options(icorecomp-parallel-gs PRIVATE -Wl,--no-undefined)
    endif()
    if(ICORECOMP_PGS_HAVE_SDL)
        target_compile_definitions(icorecomp-parallel-gs PRIVATE ICORECOMP_PGS_SDL)
        target_link_libraries(icorecomp-parallel-gs PRIVATE SDL3::SDL3)
    endif()

    # Runtime: the thin GsBackend adapter over the C ABI (ours).
    target_sources(icorecomp-runtime PRIVATE src/runtime/gs/gs_parallel.cpp)
    target_compile_definitions(icorecomp-runtime PRIVATE ICORECOMP_HAVE_PARALLEL_GS)
    target_link_libraries(icorecomp-runtime PRIVATE icorecomp-parallel-gs)
    if(ICORECOMP_PGS_HAVE_SDL)
        target_compile_definitions(icorecomp-runtime PRIVATE ICORECOMP_PGS_SDL ICORECOMP_SND_SDL)
        target_link_libraries(icorecomp-runtime PRIVATE SDL3::SDL3)
    endif()

    # Headless raw-dump replay tool (ours): the CI-able consumer-side check
    # of the dump writer format. Only the C ABI; no Granite headers.
    add_executable(icorecomp-gs-replay src/runtime/gs/gs_replay_main.cpp)
    target_include_directories(icorecomp-gs-replay PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
    target_link_libraries(icorecomp-gs-replay PRIVATE icorecomp-parallel-gs)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(icorecomp-gs-replay PRIVATE -Wall -Wextra -fno-strict-aliasing)
    endif()

    message(STATUS "icorecomp: paraLLEl-GS backend enabled (SDL3 window: ${ICORECOMP_PGS_HAVE_SDL})")
else()
    message(STATUS "icorecomp: paraLLEl-GS backend disabled; ICORECOMP_GS=parallel will fail loudly")
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

if(WIN32)
    set(ICORECOMP_PACKAGE_PLATFORM "for Windows (x86_64)")
else()
    set(ICORECOMP_PACKAGE_PLATFORM "for Linux (x86_64)")
endif()
set(ICORECOMP_PACKAGE_EXE "$<TARGET_FILE_NAME:icorecomp-runtime>")

if(NOT WIN32)
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

install(TARGETS icorecomp-runtime RUNTIME DESTINATION .)

if(ICORECOMP_UI)
    # The UI's documents, stylesheets and fonts, found at run time under
    # rt_base_dir()/ui (src/runtime/ui/ui.cpp). Each font's OFL notice ships
    # beside it (PlayfairDisplay-OFL.txt, JetBrainsMono-OFL.txt), which is
    # what the license requires.
    install(DIRECTORY ui/ DESTINATION ui)
endif()

if(ICORECOMP_PARALLEL_GS)
    # RUNTIME catches the Windows DLL, LIBRARY the Linux .so.
    install(TARGETS icorecomp-parallel-gs RUNTIME DESTINATION . LIBRARY DESTINATION .)
    set(ICORECOMP_PACKAGE_GSLIB "$<TARGET_FILE_NAME:icorecomp-parallel-gs>")
    set(ICORECOMP_PACKAGE_LIBS "    ${ICORECOMP_PACKAGE_GSLIB}")
    if(ICORECOMP_PGS_HAVE_SDL)
        # SDL3 comes from an EXCLUDE_FROM_ALL subdirectory, so install its
        # file rather than its target. PROGRAMS, not FILES: FILES installs
        # 644, and a DLL without the execute bit fails to load with
        # STATUS_ACCESS_DENIED (0xC0000022) whenever the mode bits survive
        # the trip to the target machine, which they do over a share or a
        # permission-preserving unzip. On ELF platforms the soname symlink
        # has to come along too, since that is the name the exe links.
        install(PROGRAMS $<TARGET_FILE:SDL3-shared> DESTINATION .)
        if(NOT WIN32)
            install(FILES $<TARGET_SONAME_FILE:SDL3-shared> DESTINATION .)
        endif()
        string(APPEND ICORECOMP_PACKAGE_LIBS "\n    $<TARGET_FILE_NAME:SDL3-shared>")
    endif()
else()
    set(ICORECOMP_PACKAGE_GSLIB "no renderer (this build has none)")
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

# Rendered at install time because the library file names are generator
# expressions; configure_file alone cannot resolve them.
install(CODE "
    set(ICORECOMP_PACKAGE_PLATFORM \"${ICORECOMP_PACKAGE_PLATFORM}\")
    set(ICORECOMP_PACKAGE_EXE \"${ICORECOMP_PACKAGE_EXE}\")
    set(ICORECOMP_PACKAGE_GSLIB \"${ICORECOMP_PACKAGE_GSLIB}\")
    set(ICORECOMP_PACKAGE_LIBS \"${ICORECOMP_PACKAGE_LIBS}\")
    set(ICORECOMP_PACKAGE_UI \"${ICORECOMP_PACKAGE_UI}\")
    set(ICORECOMP_PACKAGE_UI_NOTE \"${ICORECOMP_PACKAGE_UI_NOTE}\")
    set(ICORECOMP_PACKAGE_UI_LICENSE \"${ICORECOMP_PACKAGE_UI_LICENSE}\")
    configure_file(\"${CMAKE_SOURCE_DIR}/packaging/README.txt.in\"
                   \"\${CMAKE_INSTALL_PREFIX}/README.txt\" @ONLY)
")
