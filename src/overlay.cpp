#include "overlay.h"
#include "frame_limiter_math.h"
#include "nr_status.h"
#include "hud.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <shlobj.h>
#include <commdlg.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>
#include <cstring>

// ============================================================================
// NRLive overlay — OpenNR look: near-black panels, dark red title, red accent
// ============================================================================

static constexpr int BTN_SIZE = 56, BTN_GAP = 10, BAR_PAD = 12;

// ── Design colors (OpenNR: OptiScaler menu + installer) ───────────────────
static constexpr COLORREF C_WIN      = RGB(20,20,22);      // #141416
static constexpr COLORREF C_DARK     = RGB(110,13,19);     // #6E0D13 dark red title bar
static constexpr COLORREF C_CARD2    = RGB(42,42,46);      // #2A2A2E separators
static constexpr COLORREF C_HOVER    = RGB(52,52,58);      // #34343A
static constexpr COLORREF C_INPUT    = RGB(30,30,34);      // #1E1E22
static constexpr COLORREF C_GHOST    = RGB(35,35,39);      // #232327
static constexpr COLORREF C_RED      = RGB(237,28,36);     // #ED1C24 OpenNR accent
static constexpr COLORREF C_RED_HOT  = RGB(255,77,84);     // #FF4D54
static constexpr COLORREF C_RED_TINT = RGB(58,18,20);      // #3A1214 active button
static constexpr COLORREF C_TAB      = RGB(76,10,14);      // #4C0A0E inactive tab on the title bar
static constexpr COLORREF C_TEXT     = RGB(236,236,238);   // #ECECEE
static constexpr COLORREF C_DIM      = RGB(150,150,158);   // #96969E
static constexpr COLORREF C_MUTED    = RGB(110,110,118);   // #6E6E76

// ── State ─────────────────────────────────────────────────────────────────
static const wchar_t* UI_CLS = L"NRLiveOverlayUI";
static const wchar_t* SET_CLS = L"NRLiveSettings";

static HWND g_output = nullptr, g_ui = nullptr;
static HWND g_settings = nullptr, g_pathEdit = nullptr;
static HINSTANCE g_inst = nullptr;
static bool g_open = false, g_fsr = true, g_fpsVisible = true;
static bool g_initialized = false;
static bool g_toggleFsr = false, g_screenshot = false, g_toggleFrameLimit = false, g_toggleFg = false;
static OverlayFrameLimitConfig g_frameLimit{};
static bool g_fgEnabled = false;
static bool g_fgActive = false;
static bool g_toggleDlssNr = false;
static bool g_dlssNrTab = false;
static bool g_hudTab = false;
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

// ── GDI helpers ───────────────────────────────────────────────────────────
static HFONT makeFont(int size, bool bold = false) {
  return CreateFontW(-size, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
    bold ? L"Segoe UI" : L"Consolas");
}

static void drawText(HDC dc, const wchar_t* s, int x, int y, int size,
                     COLORREF color, bool outline = true) {
  HFONT f = makeFont(size, false);
  auto old = (HFONT)SelectObject(dc, f);
  SetBkMode(dc, TRANSPARENT);
  RECT r{x, y, x + 600, y + size + 4};
  if (outline && g_cfg.text_outline) {
    SetTextColor(dc, RGB(0,0,0));
    for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) {
      if (dx == 0 && dy == 0) continue;
      RECT ro = r; ro.left += dx; ro.top += dy;
      DrawTextW(dc, s, -1, &ro, DT_LEFT | DT_NOPREFIX);
    }
  }
  SetTextColor(dc, color);
  DrawTextW(dc, s, -1, &r, DT_LEFT | DT_NOPREFIX);
  SelectObject(dc, old); DeleteObject(f);
}

static void roundRect(HDC dc, RECT r, int radius, HBRUSH br) {
  HPEN oldp = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
  HBRUSH oldb = (HBRUSH)SelectObject(dc, br);
  RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
  SelectObject(dc, oldb); SelectObject(dc, oldp);
}

