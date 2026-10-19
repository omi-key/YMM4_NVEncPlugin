# 破棄モードとプロファイリング

最適化の前に「YMM4からフレームが届くまで」と「プラグイン内の待ち・処理」を切り分けるための診断機能です。通常出力の画質や設定を変えるものではありません。どちらも既定でオフで、設定は次に開始する出力から有効になります。

- **プロファイリング結果を書き出す（集計JSON）**: 終了時に `<出力名>.nvenc_profile.json` を書き出します。回数・合計・平均・最大時間だけを保持し、フレームごとの記録は持ちません。同名JSONは置き換えます。デバッグログ（`.nvenc_log.txt` 等）とは独立です。
- **計測専用：映像・音声を破棄（MP4を作成しない）**: 映像・音声を受け取った時点で捨てます。D3Dテクスチャの取得、NVENCの初期化・コピー・エンコード、AAC、MP4保存を行いません。既存のMP4も更新されないため、必ず新しい出力名を使ってください。

## 比較手順

1. 同じビルドの `NVEncPlugin.dll` と `NvencNative.dll` を配置します。
2. 同じプロジェクト・範囲・解像度・fps・コーデック・ビットレート・品質・プール枚数で比較します。
3. プロファイリングのみオンにして通常出力（例：`通常1.mp4`）、次に破棄もオンにして別名（例：`破棄1.mp4`）で出力します。
4. 各JSONを比較します。順序を交互にして数回測り、ウォームアップを除いてください。
5. 計測自体の負荷は、通常出力でプロファイリングのオフ・オンを別に比較します。調査後は両オプションをオフに戻してください。

破棄モードでも、GPU直渡しOFFではYMM4側の入力前コピーは残ります。

## JSONの読み方

トップレベルの主な項目は次のとおりです。

| 項目 | 意味 |
| --- | --- |
| `mode` / `status` | `encode`または`discard`。例外を観測した場合は`failed`と最初のエラーを記録します。 |
| `configuration` | プラグイン版・解像度・fps・コーデック・品質・ビットレート方式・目標ビットレート・要求プール枚数・`gpu_direct_input_enabled`等。 |
| `effective_pool_size` | ネイティブ側の実効プール枚数（破棄時は`null`）。 |
| `input_delivery_path` | `gpu_direct_IVideoFileWriter3` / `cpu_read_bitmap_IVideoFileWriter2` / `not_observed`。 |
| `metrics` | 受け取った映像枚数・音声サンプル数、`delivery_span_ms`、`callback_union_ms`、`delivery_outside_callbacks_ms`等。`received_fps`は検証済み出力fpsではありません。 |
| `managed_stages` | 管理側の区間（下表）。 |
| `native_status` / `native` | ネイティブ計測の状態と、Destroy直前に取得したスナップショット。 |

管理側の区間（`managed_stages`）:

| 区間 | 内容 |
| --- | --- |
| `video_callback` / `audio_callback` / `cpu_video_callback` | 各コールバック全体。 |
| `texture_access` | `Surface`と`ID3D11Texture2D`の取得。 |
| `video_lock_wait` / `audio_lock_wait` | 映像・音声共通ロックの取得待ち。 |
| `encoder_initialize` | 初回のネイティブ作成（先行音声の処理を含む）。 |
| `native_video` / `native_audio` | ネイティブ呼び出し。 |
| `native_finalize` / `native_destroy` / `dispose` | 終了処理。 |

ネイティブの区間（`native.stages`）。すべてCPUのwall-timeです。

| 区間 | 内容 |
| --- | --- |
| `slot_wait` | 空き入力テクスチャスロットの確保待ち（出力側が詰まると増えます）。 |
| `copy_resource_cpu` | `CopyResource`を発行するCPU時間。GPU上のコピー完了時間ではありません。 |
| `map_input` / `encode_picture` | `nvEncMapInputResource` / `nvEncEncodePicture`の呼び出し時間。 |
| `output_event_wait` | 出力スレッドが非同期完了イベントを待った時間（GPUエンコード完了待ちを含み、アイドル待ちも含みます）。同期モードでは0件です。 |
| `lock_bitstream` | `nvEncLockBitstream`の呼び出し。同期モードではエンコード完了待ちを含みます。 |
| `bitstream_mux` | ビットストリームの解析とMP4書き込みキュー投入（mux排他中）。 |
| `audio_write` | 音声API全体（ロック待ち・PCM変換・AAC処理を含む）。 |
| `writer_sample` | 書き込みスレッドのサンプル書き込み。物理書き込み完了時間そのものではありません。 |
| `finalize` / `mp4_finalize` | ネイティブ終了処理全体と、AAC flush・writer join・MP4末尾処理（入れ子）。 |
| `slot_residence` | スロット確保から対応出力を処理するまで。 |

区間は入れ子・非同期で重なります。合計して全体時間の内訳にしないでください。`submitted_frames` / `completed_frames`はネイティブが受理・出力処理した枚数です。ネイティブ計測は初期化後に開始し、`NvencNative.dll`が古い場合は`native_status`に`unavailable`と記録するか、計測開始時にエラーになります。

## 解釈の注意

- 破棄ではエンコード負荷だけでなくGPU競合やバックプレッシャーも無くなり、上流の挙動が変わります。差を「純粋なエンコード時間」「GPU性能の限界」と扱わないでください。
- `slot_wait`が長い場合は、NVENCだけでなく出力・mux・ファイルI/Oの混雑も疑ってください。
- `host_completion`は常に`unknown_no_completion_signal`です。キャンセルや中断した出力のデータは速度比較に使わないでください。
- JSONの保存失敗は、動画処理の別のエラーがすでにあれば元のエラーを優先します。
- 絶対パスは設定に含めませんが、エラー文に含まれる場合があります。共有前に確認してください。
