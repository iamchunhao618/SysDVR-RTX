using FFmpeg.AutoGen;
using SysDVR.Client.Core;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Runtime.InteropServices;
using static FFmpeg.AutoGen.ffmpeg;

namespace SysDVR.Client.Platform.Specific.Win.RtxVideo
{
    internal readonly record struct RtxVideoFrameTiming(
        double ConversionMilliseconds,
        double BridgeMilliseconds,
        RtxVideoNativeFrameTiming Native);

    internal unsafe sealed class RtxVideoEnhancer : IDisposable
    {
        public const int InputWidth = (int)RtxVideoNative.InputWidth;
        public const int InputHeight = (int)RtxVideoNative.InputHeight;
        public const int InputStride = InputWidth * 4;

        public int OutputWidth { get; }
        public int OutputHeight { get; }
        public int OutputStride { get; }
        public RtxVideoOutputResolution OutputResolution { get; }
        public RtxVideoPresentationBackend PresentationBackend { get; }
        public bool IsGpuDirect =>
            PresentationBackend == RtxVideoPresentationBackend.GpuDirectExperimental;
        public string? FailureReason { get; private set; }

        private readonly byte*[] _sourcePlanes = new byte*[8];
        private readonly int[] _sourceStrides = new int[8];
        private readonly byte*[] _destinationPlanes = new byte*[8];
        private readonly int[] _destinationStrides = new int[8];

        private RtxVideoNative? _bridge;
        private SwsContext* _converter;
        private byte* _inputBuffer;
        private byte* _outputBuffer;
        private RtxVideoQuality _quality;
        private int _configuredMatrix = -1;
        private int _configuredSourceRange = -1;
        private bool _disposed;
        private bool _failed;

        public nint OutputBuffer => (nint)_outputBuffer;

        private RtxVideoEnhancer(
            RtxVideoNative bridge,
            RtxVideoQuality quality,
            RtxVideoOutputResolution outputResolution,
            RtxVideoPresentationBackend presentationBackend)
        {
            _bridge = bridge;
            _quality = quality;
            OutputResolution = outputResolution;
            PresentationBackend = presentationBackend;
            OutputWidth = checked((int)bridge.OutputWidth);
            OutputHeight = checked((int)bridge.OutputHeight);
            OutputStride = checked(OutputWidth * 4);

            try
            {
                _inputBuffer = (byte*)NativeMemory.AlignedAlloc(
                    (nuint)(InputStride * InputHeight), 32);
                if (!IsGpuDirect)
                {
                    _outputBuffer = (byte*)NativeMemory.AlignedAlloc(
                        (nuint)(OutputStride * OutputHeight), 32);
                }
                if (_inputBuffer == null || (!IsGpuDirect && _outputBuffer == null))
                    throw new OutOfMemoryException("Could not allocate RTX Video RGBA frame buffers.");

                _converter = sws_getContext(
                    InputWidth, InputHeight, AVPixelFormat.AV_PIX_FMT_YUV420P,
                    InputWidth, InputHeight, AVPixelFormat.AV_PIX_FMT_RGBA,
                    SWS_BILINEAR, null, null, null);
                if (_converter == null)
                    throw new InvalidOperationException("FFmpeg sws_getContext(YUV420P to RGBA) failed.");

                _destinationPlanes[0] = _inputBuffer;
                _destinationStrides[0] = InputStride;
            }
            catch
            {
                Dispose();
                throw;
            }
        }

        public static RtxVideoEnhancer? TryCreate(
            RtxVideoQuality quality,
            RtxVideoOutputResolution outputResolution,
            RtxVideoPresentationBackend presentationBackend,
            nint sdlRenderer)
        {
            RtxVideoNative? bridge = RtxVideoSupport.CreateForStream(
                quality, outputResolution, presentationBackend, sdlRenderer);
            if (bridge is null)
                return null;

            try
            {
                return new RtxVideoEnhancer(
                    bridge, quality, outputResolution, presentationBackend);
            }
            catch (Exception error)
            {
                bridge.Dispose();
                if (presentationBackend ==
                    RtxVideoPresentationBackend.GpuDirectExperimental)
                    RtxVideoSupport.ReportDirectFailure(error.Message);
                else
                    RtxVideoSupport.DisableForSession(
                        $"YUV420P to RGBA converter initialization failed: {error.Message}");
                return null;
            }
        }

