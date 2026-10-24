using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace NRLiveUI;

// Edits the [FPS] section of scaleconfig.ini (the performance HUD). Keys and
// defaults mirror include/hud_config.h in NRLive.
public partial class HudSettingsWindow : Window
{
    private const string Section = "FPS";
    private readonly string _ini;
    private bool _loading;
    private int _preset = 3;

    private static readonly (string Key, string Label, bool Default)[] Items =
    {
        ("title", "Title", true), ("fps", "FPS", true), ("fps_detail", "Source / generated FPS", true),
        ("frametime", "Frametime", true), ("frame_timing", "Frametime graph", true), ("lows", "1% / 0.1% lows", true),
        ("gpu_load", "GPU load", true), ("gpu_clock", "GPU clock", false), ("gpu_temp", "GPU temperature", true),
        ("vram", "VRAM", true), ("cpu_load", "CPU load", true), ("ram", "RAM", false),
        ("pipeline", "FSR / NR / FG state", true), ("gpu_times", "GPU pass times", false), ("latency", "NRLive latency", false),
        ("resolution", "Resolution in / out", true), ("clock", "Clock", false),
    };

    private static readonly (string Key, string Label, int Min, int Max, int Default, string Unit)[] Sliders =
    {
        ("scale", "Scale", 50, 250, 100, "%"), ("opacity", "Opacity", 20, 100, 100, "%"),
        ("background_alpha", "Background", 0, 100, 80, "%"), ("graph_samples", "Graph history", 30, 1000, 200, " fr"),
        ("sample_ms", "Refresh", 100, 2000, 500, " ms"),
    };

    private readonly Dictionary<string, CheckBox> _items = new();
    private readonly Dictionary<string, Slider> _sliders = new();

