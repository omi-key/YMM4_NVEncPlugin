# 出力ファイルの検証

NVEnc プラグインで書き出した MP4 が、コンテナ構造とデコードの両面で壊れていないかを確認するための手順です。GPU ベンダーに依存せず、書き出し済みのファイルだけを対象にします。

## 必要なもの

- PowerShell 7 以降 (`pwsh`) または Windows PowerShell
- `ffmpeg` と `ffprobe` が `PATH` にあること (`Validate-Output.ps1` のみ)

## 実行方法

### 書き出しファイルの検証

```powershell
./scripts/Validate-Output.ps1 `
    -Output ./out.mp4 `
    -Codec h264 `
    -Width 1920 -Height 1080 -Fps 60 `
    -ExpectedFrames 600 `
    -ExpectAudio
```

- `-Codec` は `h264` / `hevc` / `av1` のいずれかです。
- `-ExpectedFrames` は、書き出した範囲の期待フレーム数です。
- `-ExpectAudio` を付けると音声トラックの検査も行います。
- 任意指定: `-AudioSampleRate` / `-AudioChannels` (プロジェクトの音声設定に合わせて指定。省略時は検査しない)、`-MaxAudioDeltaMs` (既定 50)、`-ExpectBt709Limited` (色メタデータの検査。プラグインがタグ付けを保証するものではないため既定ではオフ)。
- `-Result <json>` を指定すると、既存の JSON ファイルに `validation` オブジェクトを追記します。省略時はファイルを書き換えません。

検査に失敗すると例外で終了し、終了コードは 0 以外になります。成功時は `Output validation passed: ...` を表示します。

### ヘッダー検査の回帰テスト

```powershell
pwsh -NoProfile -File ./tests/Test-Mp4Timing.ps1
```

細工した MP4 ヘッダーだけを使うため、GPU・ffmpeg・エンコーダーは不要です。成功時は `MP4 timing regression checks: N passed` を表示します。

## 検査内容

`Validate-Output.ps1`:

1. **コーデック設定ボックス**: moov 内に `avcC` (H.264) / `hvcC` (HEVC) / `av1C` (AV1) があること。
2. **MP4 ヘッダーの時間単位** (`-ExpectAudio` 時): 音声トラックの `tkhd` の長さが、`mdhd` の長さを `mvhd` のタイムスケールへ換算した値と一致すること。`tkhd` は movie のタイムスケール、`mdhd` / `stts` は音声クロックで表すため、音声サンプル数をそのまま `tkhd` に入れた場合 (コミット a8be3df で修正した不具合) を検出します。あわせて `stts` の合計が `mdhd` の長さと一致すること、編集リスト (`edts`) が無いことを確認します。
3. **ffprobe**: 映像ストリームが 1 本であること、コーデック名、解像度、フレームレート (`Fps/1`)、`-count_frames` によるデコード済みフレーム数が期待値と一致すること。
4. **全体デコード**: `ffmpeg -f null` で全ストリームをデコードし、エラーが出ないこと。
5. **音声** (`-ExpectAudio` 時): ストリームが 1 本で AAC であること、指定時はサンプルレート・チャンネル数、映像と音声の長さの差が上限以内であること、ヘッダー由来の音声長と ffprobe の音声長が 1 ms 以内で一致すること。
6. **色メタデータ** (`-ExpectBt709Limited` 時): `color_range=tv`、`color_space` / `color_transfer` / `color_primaries` が `bt709` であること。

ヘッダー検査は `scripts/Mp4Timing.Common.ps1` の `Assert-Mp4AudioTrackTiming` と `Assert-Mp4VideoCodecConfig` が担当します。

## 限界

- **音声と映像の同期は証明しません。** 確認するのは長さの差とヘッダーの整合性だけで、開始位置のずれや途中のドリフトは検出できません。
- **画質は評価しません。** デコードできることとフレーム数が合うことだけを見ます。映像の見た目やフレームの内容・順序は検査しません。
- 音声の検査対象は、このプラグインが出力する編集リスト無しの MP4 です。他のツールで編集したファイルは `edts` により拒否される場合があります。
- ビデオトラックの `tkhd` / `mdhd` は検査していません。
- 色メタデータの検査は、ファイルに付いたタグの確認にとどまります。
- Linux 上など、実際の NVEnc 出力が無い環境では、`tests/Test-Mp4Timing.ps1` によるヘッダー検査の回帰確認のみ可能です。