        public bool TryProcess(
            AVFrame* frame,
            RtxVideoQuality quality,
            out RtxVideoFrameTiming timing)
        {
            timing = default;
            if (_disposed || _failed || _bridge is null)
                return false;

            try
            {
                ValidateFrame(frame);
                if (quality != _quality)
                {
                    _bridge.Reconfigure(quality, OutputResolution);
                    _quality = quality;
                }

                ConfigureColorConversion(frame);
                _sourcePlanes[0] = frame->data[0];
                _sourcePlanes[1] = frame->data[1];
                _sourcePlanes[2] = frame->data[2];
                _sourceStrides[0] = frame->linesize[0];
                _sourceStrides[1] = frame->linesize[1];
                _sourceStrides[2] = frame->linesize[2];

                long conversionStart = Stopwatch.GetTimestamp();
                int convertedRows = sws_scale(
                    _converter,
                    _sourcePlanes,
                    _sourceStrides,
                    0,
                    InputHeight,
                    _destinationPlanes,
                    _destinationStrides);
                double conversionMilliseconds = Stopwatch.GetElapsedTime(
                    conversionStart).TotalMilliseconds;
                if (convertedRows != InputHeight)
                {
                    throw new InvalidOperationException(
                        $"FFmpeg sws_scale converted {convertedRows} rows instead of {InputHeight}.");
                }

                long bridgeStart = Stopwatch.GetTimestamp();
                RtxVideoNativeFrameTiming nativeTiming = IsGpuDirect
                    ? _bridge.ProcessGpu(_inputBuffer, InputStride)
                    : _bridge.Process(
                        _inputBuffer, InputStride, _outputBuffer,
                        checked((uint)OutputStride));
                double bridgeMilliseconds = Stopwatch.GetElapsedTime(
                    bridgeStart).TotalMilliseconds;

                timing = new RtxVideoFrameTiming(
                    conversionMilliseconds,
                    bridgeMilliseconds,
                    nativeTiming);
                return true;
            }
            catch (Exception error)
            {
                Fail(error);
                return false;
            }
        }

        public bool TryRenderGpuDirect(
            in RtxVideoDirectRenderOptions options,
            out RtxVideoNativeFrameTiming timing)
        {
            timing = default;
            if (!IsGpuDirect || _disposed || _failed || _bridge is null)
                return false;

            try
            {
                timing = _bridge.RenderOutputD3D11(options);
                return true;
            }
            catch (Exception error)
            {
                Fail(error);
                return false;
            }
        }

        public nint ReadbackForScreenshot()
        {
            if (_disposed || _failed || _bridge is null)
                throw new InvalidOperationException(
                    "RTX VSR output is unavailable for screenshot capture.");
            if (_outputBuffer == null)
            {
                _outputBuffer = (byte*)NativeMemory.AlignedAlloc(
                    (nuint)(OutputStride * OutputHeight), 32);
                if (_outputBuffer == null)
                    throw new OutOfMemoryException(
                        "Could not allocate the explicit RTX VSR screenshot buffer.");
            }
            if (IsGpuDirect)
                _bridge.ReadbackOutput(_outputBuffer, checked((uint)OutputStride));
            return (nint)_outputBuffer;
        }

        private void Fail(Exception error)
        {
            FailureReason = error.Message;
            _failed = true;
            _bridge?.Dispose();
            _bridge = null;
            if (IsGpuDirect)
                RtxVideoSupport.ReportDirectFailure(error.Message);
            else
                RtxVideoSupport.DisableForSession(error.Message);
        }

        private static void ValidateFrame(AVFrame* frame)
        {
            if (frame == null)
                throw new ArgumentNullException(nameof(frame));
            if (frame->width != InputWidth || frame->height != InputHeight)
            {
                throw new InvalidOperationException(
                    $"RTX VSR requires a {InputWidth}x{InputHeight} frame, got {frame->width}x{frame->height}.");
            }
            if ((AVPixelFormat)frame->format != AVPixelFormat.AV_PIX_FMT_YUV420P)
            {
                throw new InvalidOperationException(
                    $"RTX VSR requires FFmpeg YUV420P input, got {(AVPixelFormat)frame->format}.");
            }
            if (frame->data[0] == null || frame->data[1] == null || frame->data[2] == null)
                throw new InvalidOperationException("Decoded YUV420P frame contains a null plane.");
        }

