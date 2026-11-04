#include "overlay.h"
#include "frame_limiter_math.h"
#include "nr_status.h"
#include "hud.h"
#include "menu_kit.h"
#include "opennr.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commdlg.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>
#include <cstring>
#include <memory>
#include <objidl.h>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

// ============================================================================
// NRLive overlay — OpenNR look: near-black panels, dark red title, red accent
// ============================================================================

static constexpr int BTN_SIZE = 56, BTN_GAP = 10, BAR_PAD = 12;

// ── State ─────────────────────────────────────────────────────────────────
static const wchar_t* UI_CLS = L"NRLiveOverlayUI";
static const wchar_t* SET_CLS = L"NRLiveSettings";

static HWND g_output = nullptr, g_ui = nullptr;
static HWND g_settings = nullptr;
static HINSTANCE g_inst = nullptr;
static bool g_open = false, g_fsr = true, g_fpsVisible = true;
static bool g_initialized = false;
static bool g_toggleFsr = false, g_screenshot = false, g_toggleFrameLimit = false, g_toggleFg = false;
static OverlayFrameLimitConfig g_frameLimit{};
static bool g_fgEnabled = false;
static bool g_fgActive = false;
static bool g_toggleDlssNr = false;
static OverlayDlssNrConfig g_dlssNr{};
static float g_sharpness = 0.65f;
static OverlayHudConfig g_cfg{};
static std::wstring g_shotPath;
static std::wstring g_saveStatus = L"Changes are saved automatically";
static int g_hoverBtn = -1;
static UINT g_toggleMods = 0, g_toggleVk = 0, g_presetMods = 0, g_presetVk = 0;
static bool g_toggleDown = false, g_presetDown = false;
static FILETIME g_iniTime{};
static ULONGLONG g_iniCheckAt = 0;

// ── Config (scaleconfig.ini) ──────────────────────────────────────────────
static std::wstring iniPath() {
  wchar_t p[MAX_PATH]{}; GetModuleFileNameW(nullptr, p, MAX_PATH);
  std::wstring s = p; return s.substr(0, s.find_last_of(L"\\/") + 1) + L"scaleconfig.ini";
}

static void loadCfg() {
  const std::wstring ini = iniPath();
  g_fpsVisible = GetPrivateProfileIntW(L"FPS", L"enabled", 1, ini.c_str()) != 0;
  g_frameLimit.enabled = GetPrivateProfileIntW(L"FrameLimiter", L"enabled", 0, ini.c_str()) != 0;
  g_frameLimit.fps = frame_limit::clampFps(GetPrivateProfileIntW(L"FrameLimiter", L"fps", 60, ini.c_str()));
  g_frameLimit.method = frame_limit::normalizeMethod(GetPrivateProfileIntW(L"FrameLimiter", L"method", 0, ini.c_str()));
  g_fgEnabled = GetPrivateProfileIntW(L"FrameGeneration", L"enabled", 0, ini.c_str()) != 0;
  g_dlssNr.enabled = GetPrivateProfileIntW(L"DLSSNR", L"enabled", 0, ini.c_str()) != 0;
  g_dlssNr.newModel = GetPrivateProfileIntW(L"DLSSNR", L"model", 0, ini.c_str()) != 0;
  g_dlssNr.modelScale = std::clamp<int>((int)GetPrivateProfileIntW(L"DLSSNR", L"scale", 100, ini.c_str()), 25, 100) / 100.0f;
  g_dlssNr.style = std::clamp<int>((int)GetPrivateProfileIntW(L"DLSSNR", L"style", 0, ini.c_str()), 0, 2);
  g_dlssNr.intensity = std::clamp<int>((int)GetPrivateProfileIntW(L"DLSSNR", L"intensity", 100, ini.c_str()), 0, 200) / 100.0f;
  g_dlssNr.colorStrength = std::clamp<int>((int)GetPrivateProfileIntW(L"DLSSNR", L"color_strength", 0, ini.c_str()), 0, 100) / 100.0f;
  g_dlssNr.localTone = std::clamp<int>((int)GetPrivateProfileIntW(L"DLSSNR", L"local_tone", 100, ini.c_str()), 0, 200) / 100.0f;
  g_dlssNr.maxRatio = std::clamp<int>((int)GetPrivateProfileIntW(L"DLSSNR", L"max_ratio", 200, ini.c_str()), 100, 800) / 100.0f;
  g_dlssNr.structure = std::clamp<int>((int)GetPrivateProfileIntW(L"DLSSNR", L"structure", 100, ini.c_str()), 0, 200) / 100.0f;
  g_dlssNr.skinStructure = std::clamp<int>((int)GetPrivateProfileIntW(L"DLSSNR", L"skin_structure", -100, ini.c_str()), -100, 200) / 100.0f;
  g_dlssNr.historyStrength = std::clamp<int>((int)GetPrivateProfileIntW(L"DLSSNR", L"history_strength", 20, ini.c_str()), 0, 100) / 100.0f;
  g_dlssNr.automaticSkinMask = GetPrivateProfileIntW(L"DLSSNR", L"automatic_skin_mask", 1, ini.c_str()) != 0;
  g_dlssNr.stabilizer = GetPrivateProfileIntW(L"DLSSNR", L"stabilizer", 1, ini.c_str()) != 0;
  g_dlssNr.passes = std::clamp<int>((int)GetPrivateProfileIntW(L"DLSSNR", L"passes", 1, ini.c_str()), 1, 3);
  g_sharpness = std::clamp(GetPrivateProfileIntW(L"FSR", L"sharpness", 65, ini.c_str()) / 100.0f, 0.0f, 1.0f);
  loadHudConfig(ini, g_cfg);
  // Default screenshot path: Pictures\NRLive
  wchar_t path[MAX_PATH * 4]{};
  GetPrivateProfileStringW(L"General", L"screenshot_path", L"", path, MAX_PATH * 4, ini.c_str());
  if (path[0]) g_shotPath = path;
  else { SHGetFolderPathW(nullptr, CSIDL_MYPICTURES, nullptr, SHGFP_TYPE_CURRENT, path); g_shotPath = path; if (!g_shotPath.empty() && g_shotPath.back() != L'\\') g_shotPath += L'\\'; g_shotPath += L"NRLive"; }
  WIN32_FILE_ATTRIBUTE_DATA fa{};
  if (GetFileAttributesExW(ini.c_str(), GetFileExInfoStandard, &fa)) g_iniTime = fa.ftLastWriteTime;
}

