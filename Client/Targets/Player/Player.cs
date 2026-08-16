using FFmpeg.AutoGen;
using SysDVR.Client.Core;
using SysDVR.Client.Platform.Specific.Win.RtxVideo;
using SysDVR.Client.Sources;
using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using static FFmpeg.AutoGen.ffmpeg;
using static SDL2.SDL;

namespace SysDVR.Client.Targets.Player
{
    unsafe class DecoderContext
    {
        public AVCodecContext* CodecCtx { get; init; }

        public AVFrame* RenderFrame;
        public AVFrame* ReceiveFrame;

        public AVFrame* Frame1 { get; init; }
        public AVFrame* Frame2 { get; init; }

        public object CodecLock { get; init; }

        public StreamSynchronizationHelper SyncHelper;
        public AutoResetEvent OnFrameEvent;
    }

    unsafe struct FormatConverterContext
    {
        public SwsContext* Converter { get; init; }
        public AVFrame* Frame { get; init; }
    }

    // Guarantees that the audio and video stream are compatible with the player
    class PlayerManager : StreamManager
    {
        internal new DecodeH264Target VideoTarget;
        internal new BaseAudioPlayerTarget AudioTarget;

        readonly public bool IsCompatibleAudioStream;

        static OutStream? MakeAudioStream(bool hasAudio) 
        {
            if (!hasAudio)
                return null;

            var useCompat = Program.Options.AudioPlayerMode switch
            {
                SDLAudioMode.Compatible => true,
                SDLAudioMode.Default => false,
                // Currently only mac os seems to need the compatible mode by default
                _ => RuntimeInformation.IsOSPlatform(OSPlatform.OSX)
            };

            if (useCompat)
                return new QueuedStreamAudioTarget();
            else
                return new AudioPlayerTarget(); 
        }

        static OutStream? MakeVideoStream(bool hasVideo)
        {
            if (!hasVideo)
                return null;

            return new DecodeH264Target();
        }

        public void UseSyncManager(StreamSynchronizationHelper manager)
        {
            if (AudioTarget is AudioPlayerTarget)
            {
                ((AudioPlayerTarget)AudioTarget).SyncHelper = manager;
            }
        }

        public PlayerManager(StreamingSource source, CancellationTokenSource cancel) :
            base(source, MakeVideoStream(source.Options.HasVideo), MakeAudioStream(source.Options.HasAudio), cancel)
        {
            VideoTarget = base.VideoTarget as DecodeH264Target;
            AudioTarget = base.AudioTarget as BaseAudioPlayerTarget;
            IsCompatibleAudioStream = AudioTarget is QueuedStreamAudioTarget;
        }
    }

    class AudioPlayer : IDisposable
    {
        readonly uint DeviceID;
        
        // Are we using the MacOS strategy ?
        readonly bool IsCompatiblePlayer;
        
        // Keep a reference to the callback to prevent GC from collecting it
        SDL_AudioCallback? CallbackDelegate;

        // Manually pin the Target object so it can be used as opaque pointer for the native code
        GCHandle TargetHandle;

        public const ushort AudioFormat = AUDIO_S16LSB;

        public AudioPlayer(BaseAudioPlayerTarget target) 
        {
            Program.SdlCtx.BugCheckThreadId();

            IsCompatiblePlayer = target is QueuedStreamAudioTarget;

            SDL_AudioSpec wantedSpec = new SDL_AudioSpec()
            {
                channels = StreamInfo.AudioChannels,
                format = AudioFormat,
                freq = StreamInfo.AudioSampleRate,
                // StreamInfo.MinAudioSamplesPerPayload * 2 was the default until sysdvr 5.4
                // however SDL will pick its preferred buffer size since we pass SDL_AUDIO_ALLOW_SAMPLES_CHANGE,
                // this is fine since we have our own buffering.
                samples = StreamInfo.MinAudioSamplesPerPayload,
            };

            if (!IsCompatiblePlayer)
            {
                TargetHandle = GCHandle.Alloc(target, GCHandleType.Normal);
                CallbackDelegate = AudioStreamTargetNative.SDLCallback;
                wantedSpec.callback = CallbackDelegate;
                wantedSpec.userdata = GCHandle.ToIntPtr(TargetHandle);
            }

            DeviceID = SDL_OpenAudioDevice(IntPtr.Zero, 0, ref wantedSpec, out var obtained, (int)SDL_AUDIO_ALLOW_SAMPLES_CHANGE);

            DeviceID.AssertNotZero(SDL_GetError);

			Program.DebugLog($"SDL_Audio: requested samples per callback={wantedSpec.samples} obtained={obtained.samples}");

            if (IsCompatiblePlayer)
            {
                ((QueuedStreamAudioTarget)(target)).DeviceID = DeviceID;
            }
        }