        private void ConfigureColorConversion(AVFrame* frame)
        {
            int sourceRange;
            string rangeDescription;
            switch (frame->color_range)
            {
                case AVColorRange.AVCOL_RANGE_JPEG:
                    sourceRange = 1;
                    rangeDescription = "full range (frame metadata)";
                    break;
                case AVColorRange.AVCOL_RANGE_MPEG:
                    sourceRange = 0;
                    rangeDescription = "limited range (frame metadata)";
                    break;
                default:
                    sourceRange = 0;
                    rangeDescription = "limited range (Switch fallback; metadata absent)";
                    break;
            }

            int matrix;
            string matrixDescription;
            switch (frame->colorspace)
            {
                case AVColorSpace.AVCOL_SPC_BT709:
                    matrix = SWS_CS_ITU709;
                    matrixDescription = "BT.709 (frame metadata)";
                    break;
                case AVColorSpace.AVCOL_SPC_BT470BG:
                case AVColorSpace.AVCOL_SPC_SMPTE170M:
                    matrix = SWS_CS_ITU601;
                    matrixDescription = "BT.601 (frame metadata)";
                    break;
                case AVColorSpace.AVCOL_SPC_FCC:
                    matrix = SWS_CS_FCC;
                    matrixDescription = "FCC (frame metadata)";
                    break;
                default:
                    matrix = SWS_CS_ITU709;
                    matrixDescription = "BT.709 (Switch HD fallback; metadata absent/ambiguous)";
                    break;
            }

            if (matrix == _configuredMatrix && sourceRange == _configuredSourceRange)
                return;

            int* coefficientPointer = sws_getCoefficients(matrix);
            if (coefficientPointer == null)
                throw new InvalidOperationException("FFmpeg sws_getCoefficients returned null.");

            int_array4 coefficients = *(int_array4*)coefficientPointer;
            int result = sws_setColorspaceDetails(
                _converter,
                in coefficients,
                sourceRange,
                in coefficients,
                1,
                0,
                1 << 16,
                1 << 16);
            if (result < 0)
            {
                throw new InvalidOperationException(
                    $"FFmpeg sws_setColorspaceDetails failed with code {result}.");
            }

            _configuredMatrix = matrix;
            _configuredSourceRange = sourceRange;
            Console.WriteLine(
                $"[RTX VSR] YUV420P -> RGBA8: {matrixDescription}, {rangeDescription}; " +
                "RGBA output is full range with opaque alpha; SDR transfer is preserved.");
        }

        public void Dispose()
        {
            if (_disposed)
                return;

            _bridge?.Dispose();
            _bridge = null;

            if (_converter != null)
            {
                sws_freeContext(_converter);
                _converter = null;
            }
            if (_outputBuffer != null)
            {
                NativeMemory.AlignedFree(_outputBuffer);
                _outputBuffer = null;
            }
            if (_inputBuffer != null)
            {
                NativeMemory.AlignedFree(_inputBuffer);
                _inputBuffer = null;
            }

            _disposed = true;
            GC.SuppressFinalize(this);
        }
    }

    internal sealed class RtxVideoTelemetry
    {
        private const int WarmupFrames = 20;
        private const int ReportingInterval = 120;

        private sealed class PendingFrame
        {
            public required RtxVideoQuality Quality { get; init; }
            public required RtxVideoOutputResolution Resolution { get; init; }
            public required RtxVideoPostProcessAa PostAa { get; init; }
            public required RtxVideoFrameTiming Timing { get; set; }
            public required double DecodeReceiveMilliseconds { get; init; }
            public required double SdlUploadMilliseconds { get; init; }
            public required double EnhancementMilliseconds { get; set; }
            public required long FrameAvailableTimestamp { get; init; }
            public double VideoSubmitMilliseconds { get; set; } = double.NaN;
        }

