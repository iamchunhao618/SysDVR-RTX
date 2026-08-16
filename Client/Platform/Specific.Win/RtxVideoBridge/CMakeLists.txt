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

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/third_party/smaa/SMAA.hlsl"
    SMAA_REFERENCE_SOURCE)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Fxaa3_11_MicrosoftPort.hlsl"
    FXAA_SHADER_SOURCE)

# MSVC limits an individual string literal to about 16 KiB. Split the
# canonical shader into adjacent raw literals without modifying its contents.
string(LENGTH "${SMAA_REFERENCE_SOURCE}" SMAA_SOURCE_LENGTH)
set(SMAA_CHUNKED_SOURCE "")
set(SMAA_SOURCE_OFFSET 0)
while(SMAA_SOURCE_OFFSET LESS SMAA_SOURCE_LENGTH)
    math(EXPR SMAA_SOURCE_REMAINING
        "${SMAA_SOURCE_LENGTH} - ${SMAA_SOURCE_OFFSET}")
    if(SMAA_SOURCE_REMAINING GREATER 8000)
        string(SUBSTRING "${SMAA_REFERENCE_SOURCE}"
            ${SMAA_SOURCE_OFFSET} 8000 SMAA_SOURCE_PROBE)
        string(FIND "${SMAA_SOURCE_PROBE}" "\n" SMAA_SOURCE_LINE_END REVERSE)
        if(SMAA_SOURCE_LINE_END GREATER 0)
            math(EXPR SMAA_SOURCE_CHUNK_LENGTH "${SMAA_SOURCE_LINE_END} + 1")
        else()
            set(SMAA_SOURCE_CHUNK_LENGTH 8000)
        endif()
    else()
        set(SMAA_SOURCE_CHUNK_LENGTH ${SMAA_SOURCE_REMAINING})
    endif()
    string(SUBSTRING "${SMAA_REFERENCE_SOURCE}"
        ${SMAA_SOURCE_OFFSET} ${SMAA_SOURCE_CHUNK_LENGTH} SMAA_SOURCE_CHUNK)
    string(APPEND SMAA_CHUNKED_SOURCE "${SMAA_SOURCE_CHUNK}")
    math(EXPR SMAA_SOURCE_OFFSET
        "${SMAA_SOURCE_OFFSET} + ${SMAA_SOURCE_CHUNK_LENGTH}")
    if(SMAA_SOURCE_OFFSET LESS SMAA_SOURCE_LENGTH)
        string(APPEND SMAA_CHUNKED_SOURCE
            ")RVB_SMAA\"\nR\"RVB_SMAA(")
    endif()
endwhile()
set(SMAA_REFERENCE_SOURCE "${SMAA_CHUNKED_SOURCE}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(src/EmbeddedSmaaShader.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedSmaaShader.h" @ONLY)
configure_file(src/EmbeddedFxaaShader.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedFxaaShader.h" @ONLY)

add_library(RtxVideoBridge SHARED
    src/PostProcessAA.cpp
    src/RtxVideoBridge.cpp
)

target_include_directories(RtxVideoBridge PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_include_directories(RtxVideoBridge PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/smaa/Textures"
)
target_include_directories(RtxVideoBridge SYSTEM PRIVATE
    "${RTX_VIDEO_SDK_DIR}/include"
)

target_link_libraries(RtxVideoBridge PRIVATE
    "${RTX_VIDEO_SDK_DIR}/lib/Windows/x64/nvsdk_ngx_s.lib"
    d3d11
    d3dcompiler
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
