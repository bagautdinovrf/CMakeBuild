include_guard(GLOBAL)

# The final May 2026 fix for Windows mixed-DPI monitor transitions (#1439).
# Pin this development snapshot rather than tracking the moving master branch.
# https://github.com/fltk/fltk/commit/b8ccd16eb8cf084abca213ef6be5a407f33cc090
set(CMAKEBUILD_FLTK_VERSION "1.5.0")
set(CMAKEBUILD_FLTK_REVISION "b8ccd16eb8cf084abca213ef6be5a407f33cc090")
set(CMAKEBUILD_FLTK_ARCHIVE_SHA256 "e4660e4b58e835eed27ab0a2af48278d53af35c191cd492642111030c7502297")
set(CMAKEBUILD_FLTK_SOURCE_DIR "" CACHE PATH
    "Local pinned FLTK 1.5.0 development snapshot; bypasses the network download")

function(cmakebuild_add_fltk)
    # These names are checked against the pinned snapshot's CMake/options.cmake.
    foreach(option IN ITEMS
        FLTK_BUILD_SHARED_LIBS FLTK_BUILD_GL FLTK_BUILD_FORMS
        FLTK_BUILD_FLUID FLTK_BUILD_FLTK_OPTIONS FLTK_BUILD_TEST
        FLTK_BUILD_EXAMPLES FLTK_BUILD_HTML_DOCS FLTK_BUILD_PDF_DOCS
        FLTK_BUILD_FLUID_DOCS FLTK_OPTION_CAIRO_WINDOW
        FLTK_OPTION_CAIRO_EXT FLTK_OPTION_SVG FLTK_OPTION_PRINT_SUPPORT
        FLTK_OPTION_PEN_SUPPORT FLTK_BUILD_SCREENSHOTS)
        set(${option} OFF CACHE BOOL "Unused by CMakeBuild" FORCE)
    endforeach()
    if(MSVC)
        # FLTK sets its own runtime variable, so both settings are necessary.
        set(FLTK_MSVC_RUNTIME_DLL OFF CACHE BOOL "Use static MSVC runtime" FORCE)
    endif()
    # The image libraries are never linked, but should remain reproducible if
    # explicitly requested. No system image-codec DLLs are used.
    foreach(option IN ITEMS FLTK_USE_SYSTEM_LIBJPEG FLTK_USE_SYSTEM_LIBPNG FLTK_USE_SYSTEM_ZLIB)
        set(${option} OFF CACHE BOOL "Use bundled image libraries" FORCE)
    endforeach()

    if(CMAKEBUILD_FLTK_SOURCE_DIR)
        get_filename_component(fltk_source "${CMAKEBUILD_FLTK_SOURCE_DIR}" ABSOLUTE)
        set(fltk_binary "${CMAKE_CURRENT_BINARY_DIR}/_deps/fltk-build")
    else()
        include(FetchContent)
        FetchContent_Declare(fltk
            URL "https://codeload.github.com/fltk/fltk/tar.gz/${CMAKEBUILD_FLTK_REVISION}"
            URL_HASH "SHA256=${CMAKEBUILD_FLTK_ARCHIVE_SHA256}"
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            TLS_VERIFY TRUE
            # Populate only, then add EXCLUDE_FROM_ALL below. This works with
            # CMake 3.24, before MakeAvailable gained EXCLUDE_FROM_ALL in 3.28.
            SOURCE_SUBDIR _cmakebuild_populate_only
        )
        FetchContent_MakeAvailable(fltk)
        set(fltk_source "${fltk_SOURCE_DIR}")
        set(fltk_binary "${fltk_BINARY_DIR}")
    endif()

    if(NOT EXISTS "${fltk_source}/CMakeLists.txt" OR NOT EXISTS "${fltk_source}/FL/Enumerations.H")
        message(FATAL_ERROR "CMAKEBUILD_FLTK_SOURCE_DIR must name an unpacked FLTK ${CMAKEBUILD_FLTK_VERSION} source directory.")
    endif()
    # FLTK 1.5 generates its version macros; the source CMake project is the
    # version authority. Also check the Win32 driver for local source overrides
    # so a different 1.5 snapshot cannot silently omit the required DPI fixes.
    file(READ "${fltk_source}/CMakeLists.txt" fltk_project)
    if(NOT fltk_project MATCHES "project\\(FLTK[ \t\r\n]+VERSION[ \t]+${CMAKEBUILD_FLTK_VERSION}\\)")
        message(FATAL_ERROR "CMakeBuild requires FLTK ${CMAKEBUILD_FLTK_VERSION}-dev at ${CMAKEBUILD_FLTK_REVISION}.")
    endif()
    if(NOT EXISTS "${fltk_source}/src/Fl_win32.cxx")
        message(FATAL_ERROR "The pinned FLTK Win32 driver is missing.")
    endif()
    file(READ "${fltk_source}/src/Fl_win32.cxx" fltk_win32)
    string(REPLACE "\r\n" "\n" fltk_win32 "${fltk_win32}")
    string(SHA256 fltk_win32_sha256 "${fltk_win32}")
    if(NOT fltk_win32_sha256 STREQUAL "2881d7b9a6e956cc121d05a7b85fa4b4f02a3832f254a2ced1a266b13e8a272a")
        message(FATAL_ERROR "FLTK sources do not contain the pinned Win32 DPI driver at ${CMAKEBUILD_FLTK_REVISION}. Use that snapshot, or clear CMAKEBUILD_FLTK_SOURCE_DIR to download it.")
    endif()

    # Upstream also defines images/codecs; exclude its entire subdirectory from
    # ALL so only fltk::fltk and its actual dependencies are built.
    add_subdirectory("${fltk_source}" "${fltk_binary}" EXCLUDE_FROM_ALL)
    if(NOT TARGET fltk::fltk)
        message(FATAL_ERROR "The FLTK ${CMAKEBUILD_FLTK_VERSION} static target is missing.")
    endif()
    # Keep the application's strict warnings while treating upstream headers
    # as external headers for consumers (CMake 3.24-compatible).
    get_target_property(fltk_public_includes fltk INTERFACE_INCLUDE_DIRECTORIES)
    set_property(TARGET fltk PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
        "${fltk_public_includes}")
    if(MSVC)
        set_property(TARGET fltk PROPERTY MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    set(CMAKEBUILD_FLTK_RESOLVED_SOURCE_DIR "${fltk_source}" CACHE INTERNAL
        "Source directory used for the pinned FLTK dependency" FORCE)
endfunction()

cmakebuild_add_fltk()
