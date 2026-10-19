#pragma once
#include "NvencNative.h"
#include <atomic>
#include <chrono>

inline constexpr const char* NvencProfileStageNames[] = {
    "slot_wait", "copy_resource_cpu", "map_input", "encode_picture", "output_event_wait",
    "lock_bitstream", "bitstream_mux", "audio_write", "writer_sample",
    "finalize", "mp4_finalize", "slot_residence"
};
static_assert(sizeof(NvencProfileStageNames) / sizeof(NvencProfileStageNames[0]) == NvencProfileStageCount, "Profile stage names mismatch");

// Fixed-size, allocation-free CPU wall-time counters. No logging or GPU waits.
struct NvencProfiler
{
    struct Metric
    {
        std::atomic<uint64_t> count{ 0 }, total{ 0 }, maximum{ 0 };
        void Add(uint64_t ns)
        {
            total.fetch_add(ns, std::memory_order_relaxed);
            count.fetch_add(1, std::memory_order_relaxed);
            uint64_t current = maximum.load(std::memory_order_relaxed);
            while (ns > current && !maximum.compare_exchange_weak(current, ns, std::memory_order_relaxed)) {}
        }
    };
    std::atomic_bool enabled{ false };
    Metric metrics[NvencProfileStageCount];

    static uint64_t Now()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    bool IsEnabled() const { return enabled.load(std::memory_order_relaxed); }

    // Adds a duration measured by the caller (e.g. slot residence). No-op when disabled.
    void Record(NvencProfileStage stage, uint64_t startNs)
    {
        if (startNs != 0 && IsEnabled())
        {
            metrics[stage].Add(Now() - startNs);
        }
    }

    NvencProfileSnapshot Snapshot(uint64_t submittedFrames, uint64_t completedFrames) const
    {
        NvencProfileSnapshot result{};
        result.version = 1;
        result.metricCount = NvencProfileStageCount;
        result.submittedFrames = submittedFrames;
        result.completedFrames = completedFrames;
        for (uint32_t i = 0; i < NvencProfileStageCount; ++i)
        {
            result.metrics[i] = { metrics[i].count.load(std::memory_order_relaxed),
                metrics[i].total.load(std::memory_order_relaxed), metrics[i].maximum.load(std::memory_order_relaxed) };
        }
        return result;
    }
};

class NvencProfileScope
{
    NvencProfiler::Metric* metric = nullptr;
    uint64_t start = 0;
public:
    NvencProfileScope(NvencProfiler& profiler, NvencProfileStage stage)
    {
        if (profiler.enabled.load(std::memory_order_relaxed))
        {
            metric = &profiler.metrics[stage];
            start = NvencProfiler::Now();
        }
    }
    NvencProfileScope(const NvencProfileScope&) = delete;
    NvencProfileScope& operator=(const NvencProfileScope&) = delete;
    void Stop()
    {
        if (metric)
        {
            metric->Add(NvencProfiler::Now() - start);
            metric = nullptr;
        }
    }
    ~NvencProfileScope() { Stop(); }
};
