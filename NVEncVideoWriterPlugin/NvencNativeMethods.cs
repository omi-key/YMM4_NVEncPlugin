using System.Runtime.InteropServices;

namespace NVEncVideoWriterPlugin;

internal static class NvencNativeMethods
{
    [DllImport("NvencNative.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr NvencCreate(
        IntPtr device,
        int width,
        int height,
        int fps,
        int bitrateKbps,
        int codec,
        int quality,
        int fastPreset,
        int rateControlMode,
        int maxBitrateKbps,
        int bufferFormat,
        int hevcAsync,
        int enableDebugLog,
        string outputPath);

    [DllImport("NvencNative.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr NvencCreateWithPool(
        IntPtr device,
        int width,
        int height,
        int fps,
        int bitrateKbps,
        int codec,
        int quality,
        int fastPreset,
        int rateControlMode,
        int maxBitrateKbps,
        int bufferFormat,
        int hevcAsync,
        int enableDebugLog,
        string outputPath,
        int texturePoolSize);

    [DllImport("NvencNative.dll")]
    public static extern int NvencGetPoolSize(IntPtr handle);

    [DllImport("NvencNative.dll")]
    public static extern int NvencEncode(IntPtr handle, IntPtr texture);

    [DllImport("NvencNative.dll")]
    public static extern int NvencWriteAudio(IntPtr handle, float[] samples, int sampleCount, int sampleRate, int channels);

    [DllImport("NvencNative.dll")]
    public static extern int NvencFinalize(IntPtr handle);

    [DllImport("NvencNative.dll")]
    public static extern void NvencDestroy(IntPtr handle);

    [DllImport("NvencNative.dll")]
    public static extern IntPtr NvencGetLastError(IntPtr handle);

    [DllImport("NvencNative.dll")]
    public static extern int NvencEnableProfiling(IntPtr handle);

    [DllImport("NvencNative.dll")]
    public static extern int NvencGetProfile(IntPtr handle, out NativeProfileSnapshot result, uint resultSize);
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeProfileMetric
{
    public ulong Count;
    public ulong TotalNanoseconds;
    public ulong MaxNanoseconds;
}

// Must match NvencProfileSnapshot in NvencNative.h (24 + 12 * 24 = 312 bytes).
[StructLayout(LayoutKind.Sequential)]
internal struct NativeProfileSnapshot
{
    public uint Version;
    public uint MetricCount;
    public ulong SubmittedFrames;
    public ulong CompletedFrames;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 12)]
    public NativeProfileMetric[] Metrics;

    internal static readonly string[] StageNames =
    [
        "slot_wait", "copy_resource_cpu", "map_input", "encode_picture", "output_event_wait",
        "lock_bitstream", "bitstream_mux", "audio_write", "writer_sample",
        "finalize", "mp4_finalize", "slot_residence",
    ];

    public object ToReport()
    {
        if (Version != 1 || MetricCount != StageNames.Length || Metrics is null || Metrics.Length != StageNames.Length)
            throw new InvalidOperationException("NVENCプロファイリングのDLLバージョンが一致しません。");
        var stages = new Dictionary<string, object>();
        for (var i = 0; i < StageNames.Length; i++)
        {
            var metric = Metrics[i];
            stages.Add(StageNames[i], new
            {
                count = metric.Count,
                total_ms = metric.TotalNanoseconds / 1_000_000d,
                mean_ms = metric.Count == 0 ? 0 : metric.TotalNanoseconds / 1_000_000d / metric.Count,
                max_ms = metric.MaxNanoseconds / 1_000_000d,
            });
        }
        return new { version = Version, submitted_frames = SubmittedFrames, completed_frames = CompletedFrames, stages };
    }
}