        public void Pause() 
        {
            SDL_PauseAudioDevice(DeviceID, 1);
        }

        public void Resume() 
        {
            SDL_PauseAudioDevice(DeviceID, 0);
        }

        public void Dispose()
        {
            Pause();
            SDL_CloseAudioDevice(DeviceID);
            
            if (TargetHandle.IsAllocated)
                TargetHandle.Free();
        }
    }

    class VideoPlayer : IDisposable
    {
        public const AVPixelFormat TargetDecodingFormat = AVPixelFormat.AV_PIX_FMT_YUV420P;
        public readonly uint TargetTextureFormat = SDL_PIXELFORMAT_IYUV;

        public DecoderContext Decoder { get; private set; }
        FormatConverterContext Converter; // Initialized only when the decoder output format doesn't match the SDL texture format

        public string DecoderName { get; private set; }
        public bool UsingCustomDecoder { get; private set; }

        public object TextureLock;
        public IntPtr TargetTexture;
        public SDL_Rect TargetTextureSize;

        IntPtr IYUVTexture;
        SDL_Rect IYUVTextureSize;
        IntPtr RtxTexture;
        SDL_Rect RtxTextureSize;
        RtxVideoEnhancer? RtxEnhancer;
        readonly RtxVideoTelemetry RtxTelemetry = new();
        RtxVideoQuality LastRtxQuality = RtxVideoQuality.Medium;
        RtxVideoOutputResolution LastRtxResolution =
            RtxVideoOutputResolution.QuadHd1440p;
        readonly string? ComparisonCaptureDirectory =
            Environment.GetEnvironmentVariable("SYSDVR_RTX_CAPTURE_DIR");
        int ComparisonCaptureCountdown = 60;
        bool ComparisonCaptured;

        public VideoPlayer(string? preferredDecoderName)
        {
            InitVideoDecoder(preferredDecoderName);
            InitSDLRenderTexture();
        }

        void InitSDLRenderTexture()
        {
            Program.SdlCtx.BugCheckThreadId();

            var tex = SDL_CreateTexture(Program.SdlCtx.RendererHandle, TargetTextureFormat,
                (int)SDL_TextureAccess.SDL_TEXTUREACCESS_STREAMING,
                StreamInfo.VideoWidth, StreamInfo.VideoHeight).AssertNotNull(SDL_GetError);

            if (Program.Options.Debug.Log)
            {
                var pixfmt = SDL_QueryTexture(tex, out var format, out var a, out var w, out var h);
                Console.WriteLine($"SDL texture info: f = {SDL_GetPixelFormatName(format)} a = {a} w = {w} h = {h}");

                SDL_RendererInfo info;
                SDL_GetRendererInfo(Program.SdlCtx.RendererHandle, out info);
                for (int i = 0; i < info.num_texture_formats; i++) unsafe
                    {
                        Console.WriteLine($"Renderer supports pixel format {SDL_GetPixelFormatName(info.texture_formats[i])}");
                    }
            }

            IYUVTextureSize = new SDL_Rect() { x = 0, y = 0, w = StreamInfo.VideoWidth, h = StreamInfo.VideoHeight };
            IYUVTexture = tex;
            UseIYUVTexture();
            TextureLock = new object();
        }

        void UseIYUVTexture()
        {
            TargetTexture = IYUVTexture;
            TargetTextureSize = IYUVTextureSize;
        }

        void UseRtxTexture()
        {
            TargetTexture = RtxTexture;
            TargetTextureSize = RtxTextureSize;
        }

        unsafe void InitVideoDecoder(string? name)
        {
            AVCodec* codec = null;

            if (name is not null)
            {
                codec = avcodec_find_decoder_by_name(name);
                
                if (codec != null)
                    UsingCustomDecoder = true;
            }

            if (codec == null)
                codec = avcodec_find_decoder(AVCodecID.AV_CODEC_ID_H264);

            if (codec == null)
                throw new Exception("Couldn't find any compatible video codecs");

            Decoder = CreateDecoderContext(codec);
            DecoderName = Marshal.PtrToStringAnsi((IntPtr)codec->name);
        }

