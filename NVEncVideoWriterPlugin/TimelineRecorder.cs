namespace NVEncVideoWriterPlugin;

// Cumulative native counters (nanoseconds) taken from NvencGetProfile snapshots.
internal readonly record struct NativeTotals(
    ulong SlotWait, ulong EncodePicture, ulong OutputEventWait, ulong LockBitstream, ulong BitstreamMux);

// Fixed-width wall-clock buckets measured from the first video frame.
// Not thread-safe by itself: ExportProfiler calls every member while holding its own gate.
// Time is passed in as raw timestamps (ticks of `frequency`), so tests can drive it with a fake clock.
// Memory is bounded: one small struct per bucket, at most maxBuckets of them.
internal sealed class TimelineRecorder
{
    public const int DefaultMaxBuckets = 20000;

    private struct Bucket
    {
        public int Index;
        public long FrameIndexStart, Frames;
        public long VideoCallback, NativeVideo, AudioCallback, Busy; // ticks
        public double WallEndS;
        public bool Partial, HasNative;
        public double SlotWaitMs, EncodePictureMs, OutputEventWaitMs, LockBitstreamMs, BitstreamMuxMs;
    }

    private readonly double _frequency;
    private readonly long _bucketTicks;
    private readonly int _maxBuckets;
    private readonly List<Bucket> _buckets = new();
    private Bucket _cur;
    private bool _started, _stopped, _completed;
    private long _t0, _totalFrames;
    private int _pendingNative = -1;
    private NativeTotals _lastNative;
    private NativeTotals? _finalNative;

    public TimelineRecorder(long frequency, double bucketSec, int maxBuckets = DefaultMaxBuckets)
    {
        _frequency = frequency;
        BucketSec = bucketSec;
        _bucketTicks = Math.Max(1, (long)(bucketSec * frequency));
        _maxBuckets = maxBuckets;
    }

    public double BucketSec { get; }
    public bool Truncated { get; private set; }
    // True after a bucket boundary was crossed and the caller should supply a native snapshot.
    public bool WantsNativeSnapshot => _pendingNative >= 0 && !_stopped;

    // Parses NVENC_PROFILE_BUCKET_SEC. Unset/invalid -> 10; out of range -> clamped to 1..300.
    public static double ParseBucketSec(string? text)
    {
        if (!double.TryParse(text, System.Globalization.NumberStyles.Float,
                System.Globalization.CultureInfo.InvariantCulture, out var value) || double.IsNaN(value))
            return 10;
        return Math.Clamp(value, 1, 300);
    }

    // Registers a received video frame. Returns true when it opened a new bucket (a native snapshot is wanted).
    public bool VideoFrame(long now)
    {
        if (_stopped || _completed) return false;
        var crossed = false;
        if (!_started)
        {
            _started = true;
            _t0 = now;
            _cur = new Bucket { Index = 0, FrameIndexStart = 0 };
        }
        else
        {
            var index = Math.Max(0, now - _t0) / _bucketTicks;
            if (index > _cur.Index)
            {
                crossed = true;
                _cur.WallEndS = (_cur.Index + 1) * BucketSec;
                if (!Add(_cur)) return false;
                _pendingNative = _buckets.Count - 1;
                for (var gap = _cur.Index + 1; gap < index; gap++)
                {
                    if (!Add(new Bucket { Index = (int)gap, FrameIndexStart = _totalFrames, WallEndS = (gap + 1) * BucketSec }))
                        return false;
                }
                _cur = new Bucket { Index = (int)Math.Min(index, int.MaxValue), FrameIndexStart = _totalFrames };
            }
        }
        _cur.Frames++;
        _totalFrames++;
        return crossed;
    }

    public void AddVideoCallback(long ticks) { if (Active) _cur.VideoCallback += ticks; }
    public void AddNativeVideo(long ticks) { if (Active) _cur.NativeVideo += ticks; }
    public void AddAudioCallback(long ticks) { if (Active) _cur.AudioCallback += ticks; }
    // A finished "at least one callback running" interval, attributed to the bucket current at its end.
    public void AddBusy(long ticks) { if (Active) _cur.Busy += ticks; }

    // Attributes the counter growth since the previous snapshot to the most recently closed bucket.
    public void ApplyNative(NativeTotals totals)
    {
        var pending = _pendingNative;
        _pendingNative = -1;
        if (_stopped) return;
        if (pending >= 0 && pending < _buckets.Count)
        {
            var bucket = _buckets[pending];
            SetDelta(ref bucket, totals);
            _buckets[pending] = bucket;
        }
        _lastNative = totals;
    }

    // Snapshot failed: stop asking, leave native fields out.
    public void NativeUnavailable() => _pendingNative = -1;

    // Final counters (before native destroy); applied to the last partial bucket on Complete.
    public void SetFinalNative(NativeTotals totals) => _finalNative = totals;

