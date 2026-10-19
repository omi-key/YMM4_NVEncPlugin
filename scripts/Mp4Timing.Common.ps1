# Inspect the unedited MP4 files emitted by this plugin. ffprobe duration
# alone does not catch an incorrect tkhd clock when mdhd/stts are valid.
function Assert-Mp4AudioTrackTiming {
    param([Parameter(Mandatory)][string]$Path)
    $stream = [IO.File]::OpenRead([IO.Path]::GetFullPath($Path))
    try {
        function Read-U32([long]$Offset) {
            $stream.Position = $Offset
            $bytes = [byte[]]::new(4)
            if ($stream.Read($bytes, 0, 4) -ne 4) { throw 'Truncated MP4 integer.' }
            return [uint64]$bytes[0] * 16777216 + [uint64]$bytes[1] * 65536 + [uint64]$bytes[2] * 256 + [uint64]$bytes[3]
        }
        function Read-U64([long]$Offset) {
            return [decimal](Read-U32 $Offset) * 4294967296 + [decimal](Read-U32 ($Offset + 4))
        }
        function Read-Type([long]$Offset) {
            $stream.Position = $Offset
            $bytes = [byte[]]::new(4)
            if ($stream.Read($bytes, 0, 4) -ne 4) { throw 'Truncated MP4 type.' }
            return [Text.Encoding]::ASCII.GetString($bytes)
        }
        function Read-Boxes([long]$Start, [long]$End) {
            $position = $Start
            while ($position -lt $End) {
                if ($End - $position -lt 8) { throw 'Truncated MP4 box header.' }
                $size = Read-U32 $position
                $type = Read-Type ($position + 4)
                $header = 8
                if ($size -eq 1) { $size = Read-U64 ($position + 8); $header = 16 }
                if ($size -eq 0) { $size = $End - $position }
                if ($size -lt $header -or $size -gt $End - $position) { throw "Invalid MP4 box size: $type" }
                [pscustomobject]@{ Type = $type; Payload = $position + $header; End = $position + [long]$size }
                $position += [long]$size
            }
        }
        function Read-Clock($Box, [bool]$TrackHeader = $false) {
            $stream.Position = $Box.Payload
            $version = $stream.ReadByte()
            if ($version -notin @(0, 1)) { throw 'Unsupported MP4 timing version.' }
            $offset = if ($version -eq 1) { 20 } else { 12 }
            $durationOffset = if ($TrackHeader) { $offset + 8 } else { $offset + 4 }
            $width = if ($version -eq 1) { 8 } else { 4 }
            if ($Box.Payload + $durationOffset + $width -gt $Box.End) { throw 'Truncated MP4 clock.' }
            $duration = if ($version -eq 1) { Read-U64 ($Box.Payload + $durationOffset) } else { Read-U32 ($Box.Payload + $durationOffset) }
            [pscustomobject]@{ Duration = [decimal]$duration; Timescale = if ($TrackHeader) { 0 } else { Read-U32 ($Box.Payload + $offset) } }
        }
        # Sum of stts sample_count * sample_delta, in media timescale ticks.
        function Read-SttsDuration($Box) {
            if ($Box.Payload + 8 -gt $Box.End) { throw 'Truncated stts box.' }
            $entries = [long](Read-U32 ($Box.Payload + 4))
            if ($Box.Payload + 8 + $entries * 8 -gt $Box.End) { throw 'Truncated stts entries.' }
            $total = [decimal]0
            for ($i = 0; $i -lt $entries; ++$i) {
                $entry = $Box.Payload + 8 + $i * 8
                $total += [decimal](Read-U32 $entry) * [decimal](Read-U32 ($entry + 4))
            }
            return $total
        }
        $moov = @(Read-Boxes 0 $stream.Length | Where-Object Type -eq 'moov')
        if ($moov.Count -ne 1) { throw 'Expected exactly one moov box.' }
        $children = @(Read-Boxes $moov[0].Payload $moov[0].End)
        $mvhd = @($children | Where-Object Type -eq 'mvhd')
        if ($mvhd.Count -ne 1) { throw 'Expected exactly one mvhd box.' }
        $movie = Read-Clock $mvhd[0]
        if ($movie.Timescale -eq 0) { throw 'Movie timescale is zero.' }
        $audioCount = 0
        $sttsChecked = 0
        $audioSeconds = [double]0
        foreach ($track in @($children | Where-Object Type -eq 'trak')) {
            $parts = @(Read-Boxes $track.Payload $track.End)
            $mdia = @($parts | Where-Object Type -eq 'mdia')
            if ($mdia.Count -ne 1) { throw 'Expected one mdia per track.' }
            $mediaParts = @(Read-Boxes $mdia[0].Payload $mdia[0].End)
            $handler = @($mediaParts | Where-Object Type -eq 'hdlr')
            if ($handler.Count -ne 1 -or $handler[0].Payload + 12 -gt $handler[0].End) { throw 'Invalid track handler.' }
            if ((Read-Type ($handler[0].Payload + 8)) -ne 'soun') { continue }
            if (@($parts | Where-Object Type -eq 'edts').Count) { throw 'Audio edit lists are outside this plugin timing contract.' }
            $tkhd = @($parts | Where-Object Type -eq 'tkhd')
            $mdhd = @($mediaParts | Where-Object Type -eq 'mdhd')
            if ($tkhd.Count -ne 1 -or $mdhd.Count -ne 1) { throw 'Missing or duplicate audio clock.' }
            $trackClock = Read-Clock $tkhd[0] $true
            $mediaClock = Read-Clock $mdhd[0]
            if ($mediaClock.Timescale -eq 0) { throw 'Audio timescale is zero.' }
            $expected = [decimal]::Floor($mediaClock.Duration * $movie.Timescale / $mediaClock.Timescale)
            if ($trackClock.Duration -ne $expected) {
                throw "Audio tkhd duration uses the wrong clock: actual=$($trackClock.Duration), expected=$expected (movie timescale=$($movie.Timescale))."
            }
            # mdhd must also match the actual sample table when one is present.
            $minf = @($mediaParts | Where-Object Type -eq 'minf')
            if ($minf.Count -eq 1) {
                $stbl = @(Read-Boxes $minf[0].Payload $minf[0].End | Where-Object Type -eq 'stbl')
                if ($stbl.Count -eq 1) {
                    $stts = @(Read-Boxes $stbl[0].Payload $stbl[0].End | Where-Object Type -eq 'stts')
                    if ($stts.Count -eq 1) {
                        $sttsDuration = Read-SttsDuration $stts[0]
                        if ($sttsDuration -ne $mediaClock.Duration) {
                            throw "Audio mdhd duration does not match stts: mdhd=$($mediaClock.Duration), stts=$sttsDuration."
                        }
                        $sttsChecked++
                    }
                }
            }
            $audioSeconds = [double]($mediaClock.Duration / $mediaClock.Timescale)
            $audioCount++
        }
        if ($audioCount -eq 0) { throw 'No audio track found for timing validation.' }
        [pscustomobject]@{
            audio_tracks = $audioCount
            movie_timescale = $movie.Timescale
            stts_checked = $sttsChecked
            audio_seconds = [Math]::Round($audioSeconds, 6)
            status = 'passed'
        }
    }
    finally { $stream.Dispose() }
}