        static unsafe DecoderContext CreateDecoderContext(AVCodec* codec)
        {
            if (codec == null)
                throw new Exception("Codec can't be null");

            string codecName = Marshal.PtrToStringAnsi((IntPtr)codec->name);

            Console.WriteLine(string.Format(Program.Strings.Player.PlayerInitializationMessage, codecName));

            var codectx = avcodec_alloc_context3(codec);
            if (codectx == null)
                throw new Exception("Couldn't allocate a codec context");

            // These are set in ffplay
            codectx->codec_id = codec->id;
            codectx->codec_type = AVMediaType.AVMEDIA_TYPE_VIDEO;
            codectx->bit_rate = 0;

            // Some decoders break without this
            codectx->width = StreamInfo.VideoWidth;
            codectx->height = StreamInfo.VideoHeight;

            var (ex, sz) = LibavUtils.AllocateH264Extradata();
            codectx->extradata_size = sz;
            codectx->extradata = (byte*)ex.ToPointer();

            avcodec_open2(codectx, codec, null).AssertZero("Couldn't open the codec.");

            var pic = av_frame_alloc();
            if (pic == null)
                throw new Exception("Couldn't allocate the decoding frame");

            var pic2 = av_frame_alloc();
            if (pic2 == null)
                throw new Exception("Couldn't allocate the decoding frame");

            return new DecoderContext()
            {
                CodecCtx = codectx,
                Frame1 = pic,
                Frame2 = pic2,
                ReceiveFrame = pic,
                RenderFrame = pic2,
                CodecLock = new object(),
                OnFrameEvent = new AutoResetEvent(true)
            };
        }

        public unsafe bool DecodeFrame() 
        {
            if (DecodeFrameInternal())
            {
                // TODO: this call is needed only with opengl on linux (and not on every linux install i tested) where TextureUpdate must be called by the main thread,
                // Check if are there any performance improvements by moving this to the decoder thread on other OSes
                if (Program.IsWindows && Program.Options.Windows_RtxVideo.Enabled)
                {
                    if (TryUpdateRtxTexture(Decoder.RenderFrame))
                        return true;
                }
                else
                {
                    StopRtxEnhancer();
                }

                UseIYUVTexture();
                UpdateSDLTexture(Decoder.RenderFrame);
                TryCaptureComparison(IYUVTexture, "vanilla_720p");

                return true;
            }
            return false;
        }

        unsafe bool TryUpdateRtxTexture(AVFrame* frame)
        {
            if (RtxVideoSupport.SessionDisabled)
            {
                StopRtxEnhancer();
                return false;
            }

            RtxVideoQuality quality = Program.Options.Windows_RtxVideo.Quality;
            RtxVideoOutputResolution resolution =
                Program.Options.Windows_RtxVideo.OutputResolution;
            LastRtxQuality = quality;
            LastRtxResolution = resolution;
            if (RtxEnhancer is not null &&
                RtxEnhancer.OutputResolution != resolution)
            {
                StopRtxEnhancer();
                DestroyRtxTexture();
            }

            RtxEnhancer ??= RtxVideoEnhancer.TryCreate(quality, resolution);
            if (RtxEnhancer is null)
                return false;

            long pathStart = Stopwatch.GetTimestamp();
            if (!RtxEnhancer.TryProcess(frame, quality, out var frameTiming))
            {
                StopRtxEnhancer();
                return false;
            }

            try
            {
                EnsureRtxTexture();
                long uploadStart = Stopwatch.GetTimestamp();
                int uploadResult = SDL_UpdateTexture(
                    RtxTexture,
                    ref RtxTextureSize,
                    RtxEnhancer.OutputBuffer,
                    RtxEnhancer.OutputStride);
                double uploadMilliseconds = Stopwatch.GetElapsedTime(
                    uploadStart).TotalMilliseconds;
                if (uploadResult != 0)
                {
                    throw new InvalidOperationException(
                        $"SDL_UpdateTexture(RGBA8) failed: {SDL_GetError()}");
                }

                UseRtxTexture();
                double pathMilliseconds = Stopwatch.GetElapsedTime(
                    pathStart).TotalMilliseconds;
                RtxTelemetry.Record(
                    quality, resolution, frameTiming,
                    uploadMilliseconds, pathMilliseconds);
                TryCaptureComparison(
                    RtxTexture,
                    $"vsr_{RtxEnhancer.OutputHeight}p_{quality.ToString().ToLowerInvariant()}");
                return true;
            }
            catch (Exception error)
            {
                RtxVideoSupport.DisableForSession(error.Message);
                StopRtxEnhancer();
                return false;
            }
        }

