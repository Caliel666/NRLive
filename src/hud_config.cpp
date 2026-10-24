#include "hud_config.h"
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>

static const wchar_t* kSec = L"FPS";

const wchar_t* hudPresetName(int preset) {
  switch (preset) {
    case HudPresetMinimal: return L"Minimal";
    case HudPresetCompact: return L"Compact";
    case HudPresetStandard: return L"Standard";
    case HudPresetFull: return L"Full";
    default: return L"Custom";
  }
}

int nextHudPreset(int preset) {
  return preset >= HudPresetMinimal && preset < HudPresetFull ? preset + 1 : HudPresetMinimal;
}

void applyHudPreset(OverlayHudConfig& c, int preset) {
  if (preset <= HudPresetCustom || preset >= HudPresetCount) return;
  const bool full = preset == HudPresetFull;
  const bool standard = preset == HudPresetStandard || full;
  const bool compact = preset == HudPresetCompact;
  c.fps = true;
  c.title = standard;
  c.fps_detail = standard;
  c.frametime = standard || compact;
  c.frame_timing = standard;
  c.lows = standard;
  c.gpu_load = standard || compact;
  c.gpu_temp = standard || compact;
  c.vram = standard;
  c.cpu_load = standard || compact;
  c.pipeline = standard || compact;
  c.resolution = standard;
  c.gpu_clock = full;
  c.ram = full;
  c.gpu_times = full;
  c.latency = full;
  c.clock = full;
  c.layout = compact ? 1 : 0;
  c.preset = preset;
}