        private readonly List<double> _decodeReceive = new(ReportingInterval);
        private readonly List<double> _conversion = new(ReportingInterval);
        private readonly List<double> _bridgeHost = new(ReportingInterval);
        private readonly List<double> _uploadSubmitCpu = new(ReportingInterval);
        private readonly List<double> _uploadGpu = new(ReportingInterval);
        private readonly List<double> _evaluateCallCpu = new(ReportingInterval);
        private readonly List<double> _nativeProcess = new(ReportingInterval);
        private readonly List<double> _vsrGpu = new(ReportingInterval);
        private readonly List<double> _copySubmitCpu = new(ReportingInterval);
        private readonly List<double> _copyGpu = new(ReportingInterval);
        private readonly List<double> _totalGpu = new(ReportingInterval);
        private readonly List<double> _readback = new(ReportingInterval);
        private readonly List<double> _mapWait = new(ReportingInterval);
        private readonly List<double> _rowCopy = new(ReportingInterval);
        private readonly List<double> _queryResolve = new(ReportingInterval);
        private readonly List<double> _directDrawSubmitCpu = new(ReportingInterval);
        private readonly List<double> _directDrawGpu = new(ReportingInterval);
        private readonly List<double> _fxaaGpu = new(ReportingInterval);
        private readonly List<double> _smaaEdgeGpu = new(ReportingInterval);
        private readonly List<double> _smaaBlendGpu = new(ReportingInterval);
        private readonly List<double> _smaaNeighborhoodGpu = new(ReportingInterval);
        private readonly List<double> _postAaGpu = new(ReportingInterval);
        private readonly List<double> _cpuBlockingWait = new(ReportingInterval);
        private readonly List<double> _gpuTimingLatencyFrames = new(ReportingInterval);
        private readonly List<double> _sdlUpload = new(ReportingInterval);
        private readonly List<double> _enhancement = new(ReportingInterval);
        private readonly List<double> _videoSubmit = new(ReportingInterval);
        private readonly List<double> _uiBuild = new(ReportingInterval);
        private readonly List<double> _uiSubmit = new(ReportingInterval);
        private readonly List<double> _present = new(ReportingInterval);
        private readonly List<double> _decodedToPresent = new(ReportingInterval);
        private readonly List<double> _frameInterval = new(ReportingInterval);
        private RtxVideoQuality? _activeQuality;
        private RtxVideoOutputResolution? _activeResolution;
        private RtxVideoPostProcessAa? _activePostAa;
        private PendingFrame? _pending;
        private long _lastPresentedTimestamp;
        private int _warmupRemaining;

        public void BeginFrame(
            RtxVideoQuality quality,
            RtxVideoOutputResolution resolution,
            RtxVideoPostProcessAa postAa,
            RtxVideoFrameTiming frame,
            double decodeReceiveMilliseconds,
            double sdlUploadMilliseconds,
            double enhancementMilliseconds,
            long frameAvailableTimestamp)
        {
            bool configurationChanged = !_activeQuality.HasValue ||
                _activeQuality.Value != quality ||
                _activeResolution != resolution ||
                _activePostAa != postAa;
            if (configurationChanged)
            {
                if (_activeQuality.HasValue && _conversion.Count != 0)
                    Log(
                        _activeQuality.Value,
                        _activeResolution!.Value,
                        _activePostAa ?? RtxVideoPostProcessAa.Off,
                        true);
                ResetSamples();
                _warmupRemaining = WarmupFrames;
            }
            _activeQuality = quality;
            _activeResolution = resolution;
            _activePostAa = postAa;

            _pending = new PendingFrame
            {
                Quality = quality,
                Resolution = resolution,
                PostAa = postAa,
                Timing = frame,
                DecodeReceiveMilliseconds = decodeReceiveMilliseconds,
                SdlUploadMilliseconds = sdlUploadMilliseconds,
                EnhancementMilliseconds = enhancementMilliseconds,
                FrameAvailableTimestamp = frameAvailableTimestamp,
            };
        }

        public void RecordVideoSubmission(
            double milliseconds,
            RtxVideoNativeFrameTiming? updatedNativeTiming = null,
            double? enhancementMilliseconds = null)
        {
            if (_pending is not null)
            {
                _pending.VideoSubmitMilliseconds = milliseconds;
                if (updatedNativeTiming.HasValue)
                {
                    _pending.Timing = _pending.Timing with
                    {
                        Native = updatedNativeTiming.Value,
                    };
                }
                if (enhancementMilliseconds.HasValue)
                    _pending.EnhancementMilliseconds = enhancementMilliseconds.Value;
            }
        }

