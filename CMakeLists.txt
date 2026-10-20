cmake_minimum_required(VERSION 3.20)
project(WinTray LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Read metadata from manifest.json
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json" MANIFEST_RAW)
string(JSON APP_NAME GET "${MANIFEST_RAW}" name)
string(JSON APP_VERSION GET "${MANIFEST_RAW}" version)
string(JSON APP_AUTHOR GET "${MANIFEST_RAW}" author)
string(JSON APP_REPO GET "${MANIFEST_RAW}" repository)

string(REPLACE "." ";" VERSION_PARTS "${APP_VERSION}")
list(GET VERSION_PARTS 0 VERSION_MAJOR)
list(GET VERSION_PARTS 1 VERSION_MINOR)
list(GET VERSION_PARTS 2 VERSION_PATCH)

# 2. Configure version.rc from template
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/version.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
    @ONLY
)

# 3. Generate version_info.h for C++
set(VERSION_HEADER
"// Auto-generated from manifest.json - Do not edit directly
#pragma once
#include <string>

inline const std::wstring CURRENT_VERSION = L\"v${APP_VERSION}\";
inline const std::wstring GITHUB_REPO     = L\"${APP_REPO}\";
inline const std::wstring APP_AUTHOR      = L\"${APP_AUTHOR}\";
"
)
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/src/version_info.h" "${VERSION_HEADER}")

# 4. Modular Target Sources
set(SOURCES
    src/main.cpp
    src/state.h
    src/settings.h
    src/settings.cpp
    src/updater.h
    src/updater.cpp
    src/media.h
    src/media.cpp
    src/appbar.h
    src/appbar.cpp
    src/blur.h
    src/blur.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest"
)

add_executable(${PROJECT_NAME} WIN32 ${SOURCES})

set_property(TARGET ${PROJECT_NAME} PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    User32
    Shell32
    Dwmapi
    Ole32
    Msimg32
    Wininet
    Comctl32
    Crypt32
    Gdi32
    d2d1
    dwrite
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)