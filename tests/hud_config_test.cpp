#include "hud_config.h"
#include "sysmon.h"
#include <dxgi1_6.h>
#include <wrl.h>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

static int fail(int code, const char* what) { std::cerr << what << "\n"; return code; }

int main() {
  // Presets: minimal is FPS only, compact is horizontal, full turns everything on.
  OverlayHudConfig c;
  applyHudPreset(c, HudPresetMinimal);
  if (!c.fps || c.frametime || c.frame_timing || c.gpu_load || c.title || c.layout != 0 || c.preset != HudPresetMinimal)
    return fail(1, "minimal preset");
  applyHudPreset(c, HudPresetCompact);
  if (c.layout != 1 || !c.gpu_load || !c.cpu_load || c.frame_timing) return fail(2, "compact preset");
  applyHudPreset(c, HudPresetFull);
  if (!c.clock || !c.gpu_times || !c.latency || !c.ram || !c.gpu_clock || c.layout != 0) return fail(3, "full preset");
  if (nextHudPreset(HudPresetFull) != HudPresetMinimal || nextHudPreset(HudPresetCustom) != HudPresetMinimal ||
      nextHudPreset(HudPresetMinimal) != HudPresetCompact)
    return fail(4, "preset cycle");

  // Lows: 1000 frames at 10 ms with ten 40 ms spikes -> 1% low = 25 fps, 0.1% low = 25 fps.
  std::vector<float> ft(1000, 10.0f);
  for (int i = 0; i < 10; ++i) ft[i * 97] = 40.0f;
  if (std::fabs(percentileLowFps(ft, 0.01f) - 100.0f) > 0.01f) return fail(5, "1% low with exactly 1% spikes");
  if (std::fabs(percentileLowFps(ft, 0.001f) - 25.0f) > 0.01f) return fail(6, "0.1% low");
  ft[500] = 50.0f; // 11 slow frames: index 10 is now a 40 ms frame
  if (std::fabs(percentileLowFps(ft, 0.01f) - 25.0f) > 0.01f) return fail(7, "1% low with spikes");
  if (percentileLowFps({}, 0.01f) != 0.0f) return fail(8, "empty lows");

  // Hotkeys and colours.
  UINT mods = 0, vk = 0;
  if (!parseHudHotkey(L"shift+f12", mods, vk) || mods != MOD_SHIFT || vk != VK_F12) return fail(9, "shift+f12");
  if (!parseHudHotkey(L"Ctrl + Alt + H", mods, vk) || mods != (MOD_CONTROL | MOD_ALT) || vk != 'H') return fail(10, "ctrl+alt+h");
  if (parseHudHotkey(L"shift+f25", mods, vk) || parseHudHotkey(L"a+b", mods, vk)) return fail(11, "bad hotkeys");
  COLORREF col = 0;
  if (!hexToColor(L"#ED1C24", col) || col != RGB(237, 28, 36) || colorToHex(col) != L"#ED1C24") return fail(12, "colour hex");

  // Round trip through an ini file, including the old font_size migration.
  wchar_t dir[MAX_PATH], ini[MAX_PATH];
  GetTempPathW(MAX_PATH, dir);
  GetTempFileNameW(dir, L"hud", 0, ini);
  WritePrivateProfileStringW(L"FPS", L"font_size", L"36", ini);
  OverlayHudConfig m;
  loadHudConfig(ini, m);
  if (m.scale != 150) return fail(13, "font_size migration");
  OverlayHudConfig a;
  applyHudPreset(a, HudPresetCompact);
  a.position = 5; a.scale = 130; a.opacity = 0.6f; a.graph_samples = 400; a.sample_ms = 250;
  a.accent_color = RGB(1, 2, 3); a.toggle_key = L"ctrl+f9";
  saveHudConfig(ini, a);
  OverlayHudConfig b;
  loadHudConfig(ini, b);
  DeleteFileW(ini);
  if (b.layout != 1 || b.position != 5 || b.scale != 130 || std::fabs(b.opacity - 0.6f) > 0.001f ||
      b.graph_samples != 400 || b.sample_ms != 250 || b.accent_color != RGB(1, 2, 3) ||
      b.toggle_key != L"ctrl+f9" || b.preset != HudPresetCompact || b.frame_timing)
    return fail(14, "ini round trip");

  // Sensors: must start, stop and report sane values where the system has them.
  Microsoft::WRL::ComPtr<IDXGIFactory1> f;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  SysMon mon;
  if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) && SUCCEEDED(f->EnumAdapters1(0, &adapter))) {
    DXGI_ADAPTER_DESC1 d{};
    adapter->GetDesc1(&d);
    mon.setAdapter(d.AdapterLuid);
  }
  SysMonWants w; w.gpuLoad = w.gpuClock = w.gpuTemp = w.vram = w.cpu = w.ram = true;
  mon.configure(w, 250);
  // PDH's first GPU counter query can take a few seconds; poll for values.
  SysSnapshot s;
  for (int i = 0; i < 40; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    s = mon.snapshot();
    if (s.cpuLoad >= 0 && s.ramTotalGb > 0 && (s.gpuLoad >= 0 || i > 30)) break;
  }
  mon.stop();
  std::printf("gpu %.0f%% %.0f MHz %.1f C vram %.2f/%.2f GB cpu %.0f%% ram %.1f/%.1f GB\n",
              s.gpuLoad, s.gpuClockMhz, s.gpuTempC, s.vramUsedGb, s.vramTotalGb, s.cpuLoad, s.ramUsedGb, s.ramTotalGb);
  if (s.cpuLoad < 0 || s.cpuLoad > 100.5f) return fail(15, "cpu load");
  if (s.ramTotalGb <= 0 || s.ramUsedGb <= 0 || s.ramUsedGb > s.ramTotalGb) return fail(16, "ram");
  if (s.gpuLoad > 100.5f || (s.vramTotalGb > 0 && s.vramUsedGb > s.vramTotalGb * 1.05f)) return fail(17, "gpu values");
  return 0;
}
