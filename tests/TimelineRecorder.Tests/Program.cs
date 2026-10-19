using System.Text.Json;
using NVEncVideoWriterPlugin;

// Self-contained checks driven by a fake clock (1 tick = 1 ms). Exit code 1 on failure.
var failures = 0;
void Check(bool ok, string name) { if (!ok) { failures++; Console.WriteLine("FAIL: " + name); } else Console.WriteLine("ok: " + name); }
JsonElement Json(object o) => JsonSerializer.SerializeToElement(o);

const long Hz = 1000;
long Sec(double s) => (long)(s * Hz);

// 1) Basic bucketing, frame index, partial flush, native deltas.
{
    var r = new TimelineRecorder(Hz, 10);
    for (var i = 0; i < 1230; i++) // 60 fps for 20.5 s -> frames at t = i*1000/60 ms
    {
        var t = i * 1000 / 60;
        if (r.VideoFrame(t)) r.ApplyNative(new NativeTotals((ulong)i * 1000, 0, 0, 0, 0));
        r.AddVideoCallback(2);
    }
    r.SetFinalNative(new NativeTotals(2_000_000_000, 0, 0, 0, 0));
    r.Complete(Sec(25));
    var (tl, info, sum) = r.BuildReport();
    Check(tl.Length == 3, "20.5 s of video -> 2 full + 1 partial bucket");
    var b0 = Json(tl[0]); var b1 = Json(tl[1]);
    Check(b0.GetProperty("frame_index_start").GetInt64() == 0 && b0.GetProperty("frames").GetInt64() == 600, "bucket0 frames");
    Check(b1.GetProperty("frame_index_start").GetInt64() == 600 && b1.GetProperty("frames").GetInt64() == 600, "bucket1 start index");
    Check(Math.Abs(b1.GetProperty("fps").GetDouble() - 60) < 1e-9, "bucket1 fps = 60");
    Check(b0.GetProperty("video_callback_ms").GetDouble() == 1200, "bucket0 callback ms");
    Check(Math.Abs(b0.GetProperty("slot_wait_ms").GetDouble() - 0.6) < 1e-9 && Math.Abs(b1.GetProperty("slot_wait_ms").GetDouble() - 0.6) < 1e-9, "native deltas = snapshot differences");
    Check(Math.Abs(Json(tl[2]).GetProperty("slot_wait_ms").GetDouble() - 1998.8) < 1e-6, "final delta on partial bucket");
    Check(b1.GetProperty("wall_start_s").GetDouble() == 10 && b1.GetProperty("wall_end_s").GetDouble() == 20, "bucket1 wall range");
    Check(Json(sum).GetProperty("full_buckets").GetInt32() == 2, "summary counts full buckets only");
    Check(Json(tl[2]).GetProperty("partial").GetBoolean(), "last bucket is partial");
    Check(Json(tl[2]).GetProperty("wall_end_s").GetDouble() <= 30, "partial wall end clamped to bucket end");
    Check(!Json(info).GetProperty("truncated").GetBoolean(), "not truncated");
}

// 2) Slow section shows up in summary; native delta attribution and gap buckets.
{
    var r = new TimelineRecorder(Hz, 1);
    long frame = 0;
    for (var s = 0; s < 6; s++)
    {
        var n = s == 3 ? 10 : 50; // bucket 3 is slow
        for (var k = 0; k < n; k++)
        {
            var wanted = r.VideoFrame(Sec(s) + k * 1000 / n);
            if (wanted) r.ApplyNative(new NativeTotals((ulong)(s * 100_000_000), 0, 0, 0, 0));
            frame++;
        }
    }
    r.VideoFrame(Sec(9)); // gap: buckets 6,7,8 have no frames
    r.Complete(Sec(9) + 100);
    var (tl, _, sum) = r.BuildReport();
    Check(tl.Length == 10, "gap buckets filled (10 buckets)");
    var slow = Json(sum).GetProperty("slowest_buckets")[0];
    Check(slow.GetProperty("fps").GetDouble() == 0 && slow.GetProperty("frames").GetInt64() == 0, "empty buckets are slowest");
    Check(Json(tl[7]).GetProperty("frame_index_start").GetInt64() == frame, "gap bucket frame index");
    Check(Math.Abs(Json(tl[3]).GetProperty("fps").GetDouble() - 10) < 1e-9, "slow bucket fps");
    Check(Json(tl[0]).GetProperty("slot_wait_ms").GetDouble() == 100 && Json(tl[2]).GetProperty("slot_wait_ms").GetDouble() == 100, "native delta attributed to closed bucket");
}

// 3) Truncation cap.
{
    var r = new TimelineRecorder(Hz, 1, maxBuckets: 5);
    for (var s = 0; s < 50; s++) r.VideoFrame(Sec(s));
    r.Complete(Sec(50));
    var (tl, info, _) = r.BuildReport();
    Check(tl.Length == 5 && Json(info).GetProperty("truncated").GetBoolean(), "capped at max buckets with truncated=true");
    Check(!r.WantsNativeSnapshot, "no snapshot requests after truncation");
}

// 4) No frames -> empty; discard mode has no native fields; busy / outside.
{
    var empty = new TimelineRecorder(Hz, 10); empty.Complete(0);
    Check(empty.BuildReport().Timeline.Length == 0, "no frames -> empty timeline");
    var r = new TimelineRecorder(Hz, 10);
    r.VideoFrame(0); r.AddBusy(Sec(2)); r.Complete(Sec(4));
    var b = Json(r.BuildReport().Timeline[0]);
    Check(!b.TryGetProperty("encode_picture_ms", out _), "no native fields without snapshots");
    Check(b.GetProperty("outside_callbacks_ms").GetDouble() == 2000, "outside = wall - busy");
}

Check(TimelineRecorder.ParseBucketSec(null) == 10 && TimelineRecorder.ParseBucketSec("0") == 1 &&
      TimelineRecorder.ParseBucketSec("999") == 300 && TimelineRecorder.ParseBucketSec("abc") == 10 &&
      TimelineRecorder.ParseBucketSec("5") == 5, "env parsing/clamping");

Console.WriteLine(failures == 0 ? "ALL PASSED" : failures + " FAILED");
return failures == 0 ? 1 - 1 : 1;
