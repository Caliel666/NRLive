#pragma once
#include <windows.h>
#include <string>
#include <vector>

// Performance HUD settings, stored in the [FPS] section of scaleconfig.ini.
// Every item is an independent toggle; presets only rewrite the item toggles
// and the layout, never position, scale or colours.
struct OverlayHudConfig {
  // Items
  bool title = true;         // NRLIVE header strip
  bool fps = true;           // presented (output) fps
  bool fps_detail = true;    // source (captured) and generated fps
  bool frametime = true;     // source frametime in ms
  bool frame_timing = true;  // frametime graph
  bool lows = true;          // 1% and 0.1% lows
  bool gpu_load = true;
  bool gpu_clock = false;
  bool gpu_temp = true;
  bool vram = true;
  bool cpu_load = true;
  bool ram = false;
  bool pipeline = true;      // FSR / NR / FG state
  bool gpu_times = false;    // GPU ms of motion, NR and FSR passes
  bool latency = false;      // capture -> present
  bool resolution = true;    // capture -> output size
  bool clock = false;        // wall clock
  // Look
  bool background = true;
  bool text_outline = true;
  int layout = 0;            // 0 vertical, 1 horizontal
  int position = 0;          // 0 TL, 1 TR, 2 BL, 3 BR, 4 top centre, 5 bottom centre
  int scale = 100;           // percent, 50..250
  float opacity = 1.0f;      // whole HUD, 0.2..1
  float background_alpha = 0.8f;
  int graph_samples = 200;   // frametime history length, 30..1000 frames
  int sample_ms = 500;       // value and sensor refresh period, 100..2000 ms
  int preset = 3;            // 0 custom, 1 minimal, 2 compact, 3 standard, 4 full
  std::wstring toggle_key = L"shift+f12";
  std::wstring preset_key = L"shift+f10";
  COLORREF accent_color = RGB(237, 28, 36);   // OpenNR red #ED1C24
  COLORREF text_color = RGB(240, 240, 242);
  COLORREF graph_color = RGB(237, 28, 36);
};

enum HudPreset { HudPresetCustom = 0, HudPresetMinimal, HudPresetCompact, HudPresetStandard, HudPresetFull, HudPresetCount };

const wchar_t* hudPresetName(int preset);
void applyHudPreset(OverlayHudConfig& cfg, int preset);
int nextHudPreset(int preset); // cycles 1..4

void loadHudConfig(const std::wstring& ini, OverlayHudConfig& cfg);
void saveHudConfig(const std::wstring& ini, const OverlayHudConfig& cfg);

// "#RRGGBB" <-> COLORREF
std::wstring colorToHex(COLORREF c);
bool hexToColor(const std::wstring& s, COLORREF& out);

// "shift+f12" style hotkey parsing (ctrl/shift/alt/win + key).
bool parseHudHotkey(const std::wstring& text, UINT& modifiers, UINT& vk);

// MangoHud-style percentile low: the fps of the frame at the given percentile
// of the slowest frametimes (0.01 = 1% low). Returns 0 when there is no data.
float percentileLowFps(std::vector<float> frametimesMs, float fraction);
