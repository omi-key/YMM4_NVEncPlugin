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

## 時系列（`timeline`）

合計値だけでは長い出力の「どの部分が遅いか」が分かりません。そこで、最初の映像フレームを受け取った時刻を0として、実時間（wall time）を一定幅の区間（バケット）に区切った記録を `timeline` 配列に出力します。

- 幅は既定10秒。環境変数 `NVENC_PROFILE_BUCKET_SEC`（1〜300、小数可）で変更できます。範囲外は1〜300に丸め、数値でない場合は10秒です。YMM4を起動する前に設定してください。
- バケット数の上限は20000です。超えた分は記録せず、`timeline_info.truncated` が `true` になります（上限に達した時点以降の区間は欠けます。幅を広げてください）。
- バケットは映像フレームが届いた時点で切り替わります。フレームが1枚も届かなかった区間も、`frames`=0のバケットとして出力します。
- 最後のバケットは出力終了時に確定し、`partial: true` が付きます（幅が短いので `timeline_summary` の集計からは除外されます）。

各要素の項目:

| 項目 | 意味 |
| --- | --- |
| `bucket` | バケット番号（0始まり）。 |
| `wall_start_s` / `wall_end_s` | 最初の映像フレームからの経過秒。 |
| `frame_index_start` | そのバケットで最初に受け取った映像フレームの番号（0始まり、プラグインが受け取った順）。 |
| `frames` / `fps` | そのバケットで受け取った映像フレーム数と、`frames`÷実時間秒（部分バケットは実際の長さで割る）。 |
| `video_callback_ms` / `audio_callback_ms` | `WriteVideo` / `WriteAudio`のコールバック時間の合計（ロック待ちを含む）。 |
| `native_video_ms` | ネイティブの`NvencEncode`呼び出し時間の合計。 |
| `outside_callbacks_ms` | バケットの実時間からコールバック和集合時間を引いた値。YMM4側の描画・待ちが主に含まれますが近似です（下記）。 |
| `slot_wait_ms` / `encode_picture_ms` / `output_event_wait_ms` / `lock_bitstream_ms` / `bitstream_mux_ms` | ネイティブ計測の各区間について、バケット境界のスナップショット間の差分（ms）。意味は上の`native.stages`と同じです。破棄モードやスナップショット取得に失敗した場合は項目自体がありません。 |

`timeline_info` にはバケット幅・数・`truncated`、`timeline_summary` には満了バケットだけの `min_fps` / `median_fps` / `max_fps` と、fpsが低い順の `slowest_buckets`（最大5件。`bucket`・`frame_index_start`・`frame_index_end`・`frames`・`fps`）が入ります。

近似についての注意:

- コールバック時間は「呼び出しが終わった時刻」のバケットに加算します。バケット境界をまたぐ呼び出しは、片方に寄ります。
- ネイティブ差分は、境界を越えた直後の最初の映像フレームの処理中（エンコード呼び出し後）にスナップショットを1回取り、直前に閉じたバケットへ割り当てます。ネイティブ側は非同期なので、実際にはわずかにずれます。最後の部分バケットには終了処理（キュー排出）分も含まれます。
- 開始時刻は映像フレームの最初の受信です。それより前の音声コールバックは時系列に入りません。
- 計測のためのスナップショットはバケット境界ごとに1回（既定で10秒に1回）だけで、通常の負荷にはほぼ影響しません。

使い方:

1. `timeline_summary.slowest_buckets` で遅い区間の `frame_index_start`〜`frame_index_end` を確認します。
2. YMM4上の位置に直すには、`frame_index` を fps で割って秒にし、YMM4で選択した出力範囲の開始位置を足します。例: 60fpsで範囲開始が0:00なら、`frame_index_start`=54000 は 54000÷60=900秒＝15:00 付近です。範囲を指定している場合は、範囲開始＋900秒です。
3. `timeline` の遅いバケットを前後と比べ、`slot_wait_ms`（出力側の詰まり）や`output_event_wait_ms`（GPUエンコード待ち）が増えているのか、`outside_callbacks_ms`（YMM4側の描画・読み込み）が増えているのかを見ます。`outside_callbacks_ms` が支配的ならエンコーダーではなく、その区間のプロジェクトの内容（重いエフェクト、動画素材等）が遅い原因の候補です。
4. 破棄モードでも同じ `timeline` が取れるため（ネイティブ差分なし）、エンコード抜きの上流速度の区間比較ができます。

## 解釈の注意

- 破棄ではエンコード負荷だけでなくGPU競合やバックプレッシャーも無くなり、上流の挙動が変わります。差を「純粋なエンコード時間」「GPU性能の限界」と扱わないでください。
- `slot_wait`が長い場合は、NVENCだけでなく出力・mux・ファイルI/Oの混雑も疑ってください。
- `host_completion`は常に`unknown_no_completion_signal`です。キャンセルや中断した出力のデータは速度比較に使わないでください。
- JSONの保存失敗は、動画処理の別のエラーがすでにあれば元のエラーを優先します。
- 絶対パスは設定に含めませんが、エラー文に含まれる場合があります。共有前に確認してください。