static void saveCfg() {
  const std::wstring ini = iniPath();
  WritePrivateProfileStringW(L"FPS", L"enabled", g_fpsVisible ? L"1" : L"0", ini.c_str());
  saveHudConfig(ini, g_cfg);
  WritePrivateProfileStringW(L"General", L"screenshot_path", g_shotPath.c_str(), ini.c_str());
  WritePrivateProfileStringW(L"FrameLimiter", L"enabled", g_frameLimit.enabled ? L"1" : L"0", ini.c_str());
  wchar_t limitValue[32]{};
  swprintf_s(limitValue, L"%d", g_frameLimit.fps); WritePrivateProfileStringW(L"FrameLimiter", L"fps", limitValue, ini.c_str());
  swprintf_s(limitValue, L"%d", g_frameLimit.method); WritePrivateProfileStringW(L"FrameLimiter", L"method", limitValue, ini.c_str());
  WritePrivateProfileStringW(L"FrameGeneration", L"enabled", g_fgEnabled ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"DLSSNR", L"enabled", g_dlssNr.enabled ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"DLSSNR", L"model", g_dlssNr.newModel ? L"1" : L"0", ini.c_str());
  wchar_t nrValue[32]{};
  swprintf_s(nrValue, L"%d", (int)std::lround(g_dlssNr.modelScale * 100)); WritePrivateProfileStringW(L"DLSSNR", L"scale", nrValue, ini.c_str());
  swprintf_s(nrValue, L"%d", g_dlssNr.style); WritePrivateProfileStringW(L"DLSSNR", L"style", nrValue, ini.c_str());
  swprintf_s(nrValue, L"%d", (int)std::lround(g_dlssNr.intensity * 100)); WritePrivateProfileStringW(L"DLSSNR", L"intensity", nrValue, ini.c_str());
  swprintf_s(nrValue, L"%d", (int)std::lround(g_dlssNr.colorStrength * 100)); WritePrivateProfileStringW(L"DLSSNR", L"color_strength", nrValue, ini.c_str());
  swprintf_s(nrValue, L"%d", (int)std::lround(g_dlssNr.localTone * 100)); WritePrivateProfileStringW(L"DLSSNR", L"local_tone", nrValue, ini.c_str());
  swprintf_s(nrValue, L"%d", (int)std::lround(g_dlssNr.maxRatio * 100)); WritePrivateProfileStringW(L"DLSSNR", L"max_ratio", nrValue, ini.c_str());
  swprintf_s(nrValue, L"%d", (int)std::lround(g_dlssNr.structure * 100)); WritePrivateProfileStringW(L"DLSSNR", L"structure", nrValue, ini.c_str());
  swprintf_s(nrValue, L"%d", (int)std::lround(g_dlssNr.skinStructure * 100)); WritePrivateProfileStringW(L"DLSSNR", L"skin_structure", nrValue, ini.c_str());
  swprintf_s(nrValue, L"%d", (int)std::lround(g_dlssNr.historyStrength * 100)); WritePrivateProfileStringW(L"DLSSNR", L"history_strength", nrValue, ini.c_str());
  WritePrivateProfileStringW(L"DLSSNR", L"automatic_skin_mask", g_dlssNr.automaticSkinMask ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"DLSSNR", L"stabilizer", g_dlssNr.stabilizer ? L"1" : L"0", ini.c_str());
  swprintf_s(nrValue, L"%d", std::clamp(g_dlssNr.passes, 1, 3)); WritePrivateProfileStringW(L"DLSSNR", L"passes", nrValue, ini.c_str());
  wchar_t sharpnessValue[16]{}; swprintf_s(sharpnessValue, L"%d", (int)std::lround(g_sharpness * 100.0f));
  WritePrivateProfileStringW(L"FSR", L"sharpness", sharpnessValue, ini.c_str());
  WIN32_FILE_ATTRIBUTE_DATA fa{};
  if (GetFileAttributesExW(ini.c_str(), GetFileExInfoStandard, &fa)) g_iniTime = fa.ftLastWriteTime;
}