std::wstring colorToHex(COLORREF c) {
  wchar_t b[8];
  swprintf_s(b, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
  return b;
}

bool hexToColor(const std::wstring& s, COLORREF& out) {
  std::wstring t = s;
  if (!t.empty() && t[0] == L'#') t.erase(0, 1);
  if (t.size() != 6) return false;
  wchar_t* end = nullptr;
  unsigned long v = wcstoul(t.c_str(), &end, 16);
  if (!end || *end) return false;
  out = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
  return true;
}

static int readInt(const std::wstring& ini, const wchar_t* key, int def, int lo, int hi) {
  return std::clamp((int)GetPrivateProfileIntW(kSec, key, def, ini.c_str()), lo, hi);
}
static bool readBool(const std::wstring& ini, const wchar_t* key, bool def) {
  return GetPrivateProfileIntW(kSec, key, def ? 1 : 0, ini.c_str()) != 0;
}
static std::wstring readStr(const std::wstring& ini, const wchar_t* key, const std::wstring& def) {
  wchar_t b[128]{};
  GetPrivateProfileStringW(kSec, key, def.c_str(), b, 128, ini.c_str());
  return b;
}
static void readColor(const std::wstring& ini, const wchar_t* key, COLORREF& c) {
  COLORREF v;
  if (hexToColor(readStr(ini, key, L""), v)) c = v;
}

void loadHudConfig(const std::wstring& ini, OverlayHudConfig& c) {
  const OverlayHudConfig d{};
  c.title = readBool(ini, L"title", d.title);
  c.fps = readBool(ini, L"fps", d.fps);
  c.fps_detail = readBool(ini, L"fps_detail", d.fps_detail);
  c.frametime = readBool(ini, L"frametime", d.frametime);
  c.frame_timing = readBool(ini, L"frame_timing", d.frame_timing);
  c.lows = readBool(ini, L"lows", d.lows);
  c.gpu_load = readBool(ini, L"gpu_load", d.gpu_load);
  c.gpu_clock = readBool(ini, L"gpu_clock", d.gpu_clock);
  c.gpu_temp = readBool(ini, L"gpu_temp", d.gpu_temp);
  c.vram = readBool(ini, L"vram", d.vram);
  c.cpu_load = readBool(ini, L"cpu_load", d.cpu_load);
  c.ram = readBool(ini, L"ram", d.ram);
  c.pipeline = readBool(ini, L"pipeline", d.pipeline);
  c.gpu_times = readBool(ini, L"gpu_times", d.gpu_times);
  c.latency = readBool(ini, L"latency", d.latency);
  c.resolution = readBool(ini, L"resolution", d.resolution);
  c.clock = readBool(ini, L"clock", d.clock);
  c.background = readBool(ini, L"background", d.background);
  c.text_outline = readBool(ini, L"text_outline", d.text_outline);
  c.layout = readInt(ini, L"layout", d.layout, 0, 1);
  c.position = readInt(ini, L"position", d.position, 0, 5);
  // Older builds stored a font size (24 = default). Map it onto scale once.
  int scale = (int)GetPrivateProfileIntW(kSec, L"scale", -1, ini.c_str());
  if (scale < 0) {
    int fontSize = (int)GetPrivateProfileIntW(kSec, L"font_size", -1, ini.c_str());
    scale = fontSize > 0 ? fontSize * 100 / 24 : d.scale;
  }
  c.scale = std::clamp(scale, 50, 250);
  c.opacity = readInt(ini, L"opacity", (int)std::lround(d.opacity * 100), 20, 100) / 100.0f;
  c.background_alpha = readInt(ini, L"background_alpha", (int)std::lround(d.background_alpha * 100), 0, 100) / 100.0f;
  c.graph_samples = readInt(ini, L"graph_samples", d.graph_samples, 30, 1000);
  c.sample_ms = readInt(ini, L"sample_ms", d.sample_ms, 100, 2000);
  c.preset = readInt(ini, L"preset", d.preset, 0, HudPresetCount - 1);
  c.toggle_key = readStr(ini, L"toggle_key", d.toggle_key);
  c.preset_key = readStr(ini, L"preset_key", d.preset_key);
  readColor(ini, L"accent_color", c.accent_color);
  readColor(ini, L"text_color", c.text_color);
  readColor(ini, L"graph_color", c.graph_color);
}

void saveHudConfig(const std::wstring& ini, const OverlayHudConfig& c) {
  auto b = [&](const wchar_t* k, bool v) { WritePrivateProfileStringW(kSec, k, v ? L"1" : L"0", ini.c_str()); };
  auto i = [&](const wchar_t* k, int v) { wchar_t s[16]; swprintf_s(s, L"%d", v); WritePrivateProfileStringW(kSec, k, s, ini.c_str()); };
  auto s = [&](const wchar_t* k, const std::wstring& v) { WritePrivateProfileStringW(kSec, k, v.c_str(), ini.c_str()); };
  b(L"title", c.title); b(L"fps", c.fps); b(L"fps_detail", c.fps_detail);
  b(L"frametime", c.frametime); b(L"frame_timing", c.frame_timing); b(L"lows", c.lows);
  b(L"gpu_load", c.gpu_load); b(L"gpu_clock", c.gpu_clock); b(L"gpu_temp", c.gpu_temp);
  b(L"vram", c.vram); b(L"cpu_load", c.cpu_load); b(L"ram", c.ram);
  b(L"pipeline", c.pipeline); b(L"gpu_times", c.gpu_times); b(L"latency", c.latency);
  b(L"resolution", c.resolution); b(L"clock", c.clock);
  b(L"background", c.background); b(L"text_outline", c.text_outline);
  i(L"layout", c.layout); i(L"position", c.position); i(L"scale", c.scale);
  i(L"opacity", (int)std::lround(c.opacity * 100)); i(L"background_alpha", (int)std::lround(c.background_alpha * 100));
  i(L"graph_samples", c.graph_samples); i(L"sample_ms", c.sample_ms); i(L"preset", c.preset);
  s(L"toggle_key", c.toggle_key); s(L"preset_key", c.preset_key);
  s(L"accent_color", colorToHex(c.accent_color)); s(L"text_color", colorToHex(c.text_color));
  s(L"graph_color", colorToHex(c.graph_color));
  WritePrivateProfileStringW(kSec, L"font_size", nullptr, ini.c_str()); // superseded by scale
}

bool parseHudHotkey(const std::wstring& text, UINT& modifiers, UINT& vk) {
  modifiers = 0; vk = 0;
  std::wstring key;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(L'+', start);
    std::wstring part = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
    part.erase(std::remove_if(part.begin(), part.end(), [](wchar_t ch) { return iswspace(ch) != 0; }), part.end());
    for (auto& ch : part) ch = (wchar_t)towlower(ch);
    if (part.empty()) return false;
    if (part == L"ctrl" || part == L"control") modifiers |= MOD_CONTROL;
    else if (part == L"shift") modifiers |= MOD_SHIFT;
    else if (part == L"alt") modifiers |= MOD_ALT;
    else if (part == L"win") modifiers |= MOD_WIN;
    else if (!key.empty()) return false;
    else key = part;
    if (end == std::wstring::npos) break;
    start = end + 1;
  }
  if (key.size() == 1 && iswalnum(key[0])) { vk = (UINT)towupper(key[0]); return true; }
  if (key.size() >= 2 && key[0] == L'f') {
    int n = _wtoi(key.c_str() + 1);
    if (n >= 1 && n <= 24 && std::to_wstring(n) == key.substr(1)) { vk = VK_F1 + n - 1; return true; }
  }
  static const std::pair<const wchar_t*, UINT> named[] = {
    {L"insert", VK_INSERT}, {L"home", VK_HOME}, {L"end", VK_END}, {L"pageup", VK_PRIOR},
    {L"pagedown", VK_NEXT}, {L"delete", VK_DELETE}, {L"pause", VK_PAUSE}, {L"scrolllock", VK_SCROLL},
    {L"space", VK_SPACE}, {L"tab", VK_TAB}};
  for (const auto& n : named)
    if (key == n.first) { vk = n.second; return true; }
  return false;
}

float percentileLowFps(std::vector<float> ft, float fraction) {
  ft.erase(std::remove_if(ft.begin(), ft.end(), [](float v) { return !(v > 0.0f); }), ft.end());
  if (ft.empty()) return 0.0f;
  // Slowest frames first; the value at the fraction index is the low.
  size_t idx = std::min(ft.size() - 1, (size_t)std::floor(fraction * (float)ft.size()));
  std::nth_element(ft.begin(), ft.begin() + idx, ft.end(), std::greater<float>());
  return 1000.0f / ft[idx];
}
