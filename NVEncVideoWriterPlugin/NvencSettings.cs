namespace NVEncVideoWriterPlugin;

internal sealed class NvencSettings
{
    public NvencCodec Codec { get; set; } = NvencCodec.H264;
    public int BitrateKbps { get; set; } = 12000;
    public NvencQuality Quality { get; set; } = NvencQuality.Balanced;
    public NvencRateControl RateControl { get; set; } = NvencRateControl.YouTubeRecommended;
    public bool HevcAsync { get; set; } = true;
    public bool EnableDebugLog { get; set; }
    // Input texture pool size (allowed: 4 / 6 / 8).
    public int TexturePoolSize { get; set; } = 6;
    // Opt-in: advertise IVideoFileWriter3.IsGpuFrameSupported so YMM4 hands over GPU frames directly.
    public bool EnableGpuDirectInput { get; set; }
}

internal enum NvencCodec
{
    H264,
    H265,
    AV1,
}

internal enum NvencQuality
{
    Speed,
    Balanced,
    Quality,
}

internal enum NvencRateControl
{
    Fixed,
    Variable,
    YouTubeRecommended,
}