// Hand the current HUD settings to the HUD thread and refresh hotkeys.
static void pushHud() {
  if (!parseHudHotkey(g_cfg.toggle_key, g_toggleMods, g_toggleVk)) g_toggleVk = 0;
  if (!parseHudHotkey(g_cfg.preset_key, g_presetMods, g_presetVk)) g_presetVk = 0;
  hudSetConfig(g_cfg);
  hudSetEnabled(g_fpsVisible);
}

// ── Layout (96 DPI pixels, scaled by the output's DPI) ───────────────────
static float uiScale() {
  const UINT dpi = g_output ? GetDpiForWindow(g_output) : 96;
  return (dpi ? dpi : 96) / 96.0f;
}
static int sc(int v) { return (int)std::lround(v * uiScale()); }
static constexpr int kBarButtons = 7;
static int barWidth() { return sc(BAR_PAD * 2 + kBarButtons * BTN_SIZE + (kBarButtons - 1) * BTN_GAP); }
static int barHeight() { return sc(BAR_PAD * 2 + BTN_SIZE); }
static RECT btnRect(int i) {
  const int x = sc(BAR_PAD + i * (BTN_SIZE + BTN_GAP));
  return {x, sc(BAR_PAD), x + sc(BTN_SIZE), sc(BAR_PAD + BTN_SIZE)};
}

// ── Top bar paint ─────────────────────────────────────────────────────────
static void paintTopBar(HWND h, HDC dc) {
  RECT rc{}; GetClientRect(h, &rc);
  const kit::BarButton buttons[kBarButtons] = {
    {L"FSR", kit::BarText, g_fsr}, {L"FPS", kit::BarText, g_fpsVisible}, {L"", kit::BarCamera, false},
    {L"CAP", kit::BarText, g_frameLimit.enabled}, {L"FG", kit::BarText, g_fgEnabled},
    {L"NR", kit::BarText, g_dlssNr.enabled}, {L"", kit::BarGear, IsWindowVisible(g_settings) != FALSE},
  };
  HDC mem = CreateCompatibleDC(dc);
  HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
  HGDIOBJ old = SelectObject(mem, bmp);
  kit::drawBar(mem, rc, uiScale(), buttons, kBarButtons, g_hoverBtn, sc(BTN_SIZE), sc(BTN_GAP), sc(BAR_PAD));
  BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
  SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
}

// ── Settings panel ────────────────────────────────────────────────────────
struct HudItem { const wchar_t* name; bool OverlayHudConfig::* field; };
static const HudItem kHudItems[] = {
  {L"Title", &OverlayHudConfig::title}, {L"FPS", &OverlayHudConfig::fps},
  {L"Src / gen FPS", &OverlayHudConfig::fps_detail}, {L"Frametime", &OverlayHudConfig::frametime},
  {L"Frame graph", &OverlayHudConfig::frame_timing}, {L"1% / 0.1% lows", &OverlayHudConfig::lows},
  {L"GPU load", &OverlayHudConfig::gpu_load}, {L"GPU clock", &OverlayHudConfig::gpu_clock},
  {L"GPU temp", &OverlayHudConfig::gpu_temp}, {L"VRAM", &OverlayHudConfig::vram},
  {L"CPU load", &OverlayHudConfig::cpu_load}, {L"RAM", &OverlayHudConfig::ram},
  {L"FSR / NR / FG", &OverlayHudConfig::pipeline}, {L"GPU pass times", &OverlayHudConfig::gpu_times},
  {L"Latency", &OverlayHudConfig::latency}, {L"Resolution", &OverlayHudConfig::resolution},
  {L"Clock", &OverlayHudConfig::clock},
};
static constexpr int kHudItemCount = (int)(sizeof(kHudItems) / sizeof(kHudItems[0]));
static const wchar_t* kPositionNames[] = {L"Top left", L"Top right", L"Bottom left", L"Bottom right", L"Top centre", L"Bottom centre"};

static std::unique_ptr<kit::Panel> g_panel;
static kit::Input g_in;
static int g_tab = 0;
static NrConfigWatcher* g_nrCfg = nullptr;
static bool g_openNrRuntime = false;
enum class Pending { None, Close, Browse, OpenFolder, AccentColour, TextColour, GraphColour };
static Pending g_pending = Pending::None;
// What the last interaction changed; saved once the mouse is released.
static bool g_dirtyCfg = false, g_dirtyNr = false;
// Which sections are open: General 0-2, HUD 3-6, NR 8-11 (Performance starts closed).
static bool g_sec[12] = {true, true, true, true, true, true, true, false, true, true, true, false};

static void generalTab(kit::Panel& p) {
  if (p.section(L"FSR", &g_sec[0])) {
    g_dirtyCfg |= p.slider(L"Sharpness", &g_sharpness, 0.0f, 1.0f, L"%.2f",
      L"RCAS sharpening applied by the upscaler. 0 is off.");
  }
  if (p.section(L"Frame limiter", &g_sec[1])) {
    g_dirtyCfg |= p.checkbox(L"Enabled", &g_frameLimit.enabled, L"Caps NRLive's presented frames. The CAP button toggles it too.");
    g_dirtyCfg |= p.slider(L"FPS cap", &g_frameLimit.fps, frame_limit::kMinFps, frame_limit::kMaxFps, L"%d fps");
    static const wchar_t* methods[] = {L"Early", L"Late"};
    g_dirtyCfg |= p.choice(L"Method", &g_frameLimit.method, methods, 2,
      L"Early waits before capture and is smoother. Late waits after the frame is recorded and adds less latency.");
  }
  if (p.section(L"Screenshots", &g_sec[2])) {
    p.text(g_shotPath.c_str(), false);
    p.row(2);
    if (p.button(L"Choose folder...")) g_pending = Pending::Browse;
    if (p.button(L"Open folder")) g_pending = Pending::OpenFolder;
    p.text(L"The camera button saves the upscaled frame as PNG, without the overlay.");
  }
}

