# Self-contained regression test for scripts/Mp4Timing.Common.ps1.
# Uses crafted MP4 headers only; no GPU, ffmpeg or encoder is required.
$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $repositoryRoot 'scripts/Mp4Timing.Common.ps1')
function U32([uint32]$Value) {
    $bytes = [BitConverter]::GetBytes($Value)
    [Array]::Reverse($bytes)
    return $bytes
}
function Box([string]$Type, [byte[]]$Payload) {
    return [byte[]]((U32 ($Payload.Length + 8)) + [Text.Encoding]::ASCII.GetBytes($Type) + $Payload)
}
# $SttsSamples < 0 omits the sample table; otherwise stts holds one entry.
function Fixture([uint32]$Rate, [uint32]$Samples, [uint32]$TrackDuration, [long]$SttsSamples = -1, [string]$Extra = '') {
    $movie = [byte[]]([byte[]]::new(12) + (U32 90000) + (U32 21600000))
    $track = [byte[]]([byte[]]::new(12) + (U32 2) + (U32 0) + (U32 $TrackDuration))
    $media = [byte[]]([byte[]]::new(12) + (U32 $Rate) + (U32 $Samples))
    $handler = [byte[]]([byte[]]::new(8) + [Text.Encoding]::ASCII.GetBytes('soun'))
    $mediaBoxes = [byte[]]((Box 'mdhd' $media) + (Box 'hdlr' $handler))
    if ($SttsSamples -ge 0) {
        $stts = Box 'stts' ([byte[]]([byte[]]::new(4) + (U32 1) + (U32 ([uint32]$SttsSamples)) + (U32 1)))
        $mediaBoxes = [byte[]]($mediaBoxes + (Box 'minf' (Box 'stbl' $stts)))
    }
    $mdia = Box 'mdia' $mediaBoxes
    $trak = Box 'trak' ([byte[]]((Box 'tkhd' $track) + $mdia))
    $udta = if ($Extra) { Box 'udta' ([Text.Encoding]::ASCII.GetBytes($Extra)) } else { [byte[]]@() }
    return Box 'moov' ([byte[]]((Box 'mvhd' $movie) + $trak + $udta))
}
function Assert-Rejected([scriptblock]$Action, [string]$Pattern, [string]$Message) {
    $rejected = $false
    try { & $Action | Out-Null }
    catch { if ($Pattern -and $_.Exception.Message -notlike $Pattern) { throw }; $rejected = $true }
    if (-not $rejected) { throw $Message }
}
$root = Join-Path $repositoryRoot ('artifacts/runs/mp4-timing-tests-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -Force | Out-Null
$path = Join-Path $root 'fixture.mp4'
$checks = 0
try {
    foreach ($rate in @(32000, 44100, 48000)) {
        $samples = [uint32]($rate * 240)
        [IO.File]::WriteAllBytes($path, (Fixture $rate $samples 21600000))
        $report = Assert-Mp4AudioTrackTiming $path
        if ($report.status -ne 'passed') { throw 'Valid four-minute track rejected.' }
        $checks++
        # Reproduce the pre-fix writer: audio sample ticks were copied into tkhd.
        [IO.File]::WriteAllBytes($path, (Fixture $rate $samples $samples))
        Assert-Rejected { Assert-Mp4AudioTrackTiming $path } '*wrong clock*' 'Wrong-clock regression was accepted.'
        $checks++
        # Valid header with a matching sample table is accepted and counted.
        [IO.File]::WriteAllBytes($path, (Fixture $rate $samples 21600000 $samples))
        $report = Assert-Mp4AudioTrackTiming $path
        if ($report.stts_checked -ne 1) { throw 'stts cross-check was not performed.' }
        $checks++
        # mdhd claims more audio than the sample table holds.
        [IO.File]::WriteAllBytes($path, (Fixture $rate $samples 21600000 ($samples - 1024)))
        Assert-Rejected { Assert-Mp4AudioTrackTiming $path } '*does not match stts*' 'mdhd/stts mismatch was accepted.'
        $checks++
    }
    # AAC sample count need not map to an integer movie tick at 44.1 kHz.
    [IO.File]::WriteAllBytes($path, (Fixture 44100 1024 2089))
    Assert-Mp4AudioTrackTiming $path | Out-Null
    $checks++
    [IO.File]::WriteAllBytes($path, [byte[]](0, 0, 0, 16, 109, 111, 111, 118))
    Assert-Rejected { Assert-Mp4AudioTrackTiming $path } '' 'Truncated MP4 box was accepted.'
    $checks++
    # Codec configuration box lookup (H.264 / HEVC / AV1).
    [IO.File]::WriteAllBytes($path, (Fixture 48000 1024 1920 -1 'xxxxav1Cxxxx'))
    $report = Assert-Mp4VideoCodecConfig $path 'av1'
    if ($report.status -ne 'passed') { throw 'av1C lookup failed.' }
    $checks++
    Assert-Rejected { Assert-Mp4VideoCodecConfig $path 'hevc' } '*hvcC*' 'Missing hvcC was accepted.'
    $checks++
    [IO.File]::WriteAllBytes($path, (Fixture 48000 1024 1920 -1 'xxxxavcCxxxx'))
    Assert-Mp4VideoCodecConfig $path 'h264' | Out-Null
    $checks++
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "MP4 timing regression checks: $checks passed"
