cmake_minimum_required(VERSION 3.24)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" CMAKEBUILD_TEST_VERSION)
string(STRIP "${CMAKEBUILD_TEST_VERSION}" CMAKEBUILD_TEST_VERSION)
project(CMakeBuildEngineTests VERSION "${CMAKEBUILD_TEST_VERSION}" LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The Engine integration tests require Windows.")
endif()
enable_language(RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(../cmake/fltk.cmake)

add_executable(engine_qt_discovery qt_discovery.cpp ../src/engine.cpp)
add_executable(engine_build_tests build_tests.cpp ../src/engine.cpp)
add_executable(engine_rebuild_tests rebuild_tests.cpp ../src/engine.cpp)
configure_file(../src/version.hpp.in generated/version.hpp @ONLY)
configure_file(../app.manifest.in generated/ui.manifest @ONLY)
file(TO_CMAKE_PATH "${CMAKE_CURRENT_BINARY_DIR}/generated/ui.manifest" CMAKEBUILD_UI_MANIFEST_PATH)
configure_file(ui_manifest.rc.in generated/ui_manifest.rc @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/generated/ui_manifest.rc" PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/generated/ui.manifest"
)
add_executable(journal_ui journal_ui.cpp ../src/engine.cpp)
add_executable(run_ui run_ui.cpp ../src/engine.cpp)
add_executable(native_dpi_ui native_dpi_ui.cpp ../src/engine.cpp)
add_executable(FltkBench fltk_bench.cpp ../src/engine.cpp)
target_include_directories(FltkBench PRIVATE ../benchmarks)
target_link_libraries(FltkBench PRIVATE psapi)
target_compile_definitions(FltkBench PRIVATE
    "CMAKEBUILD_BENCH_APP_VERSION=\"${PROJECT_VERSION}\""
    "CMAKEBUILD_BENCH_TOOLKIT_REVISION=\"${CMAKEBUILD_FLTK_REVISION}\"")
foreach(ui_harness IN ITEMS journal_ui run_ui native_dpi_ui FltkBench)
    # Exercise the real FLTK panel with the same manifest and platform helpers.
    target_sources(${ui_harness} PRIVATE
        ../src/ui.cpp ../src/settings_dialog.cpp ../src/app_state.cpp ../src/platform.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/generated/ui_manifest.rc")
    target_include_directories(${ui_harness} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(${ui_harness} PRIVATE fltk::fltk comdlg32 shell32 ole32 uuid user32 gdi32 dwmapi)
    if(MSVC)
        target_link_options(${ui_harness} PRIVATE /MANIFEST:NO)
    endif()
endforeach()
foreach(harness IN ITEMS engine_qt_discovery engine_build_tests engine_rebuild_tests journal_ui run_ui native_dpi_ui FltkBench)
    target_include_directories(${harness} PRIVATE ../src)
    target_compile_definitions(${harness} PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        _WIN32_WINNT=0x0A00 WINVER=0x0A00
    )
    target_link_libraries(${harness} PRIVATE advapi32)
    if(MSVC)
        target_compile_options(${harness} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    elseif(MINGW)
        target_compile_options(${harness} PRIVATE -Wall -Wextra -Wpedantic)
        target_link_options(${harness} PRIVATE -municode -static -static-libgcc -static-libstdc++)
    endif()
endforeach()

set(CMAKEBUILD_TEST_MSVC_QT_PREFIX "C:/Qt/6.11.2/msvc2022_64" CACHE PATH "Native MSVC Qt 6.11+ kit")
set(CMAKEBUILD_TEST_MINGW_QT_PREFIX "C:/Qt/6.11.2/mingw_64" CACHE PATH "Native MinGW Qt 6.11+ kit")
set(CMAKEBUILD_TEST_MINGW_BIN "C:/Qt/Tools/mingw1310_64/bin" CACHE PATH "Compiler matching the MinGW Qt kit")

enable_testing()
add_test(NAME engine_qt_discovery_msvc COMMAND engine_qt_discovery
    --toolchain msvc --cmake "${CMAKE_COMMAND}" --qt-prefix "${CMAKEBUILD_TEST_MSVC_QT_PREFIX}")
add_test(NAME engine_qt_discovery_mingw COMMAND engine_qt_discovery
    --toolchain mingw --cmake "${CMAKE_COMMAND}" --qt-prefix "${CMAKEBUILD_TEST_MINGW_QT_PREFIX}"
    --mingw-bin "${CMAKEBUILD_TEST_MINGW_BIN}" --discover-mingw)
set_tests_properties(engine_qt_discovery_msvc engine_qt_discovery_mingw PROPERTIES
    LABELS "integration;qt" TIMEOUT 600)

add_test(NAME engine_build_tests_msvc COMMAND engine_build_tests
    --toolchain msvc --cmake "${CMAKE_COMMAND}")
add_test(NAME engine_build_tests_mingw COMMAND engine_build_tests
    --toolchain mingw --cmake "${CMAKE_COMMAND}" --mingw-bin "${CMAKEBUILD_TEST_MINGW_BIN}")
set_tests_properties(engine_build_tests_msvc engine_build_tests_mingw PROPERTIES
    LABELS "integration;build-options" TIMEOUT 600)

add_test(NAME engine_rebuild COMMAND engine_rebuild_tests --cmake "${CMAKE_COMMAND}")
set_tests_properties(engine_rebuild PROPERTIES LABELS "integration;rebuild" TIMEOUT 240)

add_test(NAME journal_ui COMMAND journal_ui)
set_tests_properties(journal_ui PROPERTIES LABELS "journal" TIMEOUT 30)

add_test(NAME run_ui COMMAND run_ui)
set_tests_properties(run_ui PROPERTIES LABELS "integration;run-selection" TIMEOUT 90)

add_test(NAME native_dpi_ui COMMAND native_dpi_ui)
set_tests_properties(native_dpi_ui PROPERTIES LABELS "integration;native-dpi" TIMEOUT 30)
