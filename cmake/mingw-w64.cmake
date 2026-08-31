# Cross toolchain: Linux host -> Windows x86_64 via mingw-w64.
#
# Used by the windows-mingw-cross preset. Prefers the -posix flavored
# drivers (winpthreads-backed std::thread/std::mutex in libstdc++); plain
# names are the fallback for distributions that ship only one flavor.
# ICORECOMP_MINGW_ROOT can point at an extracted or self-built toolchain
# whose bin/ holds the cross drivers.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(_prefix x86_64-w64-mingw32)
set(_hints "")
if(DEFINED ENV{ICORECOMP_MINGW_ROOT})
    list(APPEND _hints "$ENV{ICORECOMP_MINGW_ROOT}/bin")
endif()

find_program(CMAKE_C_COMPILER
    NAMES ${_prefix}-gcc-posix ${_prefix}-gcc
    HINTS ${_hints}
    REQUIRED)
find_program(CMAKE_CXX_COMPILER
    NAMES ${_prefix}-g++-posix ${_prefix}-g++
    HINTS ${_hints}
    REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES ${_prefix}-windres HINTS ${_hints})

# Cross-compiled binaries do not run on the build host.
set(CMAKE_CROSSCOMPILING_EMULATOR "")

set(CMAKE_FIND_ROOT_PATH /usr/${_prefix})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Static libgcc/libstdc++/winpthread so the .exe and our .dlls run without
# carrying mingw runtime DLLs next to them. (msvcrt.dll stays a dynamic,
# OS-provided import either way, which also keeps one shared environment
# block across modules.)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-static")