static void hudTab(kit::Panel& p) {
  bool hud = false;
  if (p.section(L"Preset", &g_sec[3])) {
    static const wchar_t* presets[] = {L"Minimal", L"Compact", L"Standard", L"Full"};
    int preset = g_cfg.preset - 1;
    if (p.choice(nullptr, &preset, presets, 4)) { applyHudPreset(g_cfg, preset + 1); hud = true; }
  }
  if (p.section(L"Items", &g_sec[4])) {
    for (int i = 0; i < kHudItemCount; ++i) {
      if (i % 3 == 0) p.row(3);
      bool& item = g_cfg.*kHudItems[i].field;
      if (p.checkbox(kHudItems[i].name, &item)) { g_cfg.preset = HudPresetCustom; hud = true; }
    }
  }
  if (p.section(L"Layout", &g_sec[5])) {
    static const wchar_t* layouts[] = {L"Vertical", L"Horizontal"};
    if (p.choice(L"Layout", &g_cfg.layout, layouts, 2)) { g_cfg.preset = HudPresetCustom; hud = true; }
    hud |= p.stepper(L"Position", &g_cfg.position, kPositionNames, 6);
    p.row(2);
    hud |= p.checkbox(L"Background", &g_cfg.background);
    hud |= p.checkbox(L"Text shadow", &g_cfg.text_outline);
    hud |= p.slider(L"Scale", &g_cfg.scale, 50, 250, L"%d%%");
    int opacity = (int)std::lround(g_cfg.opacity * 100), bg = (int)std::lround(g_cfg.background_alpha * 100);
    if (p.slider(L"Opacity", &opacity, 20, 100, L"%d%%")) { g_cfg.opacity = opacity / 100.0f; hud = true; }
    if (p.slider(L"Background", &bg, 0, 100, L"%d%%")) { g_cfg.background_alpha = bg / 100.0f; hud = true; }
    hud |= p.slider(L"Graph history", &g_cfg.graph_samples, 30, 1000, L"%d frames");
    hud |= p.slider(L"Refresh", &g_cfg.sample_ms, 100, 2000, L"%d ms", L"How often values and sensors update.");
  }
  if (p.section(L"Colours", &g_sec[6])) {
    p.row(3);
    if (p.swatch(L"Accent", g_cfg.accent_color)) g_pending = Pending::AccentColour;
    if (p.swatch(L"Text", g_cfg.text_color)) g_pending = Pending::TextColour;
    if (p.swatch(L"Graph", g_cfg.graph_color)) g_pending = Pending::GraphColour;
  }
  wchar_t keys[160]{};
  swprintf_s(keys, L"Toggle HUD: %s    Next preset: %s", g_cfg.toggle_key.c_str(), g_cfg.preset_key.c_str());
  p.text(keys);
  if (hud) { hudSetConfig(g_cfg); g_dirtyCfg = true; }
}

static void openNrTab(kit::Panel& p, OpenNrSettings& s) {
  bool c = false;
  if (p.section(L"Model", &g_sec[8])) {
    static const wchar_t* models[] = {L"Small", L"Original"};
    int model = s.originalModel ? 1 : 0;
    if (p.choice(L"Model", &model, models, 2,
        L"Small is 310.9.16 (dlssnr-small.bin, faster). Original is 310.8 (dlssnr.bin). A change rebuilds the network.")) {
      s.originalModel = model == 1; c = true;
    }
    static const wchar_t* styles[] = {L"Standard", L"Natural", L"Cinematic"};
    int style = (int)s.MochizukiRenoStyle;
    if (p.choice(L"Style", &style, styles, 3, L"The model's look.")) { s.MochizukiRenoStyle = (uint32_t)style; c = true; }
    static const wchar_t* passes[] = {L"1", L"2", L"3", L"4"};
    int pass = (int)std::min<uint32_t>(s.MochizukiRenoPasses, 4) - 1;
    if (p.choice(L"Passes", &pass, passes, 4,
        L"Model passes, each on the last one's result. Each adds the network's whole cost. A change rebuilds the network.")) {
      s.MochizukiRenoPasses = (uint32_t)pass + 1; c = true;
    }
    c |= p.checkbox(L"Style grade", &s.styleGrade, L"Grades Natural and Cinematic after the network.");
  }
  if (p.section(L"Look", &g_sec[9])) {
    c |= p.slider(L"Intensity", &s.MochizukiRenoIntensity, 0.0f, 2.0f, L"%.2f", L"The model's intensity.");
    c |= p.slider(L"Local tone", &s.MochizukiRenoLocalTone, 0.0f, 2.0f, L"%.2f", L"The model's local lighting.");
    c |= p.slider(L"Structure", &s.MochizukiRenoLocalStructure, 0.0f, 2.0f, L"%.2f", L"The model's structure strength.");
    c |= p.slider(L"Skin structure", &s.MochizukiRenoSkinStructure, -1.0f, 1.0f, L"%.2f",
      L"Structure on skin; -1 is the model's own. Needs the auto mask.");
    c |= p.checkbox(L"Auto mask", &s.MochizukiRenoAutoMask, L"The model's character mask, so skin gets its own structure.");
    c |= p.slider(L"Colour strength", &s.MochizukiRenoColorStrength, 0.0f, 1.0f, L"%.2f",
      L"How much of the model's colour change is kept; 0 keeps the game's colour at the new brightness.");
    c |= p.slider(L"Transfer strength", &s.MochizukiRenoTransferStrength, 0.0f, 1.0f, L"%.2f",
      L"How much of the model's brightness change is kept.");
  }
  if (p.section(L"Temporal", &g_sec[10])) {
    c |= p.checkbox(L"Temporal history", &s.temporal, L"History along NRLive's motion vectors.");
    c |= p.slider(L"History strength", &s.historyStrength, 0.0f, 1.0f, L"%.2f");
  }
  if (p.section(L"Performance", &g_sec[11])) {
    c |= p.slider(L"Resolution scale", &s.MochizukiRenoResolutionScale, 0.33f, 1.0f, L"%.2f",
      L"The network's resolution as a share of the frame; the result is enlarged back. A change rebuilds the network.");
    c |= p.checkbox(L"int4 mixed", &s.int4, L"Original model only. Needs ACO and dlssnr-int4.bin.");
    c |= p.checkbox(L"ACO", &s.aco, L"ACO machine code. Read when NR starts; restart NRLive to change it.");
  }
  g_dirtyNr |= c;
}

