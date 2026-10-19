using System.Windows;
using System.Windows.Controls;

namespace NVEncVideoWriterPlugin;

internal sealed class NvencConfigView : UserControl
{
    private readonly ComboBox _codecComboBox;
    private readonly ComboBox _rateControlComboBox;
    private readonly TextBox _bitrateTextBox;
    private readonly ComboBox _qualityComboBox;
    private readonly ComboBox _texturePoolComboBox;
    private readonly CheckBox _hevcAsyncCheckBox;
    private readonly CheckBox _debugLogCheckBox;
    private readonly CheckBox _gpuDirectCheckBox;
    private readonly NvencSettings _settings;

    public NvencConfigView(NvencSettings settings)
    {
        _settings = settings;
        var panel = new StackPanel
        {
            Margin = new Thickness(8),
        };

        panel.Children.Add(new TextBlock
        {
            Text = "コーデック",
            Margin = new Thickness(0, 0, 0, 4),
        });

        _codecComboBox = new ComboBox
        {
            Margin = new Thickness(0, 0, 0, 12),
            ItemsSource = new[] { "H.264", "H.265 (HEVC)", "AV1" },
            SelectedIndex = _settings.Codec switch
            {
                NvencCodec.H265 => 1,
                NvencCodec.AV1 => 2,
                _ => 0,
            },
        };
        panel.Children.Add(_codecComboBox);

        _hevcAsyncCheckBox = new CheckBox
        {
            Content = "H.265 安定性重視（遅い）",
            IsChecked = !_settings.HevcAsync,
            IsEnabled = _settings.Codec == NvencCodec.H265,
            Margin = new Thickness(0, 0, 0, 12),
        };
        _hevcAsyncCheckBox.Checked += (_, _) => _settings.HevcAsync = false;
        _hevcAsyncCheckBox.Unchecked += (_, _) => _settings.HevcAsync = true;
        panel.Children.Add(_hevcAsyncCheckBox);
        
        _codecComboBox.SelectionChanged += (_, _) =>
        {
            _settings.Codec = _codecComboBox.SelectedIndex switch
            {
                1 => NvencCodec.H265,
                2 => NvencCodec.AV1,
                _ => NvencCodec.H264,
            };
            _hevcAsyncCheckBox.IsEnabled = _settings.Codec == NvencCodec.H265;
            if (_settings.Codec != NvencCodec.H265)
            {
                return;
            }

            _settings.HevcAsync = !_hevcAsyncCheckBox.IsChecked.GetValueOrDefault(false);
        };

        _debugLogCheckBox = new CheckBox
        {
            Content = "デバッグログを書き出す",
            IsChecked = _settings.EnableDebugLog,
            Margin = new Thickness(0, 0, 0, 12),
        };
        _debugLogCheckBox.Checked += (_, _) => _settings.EnableDebugLog = true;
        _debugLogCheckBox.Unchecked += (_, _) => _settings.EnableDebugLog = false;
        panel.Children.Add(_debugLogCheckBox);

        _gpuDirectCheckBox = new CheckBox
        {
            Content = "GPU直渡し（実験・IVideoFileWriter3）",
            IsChecked = _settings.EnableGpuDirectInput,
            ToolTip = "ON: CPU 読み取り用ビットマップへの中間コピーを省き、描画済みの GPU フレームを直接受け取ります。"
                + "プラグイン側の入力テクスチャプールへのコピーは維持します。速度向上は未検証です。"
                + "設定は次に開始する出力に適用されます。",
            Margin = new Thickness(0, 0, 0, 12),
        };
        _gpuDirectCheckBox.Checked += (_, _) => _settings.EnableGpuDirectInput = true;
        _gpuDirectCheckBox.Unchecked += (_, _) => _settings.EnableGpuDirectInput = false;
        panel.Children.Add(_gpuDirectCheckBox);

        panel.Children.Add(new TextBlock
        {
            Text = "ビットレート方式",
            Margin = new Thickness(0, 0, 0, 4),
        });

        _rateControlComboBox = new ComboBox
        {
            Margin = new Thickness(0, 0, 0, 12),
            ItemsSource = new[] { "固定 (CBR)", "可変 (VBR)", "自動 (YouTube 推奨)" },
            SelectedIndex = _settings.RateControl switch
            {
                NvencRateControl.Variable => 1,
                NvencRateControl.YouTubeRecommended => 2,
                _ => 0,
            },
        };
        panel.Children.Add(_rateControlComboBox);

        panel.Children.Add(new TextBlock
        {
            Text = "出力品質",
            Margin = new Thickness(0, 0, 0, 4),
        });

        _qualityComboBox = new ComboBox
        {
            Margin = new Thickness(0, 0, 0, 12),
            ItemsSource = new[] { "高速", "標準", "高品質" },
            SelectedIndex = (int)_settings.Quality,
        };
        _qualityComboBox.SelectionChanged += (_, _) =>
        {
            _settings.Quality = (NvencQuality)Math.Clamp(_qualityComboBox.SelectedIndex, 0, 2);
        };
        panel.Children.Add(_qualityComboBox);

        const string texturePoolTip = "NVENC に渡す入力テクスチャ（GPU メモリ上のフレームバッファ）の枚数です。"
            + "大きいほどエンコード待ちを吸収しやすくなりますが、その分 GPU メモリを多く消費します。";
        panel.Children.Add(new TextBlock
        {
            Text = "入力テクスチャプール",
            Margin = new Thickness(0, 0, 0, 4),
            ToolTip = texturePoolTip,
        });

        _texturePoolComboBox = new ComboBox
        {
            Margin = new Thickness(0, 0, 0, 12),
            ItemsSource = new[] { "4", "6（標準）", "8" },
            ToolTip = texturePoolTip,
            SelectedIndex = _settings.TexturePoolSize switch
            {
                4 => 0,
                8 => 2,
                _ => 1,
            },
        };
        _texturePoolComboBox.SelectionChanged += (_, _) =>
        {
            _settings.TexturePoolSize = _texturePoolComboBox.SelectedIndex switch
            {
                0 => 4,
                2 => 8,
                _ => 6,
            };
        };
        panel.Children.Add(_texturePoolComboBox);

        panel.Children.Add(new TextBlock
        {
            Text = "ビットレート（kbps）",
            Margin = new Thickness(0, 0, 0, 4),
        });

        _bitrateTextBox = new TextBox
        {
            Text = _settings.BitrateKbps.ToString(),
            Margin = new Thickness(0, 0, 0, 8),
            IsEnabled = _settings.RateControl != NvencRateControl.YouTubeRecommended,
        };
        _rateControlComboBox.SelectionChanged += (_, _) =>
        {
            _settings.RateControl = _rateControlComboBox.SelectedIndex switch
            {
                1 => NvencRateControl.Variable,
                2 => NvencRateControl.YouTubeRecommended,
                _ => NvencRateControl.Fixed,
            };
            if (_settings.RateControl == NvencRateControl.YouTubeRecommended)
            {
                _bitrateTextBox.IsEnabled = false;
                return;
            }

            _bitrateTextBox.IsEnabled = true;
            _bitrateTextBox.Text = _settings.BitrateKbps.ToString();
        };
        _bitrateTextBox.TextChanged += (_, _) =>
        {
            if (int.TryParse(_bitrateTextBox.Text, out var value))
            {
                _settings.BitrateKbps = Math.Clamp(value, 100, 200000);
            }
        };
        panel.Children.Add(_bitrateTextBox);

        Content = panel;
    }
}
