# Native NVENC smoke harness

`NvencSmoke.cpp` drives `NvencNative.dll` directly (no YMM4, no .NET). It loads the
DLL with `LoadLibrary`, so the same exe can exercise the current build or an older
one (it falls back to `NvencCreate` when `NvencCreateWithPool` is not exported).

It creates a D3D11 device on the NVIDIA adapter, renders synthetic frames into a
BGRA render-target/shader-resource texture (what YMM4 hands the plugin), and calls
`NvencCreateWithPool` -> `NvencEncode` xN (+ `NvencWriteAudio`, 48 kHz stereo sine,
one frame-time chunk per frame) -> `NvencFinalize` -> `NvencDestroy`.

Each frame carries a frame-index marker: 16 blocks of 32x32 px in the top-left corner
(block *b* is white when bit *b* of the frame index is set), plus a moving rectangle.
After decoding, the marker lets you verify frame order and that no frame is stale.

## Build (WSL/Linux, mingw-w64)

    tests/native-smoke/build-wsl.sh <dir containing nvEncodeAPI.h> <out dir>

e.g. with `nv-codec-headers` cloned somewhere: `.../include/ffnvcodec`. This builds
`NvencNative.dll` (static runtime, only system DLL imports) and `NvencSmoke.exe`.
Needs `x86_64-w64-mingw32-g++-posix`. Copy both to a Windows-visible directory and run
the exe from WSL via interop (the DLL needs the Windows NVIDIA driver, nvEncodeAPI64.dll).

## Run

    NvencSmoke.exe --dll C:\path\NvencNative.dll --out C:\path\o.mp4 --codec h264|hevc|av1 \
        --frames 600 --width 1920 --height 1080 --fps 60 --pool 6 --hevc-async 0|1 \
        --quality 0|1|2 --audio 0|1 [--cancel-at N] [--bitrate kbps] [--rc 0|1] \
        [--debug-log 1] [--srgb 1] [--sync 0|1|2] [--mt 0|1]

* `--cancel-at N`: call `NvencDestroy` without `NvencFinalize` at frame N (cancel path).
* `--sync`: 0 nothing, 1 `Flush()`, 2 wait on an event query after rendering (before encode).
* `--mt 1`: harness itself enables `ID3D10Multithread` protection (the DLL now does this too).
* `--debug-log 1`: plugin writes `<out>.nvenc_log.txt`.
* A watchdog prints `STALL stage=... frame=...` plus module+offset of every thread's stack
  if no progress for 10 s. Always run with an external timeout and `taskkill.exe` a hung exe.

The last line is `RESULT ok=... frames=... wall_ms=... fps=...`; exit code 0 on success.
Validate the output with ffprobe/ffmpeg (frame count, full decode, marker check, mp4
tkhd/mdhd/mvhd durations).

Validate: `python3 -I validate_mp4.py <mp4> <codec> <w> <h> <fps> <frames> [audio]`.

Caveat: frames are synthetic (small CPU uploads), so fps is bounded partly by the harness.