static void bridgeTab(kit::Panel& p) {
  bool c = false;
  if (p.section(L"Model", &g_sec[8])) {
    static const wchar_t* models[] = {L"Original", L"New / Fast"};
    int model = g_dlssNr.newModel ? 1 : 0;
    if (p.choice(L"Model", &model, models, 2, L"New / Fast is the 310.9.16 model (dlssnr-amd\\dlssnr-small.bin).")) {
      g_dlssNr.newModel = model == 1; g_toggleDlssNr = true; c = true;
    }
    static const wchar_t* styles[] = {L"Neutral", L"Natural", L"Cinematic"};
    c |= p.choice(L"Style", &g_dlssNr.style, styles, 3);
    c |= p.slider(L"Model scale", &g_dlssNr.modelScale, 0.25f, 1.0f, L"%.2f");
    static const wchar_t* passes[] = {L"1", L"2", L"3"};
    int pass = std::clamp(g_dlssNr.passes, 1, 3) - 1;
    if (p.choice(L"Passes", &pass, passes, 3, L"Neural refinement passes.")) { g_dlssNr.passes = pass + 1; c = true; }
  }
  if (p.section(L"Look", &g_sec[9])) {
    c |= p.slider(L"Intensity", &g_dlssNr.intensity, 0.0f, 2.0f, L"%.2f");
    c |= p.slider(L"Structure", &g_dlssNr.structure, 0.0f, 2.0f, L"%.2f");
    c |= p.slider(L"Skin structure", &g_dlssNr.skinStructure, -1.0f, 2.0f, L"%.2f");
    c |= p.checkbox(L"Automatic skin mask", &g_dlssNr.automaticSkinMask);
    c |= p.slider(L"Local tone", &g_dlssNr.localTone, 0.0f, 2.0f, L"%.2f");
    c |= p.slider(L"Highlight guard", &g_dlssNr.maxRatio, 1.0f, 8.0f, L"%.2f", L"Max brightness ratio the model may apply.");
    c |= p.slider(L"Colour blend", &g_dlssNr.colorStrength, 0.0f, 1.0f, L"%.2f",
      L"0 preserves the source chroma; 1 uses the neural chroma.");
  }
  if (p.section(L"Temporal", &g_sec[10])) {
    c |= p.slider(L"Temporal history", &g_dlssNr.historyStrength, 0.0f, 1.0f, L"%.2f");
    c |= p.checkbox(L"Residual stabilizer", &g_dlssNr.stabilizer, L"Motion-only residual stabilizer after the network.");
  }
  p.text(L"Model weights are not included; see DLSSNR-AMD-SETUP.txt.");
  g_dirtyCfg |= c;
}

static void nrTab(kit::Panel& p) {
  std::wstring line = g_dlssNr.enabled ? nrStatusLine() : std::wstring(L"NR is off; the NR button turns it on.");
  if (line.empty()) line = L"Starting";
  p.text((std::wstring(g_openNrRuntime ? L"OpenNR runtime  |  " : L"0.0.6 bridge  |  ") + line).c_str(), false);
  if (g_openNrRuntime && g_nrCfg) openNrTab(p, g_nrCfg->edit());
  else bridgeTab(p);
}

static void panelUi(kit::Panel& p) {
  static const wchar_t* tabs[] = {L"General", L"HUD", L"NR"};
  bool close = false;
  p.titleBar(L"NRLive", &g_tab, tabs, 3, &close);
  if (close) g_pending = Pending::Close;
  p.beginBody();
  if (g_tab == 0) generalTab(p);
  else if (g_tab == 1) hudTab(p);
  else nrTab(p);
  p.status(p.dragging() ? L"Release to save" : g_saveStatus.c_str());
  p.end();
}

