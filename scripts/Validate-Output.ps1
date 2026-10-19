[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Output,

    [Parameter(Mandatory)]
    [ValidateSet('h264', 'hevc', 'av1')]
    [string]$Codec,

    [Parameter(Mandatory)]
    [ValidateRange(2, 16384)]
    [int]$Width,

    [Parameter(Mandatory)]
    [ValidateRange(2, 16384)]
    [int]$Height,

    [Parameter(Mandatory)]
    [ValidateRange(1, [int]::MaxValue)]
    [int]$Fps,

    [Parameter(Mandatory)]
    [ValidateRange(1, [int]::MaxValue)]
    [int]$ExpectedFrames,

    [switch]$ExpectAudio,

    # Optional audio expectations. 0 means "do not check" because the plugin
    # follows the YMM4 project audio settings.
    [ValidateRange(0, 384000)]
    [int]$AudioSampleRate = 0,

    [ValidateRange(0, 32)]
    [int]$AudioChannels = 0,

    # Maximum audio/video duration difference in milliseconds.
    [ValidateRange(0, 10000)]
    [double]$MaxAudioDeltaMs = 50.0,

    # Check bt709 / limited-range color metadata. Off by default because
    # the plugin does not guarantee these tags.
    [switch]$ExpectBt709Limited,

    # Optional: when set and the file exists, a "validation" object is merged
    # into this JSON result file. The nvenc log (<output>.nvenc_log.txt) is not
    # parsed here.
    [string]$Result
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Mp4Timing.Common.ps1')

function Assert-Condition {
    param(
        [Parameter(Mandatory)]
        [bool]$Condition,

        [Parameter(Mandatory)]
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Get-NormalizedPath {
    param(
        [Parameter(Mandatory)]
        [string]$Path
    )

    if ([System.IO.Path]::IsPathFullyQualified($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Path))
}

$outputPath = Get-NormalizedPath $Output
Assert-Condition (Test-Path -LiteralPath $outputPath -PathType Leaf) "Output file was not found: $outputPath"
$resultPath = $null
if ($Result) {
    $resultPath = Get-NormalizedPath $Result
    Assert-Condition (Test-Path -LiteralPath $resultPath -PathType Leaf) "Result file was not found: $resultPath"
}

$ffprobe = Get-Command ffprobe -ErrorAction SilentlyContinue
$ffmpeg = Get-Command ffmpeg -ErrorAction SilentlyContinue
Assert-Condition ($null -ne $ffprobe -and $null -ne $ffmpeg) 'Output validation requires ffmpeg and ffprobe on PATH.'

# MP4 header checks: codec configuration box and (with audio) tkhd/mdhd/mvhd
# time units against the sample table.
$codecConfig = Assert-Mp4VideoCodecConfig -Path $outputPath -Codec $Codec
$mp4AudioTiming = $null
if ($ExpectAudio) {
    $mp4AudioTiming = Assert-Mp4AudioTrackTiming -Path $outputPath
}

$probeText = & $ffprobe.Source -v error -count_frames `
    -show_entries 'stream=index,codec_name,codec_type,nb_read_frames,width,height,r_frame_rate,duration,color_range,color_space,color_transfer,color_primaries,sample_rate,channels' `
    -of json $outputPath | Out-String
Assert-Condition ($LASTEXITCODE -eq 0) 'ffprobe failed.'
$probe = $probeText | ConvertFrom-Json
$video = @($probe.streams | Where-Object { $_.codec_type -eq 'video' })
Assert-Condition ($video.Count -eq 1) "Expected exactly one video stream, found $($video.Count)."
$video = $video[0]
Assert-Condition ($video.codec_name -eq $Codec) "Expected $Codec video, found $($video.codec_name)."
Assert-Condition ([int]$video.width -eq $Width -and [int]$video.height -eq $Height) `
    "Expected ${Width}x${Height}, found $($video.width)x$($video.height)."
Assert-Condition ([int]$video.nb_read_frames -eq $ExpectedFrames) `
    "Expected $ExpectedFrames decoded frames, found $($video.nb_read_frames)."
Assert-Condition ($video.r_frame_rate -eq "$Fps/1") "Expected $Fps/1 frame rate, found $($video.r_frame_rate)."

# Full decode of every stream; errors are printed at -v error.
$nullTarget = if ([System.IO.Path]::DirectorySeparatorChar -eq '\') { 'NUL' } else { '/dev/null' }
& $ffmpeg.Source -v error -i $outputPath -f null $nullTarget
Assert-Condition ($LASTEXITCODE -eq 0) 'Full output decode failed.'

if ($ExpectBt709Limited) {
    $expectedColor = @{
        color_range = 'tv'
        color_space = 'bt709'
        color_transfer = 'bt709'
        color_primaries = 'bt709'
    }
    foreach ($property in $expectedColor.Keys) {
        Assert-Condition ($video.$property -eq $expectedColor[$property]) `
            "Expected $property=$($expectedColor[$property]), found $($video.$property)."
    }
}

$audioDurationDeltaMs = $null
if ($ExpectAudio) {
    $audio = @($probe.streams | Where-Object { $_.codec_type -eq 'audio' })
    Assert-Condition ($audio.Count -eq 1) "Expected exactly one audio stream, found $($audio.Count)."
    $audio = $audio[0]
    Assert-Condition ($audio.codec_name -eq 'aac') "Expected AAC audio, found $($audio.codec_name)."
    if ($AudioSampleRate -gt 0) {
        Assert-Condition ([int]$audio.sample_rate -eq $AudioSampleRate) "Expected $AudioSampleRate Hz audio, found $($audio.sample_rate)."
    }
    if ($AudioChannels -gt 0) {
        Assert-Condition ([int]$audio.channels -eq $AudioChannels) "Expected $AudioChannels audio channels, found $($audio.channels)."
    }
    Assert-Condition ($audio.duration -and $video.duration) 'ffprobe did not report stream durations.'
    $audioDurationDeltaMs = [Math]::Abs(([double]$audio.duration - [double]$video.duration) * 1000.0)
    Assert-Condition ($audioDurationDeltaMs -le $MaxAudioDeltaMs) `
        "Audio/video duration delta is $audioDurationDeltaMs ms (limit: $MaxAudioDeltaMs ms)."
    # The container header must agree with what ffprobe derived from the data.
    $headerDeltaMs = [Math]::Abs(($mp4AudioTiming.audio_seconds - [double]$audio.duration) * 1000.0)
    Assert-Condition ($headerDeltaMs -le 1.0) `
        "Audio mdhd duration differs from ffprobe audio duration by $headerDeltaMs ms."
}

if ($resultPath) {
    $run = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
    if (-not $run.PSObject.Properties['validation']) {
        $run | Add-Member -NotePropertyName validation -NotePropertyValue ([pscustomobject]@{}) -Force
    }
    $values = [ordered]@{
        decode = 'passed'
        frame_count = 'passed'
        codec_config = $codecConfig.codec_config_box
        color = if ($ExpectBt709Limited) { 'passed_bt709_limited_metadata' } else { 'not_requested' }
        audio_sync = if ($ExpectAudio) { 'passed_duration_only' } else { 'not_requested' }
    }
    foreach ($key in $values.Keys) {
        $run.validation | Add-Member -NotePropertyName $key -NotePropertyValue $values[$key] -Force
    }
    if ($ExpectAudio) {
        $run.validation | Add-Member -NotePropertyName mp4_audio_timing `
            -NotePropertyValue $mp4AudioTiming -Force
        $run.validation | Add-Member -NotePropertyName audio_duration_delta_ms `
            -NotePropertyValue ([Math]::Round($audioDurationDeltaMs, 3)) -Force
    }
    $run | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $resultPath -Encoding utf8
}

Write-Host "Output validation passed: codec=$Codec frames=$ExpectedFrames codec_config=$($codecConfig.codec_config_box) audio=$($ExpectAudio.IsPresent) bt709_limited=$($ExpectBt709Limited.IsPresent)"
