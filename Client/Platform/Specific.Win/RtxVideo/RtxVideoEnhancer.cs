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
            RtxVideoOutputResolution outputResolution)
        {
            _bridge = bridge;
            _quality = quality;
            OutputResolution = outputResolution;
            OutputWidth = checked((int)bridge.OutputWidth);
            OutputHeight = checked((int)bridge.OutputHeight);
            OutputStride = checked(OutputWidth * 4);

            try
            {
                _inputBuffer = (byte*)NativeMemory.AlignedAlloc(
                    (nuint)(InputStride * InputHeight), 32);
                _outputBuffer = (byte*)NativeMemory.AlignedAlloc(
                    (nuint)(OutputStride * OutputHeight), 32);
                if (_inputBuffer == null || _outputBuffer == null)
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
            RtxVideoOutputResolution outputResolution)
        {
            RtxVideoNative? bridge = RtxVideoSupport.CreateForStream(
                quality, outputResolution);
            if (bridge is null)
                return null;

            try
            {
                return new RtxVideoEnhancer(bridge, quality, outputResolution);
            }
            catch (Exception error)
            {
                bridge.Dispose();
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
                RtxVideoNativeFrameTiming nativeTiming = _bridge.Process(
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
                _failed = true;
                _bridge.Dispose();
                _bridge = null;
                RtxVideoSupport.DisableForSession(error.Message);
                return false;
            }
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
        private const int ReportingInterval = 120;

        private readonly List<double> _conversion = new(ReportingInterval);
        private readonly List<double> _nativeProcess = new(ReportingInterval);
        private readonly List<double> _vsrGpu = new(ReportingInterval);
        private readonly List<double> _readback = new(ReportingInterval);
        private readonly List<double> _mapWait = new(ReportingInterval);
        private readonly List<double> _rowCopy = new(ReportingInterval);
        private readonly List<double> _sdlUpload = new(ReportingInterval);
        private readonly List<double> _total = new(ReportingInterval);
        private RtxVideoQuality? _activeQuality;
        private RtxVideoOutputResolution? _activeResolution;

        public void Record(
            RtxVideoQuality quality,
            RtxVideoOutputResolution resolution,
            RtxVideoFrameTiming frame,
            double uploadMilliseconds,
            double pathMilliseconds)
        {
            if (_activeQuality.HasValue &&
                (_activeQuality.Value != quality || _activeResolution != resolution))
            {
                Log(_activeQuality.Value, _activeResolution!.Value, true);
                ResetSamples();
            }
            _activeQuality = quality;
            _activeResolution = resolution;

            _conversion.Add(frame.ConversionMilliseconds);
            _nativeProcess.Add(frame.Native.ProcessCpuMilliseconds);
            _vsrGpu.Add(
                frame.Native.GpuTimingValid != 0
                    ? frame.Native.EvaluateGpuMilliseconds
                    : double.NaN);
            _readback.Add(frame.Native.ReadbackCpuMilliseconds);
            _mapWait.Add(frame.Native.MapWaitCpuMilliseconds);
            _rowCopy.Add(frame.Native.RowCopyCpuMilliseconds);
            _sdlUpload.Add(uploadMilliseconds);
            _total.Add(pathMilliseconds);

            if (_conversion.Count == ReportingInterval)
            {
                Log(quality, resolution, false);
                ResetSamples();
            }
        }

        public void Flush(
            RtxVideoQuality quality,
            RtxVideoOutputResolution resolution)
        {
            if (_conversion.Count != 0)
            {
                Log(
                    _activeQuality ?? quality,
                    _activeResolution ?? resolution,
                    true);
                ResetSamples();
                _activeQuality = null;
                _activeResolution = null;
            }
        }

        private void Log(
            RtxVideoQuality quality,
            RtxVideoOutputResolution resolution,
            bool final)
        {
            string kind = final ? "final" : "interval";
            RtxVideoOutputSize size = RtxVideoOutputSize.From(resolution);
            Console.WriteLine(
                $"[RTX VSR timing/{kind}] output={size.Width}x{size.Height} " +
                $"quality={quality} frames={_conversion.Count}; " +
                $"convert[{Describe(_conversion)}] " +
                $"vsr_gpu[{Describe(_vsrGpu)}] " +
                $"native_process[{Describe(_nativeProcess)}] " +
                $"readback[{Describe(_readback)}] " +
                $"map_wait[{Describe(_mapWait)}] " +
                $"row_copy[{Describe(_rowCopy)}] " +
                $"sdl_upload[{Describe(_sdlUpload)}] " +
                $"total[{Describe(_total)}]");
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
            _conversion.Clear();
            _nativeProcess.Clear();
            _vsrGpu.Clear();
            _readback.Clear();
            _mapWait.Clear();
            _rowCopy.Clear();
            _sdlUpload.Clear();
            _total.Clear();
        }
    }
}