// The panel is as tall as its open sections, up to the output's height.
static int settingsHeight() {
  RECT o{}; GetWindowRect(g_output, &o);
  return std::clamp(g_panel->wantedHeight(), sc(240), (int)(o.bottom - o.top) - sc(140));
}

static void fitSettings() {
  RECT r{}; GetWindowRect(g_settings, &r);
  const int h = settingsHeight();
  if (r.bottom - r.top != h)
    SetWindowPos(g_settings, nullptr, 0, 0, r.right - r.left, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void placeSettings() {
  RECT o{}; GetWindowRect(g_output, &o);
  const int w = sc(460);
  SetWindowPos(g_settings, HWND_TOPMOST, o.left + (o.right - o.left - w) / 2, o.top + sc(100), w, sc(600),
               SWP_NOACTIVATE);
  RECT rc{}; GetClientRect(g_settings, &rc);
  kit::Input none;
  g_panel->begin(nullptr, rc, uiScale(), none);
  panelUi(*g_panel);
  SetWindowPos(g_settings, HWND_TOPMOST, 0, 0, w, settingsHeight(), SWP_NOMOVE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void runPending(HWND h) {
  const Pending what = g_pending;
  g_pending = Pending::None;
  switch (what) {
  case Pending::Close: ShowWindow(h, SW_HIDE); if (g_ui) InvalidateRect(g_ui, nullptr, FALSE); break;
  case Pending::Browse: {
    wchar_t p[MAX_PATH * 4]{}; BROWSEINFOW bi{};
    bi.hwndOwner = h; bi.lpszTitle = L"Choose screenshot folder"; bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST id = SHBrowseForFolderW(&bi);
    if (id) { SHGetPathFromIDListW(id, p); CoTaskMemFree(id); if (p[0]) { g_shotPath = p; saveCfg(); } }
    break;
  }
  case Pending::OpenFolder:
    CreateDirectoryW(g_shotPath.c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", g_shotPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    break;
  case Pending::AccentColour: case Pending::TextColour: case Pending::GraphColour: {
    static COLORREF cust[16] = {};
    COLORREF* target = what == Pending::AccentColour ? &g_cfg.accent_color
                     : what == Pending::TextColour ? &g_cfg.text_color : &g_cfg.graph_color;
    CHOOSECOLORW cc{}; cc.lStructSize = sizeof(cc);
    cc.hwndOwner = h; cc.lpCustColors = cust; cc.rgbResult = *target;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&cc)) { *target = cc.rgbResult; saveCfg(); pushHud(); }
    break;
  }
  default: break;
  }
  InvalidateRect(h, nullptr, FALSE);
}

// One mouse event: the panel's interaction pass, then what it changed.
static void panelEvent(HWND h) {
  RECT rc{}; GetClientRect(h, &rc);
  g_panel->begin(nullptr, rc, uiScale(), g_in);
  panelUi(*g_panel);
  g_in.pressed = g_in.released = false;
  g_in.wheel = 0;
  if (!g_in.down && (g_dirtyCfg || g_dirtyNr)) {
    if (g_dirtyNr && g_nrCfg) g_nrCfg->save();
    if (g_dirtyCfg) saveCfg();
    g_dirtyCfg = g_dirtyNr = false;
    g_saveStatus = L"Saved to scaleconfig.ini";
    if (g_ui) InvalidateRect(g_ui, nullptr, FALSE);
  }
  fitSettings();
  InvalidateRect(h, nullptr, FALSE);
  if (g_pending != Pending::None) runPending(h);
}

static LRESULT CALLBACK uiProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
  case WM_PAINT: {
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
    paintTopBar(h, dc);
    EndPaint(h, &ps); return 0;
  }
  case WM_ERASEBKGND: return 1;
  case WM_LBUTTONUP: {
    POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    for (int i = 0; i < kBarButtons; ++i) {
      RECT b = btnRect(i);
      if (PtInRect(&b, p)) {
        if (i == 0) { g_fsr = !g_fsr; g_toggleFsr = true; }
        else if (i == 1) { g_fpsVisible = !g_fpsVisible; saveCfg(); hudSetEnabled(g_fpsVisible); }
        else if (i == 2) { g_screenshot = true; }
        else if (i == 3) { g_frameLimit.enabled = !g_frameLimit.enabled; g_toggleFrameLimit = true; saveCfg(); }
        else if (i == 4) { g_fgEnabled = !g_fgEnabled; g_toggleFg = true; saveCfg(); }
        else if (i == 5) { g_dlssNr.enabled = !g_dlssNr.enabled; g_toggleDlssNr = true; saveCfg(); }
        else if (IsWindowVisible(g_settings)) ShowWindow(g_settings, SW_HIDE);
        else placeSettings();
        if (g_settings && IsWindowVisible(g_settings)) InvalidateRect(g_settings, nullptr, FALSE);
        InvalidateRect(h, nullptr, FALSE); UpdateWindow(h); return 0;
      }
    }
    return 0;
  }
  case WM_MOUSEMOVE: {
    POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    int old = g_hoverBtn; g_hoverBtn = -1;
    for (int i = 0; i < kBarButtons; ++i) { RECT b = btnRect(i); if (PtInRect(&b, p)) { g_hoverBtn = i; break; } }
    if (g_hoverBtn != old) {
      InvalidateRect(h, nullptr, FALSE);
      TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
      TrackMouseEvent(&tme);
    }
    return 0;
  }
  case WM_MOUSELEAVE: g_hoverBtn = -1; InvalidateRect(h, nullptr, FALSE); return 0;
  default: return DefWindowProcW(h, m, w, l);
  }
}

static LRESULT CALLBACK settingsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
  case WM_PAINT: {
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
    RECT rc{}; GetClientRect(h, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    g_panel->begin(mem, rc, uiScale(), g_in);
    panelUi(*g_panel);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
    EndPaint(h, &ps); return 0;
  }
  case WM_ERASEBKGND: return 1;
  case WM_NCHITTEST: {
    POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    ScreenToClient(h, &p);
    return g_panel->isCaption(p) ? HTCAPTION : HTCLIENT;
  }
  case WM_MOUSEMOVE: {
    g_in.mouse = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
    TrackMouseEvent(&tme);
    panelEvent(h); return 0;
  }
  case WM_MOUSELEAVE:
    if (!g_in.down) { g_in.mouse = {-1, -1}; InvalidateRect(h, nullptr, FALSE); }
    return 0;
  case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
    SetCapture(h);
    g_in.mouse = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    g_in.down = g_in.pressed = true;
    panelEvent(h); return 0;
  case WM_LBUTTONUP:
    g_in.mouse = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    g_in.down = false; g_in.released = true;
    if (GetCapture() == h) ReleaseCapture();
    panelEvent(h); return 0;
  case WM_CAPTURECHANGED:
    if (g_in.down) { g_in.down = false; g_in.released = true; g_in.mouse = {-1, -1}; panelEvent(h); }
    return 0;
  case WM_MOUSEWHEEL: {
    POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    ScreenToClient(h, &p);
    g_in.mouse = p;
    g_in.wheel = GET_WHEEL_DELTA_WPARAM(w);
    panelEvent(h); return 0;
  }
  default: return DefWindowProcW(h, m, w, l);
  }
}

