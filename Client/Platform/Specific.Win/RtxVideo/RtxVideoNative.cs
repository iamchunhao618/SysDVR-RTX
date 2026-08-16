using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using SysDVR.Client.Core;
using static SDL2.SDL;

namespace SysDVR.Client.Platform.Specific.Win.RtxVideo
{
    internal enum RtxVideoStatus : int
    {
        Ok = 0,
        InvalidArgument = 2,
        MissingFeatureDll = 10,
        WrongAdapter = 11,
        VsrUnsupported = 12,
        NgxInitializationFailed = 13,
        FeatureCreationFailed = 14,
        ProcessingFailed = 15,
        DeviceLost = 16,
        D3dFailed = 17,
        BufferTooSmall = 18,
        InternalError = 19,
        AbiMismatch = 20,
        BridgeDllMissing = 100,
        BridgeExportMissing = 101,
    }

    internal sealed class RtxVideoException : Exception
    {
        public RtxVideoStatus Status { get; }

        public RtxVideoException(RtxVideoStatus status, string message)
            : base(message)
        {
            Status = status;
        }

        public RtxVideoException(RtxVideoStatus status, string message, Exception inner)
            : base(message, inner)
        {
            Status = status;
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RtxVideoProbeOptions
    {
        public uint StructSize;
        public nint FeatureDirectory;
        public nint ApplicationDataDirectory;
        public int AdapterIndex;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RtxVideoCreateOptions
    {
        public uint StructSize;
        public nint FeatureDirectory;
        public nint ApplicationDataDirectory;
        public int AdapterIndex;
        public uint InputWidth;
        public uint InputHeight;
        public uint OutputWidth;
        public uint OutputHeight;
        public uint Quality;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RtxVideoCapabilities
    {
        public uint StructSize;
        public uint ApiVersion;
        public uint InputWidth;
        public uint InputHeight;
        public uint OutputWidth;
        public uint OutputHeight;
        public uint QualityMask;
        public uint AdapterIndex;
        public uint AdapterVendorId;
        public uint AdapterDeviceId;
        public ulong AdapterDedicatedVideoMemory;
        public int AdapterLuidHigh;
        public uint AdapterLuidLow;
        public uint NeedsUpdatedDriver;
        public uint MinimumDriverMajor;
        public uint MinimumDriverMinor;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RtxVideoNativeFrameTiming
    {
        public uint StructSize;
        public uint GpuTimingValid;
        public double UploadSubmitCpuMilliseconds;
        public double EvaluateCallCpuMilliseconds;
        public double CopySubmitCpuMilliseconds;
        public double MapWaitCpuMilliseconds;
        public double RowCopyCpuMilliseconds;
        public double ReadbackCpuMilliseconds;
        public double ProcessCpuMilliseconds;
        public double QueryResolveCpuMilliseconds;
        public double UploadGpuMilliseconds;
        public double EvaluateGpuMilliseconds;
        public double ReadbackCopyGpuMilliseconds;
        public double TotalGpuMilliseconds;
        public double DirectDrawSubmitCpuMilliseconds;
        public double DirectDrawGpuMilliseconds;
        public double CpuBlockingWaitMilliseconds;
        public uint DirectMode;
        public uint GpuTimingLatencyFrames;
        public double FxaaGpuMilliseconds;
        public double SmaaEdgeGpuMilliseconds;
        public double SmaaBlendGpuMilliseconds;
        public double SmaaNeighborhoodGpuMilliseconds;
        public double PostAaGpuMilliseconds;
        public uint PostAaRequested;
        public uint PostAaApplied;
        public uint PostAaFailed;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    internal unsafe struct RtxVideoD3D11DeviceInfo
    {
        public const int AdapterNameBytes = 128;

        public uint StructSize;
        public uint AdapterIndex;
        public uint AdapterVendorId;
        public uint AdapterDeviceId;
        public ulong AdapterDedicatedVideoMemory;
        public int AdapterLuidHigh;
        public uint AdapterLuidLow;
        public uint FeatureLevel;
        public fixed byte AdapterNameUtf8[AdapterNameBytes];

        public string AdapterName
        {
            get
            {
                fixed (byte* name = AdapterNameUtf8)
                {
                    int length = 0;
                    while (length < AdapterNameBytes && name[length] != 0)
                        ++length;
                    return Encoding.UTF8.GetString(name, length);
                }
            }
        }

        public string Luid => $"{AdapterLuidHigh:X8}:{AdapterLuidLow:X8}";
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RtxVideoDirectRenderOptions
    {
        public uint StructSize;
        public float DestinationX;
        public float DestinationY;
        public float DestinationWidth;
        public float DestinationHeight;
        public uint TargetWidth;
        public uint TargetHeight;
        public uint RotationQuarterTurns;
        public uint PostAaMode;
        public float FxaaSubpixel;
        public float FxaaEdgeThreshold;
        public float FxaaEdgeThresholdMin;
    }

    internal readonly record struct RtxVideoOutputSize(uint Width, uint Height)
    {
        public static RtxVideoOutputSize From(RtxVideoOutputResolution resolution) =>
            resolution switch
            {
                RtxVideoOutputResolution.FullHd1080p => new(1920, 1080),
                RtxVideoOutputResolution.QuadHd1440p => new(2560, 1440),
                RtxVideoOutputResolution.UltraHd2160p => new(3840, 2160),
                _ => throw new ArgumentOutOfRangeException(
                    nameof(resolution), resolution, "Unsupported RTX VSR output resolution."),
            };
    }

    internal static class RtxVideoPaths
    {
        private const string BridgePathEnvironment = "SYSDVR_RTX_VIDEO_BRIDGE_PATH";
        private const string FeaturePathEnvironment = "SYSDVR_RTX_VIDEO_FEATURE_DIR";
        private const string SdkPathEnvironment = "RTX_VIDEO_SDK_DIR";

        public static string BridgePath
        {
            get
            {
                string? configured = Environment.GetEnvironmentVariable(BridgePathEnvironment);
                if (!string.IsNullOrWhiteSpace(configured))
                    return Path.GetFullPath(configured);

                return Path.Combine(
                    AppContext.BaseDirectory,
                    "runtimes", "win-x64", "native", "RtxVideoBridge.dll");
            }
        }

        public static string FeatureDirectory
        {
            get
            {
                string? configured = Environment.GetEnvironmentVariable(FeaturePathEnvironment);
                if (!string.IsNullOrWhiteSpace(configured))
                    return Path.GetFullPath(configured);

                string? sdkDirectory = Environment.GetEnvironmentVariable(SdkPathEnvironment);
                if (!string.IsNullOrWhiteSpace(sdkDirectory))
                    return Path.Combine(
                        Path.GetFullPath(sdkDirectory),
                        "bin", "Windows", "x64", "dev");

                return Path.Combine(
                    AppContext.BaseDirectory,
                    "runtimes", "win-x64", "native");
            }
        }

        public static string ApplicationDataDirectory => Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "SysDVR", "RtxVideo", "ngx-cache");
    }

    internal sealed unsafe class RtxVideoNative : IDisposable
    {
        public const uint ApiVersion = 4;
        public const uint InputWidth = 1280;
        public const uint InputHeight = 720;

        public uint OutputWidth { get; private set; }
        public uint OutputHeight { get; private set; }
        public bool IsGpuDirect { get; private set; }

        private nint _library;
        private nint _handle;
        private bool _disposed;

        private readonly delegate* unmanaged[Cdecl]<uint> _getApiVersion;
        private readonly delegate* unmanaged[Cdecl]<RtxVideoProbeOptions*, RtxVideoCapabilities*, RtxVideoStatus> _probe;
        private readonly delegate* unmanaged[Cdecl]<RtxVideoCreateOptions*, nint*, RtxVideoStatus> _create;
        private readonly delegate* unmanaged[Cdecl]<RtxVideoCreateOptions*, nint, nint*, RtxVideoStatus> _createWithD3D11Device;
        private readonly delegate* unmanaged[Cdecl]<nint, RtxVideoD3D11DeviceInfo*, RtxVideoStatus> _getD3D11DeviceInfo;
        private readonly delegate* unmanaged[Cdecl]<nint, void*, uint, void*, uint, RtxVideoStatus> _process;
        private readonly delegate* unmanaged[Cdecl]<nint, void*, uint, RtxVideoStatus> _processGpu;
        private readonly delegate* unmanaged[Cdecl]<nint, RtxVideoDirectRenderOptions*, RtxVideoStatus> _renderOutputD3D11;
        private readonly delegate* unmanaged[Cdecl]<nint, void*, uint, RtxVideoStatus> _readbackOutput;
        private readonly delegate* unmanaged[Cdecl]<nint, uint, uint, uint, RtxVideoStatus> _reconfigure;
        private readonly delegate* unmanaged[Cdecl]<nint, RtxVideoNativeFrameTiming*, RtxVideoStatus> _getLastFrameTiming;
        private readonly delegate* unmanaged[Cdecl]<nint, byte*, nuint*, RtxVideoStatus> _getLastError;
        private readonly delegate* unmanaged[Cdecl]<nint, void> _destroy;

        public RtxVideoNative()
        {
            string bridgePath = RtxVideoPaths.BridgePath;
            try
            {
                _library = NativeLibrary.Load(bridgePath);
            }
            catch (Exception error) when (
                error is DllNotFoundException ||
                error is BadImageFormatException ||
                error is FileLoadException)
            {
                throw new RtxVideoException(
                    RtxVideoStatus.BridgeDllMissing,
                    $"Could not load RtxVideoBridge.dll from '{bridgePath}': {error.Message}",
                    error);
            }

            try
            {
                _getApiVersion = (delegate* unmanaged[Cdecl]<uint>)GetExport("rvb_get_api_version");
                _probe = (delegate* unmanaged[Cdecl]<RtxVideoProbeOptions*, RtxVideoCapabilities*, RtxVideoStatus>)GetExport("rvb_probe");
                _create = (delegate* unmanaged[Cdecl]<RtxVideoCreateOptions*, nint*, RtxVideoStatus>)GetExport("rvb_create");
                _createWithD3D11Device = (delegate* unmanaged[Cdecl]<RtxVideoCreateOptions*, nint, nint*, RtxVideoStatus>)GetExport("rvb_create_with_d3d11_device");
                _getD3D11DeviceInfo = (delegate* unmanaged[Cdecl]<nint, RtxVideoD3D11DeviceInfo*, RtxVideoStatus>)GetExport("rvb_get_d3d11_device_info");
                _process = (delegate* unmanaged[Cdecl]<nint, void*, uint, void*, uint, RtxVideoStatus>)GetExport("rvb_process_rgba8");
                _processGpu = (delegate* unmanaged[Cdecl]<nint, void*, uint, RtxVideoStatus>)GetExport("rvb_process_rgba8_gpu");
                _renderOutputD3D11 = (delegate* unmanaged[Cdecl]<nint, RtxVideoDirectRenderOptions*, RtxVideoStatus>)GetExport("rvb_render_output_d3d11");
                _readbackOutput = (delegate* unmanaged[Cdecl]<nint, void*, uint, RtxVideoStatus>)GetExport("rvb_readback_output_rgba8");
                _reconfigure = (delegate* unmanaged[Cdecl]<nint, uint, uint, uint, RtxVideoStatus>)GetExport("rvb_reconfigure");
                _getLastFrameTiming = (delegate* unmanaged[Cdecl]<nint, RtxVideoNativeFrameTiming*, RtxVideoStatus>)GetExport("rvb_get_last_frame_timing");
                _getLastError = (delegate* unmanaged[Cdecl]<nint, byte*, nuint*, RtxVideoStatus>)GetExport("rvb_get_last_error");
                _destroy = (delegate* unmanaged[Cdecl]<nint, void>)GetExport("rvb_destroy");

                uint actualVersion = _getApiVersion();
                if (actualVersion != ApiVersion)
                {
                    throw new RtxVideoException(
                        RtxVideoStatus.AbiMismatch,
                        $"RtxVideoBridge ABI mismatch: client expects {ApiVersion}, DLL reports {actualVersion}.");
                }
            }
            catch
            {
                Dispose();
                throw;
            }
        }

        private nint GetExport(string name)
        {
            try
            {
                return NativeLibrary.GetExport(_library, name);
            }
            catch (Exception error)
            {
                throw new RtxVideoException(
                    RtxVideoStatus.BridgeExportMissing,
                    $"RtxVideoBridge.dll is missing required ABI export '{name}'.",
                    error);
            }
        }

        public RtxVideoCapabilities Probe()
        {
            ThrowIfDisposed();

            string featureDirectory = RtxVideoPaths.FeatureDirectory;
            string applicationDataDirectory = RtxVideoPaths.ApplicationDataDirectory;
            Directory.CreateDirectory(applicationDataDirectory);

            fixed (char* featurePath = featureDirectory)
            fixed (char* applicationDataPath = applicationDataDirectory)
            {
                RtxVideoProbeOptions options = new()
                {
                    StructSize = (uint)sizeof(RtxVideoProbeOptions),
                    FeatureDirectory = (nint)featurePath,
                    ApplicationDataDirectory = (nint)applicationDataPath,
                    AdapterIndex = -1,
                };
                RtxVideoCapabilities capabilities = new()
                {
                    StructSize = (uint)sizeof(RtxVideoCapabilities),
                };

                RtxVideoStatus status = _probe(&options, &capabilities);
                ThrowOnFailure(status, 0, "RTX VSR capability probe");
                return capabilities;
            }
        }

        public void Create(
            RtxVideoQuality quality,
            RtxVideoOutputResolution outputResolution,
            nint externalD3D11Device = 0)
        {
            ThrowIfDisposed();
            if (_handle != 0)
                throw new InvalidOperationException("The RTX Video bridge is already initialized.");

            string featureDirectory = RtxVideoPaths.FeatureDirectory;
            string applicationDataDirectory = RtxVideoPaths.ApplicationDataDirectory;
            Directory.CreateDirectory(applicationDataDirectory);
            RtxVideoOutputSize outputSize = RtxVideoOutputSize.From(outputResolution);

            fixed (char* featurePath = featureDirectory)
            fixed (char* applicationDataPath = applicationDataDirectory)
            {
                RtxVideoCreateOptions options = new()
                {
                    StructSize = (uint)sizeof(RtxVideoCreateOptions),
                    FeatureDirectory = (nint)featurePath,
                    ApplicationDataDirectory = (nint)applicationDataPath,
                    AdapterIndex = -1,
                    InputWidth = InputWidth,
                    InputHeight = InputHeight,
                    OutputWidth = outputSize.Width,
                    OutputHeight = outputSize.Height,
                    Quality = (uint)quality,
                };

                nint handle = 0;
                RtxVideoStatus status = externalD3D11Device == 0
                    ? _create(&options, &handle)
                    : _createWithD3D11Device(
                        &options, externalD3D11Device, &handle);
                if (status != RtxVideoStatus.Ok)
                    ThrowOnFailure(
                        status,
                        0,
                        externalD3D11Device == 0
                            ? "RTX VSR feature creation"
                            : "RTX VSR feature creation on the SDL D3D11 device");
                if (handle == 0)
                {
                    throw new RtxVideoException(
                        RtxVideoStatus.InternalError,
                        "RtxVideoBridge returned success with a null handle.");
                }
                _handle = handle;
                OutputWidth = outputSize.Width;
                OutputHeight = outputSize.Height;
                IsGpuDirect = externalD3D11Device != 0;
            }
        }

        public RtxVideoD3D11DeviceInfo GetD3D11DeviceInfo(nint device)
        {
            ThrowIfDisposed();
            RtxVideoD3D11DeviceInfo info = new()
            {
                StructSize = (uint)sizeof(RtxVideoD3D11DeviceInfo),
            };
            RtxVideoStatus status = _getD3D11DeviceInfo(device, &info);
            ThrowOnFailure(status, 0, "SDL D3D11 adapter inspection");
            return info;
        }

        public void Reconfigure(
            RtxVideoQuality quality,
            RtxVideoOutputResolution outputResolution)
        {
            ThrowIfNotCreated();
            RtxVideoOutputSize outputSize = RtxVideoOutputSize.From(outputResolution);
            RtxVideoStatus status = _reconfigure(
                _handle, outputSize.Width, outputSize.Height, (uint)quality);
            ThrowOnFailure(status, _handle, "RTX VSR reconfiguration");
            OutputWidth = outputSize.Width;
            OutputHeight = outputSize.Height;
        }

        public RtxVideoNativeFrameTiming Process(
            void* input,
            uint inputStride,
            void* output,
            uint outputStride)
        {
            ThrowIfNotCreated();
            RtxVideoStatus status = _process(
                _handle, input, inputStride, output, outputStride);
            ThrowOnFailure(status, _handle, "RTX VSR frame processing");

            RtxVideoNativeFrameTiming timing = new()
            {
                StructSize = (uint)sizeof(RtxVideoNativeFrameTiming),
            };
            status = _getLastFrameTiming(_handle, &timing);
            ThrowOnFailure(status, _handle, "RTX VSR timing retrieval");
            return timing;
        }

        public RtxVideoNativeFrameTiming ProcessGpu(void* input, uint inputStride)
        {
            ThrowIfNotCreated();
            if (!IsGpuDirect)
                throw new InvalidOperationException("The RTX Video bridge is not in GPU-direct mode.");

            RtxVideoStatus status = _processGpu(_handle, input, inputStride);
            ThrowOnFailure(status, _handle, "GPU-direct RTX VSR frame processing");
            return GetLastFrameTiming();
        }

        public RtxVideoNativeFrameTiming RenderOutputD3D11(
            in RtxVideoDirectRenderOptions renderOptions)
        {
            ThrowIfNotCreated();
            if (!IsGpuDirect)
                throw new InvalidOperationException("The RTX Video bridge is not in GPU-direct mode.");

            RtxVideoDirectRenderOptions options = renderOptions;
            options.StructSize = (uint)sizeof(RtxVideoDirectRenderOptions);
            RtxVideoStatus status = _renderOutputD3D11(_handle, &options);
            ThrowOnFailure(status, _handle, "GPU-direct RTX VSR presentation");
            return GetLastFrameTiming();
        }

        public void ReadbackOutput(void* output, uint outputStride)
        {
            ThrowIfNotCreated();
            RtxVideoStatus status = _readbackOutput(
                _handle, output, outputStride);
            ThrowOnFailure(status, _handle, "Explicit RTX VSR screenshot readback");
        }

        private RtxVideoNativeFrameTiming GetLastFrameTiming()
        {
            RtxVideoNativeFrameTiming timing = new()
            {
                StructSize = (uint)sizeof(RtxVideoNativeFrameTiming),
            };
            RtxVideoStatus status = _getLastFrameTiming(_handle, &timing);
            ThrowOnFailure(status, _handle, "RTX VSR timing retrieval");
            return timing;
        }

        private void ThrowOnFailure(RtxVideoStatus status, nint handle, string operation)
        {
            if (status == RtxVideoStatus.Ok)
                return;

            string detail = ReadLastError(handle);
            if (string.IsNullOrWhiteSpace(detail))
                detail = "The bridge did not provide additional error text.";
            throw new RtxVideoException(status, $"{operation} failed [{(int)status}]: {detail}");
        }

        private string ReadLastError(nint handle)
        {
            nuint size = 0;
            if (_getLastError(handle, null, &size) != RtxVideoStatus.Ok || size <= 1)
                return string.Empty;

            if (size > 1024 * 1024)
                return "Bridge error text exceeded the safety limit.";

            byte[] buffer = new byte[(int)size];
            fixed (byte* pointer = buffer)
            {
                RtxVideoStatus status = _getLastError(handle, pointer, &size);
                if (status != RtxVideoStatus.Ok)
                    return string.Empty;
            }

            int terminator = Array.IndexOf(buffer, (byte)0);
            if (terminator < 0)
                terminator = buffer.Length;
            return Encoding.UTF8.GetString(buffer, 0, terminator);
        }

        private void ThrowIfDisposed()
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
        }

        private void ThrowIfNotCreated()
        {
            ThrowIfDisposed();
            if (_handle == 0)
                throw new InvalidOperationException("The RTX Video bridge has not been initialized.");
        }

        public void Dispose()
        {
            if (_disposed)
                return;

            if (_handle != 0 && _destroy != null)
            {
                _destroy(_handle);
                _handle = 0;
            }
            if (_library != 0)
            {
                NativeLibrary.Free(_library);
                _library = 0;
            }
            _disposed = true;
            GC.SuppressFinalize(this);
        }
    }

    internal static class RtxVideoSupport
    {
        private static readonly object StateLock = new();
        private static bool _sessionDisabled;
        private static bool _errorLogged;
        private static string _status = "Not tested in this session";

        public static bool SessionDisabled
        {
            get { lock (StateLock) return _sessionDisabled; }
        }

        public static string Status
        {
            get { lock (StateLock) return _status; }
        }

        public static void ProbeFromSettings()
        {
            lock (StateLock)
            {
                if (_sessionDisabled)
                    return;
            }

            try
            {
                using RtxVideoNative bridge = new();
                RtxVideoCapabilities capabilities = bridge.Probe();
                string message =
                    $"Ready: NVIDIA DXGI adapter {capabilities.AdapterIndex}, " +
                    $"device 0x{capabilities.AdapterDeviceId:X4}, " +
                    $"{capabilities.OutputWidth}x{capabilities.OutputHeight}";
                MarkReady(message);
                Console.WriteLine($"[RTX VSR] {message}; feature path: {RtxVideoPaths.FeatureDirectory}");
            }
            catch (Exception error)
            {
                DisableForSession(error.Message);
            }
        }

        public static RtxVideoNative? CreateForStream(
            RtxVideoQuality quality,
            RtxVideoOutputResolution outputResolution,
            RtxVideoPresentationBackend presentationBackend,
            nint sdlRenderer)
        {
            lock (StateLock)
            {
                if (_sessionDisabled)
                    return null;
            }

            RtxVideoNative? bridge = null;
            try
            {
                bridge = new RtxVideoNative();
                if (presentationBackend ==
                    RtxVideoPresentationBackend.GpuDirectExperimental)
                {
                    nint d3d11Device = SDL_RenderGetD3D11Device(sdlRenderer);
                    if (d3d11Device == 0)
                    {
                        throw new RtxVideoException(
                            RtxVideoStatus.D3dFailed,
                            $"SDL_RenderGetD3D11Device failed: {SDL_GetError()}");
                    }

                    try
                    {
                        RtxVideoCapabilities expected = bridge.Probe();
                        RtxVideoD3D11DeviceInfo actual =
                            bridge.GetD3D11DeviceInfo(d3d11Device);
                        Console.WriteLine(
                            $"[RTX VSR] SDL D3D11 device: '{actual.AdapterName}', " +
                            $"DXGI adapter {actual.AdapterIndex}, vendor 0x{actual.AdapterVendorId:X4}, " +
                            $"device 0x{actual.AdapterDeviceId:X4}, LUID {actual.Luid}, " +
                            $"feature level 0x{actual.FeatureLevel:X}; " +
                            $"NGX probe LUID {expected.AdapterLuidHigh:X8}:{expected.AdapterLuidLow:X8}.");
                        if (actual.AdapterLuidHigh != expected.AdapterLuidHigh ||
                            actual.AdapterLuidLow != expected.AdapterLuidLow)
                        {
                            throw new RtxVideoException(
                                RtxVideoStatus.WrongAdapter,
                                "SDL and the NVIDIA RTX VSR probe selected different DXGI adapter LUIDs.");
                        }
                        bridge.Create(quality, outputResolution, d3d11Device);
                    }
                    finally
                    {
                        // SDL_RenderGetD3D11Device returns an AddRef'd interface.
                        Marshal.Release(d3d11Device);
                    }
                }
                else
                {
                    bridge.Create(quality, outputResolution);
                }
                string message =
                    $"Initialized {presentationBackend} live processing at {quality}, " +
                    $"{bridge.OutputWidth}x{bridge.OutputHeight}";
                MarkReady(message);
                Console.WriteLine($"[RTX VSR] {message}; feature path: {RtxVideoPaths.FeatureDirectory}");
                return bridge;
            }
            catch (Exception error)
            {
                bridge?.Dispose();
                if (presentationBackend ==
                    RtxVideoPresentationBackend.GpuDirectExperimental)
                {
                    MarkReady($"GPU direct unavailable; falling back: {error.Message}");
                    Console.WriteLine(
                        $"[RTX VSR] GPU-direct initialization failed: {error.Message}. " +
                        "Trying the supported CPU-readback path.");
                }
                else
                {
                    DisableForSession(error.Message);
                }
                return null;
            }
        }

        public static void ReportDirectFailure(string reason)
        {
            MarkReady($"GPU direct failed; CPU-readback fallback available: {reason}");
            Console.WriteLine(
                $"[RTX VSR] GPU-direct path stopped: {reason}. " +
                "No additional frame buffering was introduced.");
        }

        public static void DisableForSession(string reason)
        {
            lock (StateLock)
            {
                _sessionDisabled = true;
                _status = $"Disabled for this session: {reason}";
                if (!_errorLogged)
                {
                    Console.WriteLine(
                        $"[RTX VSR] {_status}. The original IYUV renderer remains active.");
                    _errorLogged = true;
                }
            }
        }

        private static void MarkReady(string message)
        {
            lock (StateLock)
            {
                if (!_sessionDisabled)
                    _status = message;
            }
        }
    }
}