        public void CompleteFrame(
            double uiBuildMilliseconds,
            double uiSubmitMilliseconds,
            double presentMilliseconds)
        {
            PendingFrame? pending = _pending;
            if (pending is null)
                return;
            _pending = null;

            long presentedTimestamp = Stopwatch.GetTimestamp();
            if (_warmupRemaining != 0)
            {
                --_warmupRemaining;
                _lastPresentedTimestamp = 0;
                return;
            }

            RtxVideoNativeFrameTiming native = pending.Timing.Native;
            bool gpuTimingValid = native.GpuTimingValid != 0;

            _decodeReceive.Add(pending.DecodeReceiveMilliseconds);
            _conversion.Add(pending.Timing.ConversionMilliseconds);
            _bridgeHost.Add(pending.Timing.BridgeMilliseconds);
            _uploadSubmitCpu.Add(native.UploadSubmitCpuMilliseconds);
            _uploadGpu.Add(gpuTimingValid ? native.UploadGpuMilliseconds : double.NaN);
            _evaluateCallCpu.Add(native.EvaluateCallCpuMilliseconds);
            _vsrGpu.Add(gpuTimingValid ? native.EvaluateGpuMilliseconds : double.NaN);
            _copySubmitCpu.Add(native.CopySubmitCpuMilliseconds);
            _copyGpu.Add(gpuTimingValid ? native.ReadbackCopyGpuMilliseconds : double.NaN);
            _totalGpu.Add(gpuTimingValid ? native.TotalGpuMilliseconds : double.NaN);
            _nativeProcess.Add(native.ProcessCpuMilliseconds);
            _readback.Add(native.ReadbackCpuMilliseconds);
            _mapWait.Add(native.MapWaitCpuMilliseconds);
            _rowCopy.Add(native.RowCopyCpuMilliseconds);
            _queryResolve.Add(native.QueryResolveCpuMilliseconds);
            _directDrawSubmitCpu.Add(native.DirectDrawSubmitCpuMilliseconds);
            _directDrawGpu.Add(
                gpuTimingValid ? native.DirectDrawGpuMilliseconds : double.NaN);
            _fxaaGpu.Add(gpuTimingValid ? native.FxaaGpuMilliseconds : double.NaN);
            _smaaEdgeGpu.Add(gpuTimingValid ? native.SmaaEdgeGpuMilliseconds : double.NaN);
            _smaaBlendGpu.Add(gpuTimingValid ? native.SmaaBlendGpuMilliseconds : double.NaN);
            _smaaNeighborhoodGpu.Add(
                gpuTimingValid ? native.SmaaNeighborhoodGpuMilliseconds : double.NaN);
            _postAaGpu.Add(gpuTimingValid ? native.PostAaGpuMilliseconds : double.NaN);
            _cpuBlockingWait.Add(native.CpuBlockingWaitMilliseconds);
            _gpuTimingLatencyFrames.Add(
                gpuTimingValid ? native.GpuTimingLatencyFrames : double.NaN);
            _sdlUpload.Add(pending.SdlUploadMilliseconds);
            _enhancement.Add(pending.EnhancementMilliseconds);
            _videoSubmit.Add(pending.VideoSubmitMilliseconds);
            _uiBuild.Add(uiBuildMilliseconds);
            _uiSubmit.Add(uiSubmitMilliseconds);
            _present.Add(presentMilliseconds);
            _decodedToPresent.Add(
                Stopwatch.GetElapsedTime(
                    pending.FrameAvailableTimestamp,
                    presentedTimestamp).TotalMilliseconds);
            _frameInterval.Add(
                _lastPresentedTimestamp == 0
                    ? double.NaN
                    : Stopwatch.GetElapsedTime(
                        _lastPresentedTimestamp,
                        presentedTimestamp).TotalMilliseconds);
            _lastPresentedTimestamp = presentedTimestamp;

            if (_conversion.Count == ReportingInterval)
            {
                Log(pending.Quality, pending.Resolution, pending.PostAa, false);
                ResetSamples();
            }
        }

        public void Flush(
            RtxVideoQuality quality,
            RtxVideoOutputResolution resolution)
        {
            _pending = null;
            if (_conversion.Count != 0)
            {
                Log(
                    _activeQuality ?? quality,
                    _activeResolution ?? resolution,
                    _activePostAa ?? RtxVideoPostProcessAa.Off,
                    true);
                ResetSamples();
                _activeQuality = null;
                _activeResolution = null;
                _activePostAa = null;
            }
            _lastPresentedTimestamp = 0;
        }

