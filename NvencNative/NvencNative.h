#pragma once

#include <stdint.h>

struct ID3D11Device;
struct ID3D11Texture2D;

// Append-only profiling ABI; existing encoder entry points are unchanged.
enum NvencProfileStage : uint32_t
{
    SlotWait, CopyResourceCpu, MapInput, EncodePicture, OutputEventWait,
    LockBitstream, BitstreamMux, AudioWrite, WriterSample,
    Finalize, Mp4Finalize, SlotResidence, NvencProfileStageCount
};

struct NvencProfileMetric
{
    uint64_t count;
    uint64_t totalNanoseconds;
    uint64_t maxNanoseconds;
};

struct NvencProfileSnapshot
{
    uint32_t version;
    uint32_t metricCount;
    uint64_t submittedFrames;
    uint64_t completedFrames;
    NvencProfileMetric metrics[NvencProfileStageCount];
};

static_assert(sizeof(NvencProfileMetric) == 24, "NvencProfileMetric ABI mismatch");
static_assert(sizeof(NvencProfileSnapshot) == 24 + 12 * 24, "NvencProfileSnapshot ABI mismatch");

extern "C" {
    __declspec(dllexport) void* NvencCreate(
        ID3D11Device* device,
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
        const wchar_t* outputPath);

    // Same as NvencCreate plus an input texture pool size (clamped to 4..8, <=0 means 6).
    __declspec(dllexport) void* NvencCreateWithPool(
        ID3D11Device* device,
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
        const wchar_t* outputPath,
        int texturePoolSize);

    // Effective pool size (0 if the pool is not initialized).
    __declspec(dllexport) int NvencGetPoolSize(void* handle);

    __declspec(dllexport) int NvencEncode(void* handle, ID3D11Texture2D* texture);

    // ---- Multi-session ordered mux (append-only; the entry points above are unchanged) ----
    // A primary encoder (NvencCreate/NvencCreateWithPool) can drive additional NVENC sessions on the same
    // device whose packets are merged into the primary's single MP4 muxer in ascending sequence order.
    // 1. NvencEnableOrderedMux(primary) before the first frame. Returns 1 on success, 0 if unsupported
    //    (needs the async texture pool and H.264/HEVC). From then on the primary must be fed with NvencEncodeEx.
    __declspec(dllexport) int NvencEnableOrderedMux(void* primary);
    // 2. NvencCreateMember(primary, poolSize): a further session with the primary's configuration and no
    //    output file. Always returns a handle once the primary is valid; check NvencGetLastError (non-empty
    //    means it failed and must be destroyed). Returns null if the primary is not in ordered-mux mode.
    __declspec(dllexport) void* NvencCreateMember(void* primary, int texturePoolSize);
    // 3. NvencEncodeEx: like NvencEncode, plus the frame's global position `sequence` (0, 1, 2, ... each
    //    used exactly once across all sessions) and flags (bit 0 = force an IDR). Each session must be fed
    //    from one thread at a time; sessions may be fed concurrently.
    __declspec(dllexport) int NvencEncodeEx(void* handle, ID3D11Texture2D* texture, uint64_t sequence, int flags);
    // 4. Finalize every member (drains it; writes nothing), then the primary (checks that no sequence gap
    //    remains and writes the MP4). Destroy members before the primary.

    __declspec(dllexport) int NvencWriteAudio(void* handle, const float* samples, int sampleCount, int sampleRate, int channels);

    __declspec(dllexport) int NvencFinalize(void* handle);

    __declspec(dllexport) void NvencDestroy(void* handle);

    __declspec(dllexport) const wchar_t* NvencGetLastError(void* handle);

    // Opt-in CPU wall-time counters. Call right after create, before any frame is submitted.
    // Returns 1 on success, 0 if the handle is invalid or frames were already submitted.
    __declspec(dllexport) int NvencEnableProfiling(void* handle);

    // Copies a snapshot. resultSize must equal sizeof(NvencProfileSnapshot). Returns 1 on success.
    __declspec(dllexport) int NvencGetProfile(void* handle, NvencProfileSnapshot* result, uint32_t resultSize);
}