    public HudSettingsWindow(string iniPath)
    {
        InitializeComponent();
        _ini = iniPath;
        foreach (var (key, label, _) in Items)
        {
            var box = new CheckBox { Content = label, Width = 172, Margin = new Thickness(0, 0, 0, 10) };
            box.Click += (_, _) => { if (!_loading) { _preset = 0; UpdatePresetHint(); } };
            _items[key] = box;
            ItemsPanel.Children.Add(box);
        }
        for (int i = 0; i < Sliders.Length; i++)
        {
            var (key, label, min, max, _, unit) = Sliders[i];
            SliderGrid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(34) });
            var text = new TextBlock { Text = label, VerticalAlignment = VerticalAlignment.Center, Foreground = (Brush)FindResource("MutedBrush") };
            var slider = new Slider { Minimum = min, Maximum = max, IsSnapToTickEnabled = true, TickFrequency = key == "graph_samples" ? 10 : key == "sample_ms" ? 50 : 5, VerticalAlignment = VerticalAlignment.Center };
            var value = new TextBlock { VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Right };
            slider.ValueChanged += (_, _) => value.Text = ((int)slider.Value).ToString(CultureInfo.InvariantCulture) + unit;
            Grid.SetRow(text, i); Grid.SetRow(slider, i); Grid.SetRow(value, i);
            Grid.SetColumn(slider, 1); Grid.SetColumn(value, 2);
            SliderGrid.Children.Add(text); SliderGrid.Children.Add(slider); SliderGrid.Children.Add(value);
            _sliders[key] = slider;
        }
        LayoutBox.SelectionChanged += (_, _) => { if (!_loading) { _preset = 0; UpdatePresetHint(); } };
        foreach (var (box, swatch) in new[] { (AccentBox, AccentSwatch), (TextColorBox, TextSwatch), (GraphColorBox, GraphSwatch) })
            box.TextChanged += (_, _) => swatch.Background = TryColor(box.Text, out var c) ? new SolidColorBrush(c) : Brushes.Transparent;
        Load();
    }

    private void Load()
    {
        _loading = true;
        foreach (var (key, _, def) in Items) _items[key].IsChecked = ReadInt(key, def ? 1 : 0) != 0;
        int legacyFont = ReadInt("font_size", -1);
        foreach (var (key, _, min, max, def, _) in Sliders)
        {
            int fallback = key == "scale" && legacyFont > 0 ? legacyFont * 100 / 24 : def;
            _sliders[key].Value = Math.Clamp(ReadInt(key, fallback), min, max);
        }
        LayoutBox.SelectedIndex = Math.Clamp(ReadInt("layout", 0), 0, 1);
        PositionBox.SelectedIndex = Math.Clamp(ReadInt("position", 0), 0, 5);
        BackgroundBox.IsChecked = ReadInt("background", 1) != 0;
        OutlineBox.IsChecked = ReadInt("text_outline", 1) != 0;
        EnabledBox.IsChecked = ReadInt("enabled", 1) != 0;
        AccentBox.Text = ReadString("accent_color", "#ED1C24");
        TextColorBox.Text = ReadString("text_color", "#F0F0F2");
        GraphColorBox.Text = ReadString("graph_color", "#ED1C24");
        ToggleKeyBox.Text = ReadString("toggle_key", "shift+f12");
        PresetKeyBox.Text = ReadString("preset_key", "shift+f10");
        _preset = Math.Clamp(ReadInt("preset", 3), 0, 4);
        _loading = false;
        UpdatePresetHint();
        StatusLine.Text = File.Exists(_ini) ? _ini : "New file: " + _ini;
    }

    private void Preset_Click(object sender, RoutedEventArgs e)
    {
        int p = int.Parse((string)((Button)sender).Tag, CultureInfo.InvariantCulture);
        bool full = p == 4, standard = p >= 3, compact = p == 2;
        var on = new Dictionary<string, bool>
        {
            ["fps"] = true, ["title"] = standard, ["fps_detail"] = standard, ["frametime"] = standard || compact,
            ["frame_timing"] = standard, ["lows"] = standard, ["gpu_load"] = standard || compact,
            ["gpu_temp"] = standard || compact, ["vram"] = standard, ["cpu_load"] = standard || compact,
            ["pipeline"] = standard || compact, ["resolution"] = standard, ["gpu_clock"] = full, ["ram"] = full,
            ["gpu_times"] = full, ["latency"] = full, ["clock"] = full,
        };
        _loading = true;
        foreach (var (key, value) in on) _items[key].IsChecked = value;
        LayoutBox.SelectedIndex = compact ? 1 : 0;
        _loading = false;
        _preset = p;
        UpdatePresetHint();
    }

    private void UpdatePresetHint()
    {
        string[] names = { "Custom", "Minimal", "Compact", "Standard", "Full" };
        PresetHint.Text = $"Current: {names[_preset]}. Presets set the items and orientation; position, scale and colours stay as they are.";
        var buttons = new[] { PresetMinimal, PresetCompact, PresetStandard, PresetFull };
        for (int i = 0; i < buttons.Length; i++)
            buttons[i].BorderBrush = (Brush)FindResource(i + 1 == _preset ? "AccentBrush" : "BorderBrush");
    }

    private void Save_Click(object sender, RoutedEventArgs e)
    {
        foreach (var box in new[] { AccentBox, TextColorBox, GraphColorBox })
            if (!TryColor(box.Text, out _)) { StatusLine.Text = $"\"{box.Text}\" is not a #RRGGBB colour."; box.Focus(); return; }
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(_ini)!);
            foreach (var (key, _, _) in Items) Write(key, _items[key].IsChecked == true ? "1" : "0");
            foreach (var (key, _, _, _, _, _) in Sliders) Write(key, ((int)_sliders[key].Value).ToString(CultureInfo.InvariantCulture));
            Write("layout", LayoutBox.SelectedIndex.ToString(CultureInfo.InvariantCulture));
            Write("position", PositionBox.SelectedIndex.ToString(CultureInfo.InvariantCulture));
            Write("background", BackgroundBox.IsChecked == true ? "1" : "0");
            Write("text_outline", OutlineBox.IsChecked == true ? "1" : "0");
            Write("enabled", EnabledBox.IsChecked == true ? "1" : "0");
            Write("accent_color", AccentBox.Text.Trim().ToUpperInvariant());
            Write("text_color", TextColorBox.Text.Trim().ToUpperInvariant());
            Write("graph_color", GraphColorBox.Text.Trim().ToUpperInvariant());
            Write("toggle_key", ToggleKeyBox.Text);
            Write("preset_key", PresetKeyBox.Text);
            Write("preset", _preset.ToString(CultureInfo.InvariantCulture));
            WritePrivateProfileString(Section, "font_size", null, _ini); // superseded by scale
            StatusLine.Text = "Saved " + DateTime.Now.ToString("HH:mm:ss", CultureInfo.InvariantCulture) + " to " + _ini;
        }
        catch (Exception ex) { StatusLine.Text = "Could not save: " + ex.Message; }
    }

    private void KeyBox_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        e.Handled = true;
        Key key = e.Key == Key.System ? e.SystemKey : e.Key;
        if (key is Key.LeftCtrl or Key.RightCtrl or Key.LeftAlt or Key.RightAlt or Key.LeftShift or Key.RightShift or Key.LWin or Key.RWin)
            return;
        string raw = key.ToString();
        string name = raw.Length == 1 && char.IsLetter(raw[0]) ? raw.ToLowerInvariant()
            : raw.Length == 2 && raw[0] == 'D' && char.IsDigit(raw[1]) ? raw[1].ToString()
            : raw.Length >= 2 && raw[0] == 'F' && int.TryParse(raw[1..], out int f) && f is >= 1 and <= 24 ? "f" + f
            : key switch
            {
                Key.Insert => "insert", Key.Home => "home", Key.End => "end", Key.PageUp => "pageup", Key.PageDown => "pagedown",
                Key.Delete => "delete", Key.Pause => "pause", Key.Scroll => "scrolllock", Key.Space => "space", Key.Tab => "tab",
                _ => ""
            };
        if (name.Length == 0) { StatusLine.Text = "Use a letter, digit, F1-F24, Insert/Home/End/PageUp/PageDown/Delete/Pause/ScrollLock, with optional modifiers."; return; }
        var parts = new List<string>();
        if ((Keyboard.Modifiers & ModifierKeys.Control) != 0) parts.Add("ctrl");
        if ((Keyboard.Modifiers & ModifierKeys.Alt) != 0) parts.Add("alt");
        if ((Keyboard.Modifiers & ModifierKeys.Shift) != 0) parts.Add("shift");
        if ((Keyboard.Modifiers & ModifierKeys.Windows) != 0) parts.Add("win");
        parts.Add(name);
        ((TextBox)sender).Text = string.Join("+", parts);
    }

    private void Close_Click(object sender, RoutedEventArgs e) => Close();

    private static bool TryColor(string text, out Color color)
    {
        color = default;
        string t = text.Trim().TrimStart('#');
        if (t.Length != 6 || !uint.TryParse(t, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out uint v)) return false;
        color = Color.FromRgb((byte)(v >> 16), (byte)(v >> 8), (byte)v);
        return true;
    }

    private int ReadInt(string key, int def) => (int)GetPrivateProfileInt(Section, key, def, _ini);

    private string ReadString(string key, string def)
    {
        var sb = new StringBuilder(128);
        GetPrivateProfileString(Section, key, def, sb, sb.Capacity, _ini);
        return sb.ToString();
    }

    private void Write(string key, string value)
    {
        if (!WritePrivateProfileString(Section, key, value, _ini))
            throw new IOException($"WritePrivateProfileString failed ({Marshal.GetLastWin32Error()})");
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern uint GetPrivateProfileInt(string section, string key, int def, string file);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetPrivateProfileString(string section, string key, string def, StringBuilder ret, int size, string file);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool WritePrivateProfileString(string section, string key, string? value, string file);
}