        private void Log(
            RtxVideoQuality quality,
            RtxVideoOutputResolution resolution,
            RtxVideoPostProcessAa postAa,
            bool final)
        {
            string kind = final ? "final" : "interval";
            RtxVideoOutputSize size = RtxVideoOutputSize.From(resolution);
            Console.WriteLine(
                $"[RTX VSR timing/{kind}] output={size.Width}x{size.Height} " +
                $"quality={quality} warmup={WarmupFrames} frames={_conversion.Count}; " +
                $"post_aa={postAa} " +
                $"decode_receive[{Describe(_decodeReceive)}] " +
                $"convert[{Describe(_conversion)}] " +
                $"bridge_host[{Describe(_bridgeHost)}] " +
                $"upload_submit_cpu[{Describe(_uploadSubmitCpu)}] " +
                $"upload_gpu[{Describe(_uploadGpu)}] " +
                $"vsr_submit_cpu[{Describe(_evaluateCallCpu)}] " +
                $"vsr_gpu[{Describe(_vsrGpu)}] " +
                $"staging_copy_submit_cpu[{Describe(_copySubmitCpu)}] " +
                $"staging_copy_gpu[{Describe(_copyGpu)}] " +
                $"native_gpu[{Describe(_totalGpu)}] " +
                $"native_process[{Describe(_nativeProcess)}] " +
                $"readback[{Describe(_readback)}] " +
                $"map_wait[{Describe(_mapWait)}] " +
                $"row_copy[{Describe(_rowCopy)}] " +
                $"query_resolve[{Describe(_queryResolve)}] " +
                $"direct_draw_submit_cpu[{Describe(_directDrawSubmitCpu)}] " +
                $"direct_draw_gpu[{Describe(_directDrawGpu)}] " +
                $"fxaa_gpu[{Describe(_fxaaGpu)}] " +
                $"smaa_edge_gpu[{Describe(_smaaEdgeGpu)}] " +
                $"smaa_blend_gpu[{Describe(_smaaBlendGpu)}] " +
                $"smaa_neighborhood_gpu[{Describe(_smaaNeighborhoodGpu)}] " +
                $"post_aa_gpu[{Describe(_postAaGpu)}] " +
                $"cpu_blocking_wait[{Describe(_cpuBlockingWait)}] " +
                $"gpu_timing_latency_frames[{Describe(_gpuTimingLatencyFrames)}] " +
                $"sdl_upload[{Describe(_sdlUpload)}] " +
                $"enhancement[{Describe(_enhancement)}] " +
                $"sdl_video_submit[{Describe(_videoSubmit)}] " +
                $"ui_build[{Describe(_uiBuild)}] " +
                $"ui_submit[{Describe(_uiSubmit)}] " +
                $"present[{Describe(_present)}] " +
                $"decoded_to_present[{Describe(_decodedToPresent)}] " +
                $"frame_interval[{Describe(_frameInterval)}]");
        }

        private static string Describe(List<double> samples)
        {
            double[] sorted = samples.Where(double.IsFinite).OrderBy(value => value).ToArray();
            if (sorted.Length == 0)
                return "unavailable";

            double average = sorted.Average();
            double median = Percentile(sorted, 0.50);
            double p95 = Percentile(sorted, 0.95);
            return $"avg={average:F3} median={median:F3} p95={p95:F3} max={sorted[^1]:F3}";
        }

        private static double Percentile(double[] sorted, double percentile)
        {
            if (sorted.Length == 1)
                return sorted[0];

            double position = percentile * (sorted.Length - 1);
            int lower = (int)Math.Floor(position);
            int upper = (int)Math.Ceiling(position);
            if (lower == upper)
                return sorted[lower];
            double fraction = position - lower;
            return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
        }

        private void ResetSamples()
        {
            _decodeReceive.Clear();
            _conversion.Clear();
            _bridgeHost.Clear();
            _uploadSubmitCpu.Clear();
            _uploadGpu.Clear();
            _evaluateCallCpu.Clear();
            _nativeProcess.Clear();
            _vsrGpu.Clear();
            _copySubmitCpu.Clear();
            _copyGpu.Clear();
            _totalGpu.Clear();
            _readback.Clear();
            _mapWait.Clear();
            _rowCopy.Clear();
            _queryResolve.Clear();
            _directDrawSubmitCpu.Clear();
            _directDrawGpu.Clear();
            _fxaaGpu.Clear();
            _smaaEdgeGpu.Clear();
            _smaaBlendGpu.Clear();
            _smaaNeighborhoodGpu.Clear();
            _postAaGpu.Clear();
            _cpuBlockingWait.Clear();
            _gpuTimingLatencyFrames.Clear();
            _sdlUpload.Clear();
            _enhancement.Clear();
            _videoSubmit.Clear();
            _uiBuild.Clear();
            _uiSubmit.Clear();
            _present.Clear();
            _decodedToPresent.Clear();
            _frameInterval.Clear();
        }
    }
}
