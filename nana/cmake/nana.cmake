# NanaPlus: fixed fork snapshot, immutable archive and integrity checks.
include(FetchContent)
set(CMAKEBUILD_NANA_REVISION "57fdbd96b64a4c83f0878782997a12b5a5c906b3")
set(CMAKEBUILD_NANA_ARCHIVE_SHA256 "44ff40e9f64a27eb8e22f5d4fdafa9e421f1419a02e488956b0b05e4d6156db9")
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
if(NOT editor_header_hash STREQUAL "9bfdf9ed5b88cea6c6c554fbb12e03113fb2da5e9ce3204fab440cf6b25ec49c"
    OR NOT editor_source_hash STREQUAL "0e48c68236b22380844cc567bee492e6bb0314bb319280e9df3e79009fe6f97a"
    OR NOT native_source_hash STREQUAL "a4eb389bc9b0a57aa5258883599605a1d8598bef3d53f3ab18ab787a4fdb9d25"
    OR NOT window_manager_hash STREQUAL "61e5b000278f609ed555674b4d871faa865017f188feadc061000bd048712f8b"
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
