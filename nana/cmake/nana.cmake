# NanaPlus: fixed fork snapshot, immutable archive and integrity checks.
include(FetchContent)
set(CMAKEBUILD_NANA_REVISION "dbc7805437864b524735257e3e531c1325fb453b")
set(CMAKEBUILD_NANA_ARCHIVE_SHA256 "de87198c0fb3e75e7c5a8f3525c1dec97e50bd581e27bed4e5835450ca6b1836")
set(CMAKEBUILD_NANA_SOURCE_DIR "" CACHE PATH "Local pinned NanaPlus source (optional)")
if(CMAKEBUILD_NANA_SOURCE_DIR)
    set(nana_source "${CMAKEBUILD_NANA_SOURCE_DIR}")
else()
    FetchContent_Declare(cmakebuild_nana
        URL "https://codeload.github.com/bagautdinovrf/nanaplus/zip/${CMAKEBUILD_NANA_REVISION}"
        URL_HASH "SHA256=${CMAKEBUILD_NANA_ARCHIVE_SHA256}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_GetProperties(cmakebuild_nana)
    if(NOT cmakebuild_nana_POPULATED)
        FetchContent_Populate(cmakebuild_nana)
    endif()
    set(nana_source "${cmakebuild_nana_SOURCE_DIR}")
endif()
if(NOT EXISTS "${nana_source}/include/nana/gui/widgets/textbox.hpp")
    message(FATAL_ERROR "NanaPlus source directory is invalid.")
endif()
file(READ "${nana_source}/include/nana/gui/widgets/skeletons/text_editor.hpp" editor_header_text)
file(READ "${nana_source}/source/gui/widgets/skeletons/text_editor.cpp" editor_source_text)
file(READ "${nana_source}/source/gui/detail/native_window_interface.cpp" native_source_text)
file(READ "${nana_source}/source/gui/detail/window_manager.cpp" window_manager_text)
file(READ "${nana_source}/source/gui/detail/bedrock_windows.cpp" bedrock_windows_text)
# Git may check out CRLF on Windows; normalize only the fingerprint input.
string(REPLACE "\r\n" "\n" editor_header_text "${editor_header_text}")
string(REPLACE "\r\n" "\n" editor_source_text "${editor_source_text}")
string(REPLACE "\r\n" "\n" native_source_text "${native_source_text}")
string(REPLACE "\r\n" "\n" window_manager_text "${window_manager_text}")
string(REPLACE "\r\n" "\n" bedrock_windows_text "${bedrock_windows_text}")
string(SHA256 editor_header_hash "${editor_header_text}")
string(SHA256 editor_source_hash "${editor_source_text}")
string(SHA256 native_source_hash "${native_source_text}")
string(SHA256 window_manager_hash "${window_manager_text}")
string(SHA256 bedrock_windows_hash "${bedrock_windows_text}")
file(READ "${nana_source}/source/gui/programming_interface.cpp" programming_source_text)
string(REPLACE "\r\n" "\n" programming_source_text "${programming_source_text}")
string(SHA256 programming_source_hash "${programming_source_text}")
file(READ "${nana_source}/include/nana/gui/programming_interface.hpp" programming_header_text)
file(READ "${nana_source}/source/paint/graphics.cpp" graphics_source_text)
file(READ "${nana_source}/source/gui/widgets/menu.cpp" menu_source_text)
string(REPLACE "\r\n" "\n" programming_header_text "${programming_header_text}")
string(REPLACE "\r\n" "\n" graphics_source_text "${graphics_source_text}")
string(REPLACE "\r\n" "\n" menu_source_text "${menu_source_text}")
string(SHA256 programming_header_hash "${programming_header_text}")
string(SHA256 graphics_source_hash "${graphics_source_text}")
string(SHA256 menu_source_hash "${menu_source_text}")
file(READ "${nana_source}/source/deploy.cpp" deploy_source_text)
file(READ "${nana_source}/source/charset.cpp" charset_source_text)
file(READ "${nana_source}/source/gui/widgets/skeletons/content_view.cpp" content_view_source_text)
file(READ "${nana_source}/source/gui/widgets/skeletons/content_view.hpp" content_view_header_text)
foreach(component deploy_source charset_source content_view_source content_view_header)
    string(REPLACE "\r\n" "\n" ${component}_text "${${component}_text}")
    string(SHA256 ${component}_hash "${${component}_text}")
endforeach()
file(READ "${nana_source}/source/gui/detail/basic_window.cpp" basic_window_source_text)
string(REPLACE "\r\n" "\n" basic_window_source_text "${basic_window_source_text}")
string(SHA256 basic_window_source_hash "${basic_window_source_text}")
file(READ "${nana_source}/source/gui/detail/basic_window.hpp" basic_window_header_text)
string(REPLACE "\r\n" "\n" basic_window_header_text "${basic_window_header_text}")
string(SHA256 basic_window_header_hash "${basic_window_header_text}")
file(READ "${nana_source}/source/gui/detail/bedrock_pi.cpp" bedrock_pi_source_text)
string(REPLACE "\r\n" "\n" bedrock_pi_source_text "${bedrock_pi_source_text}")
string(SHA256 bedrock_pi_source_hash "${bedrock_pi_source_text}")
file(READ "${nana_source}/include/nana/gui/detail/bedrock.hpp" bedrock_header_text)
string(REPLACE "\r\n" "\n" bedrock_header_text "${bedrock_header_text}")
string(SHA256 bedrock_header_hash "${bedrock_header_text}")
if(NOT programming_source_hash STREQUAL "450940b90ded1f72f6712657b60503275b4ba56aac61655d55f93f74085666fd"
    OR NOT basic_window_source_hash STREQUAL "4439a172556c02b2b9c242f1239d0f0839198cc116bccfd5904a078d3a15fa98"
    OR NOT basic_window_header_hash STREQUAL "4be2ae844a393dd0b992ba416e0f2cf74664a8b48ab014b370e900a716185d65"
    OR NOT bedrock_pi_source_hash STREQUAL "193900bcc60b8fe4400d950d55c0b47636370c2113f234df5a962d6664d06afd"
    OR NOT bedrock_header_hash STREQUAL "7e6f3040a50b23bc9b62636efe140b7a573febfb59258fda1d44a9a18d0a4822"
    OR NOT deploy_source_hash STREQUAL "5247a5b26446e2e35c5e8654d10543dad291e386d1805b488c6e8cd269a5b513"
    OR NOT charset_source_hash STREQUAL "7190e75632535fed616d59c1fb272799a8f28f5adf27c638099d6efbe0bf1132"
    OR NOT content_view_source_hash STREQUAL "d47319d0ae57de1a069a5e85e391e9b1026b7f232ca02a1bfb1de6a7c68de1fd"
    OR NOT content_view_header_hash STREQUAL "184e1d7556b7d4e8047dc6de6625ee33ddbac69733ee3af8e3fef26a8f655f21"
    OR NOT programming_header_hash STREQUAL "d82839ae848c0ae5bbfde32e9128c94483f6c614bddc0d135ed833c9b0ad9c8d"
    OR NOT graphics_source_hash STREQUAL "2a4414c7d309d928af8a351367f79d1d8d14f2cda602aa2d9c7b6da810d9fdba"
    OR NOT menu_source_hash STREQUAL "026c12711b801e47433c8d308a151b947ba6940d2cba896914e9ff727a1d402c"
    OR NOT editor_header_hash STREQUAL "d0d51ad3cffc1892f5a73f2df0bf07d427172d5b9d3ae3b49cdebba7567367b4"
    OR NOT editor_source_hash STREQUAL "0f0c88669dbb5d1f3ee708a205f606282116adef35e018be10910be3df9c8c3b"
    OR NOT native_source_hash STREQUAL "a4eb389bc9b0a57aa5258883599605a1d8598bef3d53f3ab18ab787a4fdb9d25"
    OR NOT window_manager_hash STREQUAL "b7e62bd6c18db3aad9553bc559873688f8dfb2edb7201c4f97217a066414f531"
    OR NOT bedrock_windows_hash STREQUAL "dadb7b0b10906f379640ec2c5f55aee86cd7152dc965e8ef2fd677a1f67293c1")
    message(FATAL_ERROR "NanaPlus source must match commit ${CMAKEBUILD_NANA_REVISION} (bagautdinovrf/nanaplus).")
endif()
# Compile an unmodified cache copy. Archive timestamps may precede existing
# objects; COPYONLY updates changed contents with a fresh output timestamp.
set(nana_cached "${CMAKE_CURRENT_BINARY_DIR}/nana-source")
file(GLOB_RECURSE nana_input_files CONFIGURE_DEPENDS RELATIVE "${nana_source}"
    "${nana_source}/include/*" "${nana_source}/source/*")
foreach(nana_input_file IN LISTS nana_input_files)
    configure_file("${nana_source}/${nana_input_file}" "${nana_cached}/${nana_input_file}" COPYONLY)
endforeach()
file(GLOB_RECURSE nana_sources CONFIGURE_DEPENDS "${nana_cached}/source/*.cpp")
list(FILTER nana_sources EXCLUDE REGEX "/audio/")
add_library(Nana STATIC ${nana_sources})
add_library(Nana::Nana ALIAS Nana)
target_include_directories(Nana SYSTEM PUBLIC "${nana_cached}/include")
target_compile_features(Nana PUBLIC cxx_std_23)
target_compile_definitions(Nana PUBLIC NANA_IGNORE_CONF NANA_dpi_aware=false)
target_link_libraries(Nana PUBLIC user32 gdi32 comdlg32 shell32 ole32 uuid comctl32)
if(MSVC)
    target_compile_options(Nana PRIVATE /utf-8 /Zc:__cplusplus /permissive- /MP)
endif()
