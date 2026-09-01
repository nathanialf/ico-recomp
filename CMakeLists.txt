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
    src/runtime/sif/sif.cpp
    src/runtime/sif/rpc.cpp
    src/runtime/sif/cdvd.cpp
    src/runtime/sif/sndn2.cpp
    src/runtime/sif/pad.cpp
    src/runtime/sif/mc.cpp
    src/runtime/host/input.cpp
    src/runtime/host/json.cpp
    src/runtime/host/settings.cpp
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
)

add_executable(icorecomp-runtime ${ICORECOMP_RUNTIME_SOURCES})

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
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(generated PRIVATE -fno-strict-aliasing)
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
#                                (gs/gs_parallel_lib.cpp).
#   icorecomp-gs-replay          small headless tool (ours) that feeds a raw
#                                dump through paraLLEl-GS's own GSDumpParser
#                                via the same C ABI; validates the dump writer
#                                against the real consumer without a display.
#
# Requires CMake >= 3.25 for add_subdirectory(SYSTEM) and
# $<LINK_LIBRARY:WHOLE_ARCHIVE>; older CMake just disables the live backend.
option(ICORECOMP_PARALLEL_GS "Build the paraLLEl-GS live GS backend" ON)
option(ICORECOMP_GS_WINDOW "Build SDL3 window presentation for the live GS backend" ON)

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
    add_subdirectory("${ICORECOMP_PGS_DIR}" EXCLUDE_FROM_ALL SYSTEM)

    # Optional SDL3 window presentation. Standalone parallel-gs does not
    # build Granite's vendored SDL3, so configure it here the same way
    # Granite's third_party/CMakeLists.txt does for its SDL platform.
    # SDL is a SHARED library on purpose: the window/WSI lives inside
    # libicorecomp-parallel-gs (gs_parallel_lib.cpp) while keyboard/gamepad
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
        set(SDL_FILESYSTEM OFF CACHE BOOL "" FORCE)
        set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
        set(SDL_LOCALE OFF CACHE BOOL "" FORCE)
        set(SDL_MISC OFF CACHE BOOL "" FORCE)
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
    # exports also make MSVC emit the import library). gs_parallel_lib.cpp
    # is ours (MIT) but compiles into the library, so it alone may use the
    # Granite/paraLLEl-GS C++ interfaces.
    add_library(icorecomp-parallel-gs SHARED src/runtime/gs/gs_parallel_lib.cpp)
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
    set(ICORECOMP_PACKAGE_EXE "icorecomp-runtime.exe")
else()
    set(ICORECOMP_PACKAGE_PLATFORM "for Linux (x86_64)")
    set(ICORECOMP_PACKAGE_EXE "icorecomp-runtime")
endif()

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

# Rendered at install time because the library file names are generator
# expressions; configure_file alone cannot resolve them.
install(CODE "
    set(ICORECOMP_PACKAGE_PLATFORM \"${ICORECOMP_PACKAGE_PLATFORM}\")
    set(ICORECOMP_PACKAGE_EXE \"${ICORECOMP_PACKAGE_EXE}\")
    set(ICORECOMP_PACKAGE_GSLIB \"${ICORECOMP_PACKAGE_GSLIB}\")
    set(ICORECOMP_PACKAGE_LIBS \"${ICORECOMP_PACKAGE_LIBS}\")
    configure_file(\"${CMAKE_SOURCE_DIR}/packaging/README.txt.in\"
                   \"\${CMAKE_INSTALL_PREFIX}/README.txt\" @ONLY)
")
