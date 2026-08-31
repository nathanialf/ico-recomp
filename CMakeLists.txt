# ico-recomp runtime build.
#
# Owns src/runtime/ only. The translator (tools/recomp, Rust) and the
# generated C in generated/ee/ (gitignored, produced locally at build time)
# are separate concerns; this file links against generated/ee when present
# and otherwise builds icorecomp-runtime in a stub mode (see src/runtime/main.cpp).
#
# third_party/parallel-gs (GS backend) is intentionally not wired up yet
# (P3 milestone).

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
)

add_executable(icorecomp-runtime ${ICORECOMP_RUNTIME_SOURCES})

target_include_directories(icorecomp-runtime PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/runtime
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