        void EnsureRtxTexture()
        {
            if (RtxTexture != 0 &&
                RtxTextureSize.w == RtxEnhancer!.OutputWidth &&
                RtxTextureSize.h == RtxEnhancer.OutputHeight)
                return;

            DestroyRtxTexture();

            Program.SdlCtx.BugCheckThreadId();
            RtxTexture = SDL_CreateTexture(
                Program.SdlCtx.RendererHandle,
                SDL_PIXELFORMAT_ABGR8888,
                (int)SDL_TextureAccess.SDL_TEXTUREACCESS_STREAMING,
                RtxEnhancer!.OutputWidth,
                RtxEnhancer.OutputHeight);
            if (RtxTexture == 0)
            {
                throw new InvalidOperationException(
                    $"SDL_CreateTexture({RtxEnhancer.OutputWidth}x{RtxEnhancer.OutputHeight} RGBA8) failed: {SDL_GetError()}");
            }

            RtxTextureSize = new SDL_Rect
            {
                x = 0,
                y = 0,
                w = RtxEnhancer.OutputWidth,
                h = RtxEnhancer.OutputHeight,
            };
            Console.WriteLine(
                $"[RTX VSR] Created separate {RtxEnhancer.OutputWidth}x{RtxEnhancer.OutputHeight} " +
                "SDL_PIXELFORMAT_ABGR8888 " +
                "streaming texture (RGBA byte order on little-endian Windows).");
        }

        void DestroyRtxTexture()
        {
            if (RtxTexture == 0)
                return;

            UseIYUVTexture();
            SDL_DestroyTexture(RtxTexture);
            RtxTexture = 0;
            RtxTextureSize = default;
        }

        void TryCaptureComparison(IntPtr texture, string name)
        {
            if (ComparisonCaptured ||
                string.IsNullOrWhiteSpace(ComparisonCaptureDirectory) ||
                --ComparisonCaptureCountdown > 0)
            {
                return;
            }

            ComparisonCaptured = true;
            try
            {
                Directory.CreateDirectory(ComparisonCaptureDirectory);
                string path = Path.Combine(
                    ComparisonCaptureDirectory, name + ".png");
                SDLCapture.ExportTexture(texture, path);
                Console.WriteLine($"[RTX VSR comparison] Captured {path}");
            }
            catch (Exception error)
            {
                Console.WriteLine(
                    $"[RTX VSR comparison] Capture failed: {error.Message}");
            }
        }

