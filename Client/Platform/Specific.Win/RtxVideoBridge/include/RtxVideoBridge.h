#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#ifdef RVB_BUILD_DLL
#define RVB_API __declspec(dllexport)
#else
#define RVB_API __declspec(dllimport)
#endif
#define RVB_CALL __cdecl
#else
#define RVB_API
#define RVB_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RVB_API_VERSION 3u
#define RVB_INPUT_WIDTH 1280u
#define RVB_INPUT_HEIGHT 720u
#define RVB_OUTPUT_1080P_WIDTH 1920u
#define RVB_OUTPUT_1080P_HEIGHT 1080u
#define RVB_OUTPUT_1440P_WIDTH 2560u
#define RVB_OUTPUT_1440P_HEIGHT 1440u
#define RVB_OUTPUT_2160P_WIDTH 3840u
#define RVB_OUTPUT_2160P_HEIGHT 2160u
#define RVB_DEFAULT_OUTPUT_WIDTH RVB_OUTPUT_1440P_WIDTH
#define RVB_DEFAULT_OUTPUT_HEIGHT RVB_OUTPUT_1440P_HEIGHT
#define RVB_NVIDIA_VENDOR_ID 0x10DEu

typedef enum RvbStatus
{
    RVB_STATUS_OK = 0,
    RVB_STATUS_INVALID_ARGUMENT = 2,
    RVB_STATUS_MISSING_FEATURE_DLL = 10,
    RVB_STATUS_WRONG_ADAPTER = 11,
    RVB_STATUS_VSR_UNSUPPORTED = 12,
    RVB_STATUS_NGX_INITIALIZATION_FAILED = 13,
    RVB_STATUS_FEATURE_CREATION_FAILED = 14,
    RVB_STATUS_PROCESSING_FAILED = 15,
    RVB_STATUS_DEVICE_LOST = 16,
    RVB_STATUS_D3D_FAILED = 17,
    RVB_STATUS_BUFFER_TOO_SMALL = 18,
    RVB_STATUS_INTERNAL_ERROR = 19,
    RVB_STATUS_ABI_MISMATCH = 20
} RvbStatus;

typedef enum RvbQuality
{
    RVB_QUALITY_BICUBIC = 0,
    RVB_QUALITY_LOW = 1,
    RVB_QUALITY_MEDIUM = 2,
    RVB_QUALITY_HIGH = 3,
    RVB_QUALITY_ULTRA = 4
} RvbQuality;

typedef struct RvbProbeOptions
{
    uint32_t struct_size;
    const wchar_t* feature_directory;
    const wchar_t* application_data_directory;
    int32_t adapter_index;
} RvbProbeOptions;

typedef struct RvbCreateOptions
{
    uint32_t struct_size;
    const wchar_t* feature_directory;
    const wchar_t* application_data_directory;
    int32_t adapter_index;
    uint32_t input_width;
    uint32_t input_height;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t quality;
} RvbCreateOptions;

typedef struct RvbCapabilities
{
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t input_width;
    uint32_t input_height;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t quality_mask;
    uint32_t adapter_index;
    uint32_t adapter_vendor_id;
    uint32_t adapter_device_id;
    uint64_t adapter_dedicated_video_memory;
    int32_t adapter_luid_high;
    uint32_t adapter_luid_low;
    uint32_t needs_updated_driver;
    uint32_t minimum_driver_major;
    uint32_t minimum_driver_minor;
} RvbCapabilities;

typedef struct RvbFrameTiming
{
    uint32_t struct_size;
    uint32_t gpu_timing_valid;
    double upload_submit_cpu_ms;
    double evaluate_call_cpu_ms;
    double copy_submit_cpu_ms;
    double map_wait_cpu_ms;
    double row_copy_cpu_ms;
    double readback_cpu_ms;
    double process_cpu_ms;
    double query_resolve_cpu_ms;
    double upload_gpu_ms;
    double evaluate_gpu_ms;
    double readback_copy_gpu_ms;
    double total_gpu_ms;
    double direct_draw_submit_cpu_ms;
    double direct_draw_gpu_ms;
    double cpu_blocking_wait_ms;
    uint32_t direct_mode;
    uint32_t gpu_timing_latency_frames;
} RvbFrameTiming;

#define RVB_ADAPTER_NAME_UTF8_SIZE 128u

typedef struct RvbD3D11DeviceInfo
{
    uint32_t struct_size;
    uint32_t adapter_index;
    uint32_t adapter_vendor_id;
    uint32_t adapter_device_id;
    uint64_t adapter_dedicated_video_memory;
    int32_t adapter_luid_high;
    uint32_t adapter_luid_low;
    uint32_t feature_level;
    char adapter_name_utf8[RVB_ADAPTER_NAME_UTF8_SIZE];
} RvbD3D11DeviceInfo;

typedef struct RvbDirectRenderOptions
{
    uint32_t struct_size;
    float destination_x;
    float destination_y;
    float destination_width;
    float destination_height;
    uint32_t target_width;
    uint32_t target_height;
    uint32_t rotation_quarter_turns;
} RvbDirectRenderOptions;

typedef struct RvbHandle RvbHandle;

RVB_API uint32_t RVB_CALL rvb_get_api_version(void);

RVB_API RvbStatus RVB_CALL rvb_probe(
    const RvbProbeOptions* options,
    RvbCapabilities* capabilities);

RVB_API RvbStatus RVB_CALL rvb_create(
    const RvbCreateOptions* options,
    RvbHandle** handle);

/*
 * external_device must point to an ID3D11Device. The bridge AddRefs it for
 * the handle lifetime and releases only that reference. Device ownership
 * remains with the caller.
 */
RVB_API RvbStatus RVB_CALL rvb_create_with_d3d11_device(
    const RvbCreateOptions* options,
    void* external_device,
    RvbHandle** handle);

RVB_API RvbStatus RVB_CALL rvb_get_d3d11_device_info(
    void* device,
    RvbD3D11DeviceInfo* info);

RVB_API RvbStatus RVB_CALL rvb_process_rgba8(
    RvbHandle* handle,
    const void* input,
    uint32_t input_stride,
    void* output,
    uint32_t output_stride);

RVB_API RvbStatus RVB_CALL rvb_process_rgba8_gpu(
    RvbHandle* handle,
    const void* input,
    uint32_t input_stride);

RVB_API RvbStatus RVB_CALL rvb_render_output_d3d11(
    RvbHandle* handle,
    const RvbDirectRenderOptions* options);

/* Explicit screenshot/debug readback; not used during normal GPU-direct frames. */
RVB_API RvbStatus RVB_CALL rvb_readback_output_rgba8(
    RvbHandle* handle,
    void* output,
    uint32_t output_stride);

RVB_API RvbStatus RVB_CALL rvb_reconfigure(
    RvbHandle* handle,
    uint32_t output_width,
    uint32_t output_height,
    uint32_t quality);

RVB_API RvbStatus RVB_CALL rvb_get_last_frame_timing(
    RvbHandle* handle,
    RvbFrameTiming* timing);

/*
 * Call with utf8_buffer == NULL to query the required size including the NUL.
 * Passing handle == NULL returns the last error from rvb_probe/rvb_create on
 * the calling thread.
 */
RVB_API RvbStatus RVB_CALL rvb_get_last_error(
    RvbHandle* handle,
    char* utf8_buffer,
    size_t* buffer_size);

RVB_API void RVB_CALL rvb_destroy(RvbHandle* handle);

#ifdef __cplusplus
}
#endif
