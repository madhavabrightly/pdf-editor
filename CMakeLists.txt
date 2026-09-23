cmake_minimum_required(VERSION 3.20)
project(Rpfg LANGUAGES CXX)

# Rpfg links prebuilt MuPDF + WebView2 static libraries, so the compiler and CRT
# must match what those libraries were built with. That means MSVC.
if(NOT MSVC)
    message(FATAL_ERROR
        "Rpfg must be built with MSVC so the CRT matches the prebuilt MuPDF and "
        "WebView2 libraries.\nRun scripts/build.ps1, which sets up the Visual "
        "Studio developer environment for you.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MUPDF_SRC     "" CACHE PATH     "Root of the extracted MuPDF source tree")
set(MUPDF_LIB_DIR "" CACHE PATH     "Directory holding the built MuPDF static libraries")
set(WEBVIEW2_SDK  "" CACHE PATH     "Root of the extracted Microsoft.Web.WebView2 package (build/native)")

foreach(var MUPDF_SRC MUPDF_LIB_DIR WEBVIEW2_SDK)
    if(NOT ${var})
        message(FATAL_ERROR
            "${var} is not set. Use scripts/build.ps1 instead of configuring "
            "CMake by hand.")
    endif()
endforeach()

if(NOT EXISTS "${MUPDF_SRC}/include/mupdf/fitz.h")
    message(FATAL_ERROR "No MuPDF headers under MUPDF_SRC=${MUPDF_SRC}")
endif()
if(NOT EXISTS "${WEBVIEW2_SDK}/include/WebView2.h")
    message(FATAL_ERROR "No WebView2.h under WEBVIEW2_SDK=${WEBVIEW2_SDK}")
endif()

# Link every static library the MuPDF solution produced (libmupdf plus its
# third-party archives: harfbuzz, freetype, jbig2dec, openjpeg, zlib, ...).
file(GLOB MUPDF_LIBS CONFIGURE_DEPENDS "${MUPDF_LIB_DIR}/*.lib")
list(LENGTH MUPDF_LIBS MUPDF_LIB_COUNT)
if(MUPDF_LIB_COUNT EQUAL 0)
    message(FATAL_ERROR "No .lib files found in MUPDF_LIB_DIR=${MUPDF_LIB_DIR}. Run scripts/build_mupdf.ps1.")
endif()

set(RPFG_WARNINGS /W4 /permissive- /utf-8 /EHsc /MP /Zc:__cplusplus /external:W0)
set(RPFG_DEFINES
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0A00   # WebView2 and SetThreadDescription need Windows 10 APIs
)

# ---------------------------------------------------------------------------
# rpfg_core: the PDF engine and utilities. Deliberately free of any windowing or
# WebView2 dependency, so it can be driven by tests and command line tools as
# well as by the UI.
# ---------------------------------------------------------------------------

file(GLOB_RECURSE RPFG_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pdf/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pdf/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/document/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/document/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/io/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/io/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/render/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/render/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/util/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/util/*.h")

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${RPFG_CORE_SOURCES})

add_library(rpfg_core STATIC ${RPFG_CORE_SOURCES})

target_include_directories(rpfg_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Third-party headers go in as SYSTEM includes: MuPDF's inline functions trigger
# C4100/C4611 chatter under /W4 that has nothing to do with this code.
target_include_directories(rpfg_core SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    "${MUPDF_SRC}/include"
)

target_compile_definitions(rpfg_core PUBLIC ${RPFG_DEFINES})
target_compile_options(rpfg_core PRIVATE ${RPFG_WARNINGS})

target_link_libraries(rpfg_core PUBLIC
    ${MUPDF_LIBS}
    ole32.lib
    oleaut32.lib
    advapi32.lib
    version.lib
    ws2_32.lib
    crypt32.lib
)

# ---------------------------------------------------------------------------
# Rpfg: the Win32 + WebView2 shell.
# ---------------------------------------------------------------------------

file(GLOB_RECURSE RPFG_APP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.h")
list(APPEND RPFG_APP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${RPFG_APP_SOURCES})

add_executable(Rpfg WIN32 ${RPFG_APP_SOURCES})

target_include_directories(Rpfg SYSTEM PRIVATE "${WEBVIEW2_SDK}/include")
target_compile_options(Rpfg PRIVATE ${RPFG_WARNINGS})

target_link_libraries(Rpfg PRIVATE
    rpfg_core
    "${WEBVIEW2_SDK}/x64/WebView2LoaderStatic.lib"
    windowscodecs.lib
    shell32.lib
    shlwapi.lib
    user32.lib
    gdi32.lib
)

# ---------------------------------------------------------------------------
# Tests. These drive rpfg_core directly - no window, no WebView2.
# ---------------------------------------------------------------------------

add_executable(rpfg_text_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/text_geometry_test.cpp")
target_compile_options(rpfg_text_test PRIVATE ${RPFG_WARNINGS})
target_link_libraries(rpfg_text_test PRIVATE rpfg_core)

add_executable(rpfg_edit_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/edit_save_test.cpp")
target_compile_options(rpfg_edit_test PRIVATE ${RPFG_WARNINGS})
target_link_libraries(rpfg_edit_test PRIVATE rpfg_core)

add_executable(rpfg_model_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/model_test.cpp")
target_compile_options(rpfg_model_test PRIVATE ${RPFG_WARNINGS})
target_link_libraries(rpfg_model_test PRIVATE rpfg_core)

add_executable(rpfg_roundtrip_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/roundtrip_test.cpp")
target_compile_options(rpfg_roundtrip_test PRIVATE ${RPFG_WARNINGS})
target_link_libraries(rpfg_roundtrip_test PRIVATE rpfg_core)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(rpfg_core PRIVATE /O2 /Oi /Gy)
    target_compile_options(Rpfg PRIVATE /O2 /Oi /Gy)
    target_compile_options(rpfg_text_test PRIVATE /O2 /Oi /Gy)
    target_compile_options(rpfg_edit_test PRIVATE /O2 /Oi /Gy)
    target_compile_options(rpfg_model_test PRIVATE /O2 /Oi /Gy)
    target_compile_options(rpfg_roundtrip_test PRIVATE /O2 /Oi /Gy)
endif()

# Stage the web UI beside the executable. The app maps this folder onto the
# virtual host name https://app.local/, so the UI is served as a real origin
# rather than through file:// (which breaks fetch/module loading).
#
# This is a stamp-driven step (not a POST_BUILD on Rpfg) so that editing *only*
# web files restages the UI: a POST_BUILD would run only when Rpfg relinks, which
# a JS/CSS change does not trigger, leaving a stale UI next to the exe.
file(GLOB_RECURSE RPFG_WEB_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/*")
set(RPFG_WEB_STAMP "${CMAKE_BINARY_DIR}/web.stamp")
add_custom_command(
    OUTPUT "${RPFG_WEB_STAMP}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_BINARY_DIR}/web"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/web"
            "${CMAKE_BINARY_DIR}/web"
    COMMAND ${CMAKE_COMMAND} -E touch "${RPFG_WEB_STAMP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/fonts"
    DEPENDS ${RPFG_WEB_SOURCES}
    COMMENT "Staging web assets next to Rpfg.exe"
    VERBATIM
)
add_custom_target(rpfg_web_stage ALL DEPENDS "${RPFG_WEB_STAMP}")
add_dependencies(Rpfg rpfg_web_stage)