// ── Button icons ─────────────────────────────────────────────────────────
static void drawBtnIcon(HDC dc, RECT r, int kind, bool active) {
  int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
  COLORREF c = active ? C_RED : C_DIM;
  HFONT f = makeFont(11, true);
  auto old = (HFONT)SelectObject(dc, f);
  SetBkMode(dc, TRANSPARENT); SetTextColor(dc, c);
  if (kind <= 1) { // FSR / FPS text
    RECT tr{cx-22, cy-8, cx+22, cy+8};
    DrawTextW(dc, kind == 0 ? L"FSR" : L"FPS", -1, &tr, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
  } else if (kind == 2) { // Camera — clean, simple icon
    HPEN p = CreatePen(PS_SOLID, 2, c);
    auto op = (HPEN)SelectObject(dc, p);
    HBRUSH nullbr = (HBRUSH)GetStockObject(NULL_BRUSH);
    auto ob = (HBRUSH)SelectObject(dc, nullbr);
    // Camera body — rounded rectangle
    RoundRect(dc, cx-14, cy-8, cx+14, cy+10, 5, 5);
    // Lens — circle inside
    Ellipse(dc, cx-6, cy-4, cx+7, cy+8);
    // Viewfinder bump on top
    MoveToEx(dc, cx-5, cy-8, nullptr);
    LineTo(dc, cx-3, cy-12);
    LineTo(dc, cx+4, cy-12);
    LineTo(dc, cx+6, cy-8);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(p);
  } else if (kind == 3) { RECT tr{cx-24, cy-8, cx+24, cy+8}; DrawTextW(dc, L"CAP", -1, &tr, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
  } else if (kind == 4) { RECT tr{cx-22, cy-8, cx+22, cy+8}; DrawTextW(dc, L"FG", -1, &tr, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
  } else if (kind == 5) { RECT tr{cx-22, cy-8, cx+22, cy+8}; DrawTextW(dc, L"NR", -1, &tr, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
  } else { // Settings gear
    HPEN p = CreatePen(PS_SOLID, 2, c);
    auto op = (HPEN)SelectObject(dc, p);
    HBRUSH nullbr = (HBRUSH)GetStockObject(NULL_BRUSH);
    auto ob = (HBRUSH)SelectObject(dc, nullbr);
    Ellipse(dc, cx-5, cy-5, cx+5, cy+5);
    for (int i = 0; i < 8; ++i) {
      double a = i * 3.14159265 / 4;
      MoveToEx(dc, cx+(int)(cos(a)*7), cy+(int)(sin(a)*7), nullptr);
      LineTo(dc, cx+(int)(cos(a)*12), cy+(int)(sin(a)*12));
    }
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(p);
  }
  SelectObject(dc, old); DeleteObject(f);
}

static RECT btnRect(int i) {
  return {BAR_PAD + i*(BTN_SIZE+BTN_GAP), BAR_PAD, BAR_PAD + i*(BTN_SIZE+BTN_GAP) + BTN_SIZE, BAR_PAD + BTN_SIZE};
}

// ── Top bar paint ─────────────────────────────────────────────────────────
static void paintTopBar(HWND h, HDC dc) {
  RECT rc{}; GetClientRect(h, &rc);
  HBRUSH bg = CreateSolidBrush(C_WIN);
  FillRect(dc, &rc, bg); DeleteObject(bg);
  for (int i = 0; i < 7; ++i) {
    RECT b = btnRect(i);
    bool active = (i==0&&g_fsr) || (i==1&&g_fpsVisible) || (i==3&&g_frameLimit.enabled) || (i==4&&g_fgEnabled) || (i==5&&g_dlssNr.enabled) || (i==6&&IsWindowVisible(g_settings));
    bool hot = (g_hoverBtn == i);
    HBRUSH br = CreateSolidBrush(active ? C_RED_TINT : (hot ? C_HOVER : C_GHOST));
    roundRect(dc, b, 8, br); DeleteObject(br);
    if (active) { // red underline, like the OpenNR menu's selected item
      HBRUSH ab = CreateSolidBrush(C_RED); RECT u{b.left + 14, b.bottom - 4, b.right - 14, b.bottom - 2};
      FillRect(dc, &u, ab); DeleteObject(ab);
    }
    drawBtnIcon(dc, b, i, active);
  }
}

// ── HUD tab model ────────────────────────────────────────────────────────
struct HudItem { const wchar_t* name; bool OverlayHudConfig::* field; };
static const HudItem kHudItems[] = {
  {L"Title", &OverlayHudConfig::title}, {L"FPS", &OverlayHudConfig::fps},
  {L"Src / gen FPS", &OverlayHudConfig::fps_detail}, {L"Frametime", &OverlayHudConfig::frametime},
  {L"Frametime graph", &OverlayHudConfig::frame_timing}, {L"1% / 0.1% lows", &OverlayHudConfig::lows},
  {L"GPU load", &OverlayHudConfig::gpu_load}, {L"GPU clock", &OverlayHudConfig::gpu_clock},
  {L"GPU temp", &OverlayHudConfig::gpu_temp}, {L"VRAM", &OverlayHudConfig::vram},
  {L"CPU load", &OverlayHudConfig::cpu_load}, {L"RAM", &OverlayHudConfig::ram},
  {L"FSR / NR / FG", &OverlayHudConfig::pipeline}, {L"GPU pass times", &OverlayHudConfig::gpu_times},
  {L"NRLive latency", &OverlayHudConfig::latency}, {L"Resolution", &OverlayHudConfig::resolution},
  {L"Clock", &OverlayHudConfig::clock},
};
static constexpr int kHudItemCount = (int)(sizeof(kHudItems) / sizeof(kHudItems[0]));
static constexpr int ID_PRESET = 701, ID_ITEM = 710, ID_LAYOUT = 730, ID_POSITION = 731, ID_BG = 732, ID_OUTLINE = 733;
static constexpr int ID_SLIDER = 740, ID_SLIDER_VALUE = 750, ID_SLIDER_LABEL = 760, ID_COLOR = 770;
static const wchar_t* kPositionNames[] = {L"Top left", L"Top right", L"Bottom left", L"Bottom right", L"Top centre", L"Bottom centre"};
static RECT presetRect(int i) { return {24 + i * 106, 84, 24 + i * 106 + 98, 112}; }
static RECT itemRect(int i) { return {24 + (i % 3) * 144, 152 + (i / 3) * 26, 24 + (i % 3) * 144 + 140, 152 + (i / 3) * 26 + 22}; }
static constexpr int kHudSliderY = 424, kHudSliderStep = 34;

struct HudSlider { const wchar_t* label; int lo, hi; };
static const HudSlider kHudSliders[] = {
  {L"Scale", 50, 250}, {L"Opacity", 20, 100}, {L"Background", 0, 100}, {L"Graph history", 30, 1000}, {L"Refresh (ms)", 100, 2000}};
static int hudSliderValue(int i) {
  switch (i) {
    case 0: return g_cfg.scale;
    case 1: return (int)std::lround(g_cfg.opacity * 100);
    case 2: return (int)std::lround(g_cfg.background_alpha * 100);
    case 3: return g_cfg.graph_samples;
    default: return g_cfg.sample_ms;
  }
}
static void setHudSliderValue(int i, int v) {
  switch (i) {
    case 0: g_cfg.scale = v; break;
    case 1: g_cfg.opacity = v / 100.0f; break;
    case 2: g_cfg.background_alpha = v / 100.0f; break;
    case 3: g_cfg.graph_samples = v; break;
    default: g_cfg.sample_ms = v; break;
  }
}
static void hudSliderText(int i, wchar_t* out, size_t n) {
  const int v = hudSliderValue(i);
  if (i <= 2) swprintf_s(out, n, L"%d%%", v);
  else swprintf_s(out, n, L"%d", v);
}

// Push every HUD control on the settings panel to the current config.
static void syncHudControls() {
  if (!g_settings) return;
  for (int i = 0; i < 5; ++i) {
    SendMessageW(GetDlgItem(g_settings, ID_SLIDER + i), TBM_SETPOS, TRUE, hudSliderValue(i));
    wchar_t b[16]; hudSliderText(i, b, 16);
    SetWindowTextW(GetDlgItem(g_settings, ID_SLIDER_VALUE + i), b);
  }
  InvalidateRect(g_settings, nullptr, FALSE);
}

static void paintSettings(HWND h, HDC dc) {
  RECT rc{}; GetClientRect(h, &rc);
  HBRUSH bg = CreateSolidBrush(C_WIN); FillRect(dc, &rc, bg); DeleteObject(bg);
  HBRUSH tb = CreateSolidBrush(C_DARK); RECT tr{0,0,rc.right,48}; FillRect(dc,&tr,tb); DeleteObject(tb);
  HBRUSH tl = CreateSolidBrush(C_RED); RECT tline{0,46,rc.right,48}; FillRect(dc,&tline,tl); DeleteObject(tl);
  HFONT title=makeFont(18,true); auto old=(HFONT)SelectObject(dc,title); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,C_TEXT);
  RECT rt{24,10,rc.right-24,40}; DrawTextW(dc,L"NRLive Settings",-1,&rt,DT_LEFT|DT_VCENTER|DT_SINGLELINE); SelectObject(dc,old); DeleteObject(title);
  auto section=[&](const wchar_t* s,int y){HFONT f=makeFont(12,true);auto o=(HFONT)SelectObject(dc,f);SetTextColor(dc,C_RED);RECT r{24,y,rc.right-24,y+18};DrawTextW(dc,s,-1,&r,DT_LEFT|DT_VCENTER|DT_SINGLELINE);HPEN p=CreatePen(PS_SOLID,1,C_CARD2);auto op=(HPEN)SelectObject(dc,p);MoveToEx(dc,24,y+21,nullptr);LineTo(dc,rc.right-24,y+21);SelectObject(dc,op);DeleteObject(p);SelectObject(dc,o);DeleteObject(f);};
  RECT tabGeneral{200, 10, 280, 38}, tabHud{286, 10, 350, 38}, tabNr{356, 10, 456, 38};
  const bool generalTab = !g_dlssNrTab && !g_hudTab;
  HBRUSH tg = CreateSolidBrush(generalTab ? C_RED : C_TAB); roundRect(dc, tabGeneral, 8, tg); DeleteObject(tg);
  HBRUSH th = CreateSolidBrush(g_hudTab ? C_RED : C_TAB); roundRect(dc, tabHud, 8, th); DeleteObject(th);
  HBRUSH tn = CreateSolidBrush(g_dlssNrTab ? C_RED : C_TAB); roundRect(dc, tabNr, 8, tn); DeleteObject(tn);
  drawText(dc, L"General", 216, 16, 12, C_TEXT, false);
  drawText(dc, L"HUD", 306, 16, 12, C_TEXT, false);
  drawText(dc, L"DLSSNR", 382, 16, 12, C_TEXT, false);
  if (g_dlssNrTab) {
    section(L"DLSSNR - BEFORE FSR", 58);
    wchar_t value[80]{};
    swprintf_s(value, L"Style: %s (click to cycle)", g_dlssNr.style == 0 ? L"Neutral" : g_dlssNr.style == 1 ? L"Natural" : L"Cinematic");
    drawText(dc, value, 24, 86, 13, C_TEXT, false);
    swprintf_s(value, L"Model scale: %.2f", g_dlssNr.modelScale); drawText(dc, value, 24, 122, 13, C_TEXT, false);
    swprintf_s(value, L"Intensity: %.2f", g_dlssNr.intensity); drawText(dc, value, 24, 172, 13, C_TEXT, false);
    swprintf_s(value, L"Multipass refinement: %d", g_dlssNr.passes); drawText(dc, value, 24, 472, 13, C_TEXT, false);
     swprintf_s(value, L"NR colour blend: %.2f", g_dlssNr.colorStrength); drawText(dc, value, 24, 522, 13, C_TEXT, false);
    swprintf_s(value, L"Structure: %.2f", g_dlssNr.structure); drawText(dc, value, 24, 222, 13, C_TEXT, false);
    swprintf_s(value, L"Skin structure: %.2f", g_dlssNr.skinStructure); drawText(dc, value, 24, 272, 13, C_TEXT, false);
    swprintf_s(value, L"Temporal history: %.2f", g_dlssNr.historyStrength); drawText(dc, value, 24, 322, 13, C_TEXT, false);
    swprintf_s(value, L"Local tone: %.2f", g_dlssNr.localTone); drawText(dc, value, 24, 372, 13, C_TEXT, false);
    swprintf_s(value, L"Highlight guard (max ratio): %.2f", g_dlssNr.maxRatio); drawText(dc, value, 24, 422, 13, C_TEXT, false);
    drawText(dc, L"Colour 0 preserves source chroma; 1 uses neural chroma.", 24, 676, 10, C_DIM, false);
    drawText(dc, L"Model weights are not included; see the DLSSNR setup instructions.", 24, 692, 10, C_DIM, false);
     drawText(dc, L"Extract both models from the same NVIDIA DLL 310.9.16.", 24, 652, 10, C_DIM, false);
    drawText(dc, g_saveStatus.c_str(), 24, 712, 10, C_MUTED, false);
    drawText(dc, nrStatusLine().c_str(), 24, 725, 10, C_TEXT, false);
    return;
  }
  if (g_hudTab) {
    section(L"PRESET", 58);
    section(L"ITEMS", 124);
    section(L"LAYOUT", 312);
    section(L"COLOURS", 600);
    wchar_t keys[160]{};
    swprintf_s(keys, L"Toggle HUD: %s     Next preset: %s", g_cfg.toggle_key.c_str(), g_cfg.preset_key.c_str());
    drawText(dc, keys, 24, 664, 11, C_DIM, false);
    drawText(dc, L"Hotkeys and every option are also in the launcher (HUD settings).", 24, 682, 10, C_MUTED, false);
    drawText(dc, g_saveStatus.c_str(), 24, 704, 10, C_MUTED, false);
    return;
  }
  section(L"FSR SHARPENING",58);
  section(L"FRAME LIMITER",132);
  section(L"SCREENSHOT",204);
  drawText(dc,g_saveStatus.c_str(),24,344,10,C_MUTED,false);
}

static LRESULT CALLBACK uiProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
  case WM_PAINT: {
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
    paintTopBar(h, dc);
    EndPaint(h, &ps); return 0;
  }
  case WM_LBUTTONUP: {
    POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    for (int i = 0; i < 7; ++i) {
      RECT b = btnRect(i);
      if (PtInRect(&b, p)) {
        if (i == 0) { g_fsr = !g_fsr; g_toggleFsr = true; }
        else if (i == 1) { g_fpsVisible = !g_fpsVisible; saveCfg(); hudSetEnabled(g_fpsVisible); }
        else if (i == 2) { g_screenshot = true; }
        else if (i == 3) { g_frameLimit.enabled = !g_frameLimit.enabled; g_toggleFrameLimit = true; saveCfg(); }
        else if (i == 4) { g_fgEnabled = !g_fgEnabled; g_toggleFg = true; saveCfg(); }
        else if (i == 5) { g_dlssNr.enabled = !g_dlssNr.enabled; g_toggleDlssNr = true; saveCfg(); }
        else { if (IsWindowVisible(g_settings)) ShowWindow(g_settings, SW_HIDE);
          else { int sw=GetSystemMetrics(SM_CXSCREEN), sh=GetSystemMetrics(SM_CYSCREEN);
            wchar_t fpsText[16]{}; swprintf_s(fpsText, L"%d", g_frameLimit.fps); SetWindowTextW(GetDlgItem(g_settings, 206), fpsText);
            SendMessageW(GetDlgItem(g_settings, 207), CB_SETCURSEL, g_frameLimit.method, 0);
            syncHudControls();
            SetWindowPos(g_settings, HWND_TOPMOST, (sw-480)/2, (sh-740)/2, 480, 740, SWP_NOACTIVATE|SWP_SHOWWINDOW);
            InvalidateRect(g_settings, nullptr, TRUE); } }
        InvalidateRect(h, nullptr, FALSE); UpdateWindow(h); return 0;
      }
    }
    return 0;
  }
  case WM_MOUSEMOVE: {
    POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    int old = g_hoverBtn; g_hoverBtn = -1;
    for (int i = 0; i < 7; ++i) { RECT b = btnRect(i); if (PtInRect(&b, p)) { g_hoverBtn = i; break; } }
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

static void drawPushButton(DRAWITEMSTRUCT* dis, const wchar_t* label, bool active) {
  HDC dc = dis->hDC; RECT r = dis->rcItem;
  HBRUSH bg = CreateSolidBrush(C_WIN); FillRect(dc, &r, bg); DeleteObject(bg);
  HBRUSH b = CreateSolidBrush(active ? C_RED : C_GHOST); roundRect(dc, r, 8, b); DeleteObject(b);
  HFONT f = makeFont(12, true); auto old = (HFONT)SelectObject(dc, f);
  SetBkMode(dc, TRANSPARENT); SetTextColor(dc, active ? RGB(255,255,255) : C_TEXT);
  DrawTextW(dc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(dc, old); DeleteObject(f);
}

static void drawOwnerButton(DRAWITEMSTRUCT* dis, const wchar_t* label, bool checked) {
  HDC dc = dis->hDC; RECT r = dis->rcItem;
  HBRUSH bg = CreateSolidBrush(C_WIN); FillRect(dc, &r, bg); DeleteObject(bg);
  RECT box{r.left, r.top+2, r.left+16, r.top+18};
  HBRUSH b = CreateSolidBrush(checked ? C_RED : C_INPUT);
  HPEN p = CreatePen(PS_SOLID, 1, checked ? C_RED_HOT : C_HOVER); auto op=(HPEN)SelectObject(dc,p);
  auto ob=(HBRUSH)SelectObject(dc,b); RoundRect(dc,box.left,box.top,box.right,box.bottom,5,5);
  SelectObject(dc,ob); DeleteObject(b);
  if(checked){HPEN tick=CreatePen(PS_SOLID,2,RGB(255,255,255));SelectObject(dc,tick);MoveToEx(dc,box.left+4,box.top+8,nullptr);LineTo(dc,box.left+7,box.top+11);LineTo(dc,box.right-4,box.top+5);SelectObject(dc,p);DeleteObject(tick);}
  SelectObject(dc,op); DeleteObject(p);
  drawText(dc,label,box.right+8,r.top,13,C_TEXT,false);
}

static void setSettingsTab(HWND h, bool dlssNr, bool hud = false) {
  // Native child controls stay visible independently of the parent's custom
  // paint path. Toggle complete control sets, not just the active tab's.
  static constexpr int generalIds[] = {106, 203,204,205,206,207,208, 303, 606,607,608};
  for (int id : generalIds)
    if (HWND control = GetDlgItem(h, id))
      ShowWindow(control, dlssNr || hud ? SW_HIDE : SW_SHOW);
  for (int id = ID_PRESET; id < ID_COLOR + 3; ++id)
    if (HWND control = GetDlgItem(h, id))
      ShowWindow(control, hud ? SW_SHOW : SW_HIDE);
  for (int id = 510; id <= 523; ++id)
    if (HWND control = GetDlgItem(h, id))
      ShowWindow(control, dlssNr ? SW_SHOW : SW_HIDE);
}

static void saveSettingsFromButton(HWND h) {
  // Both tabs share the same save action and feedback path.
  saveCfg();
  g_saveStatus = L"Saved to scaleconfig.ini";
  InvalidateRect(h, nullptr, FALSE);
}

static void invalidateNrValue(HWND h, int controlId) {
  int y = 122;
  switch (controlId) {
    case 511: y = 122; break;
    case 512: y = 172; break;
    case 513: y = 222; break;
    case 514: y = 272; break;
    case 515: y = 322; break;
    case 518: y = 372; break;
    case 519: y = 422; break;
    case 521: y = 472; break;
      case 522: y = 522; break;
    default: return;
  }
  RECT valueArea{20, y - 2, 456, y + 22};
  InvalidateRect(h, &valueArea, FALSE);
}

static LRESULT CALLBACK settingsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DRAWITEM) {
    DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)l;
    if (!dis) return TRUE;
    const int id = (int)dis->CtlID;
    if (id >= ID_PRESET && id < ID_PRESET + 4) {
      drawPushButton(dis, hudPresetName(id - ID_PRESET + 1), g_cfg.preset == id - ID_PRESET + 1); return TRUE;
    }
    if (id >= ID_ITEM && id < ID_ITEM + kHudItemCount) {
      drawOwnerButton(dis, kHudItems[id - ID_ITEM].name, g_cfg.*kHudItems[id - ID_ITEM].field); return TRUE;
    }
    if (id == ID_LAYOUT) { drawPushButton(dis, g_cfg.layout ? L"Layout: Horizontal" : L"Layout: Vertical", false); return TRUE; }
    if (id == ID_POSITION) {
      wchar_t b[48]; swprintf_s(b, L"Position: %s", kPositionNames[std::clamp(g_cfg.position, 0, 5)]);
      drawPushButton(dis, b, false); return TRUE;
    }
    if (id == ID_BG) { drawOwnerButton(dis, L"Background", g_cfg.background); return TRUE; }
    if (id == ID_OUTLINE) { drawOwnerButton(dis, L"Text shadow", g_cfg.text_outline); return TRUE; }
    if (id >= ID_COLOR && id < ID_COLOR + 3) {
      static const wchar_t* names[3] = {L"Accent", L"Text", L"Graph"};
      const COLORREF col = id == ID_COLOR ? g_cfg.accent_color : id == ID_COLOR + 1 ? g_cfg.text_color : g_cfg.graph_color;
      HDC dc = dis->hDC; RECT r = dis->rcItem;
      HBRUSH bg = CreateSolidBrush(C_WIN); FillRect(dc, &r, bg); DeleteObject(bg);
      RECT sw{r.left, r.top + 2, r.left + 36, r.bottom - 2};
      HBRUSH sb = CreateSolidBrush(col); roundRect(dc, sw, 6, sb); DeleteObject(sb);
      drawText(dc, names[id - ID_COLOR], sw.right + 8, r.top + 3, 13, C_TEXT, false);
      return TRUE;
    }
    switch (dis->CtlID) {
      case 106: drawOwnerButton(dis, L"Enabled", g_frameLimit.enabled); return TRUE;
      case 510: drawOwnerButton(dis, g_dlssNr.style == 0 ? L"Style: Neutral" : g_dlssNr.style == 1 ? L"Style: Natural" : L"Style: Cinematic", true); return TRUE;
      case 516: drawOwnerButton(dis, L"Automatic skin mask", g_dlssNr.automaticSkinMask); return TRUE;
      case 517: drawOwnerButton(dis, L"Residual stabilizer (motion-only)", g_dlssNr.stabilizer); return TRUE;
       case 523: drawOwnerButton(dis, g_dlssNr.newModel ? L"Model: New / Fast" : L"Model: Original", g_dlssNr.newModel); return TRUE;
      case 520: {
        HDC dc = dis->hDC; RECT r = dis->rcItem;
        HBRUSH bg = CreateSolidBrush(C_RED); FillRect(dc, &r, bg); DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255,255,255));
        HFONT font = makeFont(12, true); HFONT prior = (HFONT)SelectObject(dc, font);
        DrawTextW(dc, L"Save settings", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, prior); DeleteObject(font);
        return TRUE;
      }
      case 205: {
        HDC dc = dis->hDC; RECT r = dis->rcItem;
        HBRUSH bg = CreateSolidBrush(C_RED); FillRect(dc, &r, bg); DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255,255,255));
        HFONT font = makeFont(12, true); HFONT prior = (HFONT)SelectObject(dc, font);
        DrawTextW(dc, L"Save settings", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, prior); DeleteObject(font);
        return TRUE;
      }
    }
  }
  // Theme native controls — dark backgrounds for statics, edits, buttons
  if (m == WM_CTLCOLORSTATIC || m == WM_CTLCOLORBTN) {
    HDC dc = (HDC)w;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, C_TEXT);
    static HBRUSH darkBg = nullptr;
    if (!darkBg) darkBg = CreateSolidBrush(C_WIN);
    return (LRESULT)darkBg;
  }
  if (m == WM_CTLCOLOREDIT || m == WM_CTLCOLORLISTBOX) {
    HDC dc = (HDC)w;
    SetBkMode(dc, OPAQUE);
    SetTextColor(dc, C_TEXT);
    SetBkColor(dc, C_INPUT);
    static HBRUSH inputBg = nullptr;
    if (!inputBg) inputBg = CreateSolidBrush(C_INPUT);
    return (LRESULT)inputBg;
  }
  if (m == WM_CTLCOLORDLG) {
    static HBRUSH dlgBg = nullptr;
    if (!dlgBg) dlgBg = CreateSolidBrush(C_WIN);
    return (LRESULT)dlgBg;
  }
  if (m == WM_PAINT) {
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
    paintSettings(h, dc);
    EndPaint(h, &ps); return 0;
  }
  if (m == WM_NCHITTEST) {
    POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    ScreenToClient(h, &p);
    if (p.y < 48 && !(p.x >= 200 && p.x < 456)) return HTCAPTION;
    return HTCLIENT;
  }
  if (m == WM_LBUTTONUP) {
    POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    if (p.y < 48 && p.x >= 200 && p.x < 280) {
      g_dlssNrTab = false; g_hudTab = false;
      setSettingsTab(h, false);
      InvalidateRect(h, nullptr, TRUE); UpdateWindow(h); return 0;
    }
    if (p.y < 48 && p.x >= 286 && p.x < 350) {
      g_dlssNrTab = false; g_hudTab = true;
      setSettingsTab(h, false, true);
      InvalidateRect(h, nullptr, TRUE); UpdateWindow(h); return 0;
    }
    if (p.y < 48 && p.x >= 356 && p.x < 456) {
      g_dlssNrTab = true; g_hudTab = false;
      setSettingsTab(h, true);
      InvalidateRect(h, nullptr, TRUE); UpdateWindow(h); return 0;
    }
    return 0;
  }
  if (m == WM_COMMAND) {
    if ((LOWORD(w) == 205 || LOWORD(w) == 520) && HIWORD(w) == BN_CLICKED) {
      saveSettingsFromButton(h);
      return 0;
    }
    if (LOWORD(w) == 510) {
      g_dlssNr.style = (g_dlssNr.style + 1) % 3;
      saveCfg(); InvalidateRect(h, nullptr, FALSE); return 0;
    }
    if (LOWORD(w) == 516 || LOWORD(w) == 517 || LOWORD(w) == 523) {
      if (LOWORD(w) == 516) g_dlssNr.automaticSkinMask = !g_dlssNr.automaticSkinMask;
      else if (LOWORD(w) == 517) g_dlssNr.stabilizer = !g_dlssNr.stabilizer;
      else { g_dlssNr.newModel = !g_dlssNr.newModel; g_toggleDlssNr = true; }
      saveCfg(); InvalidateRect(h, nullptr, FALSE); return 0;
    }
    if (LOWORD(w) == 106) {
      // BS_OWNERDRAW: Windows keeps no checkbox state, toggle ours directly.
      g_frameLimit.enabled = !g_frameLimit.enabled;
      InvalidateRect(h, nullptr, FALSE);
      saveCfg();
      InvalidateRect(g_ui, nullptr, FALSE); UpdateWindow(g_ui);
    }
    const int cid = LOWORD(w);
    if (HIWORD(w) == BN_CLICKED && cid >= ID_PRESET && cid < ID_COLOR + 3) {
      if (cid < ID_PRESET + 4) applyHudPreset(g_cfg, cid - ID_PRESET + 1);
      else if (cid >= ID_ITEM && cid < ID_ITEM + kHudItemCount) {
        bool& item = g_cfg.*kHudItems[cid - ID_ITEM].field;
        item = !item; g_cfg.preset = HudPresetCustom;
      }
      else if (cid == ID_LAYOUT) { g_cfg.layout = !g_cfg.layout; g_cfg.preset = HudPresetCustom; }
      else if (cid == ID_POSITION) g_cfg.position = (g_cfg.position + 1) % 6;
      else if (cid == ID_BG) g_cfg.background = !g_cfg.background;
      else if (cid == ID_OUTLINE) g_cfg.text_outline = !g_cfg.text_outline;
      else if (cid >= ID_COLOR) {
        static COLORREF cust[16] = {};
        COLORREF* target = cid == ID_COLOR ? &g_cfg.accent_color : cid == ID_COLOR + 1 ? &g_cfg.text_color : &g_cfg.graph_color;
        CHOOSECOLORW cc{}; cc.lStructSize = sizeof(cc);
        cc.hwndOwner = h; cc.lpCustColors = cust; cc.rgbResult = *target;
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (ChooseColorW(&cc)) *target = cc.rgbResult;
      }
      saveCfg(); pushHud(); syncHudControls();
      return 0;
    }
    if (HIWORD(w) == EN_CHANGE && LOWORD(w) == 206) {
      wchar_t fpsText[16]{}; GetWindowTextW(GetDlgItem(h, 206), fpsText, 16);
      const int value = _wtoi(fpsText);
      if (value > 0) g_frameLimit.fps = frame_limit::clampFps(value);
      saveCfg();
    }
    if (HIWORD(w) == CBN_SELCHANGE && LOWORD(w) == 207) {
      g_frameLimit.method = frame_limit::normalizeMethod((int)SendMessageW(GetDlgItem(h, 207), CB_GETCURSEL, 0, 0));
      saveCfg();
    }
    if (HIWORD(w) == EN_CHANGE && LOWORD(w) == 203) {
      wchar_t p[MAX_PATH*4]{}; GetWindowTextW(g_pathEdit, p, MAX_PATH*4);
      g_shotPath = p; saveCfg(); return 0;
    }
    if (LOWORD(w) == 204) {
      wchar_t p[MAX_PATH*4]{}; BROWSEINFOW bi{};
      bi.hwndOwner = h; bi.lpszTitle = L"Choose screenshot folder";
      LPITEMIDLIST id = SHBrowseForFolderW(&bi);
      if (id) { SHGetPathFromIDListW(id, p); CoTaskMemFree(id); if (p[0]) { g_shotPath = p; saveCfg(); SetWindowTextW(g_pathEdit, g_shotPath.c_str()); } }
    }
  }
  if (m == WM_HSCROLL) {
    HWND ctrl = (HWND)l;
    if (ctrl == GetDlgItem(h, 522)) {
      g_dlssNr.colorStrength = (float)SendMessageW(ctrl, TBM_GETPOS, 0, 0) / 100.0f;
      if (HIWORD(w) != TB_THUMBTRACK) saveCfg();
      invalidateNrValue(h, 521);
    } else if (ctrl == GetDlgItem(h, 521)) {
      g_dlssNr.passes = std::clamp((int)SendMessageW(ctrl, TBM_GETPOS, 0, 0), 1, 3);
      if (HIWORD(w) != TB_THUMBTRACK) saveCfg();
      invalidateNrValue(h, 521);
    } else if (ctrl == GetDlgItem(h, 511)) {
      g_dlssNr.modelScale = (float)SendMessageW(ctrl, TBM_GETPOS, 0, 0) / 100.0f;
      // Avoid disk I/O and full-panel repaint for every thumb-tracking message.
      if (HIWORD(w) != TB_THUMBTRACK) saveCfg();
      invalidateNrValue(h, 511);
    } else if (ctrl == GetDlgItem(h, 512)) {
      g_dlssNr.intensity = (float)SendMessageW(ctrl, TBM_GETPOS, 0, 0) / 100.0f;
      // Avoid disk I/O and full-panel repaint for every thumb-tracking message.
      if (HIWORD(w) != TB_THUMBTRACK) saveCfg();
      invalidateNrValue(h, 512);
    } else if (ctrl == GetDlgItem(h, 513)) {
      g_dlssNr.structure = (float)SendMessageW(ctrl, TBM_GETPOS, 0, 0) / 100.0f;
      // Avoid disk I/O and full-panel repaint for every thumb-tracking message.
      if (HIWORD(w) != TB_THUMBTRACK) saveCfg();
      invalidateNrValue(h, 513);
    } else if (ctrl == GetDlgItem(h, 514)) {
      g_dlssNr.skinStructure = (float)SendMessageW(ctrl, TBM_GETPOS, 0, 0) / 100.0f;
      // Avoid disk I/O and full-panel repaint for every thumb-tracking message.
      if (HIWORD(w) != TB_THUMBTRACK) saveCfg();
      invalidateNrValue(h, 514);
    } else if (ctrl == GetDlgItem(h, 515)) {
      g_dlssNr.historyStrength = (float)SendMessageW(ctrl, TBM_GETPOS, 0, 0) / 100.0f;
      // Avoid disk I/O and full-panel repaint for every thumb-tracking message.
      if (HIWORD(w) != TB_THUMBTRACK) saveCfg();
      invalidateNrValue(h, 515);
    } else if (ctrl == GetDlgItem(h, 518)) {
      g_dlssNr.localTone = (float)SendMessageW(ctrl, TBM_GETPOS, 0, 0) / 100.0f;
      // Avoid disk I/O and full-panel repaint for every thumb-tracking message.
      if (HIWORD(w) != TB_THUMBTRACK) saveCfg();
      invalidateNrValue(h, 518);
    } else if (ctrl == GetDlgItem(h, 519)) {
      g_dlssNr.maxRatio = (float)SendMessageW(ctrl, TBM_GETPOS, 0, 0) / 100.0f;
      // Avoid disk I/O and full-panel repaint for every thumb-tracking message.
      if (HIWORD(w) != TB_THUMBTRACK) saveCfg();
      invalidateNrValue(h, 519);
    } else if (ctrl == GetDlgItem(h, 208)) {
      g_sharpness = (float)SendMessageW(ctrl, TBM_GETPOS, 0, 0) / 100.0f;
      wchar_t sharpText[16]{}; swprintf_s(sharpText, L"%.2f", g_sharpness);
      SetWindowTextW(GetDlgItem(h, 303), sharpText);
      saveCfg(); InvalidateRect(h, nullptr, FALSE);
    } else if (const int sid = GetDlgCtrlID(ctrl); sid >= ID_SLIDER && sid < ID_SLIDER + 5) {
      setHudSliderValue(sid - ID_SLIDER, (int)SendMessageW(ctrl, TBM_GETPOS, 0, 0));
      wchar_t b[16]; hudSliderText(sid - ID_SLIDER, b, 16);
      SetWindowTextW(GetDlgItem(h, ID_SLIDER_VALUE + sid - ID_SLIDER), b);
      hudSetConfig(g_cfg);
      if (LOWORD(w) != TB_THUMBTRACK) saveCfg();
    }
  }
  return DefWindowProcW(h, m, w, l);
}