// ── Public API ────────────────────────────────────────────────────────────
static ULONG_PTR g_gdipToken = 0;

bool overlayInit(HINSTANCE inst, HWND output) {
  if (g_initialized) return true;
  g_inst = inst; g_output = output; loadCfg();
  Gdiplus::GdiplusStartupInput gin;
  Gdiplus::GdiplusStartup(&g_gdipToken, &gin, nullptr);
  g_panel = std::make_unique<kit::Panel>();

  WNDCLASSEXW c{sizeof(c)}; c.hInstance = inst; c.hCursor = LoadCursor(nullptr, IDC_ARROW); c.hbrBackground = nullptr;
  c.lpfnWndProc = uiProc;      c.lpszClassName = UI_CLS;  RegisterClassExW(&c);
  c.lpfnWndProc = settingsProc; c.lpszClassName = SET_CLS; RegisterClassExW(&c);

  g_ui = CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST, UI_CLS, L"",
    WS_POPUP, 0, 0, barWidth(), barHeight(), nullptr, nullptr, inst, nullptr);
  g_settings = CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST, SET_CLS, L"",
    WS_POPUP, 0, 0, sc(460), sc(700), nullptr, nullptr, inst, nullptr);

  // Rounded corners on Windows 11 (ignored elsewhere).
  const DWORD round = 2; // DWMWCP_ROUND
  if (g_ui) DwmSetWindowAttribute(g_ui, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &round, sizeof(round));
  if (g_settings) DwmSetWindowAttribute(g_settings, 33, &round, sizeof(round));

  hudStart(inst, output);
  pushHud();

  g_initialized = true;
  return true;
}

void overlayShutdown() {
  saveCfg();
  hudStop();
  if (g_ui) DestroyWindow(g_ui);
  if (g_settings) DestroyWindow(g_settings);
  g_ui = g_settings = nullptr; g_initialized = false;
  g_panel.reset();
  if (g_gdipToken) Gdiplus::GdiplusShutdown(g_gdipToken);
  g_gdipToken = 0;
}

void overlaySetPresentationVisible(bool visible) {
  // The HUD thread re-anchors to the output rectangle and restores topmost.
  hudSetPresentationVisible(visible);
}

void overlaySetOpen(bool open) {
  g_open = open;
  if (g_ui) {
    if (open) {
      ShowWindow(g_ui, SW_SHOWNOACTIVATE);
      RECT o{}; if (GetWindowRect(g_output, &o)) {
        const int barW = barWidth();
        int cx = o.left + (o.right - o.left - barW) / 2;
        SetWindowPos(g_ui, HWND_TOPMOST, cx, o.top + sc(16), barW, barHeight(), SWP_NOACTIVATE|SWP_SHOWWINDOW);
      }
      InvalidateRect(g_ui, nullptr, TRUE); UpdateWindow(g_ui);
    } else ShowWindow(g_ui, SW_HIDE);
  }
  if (!open && g_settings) ShowWindow(g_settings, SW_HIDE);
  // Re-assert HUD topmost when overlay toggles — applyOverlayActivation
  // can push NRLive above it. Once per toggle, not every frame.
  hudReassertTopmost();
}