    // Flushes the last partial bucket. endNow is the last callback end (excludes finalization time).
    public void Complete(long endNow)
    {
        if (_completed) return;
        _completed = true;
        _pendingNative = -1;
        if (!_started || _stopped) return;
        var wall = Math.Max(0, endNow - _t0) / _frequency;
        _cur.WallEndS = Math.Min(wall, (_cur.Index + 1) * BucketSec);
        _cur.Partial = true;
        if (_finalNative is { } totals) SetDelta(ref _cur, totals);
        Add(_cur);
    }

    private bool Active => _started && !_stopped && !_completed;

    private bool Add(Bucket bucket)
    {
        if (_buckets.Count >= _maxBuckets)
        {
            Truncated = true;
            _stopped = true;
            return false;
        }
        _buckets.Add(bucket);
        return true;
    }

    private void SetDelta(ref Bucket bucket, NativeTotals totals)
    {
        static double Delta(ulong now, ulong before) => now >= before ? (now - before) / 1_000_000d : 0;
        bucket.HasNative = true;
        bucket.SlotWaitMs = Delta(totals.SlotWait, _lastNative.SlotWait);
        bucket.EncodePictureMs = Delta(totals.EncodePicture, _lastNative.EncodePicture);
        bucket.OutputEventWaitMs = Delta(totals.OutputEventWait, _lastNative.OutputEventWait);
        bucket.LockBitstreamMs = Delta(totals.LockBitstream, _lastNative.LockBitstream);
        bucket.BitstreamMuxMs = Delta(totals.BitstreamMux, _lastNative.BitstreamMux);
        _lastNative = totals;
    }

    private double Ms(long ticks) => ticks * 1000d / _frequency;

    private static double Fps(in Bucket b, double wallStartS) =>
        b.WallEndS - wallStartS > 1e-9 ? b.Frames / (b.WallEndS - wallStartS) : 0;

    // timeline: one element per bucket. info/summary: compact aggregates. Call after Complete.
    public (object[] Timeline, object Info, object Summary) BuildReport()
    {
        var timeline = new object[_buckets.Count];
        var full = new List<(int Position, double Fps)>();
        for (var i = 0; i < _buckets.Count; i++)
        {
            var b = _buckets[i];
            var start = b.Index * BucketSec;
            var wall = Math.Max(0, b.WallEndS - start);
            var fps = Fps(b, start);
            var item = new Dictionary<string, object>
            {
                ["bucket"] = b.Index,
                ["wall_start_s"] = start,
                ["wall_end_s"] = b.WallEndS,
                ["frame_index_start"] = b.FrameIndexStart,
                ["frames"] = b.Frames,
                ["fps"] = fps,
                ["video_callback_ms"] = Ms(b.VideoCallback),
                ["native_video_ms"] = Ms(b.NativeVideo),
                ["audio_callback_ms"] = Ms(b.AudioCallback),
                ["outside_callbacks_ms"] = Math.Max(0, wall * 1000d - Ms(b.Busy)),
            };
            if (b.Partial) item["partial"] = true;
            if (b.HasNative)
            {
                item["slot_wait_ms"] = b.SlotWaitMs;
                item["encode_picture_ms"] = b.EncodePictureMs;
                item["output_event_wait_ms"] = b.OutputEventWaitMs;
                item["lock_bitstream_ms"] = b.LockBitstreamMs;
                item["bitstream_mux_ms"] = b.BitstreamMuxMs;
            }
            timeline[i] = item;
            if (!b.Partial) full.Add((i, fps));
        }

        var info = new
        {
            bucket_sec = BucketSec,
            max_buckets = _maxBuckets,
            bucket_count = _buckets.Count,
            full_bucket_count = full.Count,
            truncated = Truncated,
            origin = "first_video_frame_callback",
        };

        object summary;
        if (full.Count == 0)
        {
            summary = new { full_buckets = 0 };
        }
        else
        {
            var sorted = full.Select(f => f.Fps).OrderBy(v => v).ToArray();
            var median = sorted.Length % 2 == 1
                ? sorted[sorted.Length / 2]
                : (sorted[sorted.Length / 2 - 1] + sorted[sorted.Length / 2]) / 2;
            var slowest = full.OrderBy(f => f.Fps).ThenBy(f => f.Position).Take(5).Select(f =>
            {
                var b = _buckets[f.Position];
                return new
                {
                    bucket = b.Index,
                    frame_index_start = b.FrameIndexStart,
                    frame_index_end = b.FrameIndexStart + Math.Max(0, b.Frames - 1),
                    frames = b.Frames,
                    fps = f.Fps,
                };
            }).ToArray();
            summary = new
            {
                full_buckets = full.Count,
                min_fps = sorted[0],
                median_fps = median,
                max_fps = sorted[^1],
                slowest_buckets = slowest,
            };
        }
        return (timeline, info, summary);
    }
}