// ── Public API ────────────────────────────────────────────────────────────
bool overlayInit(HINSTANCE inst, HWND output) {
  if (g_initialized) return true;
  g_inst = inst; g_output = output; loadCfg();

  WNDCLASSEXW c{sizeof(c)}; c.hInstance = inst; c.hCursor = LoadCursor(nullptr, IDC_ARROW); c.hbrBackground = nullptr;
  c.lpfnWndProc = uiProc;      c.lpszClassName = UI_CLS;  RegisterClassExW(&c);
  c.lpfnWndProc = settingsProc; c.lpszClassName = SET_CLS; RegisterClassExW(&c);

  int barW = BAR_PAD*2 + 7*BTN_SIZE + 6*BTN_GAP;
  g_ui = CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST, UI_CLS, L"",
    WS_POPUP, 0, 0, barW, BAR_PAD*2+BTN_SIZE, nullptr, nullptr, inst, nullptr);

  // Settings panel
  { int sw=GetSystemMetrics(SM_CXSCREEN), sh=GetSystemMetrics(SM_CYSCREEN);
    g_settings = CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST, SET_CLS, L"",
      WS_POPUP, (sw-480)/2, (sh-740)/2, 480, 740, nullptr, nullptr, inst, nullptr); }

  if (g_settings) {
    CreateWindowW(L"BUTTON", L"", WS_CHILD|BS_OWNERDRAW, 244, 82, 212, 24, g_settings, (HMENU)510, inst, nullptr);
    auto makeNrSlider = [&](int id, int y, int minimum, int maximum, int value) {
      HWND slider = CreateWindowExW(0, L"msctls_trackbar32", L"", WS_CHILD|TBS_NOTICKS|TBS_AUTOTICKS, 140, y, 310, 28, g_settings, (HMENU)(INT_PTR)id, inst, nullptr);
      SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELONG(minimum, maximum));
      SendMessageW(slider, TBM_SETPOS, TRUE, value);
      return slider;
    };
    makeNrSlider(511, 118, 25, 100, (int)std::lround(g_dlssNr.modelScale * 100));
    makeNrSlider(512, 168, 0, 200, (int)std::lround(g_dlssNr.intensity * 100));
    makeNrSlider(513, 218, 0, 200, (int)std::lround(g_dlssNr.structure * 100));
    makeNrSlider(514, 268, -100, 200, (int)std::lround(g_dlssNr.skinStructure * 100));
    makeNrSlider(515, 318, 0, 100, (int)std::lround(g_dlssNr.historyStrength * 100));
    makeNrSlider(518, 368, 0, 200, (int)std::lround(g_dlssNr.localTone * 100));
    makeNrSlider(519, 418, 100, 800, (int)std::lround(g_dlssNr.maxRatio * 100));
    makeNrSlider(521, 468, 1, 3, g_dlssNr.passes);
     makeNrSlider(522, 518, 0, 100, (int)std::lround(g_dlssNr.colorStrength * 100));
    CreateWindowW(L"BUTTON", L"", WS_CHILD|BS_OWNERDRAW, 24, 568, 200, 22, g_settings, (HMENU)516, inst, nullptr);
    CreateWindowW(L"BUTTON", L"", WS_CHILD|BS_OWNERDRAW, 24, 598, 300, 22, g_settings, (HMENU)517, inst, nullptr);
     CreateWindowW(L"BUTTON", L"", WS_CHILD|BS_OWNERDRAW, 24, 628, 230, 22, g_settings, (HMENU)523, inst, nullptr);
    // HUD tab
    for (int i = 0; i < 4; ++i) { RECT r = presetRect(i);
      CreateWindowW(L"BUTTON", L"", WS_CHILD|BS_OWNERDRAW, r.left, r.top, r.right-r.left, r.bottom-r.top, g_settings, (HMENU)(INT_PTR)(ID_PRESET+i), inst, nullptr); }
    for (int i = 0; i < kHudItemCount; ++i) { RECT r = itemRect(i);
      CreateWindowW(L"BUTTON", L"", WS_CHILD|BS_OWNERDRAW, r.left, r.top, r.right-r.left, r.bottom-r.top, g_settings, (HMENU)(INT_PTR)(ID_ITEM+i), inst, nullptr); }
    CreateWindowW(L"BUTTON", L"", WS_CHILD|BS_OWNERDRAW, 24, 338, 200, 28, g_settings, (HMENU)(INT_PTR)ID_LAYOUT, inst, nullptr);
    CreateWindowW(L"BUTTON", L"", WS_CHILD|BS_OWNERDRAW, 232, 338, 222, 28, g_settings, (HMENU)(INT_PTR)ID_POSITION, inst, nullptr);
    CreateWindowW(L"BUTTON", L"", WS_CHILD|BS_OWNERDRAW, 24, 380, 140, 22, g_settings, (HMENU)(INT_PTR)ID_BG, inst, nullptr);
    CreateWindowW(L"BUTTON", L"", WS_CHILD|BS_OWNERDRAW, 168, 380, 140, 22, g_settings, (HMENU)(INT_PTR)ID_OUTLINE, inst, nullptr);
    for (int i = 0; i < 5; ++i) {
      const int y = kHudSliderY + i * kHudSliderStep;
      static HFONT labelFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
      HWND label = CreateWindowW(L"STATIC", kHudSliders[i].label, WS_CHILD|SS_LEFT, 24, y + 4, 110, 18, g_settings, (HMENU)(INT_PTR)(ID_SLIDER_LABEL+i), inst, nullptr);
      SendMessageW(label, WM_SETFONT, (WPARAM)labelFont, FALSE);
      HWND sl = CreateWindowExW(0, L"msctls_trackbar32", L"", WS_CHILD|TBS_NOTICKS, 134, y, 266, 26, g_settings, (HMENU)(INT_PTR)(ID_SLIDER+i), inst, nullptr);
      SendMessageW(sl, TBM_SETRANGE, TRUE, MAKELONG(kHudSliders[i].lo, kHudSliders[i].hi));
      HWND value = CreateWindowW(L"STATIC", L"", WS_CHILD|SS_RIGHT, 402, y + 4, 52, 18, g_settings, (HMENU)(INT_PTR)(ID_SLIDER_VALUE+i), inst, nullptr);
      SendMessageW(value, WM_SETFONT, (WPARAM)labelFont, FALSE);
    }
    for (int i = 0; i < 3; ++i)
      CreateWindowW(L"BUTTON", L"", WS_CHILD|BS_OWNERDRAW, 24 + i*144, 626, 130, 26, g_settings, (HMENU)(INT_PTR)(ID_COLOR+i), inst, nullptr);
    // FSR sharpening slider belongs to overlay settings.
    CreateWindowW(L"STATIC", L"Sharpening amount", WS_CHILD|WS_VISIBLE|SS_LEFT, 24, 86, 130, 18, g_settings, (HMENU)606, inst, nullptr);
    wchar_t sharpText[16]{}; swprintf_s(sharpText, L"%.2f", g_sharpness);
    CreateWindowW(L"STATIC", sharpText, WS_CHILD|WS_VISIBLE|SS_LEFT, 410, 86, 40, 18, g_settings, (HMENU)303, inst, nullptr);
    CreateWindowExW(0, L"msctls_trackbar32", L"", WS_CHILD|WS_VISIBLE|TBS_NOTICKS|TBS_AUTOTICKS, 24, 106, 430, 26, g_settings, (HMENU)208, inst, nullptr);
    SendMessageW(GetDlgItem(g_settings, 208), TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    SendMessageW(GetDlgItem(g_settings, 208), TBM_SETPOS, TRUE, (int)std::lround(g_sharpness * 100.0f));
    // Frame limiter controls
    CreateWindowW(L"BUTTON", L"", WS_CHILD|WS_VISIBLE|BS_OWNERDRAW, 24, 158, 100, 22, g_settings, (HMENU)106, inst, nullptr);
     CreateWindowW(L"STATIC", L"Cap FPS", WS_CHILD|WS_VISIBLE|SS_LEFT, 142, 160, 52, 18, g_settings, (HMENU)607, inst, nullptr);
     CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD|WS_VISIBLE|ES_NUMBER|ES_AUTOHSCROLL, 196, 156, 58, 24, g_settings, (HMENU)206, inst, nullptr);
     HWND methodBox = CreateWindowW(L"COMBOBOX", L"", WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL, 266, 156, 188, 120, g_settings, (HMENU)207, inst, nullptr);
     SendMessageW(methodBox, CB_ADDSTRING, 0, (LPARAM)L"Early - smoother");
     SendMessageW(methodBox, CB_ADDSTRING, 0, (LPARAM)L"Late - snappier");
     SendMessageW(methodBox, CB_SETCURSEL, g_frameLimit.method, 0);
     wchar_t fpsText[16]{}; swprintf_s(fpsText, L"%d", g_frameLimit.fps); SetWindowTextW(GetDlgItem(g_settings, 206), fpsText);
     CreateWindowW(L"STATIC", L"Folder:", WS_CHILD|WS_VISIBLE|SS_LEFT, 24, 230, 60, 18, g_settings, (HMENU)608, inst, nullptr);
    g_pathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_shotPath.c_str(),
      WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL, 24, 252, 340, 24, g_settings, (HMENU)203, inst, nullptr);
    CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD|WS_VISIBLE, 374, 252, 80, 24, g_settings, (HMENU)204, inst, nullptr);
    CreateWindowW(L"BUTTON", L"", WS_CHILD|WS_VISIBLE|BS_OWNERDRAW, 342, 296, 112, 30, g_settings, (HMENU)205, inst, nullptr);
    CreateWindowW(L"BUTTON", L"", WS_CHILD|BS_OWNERDRAW, 342, 632, 112, 30, g_settings, (HMENU)520, inst, nullptr);
    syncHudControls();
    SetWindowTextW(g_pathEdit, g_shotPath.c_str());
    setSettingsTab(g_settings, false);
    ShowWindow(g_settings, SW_HIDE);
  }

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
        int barW = BAR_PAD*2 + 7*BTN_SIZE + 6*BTN_GAP;
        int cx = o.left + (o.right - o.left - barW) / 2;
        SetWindowPos(g_ui, HWND_TOPMOST, cx, o.top+16, barW, BAR_PAD*2+BTN_SIZE, SWP_NOACTIVATE|SWP_SHOWWINDOW);
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
    saveCfg(); pushHud(); syncHudControls();
  }
  g_presetDown = preset;

  // Pick up edits made by the launcher while NRLive runs (once a second).
  const ULONGLONG now = GetTickCount64();
  if (now >= g_iniCheckAt) {
    g_iniCheckAt = now + 1000;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(iniPath().c_str(), GetFileExInfoStandard, &fa) &&
        CompareFileTime(&fa.ftLastWriteTime, &g_iniTime) != 0) {
      g_iniTime = fa.ftLastWriteTime;
      loadHudConfig(iniPath(), g_cfg);
      g_fpsVisible = GetPrivateProfileIntW(L"FPS", L"enabled", 1, iniPath().c_str()) != 0;
      pushHud(); syncHudControls();
      if (g_ui) InvalidateRect(g_ui, nullptr, FALSE);
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
std::wstring overlayScreenshotPath() { return g_shotPath; }