        void StopRtxEnhancer()
        {
            if (RtxEnhancer is null)
                return;

            RtxTelemetry.Flush(LastRtxQuality, LastRtxResolution);
            RtxEnhancer.Dispose();
            RtxEnhancer = null;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        static int av_ceil_rshift(int a, int b) =>
            -(-a >> b);

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        unsafe void UpdateSDLTexture(AVFrame* pic)
        {
            if (pic->linesize[0] > 0 && pic->linesize[1] > 0 && pic->linesize[2] > 0)
            {
                SDL_UpdateYUVTexture(IYUVTexture, ref IYUVTextureSize,
                    (IntPtr)pic->data[0], pic->linesize[0],
                    (IntPtr)pic->data[1], pic->linesize[1],
                    (IntPtr)pic->data[2], pic->linesize[2]);
            }
#if DEBUG
            // Not sure if this is needed but ffplay source does handle this case, all my tests had positive linesize
            else if (pic->linesize[0] < 0 && pic->linesize[1] < 0 && pic->linesize[2] < 0)
            {
                Program.DebugLog("Negative Linesize");
                SDL_UpdateYUVTexture(IYUVTexture, ref IYUVTextureSize,
                    (IntPtr)(pic->data[0] + pic->linesize[0] * (pic->height - 1)), -pic->linesize[0],
                    (IntPtr)(pic->data[1] + pic->linesize[1] * (av_ceil_rshift(pic->height, 1) - 1)), -pic->linesize[1],
                    (IntPtr)(pic->data[2] + pic->linesize[2] * (av_ceil_rshift(pic->height, 1) - 1)), -pic->linesize[2]);
            }
#endif
            // While this doesn't seem to be handled in ffplay but the texture can be non-planar with some decoders
            else if (pic->linesize[0] > 0 && pic->linesize[1] == 0)
            {
                SDL_UpdateTexture(IYUVTexture, ref IYUVTextureSize, (nint)pic->data[0], pic->linesize[0]);
            }
            else Console.WriteLine($"Error: Non-positive planar linesizes are not supported, open an issue on Github. {pic->linesize[0]} {pic->linesize[1]} {pic->linesize[2]}");
        }

        bool converterFirstFrameCheck = false;
        unsafe bool DecodeFrameInternal()
        {
            int ret = 0;

            lock (Decoder.CodecLock)
                ret = avcodec_receive_frame(Decoder.CodecCtx, Decoder.ReceiveFrame);

            if (ret == AVERROR(EAGAIN))
            {
                // Try again for the next SDL frame
                return false;
            }
            else if (ret != 0)
            {
                // Should not happen
                Program.DebugLog($"avcodec_receive_frame {ret}");
                return false;
            }
            else
            {
                // On the first frame we get check if we need to use a converter
                if (!converterFirstFrameCheck && Decoder.CodecCtx->pix_fmt != AVPixelFormat.AV_PIX_FMT_NONE)
                {
					Program.DebugLog($"Decoder.CodecCtx uses pixel format {Decoder.CodecCtx->pix_fmt}");

                    converterFirstFrameCheck = true;
                    if (Decoder.CodecCtx->pix_fmt != TargetDecodingFormat)
                    {
                        Converter = InitializeConverter(Decoder.CodecCtx);
                        // Render to the converted frame
                        Decoder.RenderFrame = Converter.Frame;
                    }
                }

                if (Converter.Converter != null)
                {
                    var source = Decoder.ReceiveFrame;
                    var target = Decoder.RenderFrame;
                    sws_scale(Converter.Converter, source->data, source->linesize, 0, source->height, target->data, target->linesize);
                    // Preserve decoded color metadata for the optional RTX RGB conversion.
                    target->color_range = source->color_range;
                    target->colorspace = source->colorspace;
                    target->color_primaries = source->color_primaries;
                    target->color_trc = source->color_trc;
                    target->chroma_location = source->chroma_location;
                }
                else
                {
                    // Swap the frames so we can render source
                    var toRender = Decoder.ReceiveFrame;
                    var receiveNext = Decoder.RenderFrame;

                    Decoder.ReceiveFrame = receiveNext;
                    Decoder.RenderFrame = toRender;
                }

                return true;
            }
        }

        unsafe static FormatConverterContext InitializeConverter(AVCodecContext* codecctx)
        {
            AVFrame* dstframe = null;
            SwsContext* swsContext = null;

            Program.DebugLog($"Initializing converter for {codecctx->pix_fmt}");

            dstframe = av_frame_alloc();

            if (dstframe == null)
                throw new Exception("Couldn't allocate the the converted frame");

            dstframe->format = (int)TargetDecodingFormat;
            dstframe->width = StreamInfo.VideoWidth;
            dstframe->height = StreamInfo.VideoHeight;

            av_frame_get_buffer(dstframe, 32).AssertZero("Couldn't allocate the buffer for the converted frame");

            swsContext = sws_getContext(codecctx->width, codecctx->height, codecctx->pix_fmt,
                                        dstframe->width, dstframe->height, (AVPixelFormat)dstframe->format,
                                        SWS_FAST_BILINEAR, null, null, null);

            if (swsContext == null)
                throw new Exception("Couldn't initialize the converter");

            return new FormatConverterContext()
            {
                Converter = swsContext,
                Frame = dstframe
            };
        }

        public unsafe void Dispose()
        {
            StopRtxEnhancer();

            var ptr = Decoder.Frame1;
            av_frame_free(&ptr);

            ptr = Decoder.Frame2;
            av_frame_free(&ptr);

            var ptr2 = Decoder.CodecCtx;
            avcodec_free_context(&ptr2);

            if (Converter.Converter != null)
            {
                var ptr3 = Converter.Frame;
                av_frame_free(&ptr3);

                sws_freeContext(Converter.Converter);
            }

            DestroyRtxTexture();

            if (IYUVTexture != 0)
            {
                SDL_DestroyTexture(IYUVTexture);
                IYUVTexture = 0;
            }

            TargetTexture = 0;

            Decoder.OnFrameEvent.Dispose();
        }
    }
}