static bool comboDown(UINT mods, UINT vk) {
  auto down = [](int k) { return (GetAsyncKeyState(k) & 0x8000) != 0; };
  if (!vk || !down((int)vk)) return false;
  if (((mods & MOD_CONTROL) != 0) != down(VK_CONTROL)) return false;
  if (((mods & MOD_SHIFT) != 0) != down(VK_SHIFT)) return false;
  if (((mods & MOD_ALT) != 0) != down(VK_MENU)) return false;
  if ((mods & MOD_WIN) && !down(VK_LWIN) && !down(VK_RWIN)) return false;
  return true;
}

void overlayUpdate(const OverlayFrameStats& st) {
  HudFrame f;
  f.sourceMs = st.sourceMs; f.capture = st.capture; f.output = st.output;
  f.fsr = st.fsr; f.nr = st.nr; f.nrBuilding = st.nrBuilding; f.fg = g_fgActive; f.latencyMs = st.latencyMs;
  f.gpuMotionMs = st.gpuMotionMs; f.gpuNrMs = st.gpuNrMs; f.gpuFsrMs = st.gpuFsrMs; f.gpuTotalMs = st.gpuTotalMs;
  hudPushFrame(f);

  // HUD hotkeys (MangoHud defaults: Shift+F12 toggle, Shift+F10 next preset).
  const bool toggle = comboDown(g_toggleMods, g_toggleVk);
  if (toggle && !g_toggleDown) {
    g_fpsVisible = !g_fpsVisible;
    saveCfg(); hudSetEnabled(g_fpsVisible);
    if (g_ui) InvalidateRect(g_ui, nullptr, FALSE);
  }
  g_toggleDown = toggle;
  const bool preset = comboDown(g_presetMods, g_presetVk);
  if (preset && !g_presetDown) {
    applyHudPreset(g_cfg, nextHudPreset(g_cfg.preset));
    if (!g_fpsVisible) g_fpsVisible = true;
    saveCfg(); pushHud();
    if (g_settings) InvalidateRect(g_settings, nullptr, FALSE);
  }
  g_presetDown = preset;

  // Pick up edits made by the launcher while NRLive runs (once a second).
  const ULONGLONG now = GetTickCount64();
  // The NR tab shows the runtime's live status.
  static ULONGLONG nrRepaintAt = 0;
  if (g_tab == 2 && now >= nrRepaintAt && g_settings && IsWindowVisible(g_settings)) {
    nrRepaintAt = now + 500;
    InvalidateRect(g_settings, nullptr, FALSE);
  }
  if (now >= g_iniCheckAt) {
    g_iniCheckAt = now + 1000;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(iniPath().c_str(), GetFileExInfoStandard, &fa) &&
        CompareFileTime(&fa.ftLastWriteTime, &g_iniTime) != 0) {
      g_iniTime = fa.ftLastWriteTime;
      loadHudConfig(iniPath(), g_cfg);
      g_fpsVisible = GetPrivateProfileIntW(L"FPS", L"enabled", 1, iniPath().c_str()) != 0;
      pushHud();
      if (g_ui) InvalidateRect(g_ui, nullptr, FALSE);
      if (g_settings && IsWindowVisible(g_settings)) InvalidateRect(g_settings, nullptr, FALSE);
    }
  }
}

void overlaySetAdapter(LUID luid) { hudSetAdapter(luid); }
void overlaySetCaptureExcluded(bool exclude) { hudSetCaptureExcluded(exclude); }
bool overlayWantsGpuTimes() { return g_fpsVisible && g_cfg.gpu_times; }

void overlaySetFsrEnabled(bool e) { g_fsr = e; if (g_ui) InvalidateRect(g_ui, nullptr, FALSE); }
bool overlayConsumeFsrToggle() { bool v = g_toggleFsr; g_toggleFsr = false; return v; }
bool overlayConsumeScreenshot() { bool v = g_screenshot; g_screenshot = false; return v; }
bool overlayConsumeFrameLimitToggle() { bool v = g_toggleFrameLimit; g_toggleFrameLimit = false; return v; }
OverlayFrameLimitConfig overlayFrameLimitConfig() { return g_frameLimit; }
float overlaySharpness() { return g_sharpness; }
bool overlayConsumeFgToggle() { bool v = g_toggleFg; g_toggleFg = false; return v; }
bool overlayConsumeDlssNrToggle() { bool v = g_toggleDlssNr; g_toggleDlssNr = false; return v; }
const OverlayDlssNrConfig& overlayDlssNrConfig() { return g_dlssNr; }
void overlaySetDlssNrEnabled(bool enabled) {
  g_dlssNr.enabled = enabled; saveCfg();
  if (g_ui) InvalidateRect(g_ui, nullptr, FALSE);
}
bool overlayFgEnabled() { return g_fgEnabled; }
void overlaySetFgEnabled(bool enabled) {
  g_fgEnabled = enabled;
  saveCfg();
  if (g_ui) InvalidateRect(g_ui, nullptr, FALSE);
}
void overlaySetFgActive(bool active) {
  g_fgActive = active;
}
const OverlayHudConfig& overlayConfig() { return g_cfg; }
void overlaySetNrConfig(NrConfigWatcher* config, bool openNr) {
  if (g_nrCfg == config && g_openNrRuntime == openNr) return;
  g_nrCfg = config; g_openNrRuntime = openNr;
  if (g_settings) InvalidateRect(g_settings, nullptr, FALSE);
}
std::wstring overlayScreenshotPath() { return g_shotPath; }
