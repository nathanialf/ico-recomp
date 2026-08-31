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
    src/runtime/iso/iso9660.cpp
    src/runtime/hw/dmac.cpp
    src/runtime/hw/vif1.cpp
    src/runtime/hw/gif.cpp
    src/runtime/hw/vu1rt.cpp
    src/runtime/hw/gspriv.cpp
    src/runtime/gs/gs_dumpwriter.cpp
    src/runtime/gs/gs_select.cpp
)

add_executable(icorecomp-runtime ${ICORECOMP_RUNTIME_SOURCES})

target_include_directories(icorecomp-runtime PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/runtime
    ${CMAKE_SOURCE_DIR}/third_party/minicoro
)

target_compile_definitions(icorecomp-runtime PRIVATE
    ICORECOMP_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(icorecomp-runtime PRIVATE -Wall -Wextra -fno-strict-aliasing)
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
#   icorecomp-runtime            gains gs/gs_parallel.cpp (ours, MIT), the
#                                GsBackend implementation on GSInterface,
#                                selected at runtime by ICORECOMP_GS.
#   icorecomp-gs-replay          small headless tool (ours) that feeds a raw
#                                dump through paraLLEl-GS's own GSDumpParser
#                                and GSInterface; validates the dump writer
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
    add_subdirectory("${ICORECOMP_PGS_DIR}" EXCLUDE_FROM_ALL SYSTEM)

    # The shared library. Whole-archive the LGPL libs (parallel-gs,
    # parallel-gs-dump) and granite-vulkan (the API surface our backend
    # calls: Context/Device/WSI). Remaining granite static deps resolve
    # symbol-by-symbol into the .so via the normal transitive link.
    add_library(icorecomp-parallel-gs SHARED src/runtime/gs/gs_parallel_lib.cpp)
    target_link_libraries(icorecomp-parallel-gs PRIVATE
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,parallel-gs>"
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,parallel-gs-dump>"
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,granite-vulkan>"
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(icorecomp-parallel-gs PRIVATE -Wl,--no-undefined)
    endif()

    # Header closure for consumers of the .so, marked SYSTEM so our -Wall
    # -Wextra gate stays about our code. PUBLIC-link closure of
    # parallel-gs/parallel-gs-dump per the Granite CMake files:
    #   parallel-gs      -> granite-vulkan granite-math
    #   granite-vulkan   -> granite-util granite-volk-headers
    #   granite-util     -> granite-application-global-interface
    #   granite-volk-headers -> Vulkan::Headers
    set(ICORECOMP_PGS_HEADER_TARGETS
        parallel-gs parallel-gs-dump granite-vulkan granite-math
        granite-util granite-volk-headers granite-application-global-interface
        Vulkan::Headers)
    foreach(tgt IN LISTS ICORECOMP_PGS_HEADER_TARGETS)
        target_include_directories(icorecomp-parallel-gs SYSTEM INTERFACE
            "$<TARGET_PROPERTY:${tgt},INTERFACE_INCLUDE_DIRECTORIES>")
        target_compile_definitions(icorecomp-parallel-gs INTERFACE
            "$<TARGET_PROPERTY:${tgt},INTERFACE_COMPILE_DEFINITIONS>")
    endforeach()

    # Optional SDL3 window presentation. Standalone parallel-gs does not
    # build Granite's vendored SDL3, so configure it here the same way
    # Granite's third_party/CMakeLists.txt does for its SDL platform.
    set(ICORECOMP_PGS_HAVE_SDL OFF)
    if(ICORECOMP_GS_WINDOW AND EXISTS "${ICORECOMP_PGS_DIR}/Granite/third_party/sdl3/CMakeLists.txt")
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TIMERS ON CACHE BOOL "" FORCE)
        set(SDL_LIBC ON CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
        set(SDL_ATOMIC OFF CACHE BOOL "" FORCE)
        set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
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

    # Runtime: the live backend implementation (ours).
    target_sources(icorecomp-runtime PRIVATE src/runtime/gs/gs_parallel.cpp)
    target_compile_definitions(icorecomp-runtime PRIVATE ICORECOMP_HAVE_PARALLEL_GS)
    target_link_libraries(icorecomp-runtime PRIVATE icorecomp-parallel-gs)
    if(ICORECOMP_PGS_HAVE_SDL)
        target_compile_definitions(icorecomp-runtime PRIVATE ICORECOMP_PGS_SDL)
        target_link_libraries(icorecomp-runtime PRIVATE SDL3-static)
    endif()

    # Headless raw-dump replay tool (ours): the CI-able consumer-side check
    # of the dump writer format.
    add_executable(icorecomp-gs-replay src/runtime/gs/gs_replay_main.cpp)
    target_link_libraries(icorecomp-gs-replay PRIVATE icorecomp-parallel-gs)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(icorecomp-gs-replay PRIVATE -Wall -Wextra -fno-strict-aliasing)
    endif()

    message(STATUS "icorecomp: paraLLEl-GS backend enabled (SDL3 window: ${ICORECOMP_PGS_HAVE_SDL})")
else()
    message(STATUS "icorecomp: paraLLEl-GS backend disabled; ICORECOMP_GS=parallel will fail loudly")
endif()