# Check that the video sample entry carries the codec configuration box
# (avcC for H.264, hvcC for HEVC, av1C for AV1). The box name is searched for
# inside the moov payload, which is small compared with mdat.
function Assert-Mp4VideoCodecConfig {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][ValidateSet('h264', 'hevc', 'av1')][string]$Codec
    )
    $tag = @{ h264 = 'avcC'; hevc = 'hvcC'; av1 = 'av1C' }[$Codec]
    $stream = [IO.File]::OpenRead([IO.Path]::GetFullPath($Path))
    try {
        $moovBytes = $null
        $position = [long]0
        while ($position + 8 -le $stream.Length) {
            $header = [byte[]]::new(16)
            $stream.Position = $position
            $read = $stream.Read($header, 0, 16)
            if ($read -lt 8) { break }
            $size = [uint64]$header[0] * 16777216 + [uint64]$header[1] * 65536 + [uint64]$header[2] * 256 + [uint64]$header[3]
            $type = [Text.Encoding]::ASCII.GetString($header, 4, 4)
            $headerSize = 8
            if ($size -eq 1) {
                if ($read -lt 16) { throw 'Truncated MP4 box header.' }
                $size = [uint64]0
                for ($i = 8; $i -lt 16; ++$i) { $size = $size * 256 + $header[$i] }
                $headerSize = 16
            }
            if ($size -eq 0) { $size = [uint64]($stream.Length - $position) }
            if ($size -lt $headerSize -or $size -gt [uint64]($stream.Length - $position)) { throw "Invalid MP4 box size: $type" }
            if ($type -eq 'moov') {
                if ($size -gt 268435456) { throw 'moov box is unreasonably large.' }
                $moovBytes = [byte[]]::new([int]$size)
                $stream.Position = $position
                [void]$stream.Read($moovBytes, 0, $moovBytes.Length)
                break
            }
            $position += [long]$size
        }
        if ($null -eq $moovBytes) { throw 'moov box not found.' }
        $text = [Text.Encoding]::GetEncoding(28591).GetString($moovBytes)
        if ($text.IndexOf($tag, [StringComparison]::Ordinal) -lt 0) {
            throw "Video codec configuration box $tag not found for $Codec."
        }
        [pscustomobject]@{ codec_config_box = $tag; status = 'passed' }
    }
    finally { $stream.Dispose() }
}
