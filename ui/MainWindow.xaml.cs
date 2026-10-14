using Microsoft.Win32;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net.Http;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Input;
using System.Windows.Threading;

namespace NRLiveUI;

public partial class MainWindow : Window
{
    private const int ToggleHotkeyId = 0x4E52;
    private const uint ModAlt = 0x0001, ModControl = 0x0002, ModShift = 0x0004, ModWin = 0x0008, ModNoRepeat = 0x4000;
    private const int WmHotkey = 0x0312;
    private static readonly HttpClient UpdateHttp = CreateUpdateHttpClient();
    private static readonly HttpClient UpdateDownloadHttp = CreateUpdateDownloadHttpClient();
    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromSeconds(1) };
    private HwndSource? _source;
    private Process? _process;
    private int _secondsLeft;
    private bool _loadingProfile;
    private bool _initialized;
    private bool _registeredHotkey;
    private string _currentProfile = "Default";
    private string Root => AppContext.BaseDirectory;
    private string VersionPath => Path.Combine(Root, "version.txt");
    private string ProfilesDir => Path.Combine(Root, "profiles");
    private string UiConfigPath => Path.Combine(Root, "ui_config.json");
    private string ProfilePath(string name) => Path.Combine(ProfilesDir, SafeName(name) + ".json");

    private sealed class Profile
    {
        public string target_mode { get; set; } = "picker";
        public string target_text { get; set; } = "";
        public int delay { get; set; } = 5;
        public bool no_overlay { get; set; }
        public string motion { get; set; } = "fast";
        public string capture_mode { get; set; } = "wgc";
        public int? schema_version { get; set; }
        public bool run_as_admin { get; set; } = true;
        public string scale_hotkey { get; set; } = "ctrl+alt+s";
        public string stop_key { get; set; } = "ctrl+shift+a";
        public string overlay_key { get; set; } = "ctrl+home";
        public string bind_bypass { get; set; } = "home,insert,end,pageup,pagedown";
        public string exe_path { get; set; } = "";
    }

    public MainWindow()
    {
        InitializeComponent();
        _timer.Tick += Timer_Tick;
        TargetModeBox.SelectionChanged += (_, _) => UpdatePreview();
        MotionBox.SelectionChanged += (_, _) => UpdatePreview();
        CaptureModeBox.SelectionChanged += (_, _) => UpdatePreview();
        DelayBox.TextChanged += (_, _) => UpdatePreview();
        TargetTextBox.TextChanged += (_, _) => UpdatePreview();
        StopHotkeyBox.TextChanged += (_, _) => UpdatePreview();
        OverlayHotkeyBox.TextChanged += (_, _) => UpdatePreview();
        BindBypassBox.TextChanged += (_, _) => UpdatePreview();
        ScaleHotkeyBox.TextChanged += (_, _) => UpdatePreview();
        ExePathBox.TextChanged += (_, _) => UpdatePreview();
        HudCheckBox.Checked += (_, _) => UpdatePreview();
        HudCheckBox.Unchecked += (_, _) => UpdatePreview();
        RunAsAdminCheckBox.Checked += (_, _) => UpdatePreview();
        RunAsAdminCheckBox.Unchecked += (_, _) => UpdatePreview();
    }

    private static string SafeName(string name)
    {
        foreach (char c in Path.GetInvalidFileNameChars()) name = name.Replace(c, '_');
        return string.IsNullOrWhiteSpace(name) ? "Default" : name.Trim();
    }

    private void Window_Loaded(object sender, RoutedEventArgs e)
    {
        Directory.CreateDirectory(ProfilesDir);
        ExePathBox.Text = Path.Combine(Root, "NRLive.exe");
        if (!File.Exists(ProfilePath("Default")))
            File.WriteAllText(ProfilePath("Default"), JsonSerializer.Serialize(new Profile(), new JsonSerializerOptions { WriteIndented = true }));
        var names = Directory.GetFiles(ProfilesDir, "*.json").Select(Path.GetFileNameWithoutExtension).Where(x => x != null).OrderBy(x => x).ToList();
        ProfileBox.ItemsSource = names;
        string preferred = "Default";
        try
        {
            if (File.Exists(UiConfigPath))
            {
                using var doc = JsonDocument.Parse(File.ReadAllText(UiConfigPath));
                if (doc.RootElement.TryGetProperty("last_profile", out var p)) preferred = p.GetString() ?? "Default";
                if (doc.RootElement.TryGetProperty("exe_path", out var exe) && !string.IsNullOrWhiteSpace(exe.GetString()))
                    ExePathBox.Text = exe.GetString()!;
            }
        }
        catch { }
        ProfileBox.SelectedItem = names.Contains(preferred) ? preferred : "Default";
        if (ProfileBox.SelectedItem == null) LoadProfile("Default");
        _initialized = true;
        _source = HwndSource.FromHwnd(new WindowInteropHelper(this).Handle);
        _source?.AddHook(WndProc);
        RegisterToggleHotkey();
        UpdatePreview();
        VersionText.Text = $"v{ReadCurrentVersion()}";
        // Check in the background after the window is ready; don't block startup.
        _ = CheckForUpdatesAsync(manual: false);
    }

    private void ProfileBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loadingProfile || ProfileBox.SelectedItem is not string name) return;
        if (!_initialized) { LoadProfile(name); return; }
        SaveCurrentProfile(false);
        LoadProfile(name);
    }

    private void LoadProfile(string name)
    {
        _loadingProfile = true;
        try
        {
            _currentProfile = name;
            var p = new Profile();
            try { if (File.Exists(ProfilePath(name))) p = JsonSerializer.Deserialize<Profile>(File.ReadAllText(ProfilePath(name))) ?? p; } catch { }
            // Migrate existing profiles once: WGC is the default again, but a
            // user's explicit DXGI choice in a non-default profile is preserved.
            if (p.schema_version == null)
            {
                if (name == "Default") p.capture_mode = "wgc";
                p.schema_version = 1;
                try { File.WriteAllText(ProfilePath(name), JsonSerializer.Serialize(p, new JsonSerializerOptions { WriteIndented = true })); } catch { }
            }
            TargetModeBox.SelectedIndex = p.target_mode switch { "front" => 1, "pid" => 2, "pname" => 3, "window" => 4, _ => 0 };
            TargetTextBox.Text = p.target_text;
            DelayBox.Text = Math.Clamp(p.delay, 1, 30).ToString();
            HudCheckBox.IsChecked = !p.no_overlay;
            MotionBox.SelectedIndex = p.motion == "amdof" ? 1 : 0;
            CaptureModeBox.SelectedIndex = p.capture_mode == "dxgi" ? 1 : 0;
            RunAsAdminCheckBox.IsChecked = p.run_as_admin;
            ScaleHotkeyBox.Text = p.scale_hotkey;
            StopHotkeyBox.Text = p.stop_key;
            OverlayHotkeyBox.Text = p.overlay_key;
            BindBypassBox.Text = string.IsNullOrWhiteSpace(p.bind_bypass) ? "home,insert,end,pageup,pagedown" : p.bind_bypass;
            if (!string.IsNullOrWhiteSpace(p.exe_path)) ExePathBox.Text = p.exe_path;
            ProfileTitle.Text = name == "Default" ? "Scaling setup" : name;
            SidebarHotkey.Text = p.scale_hotkey.Replace("+", " + ").ToUpperInvariant();
        }
        finally { _loadingProfile = false; }
        RegisterToggleHotkey();
        UpdatePreview();
    }

    private Profile ReadProfile() => new()
    {
        target_mode = TargetModeBox.SelectedIndex switch { 1 => "front", 2 => "pid", 3 => "pname", 4 => "window", _ => "picker" },
        target_text = TargetTextBox.Text.Trim(),
        delay = int.TryParse(DelayBox.Text, out int delay) ? Math.Clamp(delay, 1, 30) : 5,
        no_overlay = HudCheckBox.IsChecked != true,
        motion = MotionBox.SelectedIndex == 1 ? "amdof" : "fast",
        capture_mode = CaptureModeBox.SelectedIndex == 1 ? "dxgi" : "wgc",
        run_as_admin = RunAsAdminCheckBox.IsChecked == true,
        scale_hotkey = ScaleHotkeyBox.Text.Trim(),
        stop_key = StopHotkeyBox.Text.Trim(),
        overlay_key = OverlayHotkeyBox.Text.Trim(),
        bind_bypass = BindBypassBox.Text.Trim(),
        exe_path = ExePathBox.Text.Trim()
    };

    private void SaveCurrentProfile(bool notify)
    {
        try
        {
            Directory.CreateDirectory(ProfilesDir);
            File.WriteAllText(ProfilePath(_currentProfile), JsonSerializer.Serialize(ReadProfile(), new JsonSerializerOptions { WriteIndented = true }));
            File.WriteAllText(UiConfigPath, JsonSerializer.Serialize(new { last_profile = _currentProfile, exe_path = ExePathBox.Text.Trim() }, new JsonSerializerOptions { WriteIndented = true }));
            if (notify) FooterStatus.Text = $"Saved profile “{_currentProfile}”.";
        }
        catch (Exception ex) { if (notify) MessageBox.Show(this, ex.Message, "Could not save profile", MessageBoxButton.OK, MessageBoxImage.Error); }
    }


    private static HttpClient CreateUpdateHttpClient()
    {
        var client = new HttpClient { Timeout = TimeSpan.FromSeconds(7) };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("NRLiveUI-Updater/1.0");
        client.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
        return client;
    }

    private static HttpClient CreateUpdateDownloadHttpClient()
    {
        var client = new HttpClient { Timeout = TimeSpan.FromMinutes(20) };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("NRLiveUI-Updater/1.0");
        return client;
    }

    private string ReadCurrentVersion()
    {
        try
        {
            string version = File.ReadAllText(VersionPath).Trim();
            if (!string.IsNullOrWhiteSpace(version)) return version;
        }
        catch { }
        return "0.0.0";
    }

    private async void CheckForUpdates_Click(object sender, RoutedEventArgs e)
        => await CheckForUpdatesAsync(manual: true);

    private async Task CheckForUpdatesAsync(bool manual)
    {
        if (!CheckUpdatesButton.IsEnabled) return;
        CheckUpdatesButton.IsEnabled = false;
        string oldStatus = FooterStatus.Text;
        if (manual) FooterStatus.Text = "Checking for updates…";
        try
        {
            using var response = await UpdateHttp.GetAsync(
                "https://api.github.com/repos/Caliel666/NRLive/releases/latest",
                HttpCompletionOption.ResponseHeadersRead);
            if (response.StatusCode == System.Net.HttpStatusCode.NotFound)
            {
                FooterStatus.Text = manual ? "No NRLive release has been published yet." : oldStatus;
                return;
            }
            response.EnsureSuccessStatusCode();
            using var stream = await response.Content.ReadAsStreamAsync();
            using var release = await JsonDocument.ParseAsync(stream);
            JsonElement root = release.RootElement;
            string tag = root.TryGetProperty("tag_name", out var tagValue) ? tagValue.GetString() ?? "" : "";
            string latestBase = tag.TrimStart('v', 'V').Trim();
            if (!Version.TryParse(latestBase, out Version? latestNumeric))
            {
                FooterStatus.Text = manual ? "Latest GitHub release has no valid version tag." : oldStatus;
                return;
            }

            string current = ReadCurrentVersion();
            bool isWindows10 = current.EndsWith("-win10", StringComparison.OrdinalIgnoreCase);
            string currentBase = isWindows10 ? current[..^"-win10".Length] : current;
            string latest = latestNumeric.ToString() + (isWindows10 ? "-win10" : "");
            Version currentNumeric = Version.TryParse(currentBase, out Version? parsedCurrent)
                ? parsedCurrent
                : new Version(0, 0, 0);

            if (latestNumeric <= currentNumeric)
            {
                if (manual) FooterStatus.Text = $"NRLive is up to date ({current}).";
                return;
            }

            if (!root.TryGetProperty("assets", out JsonElement assets) || assets.ValueKind != JsonValueKind.Array)
                throw new InvalidDataException("The latest release has no downloadable assets.");
            string assetName = isWindows10 ? "NRLiveUI-win10.zip" : "NRLiveUI.zip";
            string? downloadUrl = null;
            foreach (JsonElement asset in assets.EnumerateArray())
            {
                if (asset.TryGetProperty("name", out var name) &&
                    string.Equals(name.GetString(), assetName, StringComparison.OrdinalIgnoreCase) &&
                    asset.TryGetProperty("browser_download_url", out var url))
                {
                    downloadUrl = url.GetString();
                    break;
                }
            }
            if (string.IsNullOrWhiteSpace(downloadUrl))
            {
                FooterStatus.Text = manual
                    ? $"Version {latest} is listed, but {assetName} is not attached to its GitHub release yet."
                    : oldStatus;
                return;
            }

            var answer = MessageBox.Show(this,
                $"NRLive {latest} is available. You are using {current}.\n\nDownload and install the update now?",
                "NRLive update available", MessageBoxButton.YesNo, MessageBoxImage.Information);
            if (answer != MessageBoxResult.Yes)
            {
                FooterStatus.Text = $"Update available: {latest}.";
                return;
            }

            FooterStatus.Text = $"Downloading NRLive {latest}…";
            string tempRoot = Path.Combine(Path.GetTempPath(), "NRLive-update-" + Guid.NewGuid().ToString("N"));
            string zipPath = Path.Combine(tempRoot, assetName);
            string stagePath = Path.Combine(tempRoot, "package");
            Directory.CreateDirectory(stagePath);
            try
            {
                using (var download = await UpdateDownloadHttp.GetAsync(downloadUrl, HttpCompletionOption.ResponseHeadersRead))
                {
                    download.EnsureSuccessStatusCode();
                    await using var source = await download.Content.ReadAsStreamAsync();
                    await using var destination = new FileStream(zipPath, FileMode.CreateNew, FileAccess.Write, FileShare.None);
                    await source.CopyToAsync(destination);
                }

                ZipFile.ExtractToDirectory(zipPath, stagePath);
                if (!File.Exists(Path.Combine(stagePath, "NRLiveUI.exe")) ||
                    !File.Exists(Path.Combine(stagePath, "NRLive.exe")) ||
                    !File.Exists(Path.Combine(stagePath, "version.txt")))
                    throw new InvalidDataException("The downloaded package is missing required NRLive files.");
                string stagedVersion = File.ReadAllText(Path.Combine(stagePath, "version.txt")).Trim();
                if (!string.Equals(stagedVersion, latest, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException($"The package version ({stagedVersion}) does not match the release ({latest}).");

                string scriptPath = Path.Combine(tempRoot, "install-update.ps1");
                string target = Root.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
                string script = "$ErrorActionPreference = 'Stop'\n" +
                    "$target = '" + EscapePowerShell(target) + "'\n" +
                    "$stage = '" + EscapePowerShell(stagePath) + "'\n" +
                    "$pidToWait = " + Environment.ProcessId + "\n" +
                    "try {\n" +
                    "  while (Get-Process -Id $pidToWait -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 400 }\n" +
                    "  Get-ChildItem -LiteralPath $stage -Force | Where-Object { $_.Name -ne 'profiles' -and $_.Name -ne 'ui_config.json' } | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $target -Recurse -Force }\n" +
                    "  Start-Process -FilePath (Join-Path $target 'NRLiveUI.exe') -WorkingDirectory $target\n" +
                    "} catch { $_ | Out-String | Set-Content -LiteralPath (Join-Path $env:TEMP 'NRLive-update-error.txt') }\n" +
                    "Start-Sleep -Seconds 2\nRemove-Item -LiteralPath '" + EscapePowerShell(tempRoot) + "' -Recurse -Force -ErrorAction SilentlyContinue\n";
                await File.WriteAllTextAsync(scriptPath, script);
                Process.Start(new ProcessStartInfo
                {
                    FileName = "powershell.exe",
                    Arguments = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + scriptPath + "\"",
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    WorkingDirectory = Root
                }) ?? throw new InvalidOperationException("Could not start the update installer.");
                FooterStatus.Text = "Update downloaded. Restarting NRLive to install…";
                Close();
            }
            catch
            {
                try { Directory.Delete(tempRoot, recursive: true); } catch { }
                throw;
            }
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException or IOException or InvalidDataException or JsonException or UnauthorizedAccessException)
        {
            FooterStatus.Text = "Couldn't connect to the network.";
            // Non-modal by design: an offline launcher remains fully functional.
        }
        catch
        {
            FooterStatus.Text = "Update check failed; NRLive is still usable.";
        }
        finally
        {
            if (IsLoaded && IsVisible) CheckUpdatesButton.IsEnabled = true;
        }
    }

    private static string EscapePowerShell(string value) => value.Replace("'", "''");

    private void SaveProfile_Click(object sender, RoutedEventArgs e) => SaveCurrentProfile(true);

    private void BrowseExe_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Title = "Select NRLive.exe", Filter = "NRLive executable|NRLive.exe;*.exe|All files|*.*", CheckFileExists = true };
        if (dialog.ShowDialog(this) == true) ExePathBox.Text = dialog.FileName;
    }

    private void HotkeyBox_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        e.Handled = true;
        Key key = e.Key == Key.System ? e.SystemKey : e.Key;
        if (key is Key.LeftCtrl or Key.RightCtrl or Key.LeftAlt or Key.RightAlt or
            Key.LeftShift or Key.RightShift or Key.LWin or Key.RWin or Key.Clear or Key.DeadCharProcessed)
            return;

        var parts = new List<string>();
        ModifierKeys mods = Keyboard.Modifiers;
        if ((mods & ModifierKeys.Control) != 0) parts.Add("ctrl");
        if ((mods & ModifierKeys.Alt) != 0) parts.Add("alt");
        if ((mods & ModifierKeys.Shift) != 0) parts.Add("shift");
        if ((mods & ModifierKeys.Windows) != 0) parts.Add("win");
        string rawKey = key.ToString();
        string keyName = rawKey.Length == 1 && char.IsLetter(rawKey[0])
            ? rawKey.ToLowerInvariant()
            : rawKey.Length == 2 && rawKey[0] == 'D' && char.IsDigit(rawKey[1])
                ? rawKey[1].ToString()
                : rawKey.StartsWith("NumPad", StringComparison.Ordinal) && rawKey.Length == 7 && char.IsDigit(rawKey[6])
                    ? rawKey[6].ToString()
                    : key switch
                    {
                        Key.Space => "space", Key.Return => "enter", Key.Escape => "esc",
                        Key.PageUp => "pageup", Key.PageDown => "pagedown",
                        Key.Back => "backspace", Key.Delete => "delete",
                        Key.OemTilde or Key.OemPlus or Key.OemMinus => "",
                        _ => rawKey.ToLowerInvariant()
                    };
        if (string.IsNullOrEmpty(keyName) || keyName is "leftctrl" or "rightctrl" or "leftalt" or "rightalt" or "leftshift" or "rightshift")
        {
            FooterStatus.Text = "Choose a letter, number, function key, or named key with optional modifiers.";
            return;
        }
        parts.Add(keyName);
        string combo = string.Join("+", parts);
        if (sender == ScaleHotkeyBox) ScaleHotkeyBox.Text = combo;
        else if (sender == StopHotkeyBox) StopHotkeyBox.Text = combo;
        else if (sender == OverlayHotkeyBox) OverlayHotkeyBox.Text = combo;
        SidebarHotkey.Text = ScaleHotkeyBox.Text.Replace("+", " + ").ToUpperInvariant();
        RegisterToggleHotkey();
        UpdatePreview();
    }

    private void RegisterToggleHotkey()
    {
        if (_source == null) return;
        if (_registeredHotkey)
        {
            UnregisterHotKey(_source.Handle, ToggleHotkeyId);
            _registeredHotkey = false;
        }
        string toggle = ScaleHotkeyBox.Text.Trim();
        if (SameHotkey(toggle, StopHotkeyBox.Text) || SameHotkey(toggle, OverlayHotkeyBox.Text) || SameHotkey(StopHotkeyBox.Text, OverlayHotkeyBox.Text))
        {
            FooterStatus.Text = "Toggle hotkey conflicts with NRLive's stop or overlay key.";
            return;
        }
        if (!TryParseHotkey(toggle, out uint modifiers, out uint vk))
        {
            FooterStatus.Text = "Invalid toggle hotkey. Example: ctrl+alt+s";
            return;
        }
        _registeredHotkey = RegisterHotKey(_source.Handle, ToggleHotkeyId, modifiers | ModNoRepeat, vk);
        FooterStatus.Text = _registeredHotkey ? $"Global toggle registered: {toggle}" : $"Windows could not register {toggle}; another app may use it.";
        SidebarHotkey.Text = toggle.Replace("+", " + ").ToUpperInvariant();
    }

    private static bool SameHotkey(string a, string b) => string.Equals(NormalizeHotkey(a), NormalizeHotkey(b), StringComparison.OrdinalIgnoreCase);
    private static string NormalizeHotkey(string s) => string.Join("+", s.Split('+', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries).Select(x => x.ToLowerInvariant()).OrderBy(x => x));

    private static bool TryParseHotkey(string text, out uint modifiers, out uint vk)
    {
        modifiers = 0; vk = 0;
        string key = "";
        foreach (string raw in text.Split('+', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries))
        {
            switch (raw.ToLowerInvariant())
            {
                case "ctrl": case "control": modifiers |= ModControl; break;
                case "alt": modifiers |= ModAlt; break;
                case "shift": modifiers |= ModShift; break;
                case "win": case "windows": modifiers |= ModWin; break;
                default: if (key.Length != 0) return false; key = raw; break;
            }
        }
        if (key.Length == 1 && char.IsAsciiLetterOrDigit(key[0])) { vk = char.ToUpperInvariant(key[0]); return true; }
        if (key.Length >= 2 && (key[0] == 'f' || key[0] == 'F') && int.TryParse(key[1..], out int f) && f is >= 1 and <= 24) { vk = (uint)(0x70 + f - 1); return true; }
        vk = key.ToLowerInvariant() switch
        {
            "home" => 0x24, "end" => 0x23, "insert" => 0x2D, "delete" or "del" => 0x2E,
            "space" or "spacebar" => 0x20, "tab" => 0x09, "enter" or "return" => 0x0D, "esc" or "escape" => 0x1B,
            "pageup" => 0x21, "pagedown" => 0x22, "up" => 0x26, "down" => 0x28, "left" => 0x25, "right" => 0x27,
            _ => 0
        };
        return vk != 0;
    }

    private IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg == WmHotkey && wParam.ToInt32() == ToggleHotkeyId)
        {
            ToggleScaling();
            handled = true;
        }
        return IntPtr.Zero;
    }

    private void ScaleButton_Click(object sender, RoutedEventArgs e) => ToggleScaling();

    private void Minimize_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;

    private void Maximize_Click(object sender, RoutedEventArgs e)
    {
        WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;
        MaximizeButton.Content = WindowState == WindowState.Maximized ? "❐" : "□";
    }

    private void Close_Click(object sender, RoutedEventArgs e) => Close();

    private void ToggleScaling()
    {
        if (_process != null)
        {
            if (!_process.HasExited) { StopScaling(); return; }
            _process.Dispose();
            _process = null;
        }
        StartScaling();
    }

    private void StartScaling()
    {
        SaveCurrentProfile(false);
        var p = ReadProfile();
        if (SameHotkey(p.scale_hotkey, p.stop_key) || SameHotkey(p.scale_hotkey, p.overlay_key) || SameHotkey(p.stop_key, p.overlay_key))
        {
            MessageBox.Show(this, "The Scale / Unscale hotkey must differ from both the NRLive stop key and overlay key.", "Hotkey conflict", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        string exe = p.exe_path;
        if (string.IsNullOrWhiteSpace(exe) || !File.Exists(exe))
        {
            MessageBox.Show(this, "Select a valid NRLive.exe first.", "Executable missing", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        if (p.target_mode == "pid" && !uint.TryParse(p.target_text, out _)) { MessageBox.Show(this, "Enter a numeric process ID.", "Target", MessageBoxButton.OK, MessageBoxImage.Warning); return; }
        if ((p.target_mode == "pname" || p.target_mode == "window") && string.IsNullOrWhiteSpace(p.target_text)) { MessageBox.Show(this, "Enter a process name or window-title regex.", "Target", MessageBoxButton.OK, MessageBoxImage.Warning); return; }
        if (!TryParseHotkey(p.scale_hotkey, out _, out _) || !TryParseHotkey(p.stop_key, out _, out _) || !TryParseHotkey(p.overlay_key, out _, out _))
        {
            MessageBox.Show(this, "The Scale / Unscale, Force stop, and Overlay hotkeys must be valid combinations.", "Hotkey", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        var bypassKeys = p.bind_bypass.Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries);
        if (bypassKeys.Length == 0 || bypassKeys.Any(key => !TryParseHotkey(key, out _, out _)))
        {
            MessageBox.Show(this, "Enter one or more valid passthrough keys separated by commas. Examples: home,insert,space or ctrl+space.", "Passthrough keys", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        var start = new ProcessStartInfo(exe) { WorkingDirectory = Path.GetDirectoryName(exe) ?? Root, UseShellExecute = p.run_as_admin };
        if (p.run_as_admin) start.Verb = "runas";
        if (p.target_mode == "picker") { start.ArgumentList.Add("-picker"); start.ArgumentList.Add(p.delay.ToString()); }
        else if (p.target_mode == "front") { start.ArgumentList.Add("-front"); start.ArgumentList.Add("-delay"); start.ArgumentList.Add(p.delay.ToString()); }
        else if (p.target_mode == "pid") { start.ArgumentList.Add("-pid"); start.ArgumentList.Add(p.target_text); start.ArgumentList.Add("-delay"); start.ArgumentList.Add(p.delay.ToString()); }
        else if (p.target_mode == "pname") { start.ArgumentList.Add("-pname"); start.ArgumentList.Add(p.target_text); start.ArgumentList.Add("-delay"); start.ArgumentList.Add(p.delay.ToString()); }
        else if (p.target_mode == "window") { start.ArgumentList.Add("-window"); start.ArgumentList.Add(p.target_text); start.ArgumentList.Add("-delay"); start.ArgumentList.Add(p.delay.ToString()); }
        if (p.no_overlay) start.ArgumentList.Add("-nooverlay");
        start.ArgumentList.Add("--mv"); start.ArgumentList.Add(p.motion);
        start.ArgumentList.Add("--capture"); start.ArgumentList.Add(p.capture_mode);
        start.ArgumentList.Add("--key"); start.ArgumentList.Add(p.stop_key);
        start.ArgumentList.Add("--overlaykey"); start.ArgumentList.Add(p.overlay_key);
        start.ArgumentList.Add("--bindbypass"); start.ArgumentList.Add(p.bind_bypass);

        try
        {
            _process = Process.Start(start);
            if (_process == null) throw new InvalidOperationException("Windows did not start NRLive.");
            _secondsLeft = p.delay;
            ScaleButton.Content = $"Starting · {_secondsLeft}";
            ScaleButton.Background = System.Windows.Media.Brushes.DarkOrange;
            StatusText.Text = "Switch to the game now. NRLive is silently tracking the foreground window.";
            FooterStatus.Text = "Countdown running — toggle hotkey also cancels scaling.";
            _timer.Start();
            SaveCurrentProfile(false);
        }
        catch (Exception ex) { _process = null; MessageBox.Show(this, ex.Message, "Launch failed", MessageBoxButton.OK, MessageBoxImage.Error); }
    }

    private void Timer_Tick(object? sender, EventArgs e)
    {
        if (_process == null || _process.HasExited) { ResetScaling("NRLive exited; session stopped."); return; }
        if (_secondsLeft > 0) _secondsLeft--;
        if (_secondsLeft > 0) { ScaleButton.Content = $"Starting · {_secondsLeft}"; return; }
        if (ScaleButton.Content?.ToString()?.StartsWith("Starting", StringComparison.Ordinal) == true)
        {
            ScaleButton.Content = "Unscale  ■";
            ScaleButton.Background = new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(194, 73, 91));
            StatusText.Text = "Scaling session is running. Press Unscale or the toggle hotkey to stop NRLive.";
            FooterStatus.Text = "Active — press the same toggle hotkey to unscale.";
        }
    }

    private void StopScaling()
    {
        _timer.Stop();
        try
        {
            if (_process != null && !_process.HasExited) _process.Kill(entireProcessTree: true);
        }
        catch (Exception ex) { FooterStatus.Text = $"Stop failed: {ex.Message}"; }
        finally { _process?.Dispose(); _process = null; ResetScaling("Scaling stopped."); }
    }

    private void ResetScaling(string status)
    {
        _timer.Stop();
        ScaleButton.Content = "Scale  →";
        ScaleButton.Background = new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(181, 108, 255));
        StatusText.Text = "Press Scale, then switch to the game during the countdown.";
        FooterStatus.Text = status;
        if (_process?.HasExited == true) { _process.Dispose(); _process = null; }
    }

    private void UpdatePreview()
    {
        if (CommandPreview == null || _loadingProfile) return;
        var p = ReadProfile();
        string target = p.target_mode switch { "front" => "-front", "pid" => $"-pid {p.target_text}", "pname" => $"-pname \"{p.target_text}\"", "window" => $"-window \"{p.target_text}\"", _ => $"-picker {p.delay}" };
        CommandPreview.Text = $"{p.exe_path} {target} --capture {p.capture_mode} --mv {p.motion} --key {p.stop_key} --overlaykey {p.overlay_key} --bindbypass {p.bind_bypass}" + (p.run_as_admin ? " [run as admin]" : "");
    }

    private void Window_Closing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        _timer.Stop();
        try { if (_process != null && !_process.HasExited) _process.Kill(entireProcessTree: true); } catch { }
        if (_registeredHotkey && _source != null) UnregisterHotKey(_source.Handle, ToggleHotkeyId);
        _source?.RemoveHook(WndProc);
        _source = null;
        SaveCurrentProfile(false);
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool RegisterHotKey(IntPtr hWnd, int id, uint fsModifiers, uint vk);
    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool UnregisterHotKey(IntPtr hWnd, int id);
}
