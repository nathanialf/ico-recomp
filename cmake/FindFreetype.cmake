# FindFreetype.cmake: satisfies RmlUi's find_package(Freetype) with the
# FreeType we build ourselves from third_party/freetype.
#
# Why this file exists: RmlUi's CMake/Dependencies.cmake calls
# find_package("Freetype") and then requires the imported target
# Freetype::Freetype to exist (report_dependency_found_or_error). The stock
# module only knows how to find an installed system FreeType, so on a machine
# without freetype development packages the RmlUi configure step fails even
# though our own static FreeType target is already defined. This module is
# placed on CMAKE_MODULE_PATH before add_subdirectory(third_party/rmlui) and
# short-circuits to the target we built.
#
# When the target is not there (any other project in the tree calling
# find_package(Freetype)) this falls through to the stock module unchanged,
# so nothing else in the build sees different behavior.

if(TARGET Freetype::Freetype)
    set(Freetype_FOUND TRUE)
    set(FREETYPE_FOUND TRUE)
    # RmlUi only reads this to warn about the known-bad 2.11.0 + MSVC pair.
    # Kept in sync by hand with the third_party/freetype submodule tag
    # (VER-2-13-3); it is a version string, not a discovered fact.
    set(FREETYPE_VERSION_STRING "2.13.3")
    return()
endif()

include(${CMAKE_ROOT}/Modules/FindFreetype.cmake)
