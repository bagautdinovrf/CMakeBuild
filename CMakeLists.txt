cmake_minimum_required(VERSION 3.24)

# VERSION is the single source for the UI, PE metadata and Windows manifest.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" CMAKEBUILD_VERSION)
string(STRIP "${CMAKEBUILD_VERSION}" CMAKEBUILD_VERSION)
if(NOT CMAKEBUILD_VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
    message(FATAL_ERROR "VERSION must contain a numeric MAJOR.MINOR.PATCH version.")
endif()
foreach(component IN ITEMS "${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}" "${CMAKE_MATCH_3}")
    if(component GREATER 65535)
        message(FATAL_ERROR "Each VERSION component must be at most 65535 for Windows version resources.")
    endif()
endforeach()

project(CMakeBuild VERSION "${CMAKEBUILD_VERSION}" LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "CMakeBuild is a native Windows application.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(cmake/fltk.cmake)

configure_file(src/version.hpp.in generated/version.hpp @ONLY)
configure_file(app.manifest.in app.manifest @ONLY)
file(TO_CMAKE_PATH "${CMAKE_CURRENT_BINARY_DIR}/app.manifest" APP_MANIFEST_PATH)
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.ico" APP_ICON_PATH)
configure_file(app.rc.in app.rc @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/app.rc" PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/app.manifest;${CMAKE_CURRENT_SOURCE_DIR}/assets/app.ico"
)

add_executable(CMakeBuild WIN32
    src/main.cpp
    src/ui.cpp
    src/settings_dialog.cpp
    src/app_state.cpp
    src/platform.cpp
    src/engine.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/app.rc"
)

target_include_directories(CMakeBuild PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(CMakeBuild PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 WINVER=0x0A00
    "$<$<CONFIG:Debug>:_DEBUG>"
)
target_link_libraries(CMakeBuild PRIVATE
    fltk::fltk comdlg32 shell32 ole32 uuid user32 gdi32 dwmapi advapi32
)

# Keep configuration/toolchain outputs separate so switching builds cannot
# mistake another configuration's newer executable for an up-to-date result.
set_target_properties(CMakeBuild PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin/$<CONFIG>"
)

# The user-facing executable always has one location. POST_BUILD also covers
# explicit executable-target builds; ALL republishes an unchanged local build.
set(CMAKEBUILD_EXECUTABLE "${CMAKE_CURRENT_SOURCE_DIR}/CMakeBuild.exe")
add_custom_command(TARGET CMakeBuild POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:CMakeBuild>" "${CMAKEBUILD_EXECUTABLE}"
    COMMENT "Updating ${CMAKEBUILD_EXECUTABLE}"
    VERBATIM
)
add_custom_target(CMakeBuildPublish ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:CMakeBuild>" "${CMAKEBUILD_EXECUTABLE}"
    DEPENDS CMakeBuild
    COMMENT "Publishing ${CMAKEBUILD_EXECUTABLE}"
    VERBATIM
)

if(MSVC)
    target_compile_options(CMakeBuild PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8;/Zc:__cplusplus>"
        "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Release>>:/O2>"
    )
    # The manifest is already in app.rc; prevent a second manifest resource.
    target_link_options(CMakeBuild PRIVATE /MANIFEST:NO)
elseif(MINGW)
    target_compile_options(CMakeBuild PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>"
    )
    target_link_options(CMakeBuild PRIVATE
        # Our wWinMain keeps both Release and Debug GUI builds console-free;
        # the unused FLTK WinMain bridge would allocate a console in Debug.
        -municode -static -static-libgcc -static-libstdc++
        "$<$<CONFIG:Release>:-s>"
    )
endif()

install(TARGETS CMakeBuild RUNTIME DESTINATION .)
