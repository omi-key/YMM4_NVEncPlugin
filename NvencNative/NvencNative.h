#pragma once

#include <stdint.h>

struct ID3D11Device;
struct ID3D11Texture2D;

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

    __declspec(dllexport) int NvencWriteAudio(void* handle, const float* samples, int sampleCount, int sampleRate, int channels);

    __declspec(dllexport) int NvencFinalize(void* handle);

    __declspec(dllexport) void NvencDestroy(void* handle);

    __declspec(dllexport) const wchar_t* NvencGetLastError(void* handle);
}
