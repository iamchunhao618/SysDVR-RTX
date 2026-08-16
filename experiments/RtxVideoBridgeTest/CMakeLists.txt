cmake_minimum_required(VERSION 3.24)

project(RtxVideoBridgeTest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(RTX_VIDEO_SDK_DIR "$ENV{RTX_VIDEO_SDK_DIR}" CACHE PATH
    "Path to the extracted NVIDIA RTX Video SDK 1.1.0 directory")

if(NOT EXISTS "${RTX_VIDEO_SDK_DIR}/include/nvsdk_ngx.h")
    message(FATAL_ERROR
        "RTX_VIDEO_SDK_DIR must point to the extracted NVIDIA RTX Video SDK directory")
endif()

if(NOT EXISTS "${RTX_VIDEO_SDK_DIR}/lib/Windows/x64/nvsdk_ngx_s.lib")
    message(FATAL_ERROR "nvsdk_ngx_s.lib was not found under RTX_VIDEO_SDK_DIR")
endif()

add_executable(RtxVideoBridgeTest
    src/main.cpp
    src/WicImage.cpp
    src/RtxVsrBenchmark.cpp
)

target_include_directories(RtxVideoBridgeTest PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_include_directories(RtxVideoBridgeTest SYSTEM PRIVATE
    "${RTX_VIDEO_SDK_DIR}/include"
)

target_link_libraries(RtxVideoBridgeTest PRIVATE
    "${RTX_VIDEO_SDK_DIR}/lib/Windows/x64/nvsdk_ngx_s.lib"
    d3d11
    dxgi
    windowscodecs
    ole32
    user32
    shell32
    advapi32
)

target_compile_definitions(RtxVideoBridgeTest PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)

if(MSVC)
    set_property(TARGET RtxVideoBridgeTest PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(RtxVideoBridgeTest PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
