cmake_minimum_required(VERSION 3.24)

project(RtxVideoBridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(RTX_VIDEO_SDK_DIR "$ENV{RTX_VIDEO_SDK_DIR}" CACHE PATH
    "Path to the extracted official NVIDIA RTX Video SDK 1.1 directory")
set(SYSDVR_NATIVE_OUTPUT_DIR "" CACHE PATH
    "Optional SysDVR win-x64 native resource directory for RtxVideoBridge.dll")

if(NOT EXISTS "${RTX_VIDEO_SDK_DIR}/include/nvsdk_ngx.h")
    message(FATAL_ERROR
        "RTX_VIDEO_SDK_DIR must point to the extracted official NVIDIA RTX Video SDK")
endif()
if(NOT EXISTS "${RTX_VIDEO_SDK_DIR}/lib/Windows/x64/nvsdk_ngx_s.lib")
    message(FATAL_ERROR "nvsdk_ngx_s.lib was not found under RTX_VIDEO_SDK_DIR")
endif()

add_library(RtxVideoBridge SHARED
    src/RtxVideoBridge.cpp
)

target_include_directories(RtxVideoBridge PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_include_directories(RtxVideoBridge SYSTEM PRIVATE
    "${RTX_VIDEO_SDK_DIR}/include"
)

target_link_libraries(RtxVideoBridge PRIVATE
    "${RTX_VIDEO_SDK_DIR}/lib/Windows/x64/nvsdk_ngx_s.lib"
    d3d11
    dxgi
    user32
    shell32
    advapi32
)

target_compile_definitions(RtxVideoBridge PRIVATE
    RVB_BUILD_DLL
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)

add_executable(RtxVideoBridgeSmokeTest
    src/SmokeTest.cpp
)
target_include_directories(RtxVideoBridgeSmokeTest PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(RtxVideoBridgeSmokeTest PRIVATE RtxVideoBridge)
target_compile_definitions(RtxVideoBridgeSmokeTest PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)

if(MSVC)
    foreach(target RtxVideoBridge RtxVideoBridgeSmokeTest)
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /utf-8)
    endforeach()
endif()

if(SYSDVR_NATIVE_OUTPUT_DIR)
    add_custom_command(TARGET RtxVideoBridge POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SYSDVR_NATIVE_OUTPUT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:RtxVideoBridge>"
            "${SYSDVR_NATIVE_OUTPUT_DIR}/RtxVideoBridge.dll"
        COMMENT "Copying RtxVideoBridge.dll to the SysDVR Windows native resource directory"
    )
endif()
