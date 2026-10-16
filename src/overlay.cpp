#include "overlay.h"
#include "frame_limiter_math.h"
#include <windowsx.h>
#include <shlobj.h>
#include <commdlg.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>
#include <deque>
#include <cstring>

// ============================================================================
// NRLive overlay — dark charcoal + red brand accent
// ============================================================================

static constexpr int BTN_SIZE = 56, BTN_GAP = 10, BAR_PAD = 12;
static constexpr int FT_SAMPLES = 200, FT_HEIGHT = 50, FT_RANGE = 50;

// ── Design colors (AMD-NR ReShade Installer design language) ──────────────
static constexpr COLORREF C_WIN      = RGB(26,26,29);     // #1A1A1D
static constexpr COLORREF C_DARK     = RGB(20,20,22);     // #141416
static constexpr COLORREF C_CARD     = RGB(36,36,40);      // #242428
static constexpr COLORREF C_CARD2    = RGB(30,30,33);     // #1E1E21
static constexpr COLORREF C_HOVER    = RGB(63,63,70);      // #3F3F46
static constexpr COLORREF C_INPUT    = RGB(38,38,44);      // #26262C
static constexpr COLORREF C_GHOST    = RGB(42,42,46);      // #2A2A2E
static constexpr COLORREF C_RED      = RGB(181,108,255);   // launcher purple #B56CFF
static constexpr COLORREF C_RED_HOT   = RGB(201,149,255);   // lighter purple hover
static constexpr COLORREF C_RED_TINT = RGB(57,45,69);      // muted purple panel
static constexpr COLORREF C_GREEN    = RGB(34,197,94);     // #22C55E
static constexpr COLORREF C_TEXT     = RGB(232,232,232);   // #E8E8E8
static constexpr COLORREF C_DIM      = RGB(139,139,139);   // #8B8B8B
static constexpr COLORREF C_MUTED    = RGB(107,107,107);   // #6B6B6B
static constexpr COLORREF C_FPS_BG   = RGB(2,2,2);       // MangoHud near-black

// ── State ─────────────────────────────────────────────────────────────────
static const wchar_t* UI_CLS = L"NRLiveOverlayUI";
static const wchar_t* FPS_CLS = L"NRLiveMangoHud";
static const wchar_t* SET_CLS = L"NRLiveSettings";

static HWND g_output = nullptr, g_ui = nullptr, g_fps = nullptr;
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
static OverlayDlssNrConfig g_dlssNr{};
static float g_sharpness = 0.65f;
static OverlayHudConfig g_cfg{};
static std::wstring g_shotPath;
static std::wstring g_saveStatus = L"Changes are saved automatically";
static float g_lastFps = 0, g_lastMs = 0;
static float g_smoothFps = 0, g_smoothMs = 0;
static std::deque<float> g_frameSamples;
static float g_sampleSumMs = 0.0f;
static RECT g_fpsRect{};
static int g_fpsW = 0, g_fpsH = 0;
static Size g_cap{}, g_out{};
static float g_ftBuf[FT_SAMPLES] = {};
static int g_ftIdx = 0;
static float g_ftMin = 999, g_ftMax = 0;
static int g_hoverBtn = -1;

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
  g_cfg.fps = GetPrivateProfileIntW(L"FPS", L"fps", 1, ini.c_str()) != 0;
  g_cfg.frametime = GetPrivateProfileIntW(L"FPS", L"frametime", 1, ini.c_str()) != 0;
  g_cfg.frame_timing = GetPrivateProfileIntW(L"FPS", L"frame_timing", 1, ini.c_str()) != 0;
  g_cfg.resolution = GetPrivateProfileIntW(L"FPS", L"resolution", 1, ini.c_str()) != 0;
  g_cfg.background = GetPrivateProfileIntW(L"FPS", L"background", 1, ini.c_str()) != 0;
  g_cfg.text_outline = GetPrivateProfileIntW(L"FPS", L"text_outline", 1, ini.c_str()) != 0;
  g_cfg.fontSize = (int)std::clamp<long>(GetPrivateProfileIntW(L"FPS", L"font_size", 24, ini.c_str()), 12, 48);
  g_cfg.position = (int)std::clamp<long>(GetPrivateProfileIntW(L"FPS", L"position", 0, ini.c_str()), 0, 3);
  int alphaRaw = GetPrivateProfileIntW(L"FPS", L"background_alpha", 50, ini.c_str());
  g_cfg.background_alpha = std::clamp(alphaRaw, 0, 100) / 100.0f;
  // Default screenshot path: Pictures\NRLive
  wchar_t path[MAX_PATH * 4]{};
  GetPrivateProfileStringW(L"General", L"screenshot_path", L"", path, MAX_PATH * 4, ini.c_str());
  if (path[0]) g_shotPath = path;
  else { SHGetFolderPathW(nullptr, CSIDL_MYPICTURES, nullptr, SHGFP_TYPE_CURRENT, path); g_shotPath = path; if (!g_shotPath.empty() && g_shotPath.back() != L'\\') g_shotPath += L'\\'; g_shotPath += L"NRLive"; }
}

static void saveCfg() {
  const std::wstring ini = iniPath();
  WritePrivateProfileStringW(L"FPS", L"enabled", g_fpsVisible ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"FPS", L"fps", g_cfg.fps ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"FPS", L"frametime", g_cfg.frametime ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"FPS", L"frame_timing", g_cfg.frame_timing ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"FPS", L"resolution", g_cfg.resolution ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"FPS", L"background", g_cfg.background ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"FPS", L"text_outline", g_cfg.text_outline ? L"1" : L"0", ini.c_str());
  wchar_t b[32];
  swprintf_s(b, L"%d", g_cfg.fontSize);  WritePrivateProfileStringW(L"FPS", L"font_size", b, ini.c_str());
  swprintf_s(b, L"%d", g_cfg.position);  WritePrivateProfileStringW(L"FPS", L"position", b, ini.c_str());
  swprintf_s(b, L"%d", (int)std::lround(g_cfg.background_alpha * 100.0f)); WritePrivateProfileStringW(L"FPS", L"background_alpha", b, ini.c_str());
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
    roundRect(dc, b, 6, br); DeleteObject(br);
    drawBtnIcon(dc, b, i, active);
  }
}

// ── FPS overlay paint (double-buffered GDI — fast, no UpdateLayeredWindow) ─
static void paintFpsContent(HDC dc, int w, int h) {
  // Draw the HUD into a 32-bit DIB.  Background pixels retain their own
  // alpha so the background slider does not fade the text with it.
  if (g_cfg.background) {
    BYTE a = (BYTE)std::clamp((int)std::lround(g_cfg.background_alpha * 255.0f), 0, 255);
    DWORD bg = ((DWORD)a << 24) | ((DWORD)GetRValue(C_FPS_BG) << 16) |
               ((DWORD)GetGValue(C_FPS_BG) << 8) | GetBValue(C_FPS_BG);
    DWORD* px = (DWORD*)nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    // This function is only used with the DIB DC created by fpsProc;
    // clear it through GDI so the caller remains responsible for the bits.
    RECT rc{0,0,w,h};
    HBRUSH b = CreateSolidBrush(C_FPS_BG);
    FillRect(dc, &rc, b);
    DeleteObject(b);
  }

  int pad = 8, y = pad;
  int fs = g_cfg.fontSize, smFont = (int)(fs * 0.55f);

  if (g_cfg.fps) {
    wchar_t buf[64];
    drawText(dc, L"NRLive", pad, y, smFont, g_cfg.engine_color, g_cfg.text_outline);
    int labelW = (int)(smFont * 4.5);
    int numW = 0;
    if (g_fgActive) {
      swprintf_s(buf, L"%.0f/%.0f", g_smoothFps * 2.0f, g_smoothFps);
      drawText(dc, buf, pad+labelW, y, fs, g_cfg.text_color, g_cfg.text_outline);
      numW = (int)(wcslen(buf)*(fs*0.6));
      drawText(dc, L"FG", pad+labelW+numW+4, y+(fs-smFont), smFont, g_cfg.text_color, g_cfg.text_outline);
    } else {
      swprintf_s(buf, L"%.1f", g_smoothFps);
      drawText(dc, buf, pad+labelW, y, fs, g_cfg.text_color, g_cfg.text_outline);
      numW = buf[0] ? (int)(wcslen(buf)*(fs*0.6)) : 0;
      drawText(dc, L"FPS", pad+labelW+numW+4, y+(fs-smFont), smFont, g_cfg.text_color, g_cfg.text_outline);
    }
    if (g_cfg.frametime) {
      int ftX = pad+labelW+numW+(int)(smFont*4.5);
      swprintf_s(buf, L"%.1f", g_smoothMs);
      drawText(dc, buf, ftX, y, fs, g_cfg.text_color, g_cfg.text_outline);
      drawText(dc, L"ms", ftX+(int)(wcslen(buf)*fs*0.6)+2, y+(fs-smFont), smFont, g_cfg.text_color, g_cfg.text_outline);
    }
    y += fs + 8;
  } else if (g_cfg.frametime) {
    wchar_t buf[64]; swprintf_s(buf, L"%.1f ms", g_smoothMs);
    drawText(dc, buf, pad, y, smFont, g_cfg.text_color, g_cfg.text_outline);
    y += smFont + 8;
  }

  int graphH = g_cfg.frame_timing ? FT_HEIGHT : 0;
  int graphW = w - pad * 2;
  if (graphH > 0 && graphW > 10) {
    wchar_t buf[80];
    drawText(dc, L"Frametime", pad, y, smFont, g_cfg.engine_color, g_cfg.text_outline);
    swprintf_s(buf, L"min: %.1fms  max: %.1fms", g_ftMin, g_ftMax);
    drawText(dc, buf, pad+(int)(smFont*5), y, smFont, g_cfg.text_color, g_cfg.text_outline);
    y += smFont + 4;
    HPEN pen = CreatePen(PS_SOLID, 1, g_cfg.frametime_color);
    auto op = (HPEN)SelectObject(dc, pen);
    int n = std::min(FT_SAMPLES, graphW);
    for (int i = 0; i < n; ++i) {
      int idx = (g_ftIdx - n + i + FT_SAMPLES) % FT_SAMPLES;
      float v = std::clamp(g_ftBuf[idx], 0.0f, (float)FT_RANGE);
      int px = pad + i;
      int py = y + graphH - 1 - (int)((v / (float)FT_RANGE) * (graphH - 1));
      if (i == 0) MoveToEx(dc, px, py, nullptr); else LineTo(dc, px, py);
    }
    SelectObject(dc, op); DeleteObject(pen);
    y += graphH + 4;
  }

  if (g_cfg.resolution) {
    wchar_t buf[64]; swprintf_s(buf, L"%ux%u", g_cap.w, g_cap.h);
    drawText(dc, buf, pad, y, smFont, g_cfg.text_color, g_cfg.text_outline);
  }
}

static void renderFpsLayered() {
  if (!g_fps || !IsWindow(g_fps) || !g_fpsVisible) return;
  RECT rc{}; GetClientRect(g_fps, &rc);
  int w = rc.right, h = rc.bottom;
  if (w <= 0 || h <= 0) return;

  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bmp) { DeleteDC(mem); ReleaseDC(nullptr, screen); return; }
  HBITMAP old = (HBITMAP)SelectObject(mem, bmp);

  // Start fully transparent. paintFpsContent only draws opaque text/graph.
  std::memset(bits, 0, (size_t)w * h * 4);
  paintFpsContent(mem, w, h);

  // Convert the GDI-painted pixels into ARGB: background gets configured
  // alpha; all foreground pixels remain fully opaque.
  DWORD* px = (DWORD*)bits;
  BYTE bgR = GetRValue(C_FPS_BG), bgG = GetGValue(C_FPS_BG), bgB = GetBValue(C_FPS_BG);
  BYTE bgA = (BYTE)std::clamp((int)std::lround(g_cfg.background_alpha * 255.0f), 0, 255);
  for (int i = 0; i < w*h; ++i) {
    BYTE* p = ((BYTE*)px) + i*4;
    bool isBg = p[0] == bgB && p[1] == bgG && p[2] == bgR;
    p[3] = (g_cfg.background && isBg) ? bgA : (isBg ? 0 : 255);
  }

  POINT dst{g_fpsRect.left, g_fpsRect.top}, src{0,0};
  SIZE size{w,h};
  BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  UpdateLayeredWindow(g_fps, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);

  SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, screen);
}

static void updateFpsPos() {
  if (!g_fps || !IsWindow(g_fps) || !g_output) return;
  if (!g_fpsVisible) { ShowWindow(g_fps, SW_HIDE); return; }

  int dynW = std::max(210, g_cfg.fontSize * 10);
  int fs = g_cfg.fontSize, smFont = (int)(fs * 0.55f);
  int totalH = 8;
  if (g_cfg.fps) totalH += fs + 8;
  else if (g_cfg.frametime) totalH += smFont + 8;
  if (g_cfg.frame_timing) totalH += smFont + 4 + FT_HEIGHT + 4;
  if (g_cfg.resolution) totalH += smFont + 4;
  totalH += 8;

  RECT o{}; GetWindowRect(g_output, &o);
  int x = o.left + 10, y = o.top + 10;
  if (g_cfg.position == 1) x = o.right - dynW - 10;
  if (g_cfg.position == 2) y = o.bottom - totalH - 10;
  if (g_cfg.position == 3) { x = o.right - dynW - 10; y = o.bottom - totalH - 10; }
  RECT nr{x,y,x+dynW,y+totalH};
  bool changed = std::memcmp(&nr, &g_fpsRect, sizeof(RECT)) != 0 ||
                 g_fpsW != dynW || g_fpsH != totalH;
  if (changed) {
    g_fpsRect = nr; g_fpsW = dynW; g_fpsH = totalH;
    SetWindowPos(g_fps, HWND_TOPMOST, x, y, dynW, totalH,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
  } else if (!IsWindowVisible(g_fps)) {
    ShowWindow(g_fps, SW_SHOWNOACTIVATE);
  }
  renderFpsLayered();
}

static void paintSettings(HWND h, HDC dc) {
  RECT rc{}; GetClientRect(h, &rc);
  HBRUSH bg = CreateSolidBrush(C_WIN); FillRect(dc, &rc, bg); DeleteObject(bg);
  HBRUSH tb = CreateSolidBrush(C_DARK); RECT tr{0,0,rc.right,48}; FillRect(dc,&tr,tb); DeleteObject(tb);
  HFONT title=makeFont(18,true); auto old=(HFONT)SelectObject(dc,title); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,C_TEXT);
  RECT rt{24,10,rc.right-24,40}; DrawTextW(dc,L"NRLive Settings",-1,&rt,DT_LEFT|DT_VCENTER|DT_SINGLELINE); SelectObject(dc,old); DeleteObject(title);
  auto section=[&](const wchar_t* s,int y){HFONT f=makeFont(12,true);auto o=(HFONT)SelectObject(dc,f);SetTextColor(dc,C_RED);RECT r{24,y,rc.right-24,y+18};DrawTextW(dc,s,-1,&r,DT_LEFT|DT_VCENTER|DT_SINGLELINE);HPEN p=CreatePen(PS_SOLID,1,C_CARD2);auto op=(HPEN)SelectObject(dc,p);MoveToEx(dc,24,y+21,nullptr);LineTo(dc,rc.right-24,y+21);SelectObject(dc,op);DeleteObject(p);SelectObject(dc,o);DeleteObject(f);};
  RECT tabGeneral{244, 10, 336, 38}, tabNr{344, 10, 456, 38};
  HBRUSH tg = CreateSolidBrush(g_dlssNrTab ? C_GHOST : C_RED_TINT); FillRect(dc, &tabGeneral, tg); DeleteObject(tg);
  HBRUSH tn = CreateSolidBrush(g_dlssNrTab ? C_RED_TINT : C_GHOST); FillRect(dc, &tabNr, tn); DeleteObject(tn);
  drawText(dc, L"General", 260, 16, 12, C_TEXT, false);
  drawText(dc, L"DLSSNR", 370, 16, 12, C_TEXT, false);
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
     drawText(dc, L"New model requires weights extracted from NVIDIA DLL 310.9.16.", 24, 652, 10, C_DIM, false);
    drawText(dc, g_saveStatus.c_str(), 24, 712, 10, C_MUTED, false);
    return;
  }
  section(L"FPS OVERLAY",58);
  section(L"APPEARANCE",148);
  section(L"FSR SHARPENING",278);
  section(L"FRAME LIMITER",352);
  section(L"SCREENSHOT",424);
  // Slider labels/values are painted here; the native trackbars remain for interaction.
  drawText(dc,L"Font size",24,176,13,C_TEXT,false);
  drawText(dc,L"Bg alpha",24,212,13,C_TEXT,false);
  drawText(dc,L"Engine",24,248,12,C_DIM,false); drawText(dc,L"Text",130,248,12,C_DIM,false); drawText(dc,L"Graph",224,248,12,C_DIM,false);
  auto swatch=[&](int x,COLORREF col){HBRUSH br=CreateSolidBrush(col);RECT r{x,246,x+40,268};FillRect(dc,&r,br);DeleteObject(br);HPEN p=CreatePen(PS_SOLID,1,C_HOVER);auto op=(HPEN)SelectObject(dc,p);auto ob=(HBRUSH)SelectObject(dc,GetStockObject(NULL_BRUSH));Rectangle(dc,r.left,r.top,r.right,r.bottom);SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(p);};
  swatch(80,g_cfg.engine_color);swatch(174,g_cfg.text_color);swatch(268,g_cfg.frametime_color);
  drawText(dc,L"Sharpening",24,304,13,C_TEXT,false);
  wchar_t sharp[16]{}; swprintf_s(sharp, L"%.2f", g_sharpness); drawText(dc,sharp,410,304,13,C_RED_HOT,false);
  drawText(dc,L"Cap FPS",142,378,13,C_TEXT,false);
  drawText(dc,L"Folder",24,440,13,C_TEXT,false); drawText(dc,g_saveStatus.c_str(),24,564,10,C_MUTED,false);
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
        else if (i == 1) { g_fpsVisible = !g_fpsVisible; saveCfg(); updateFpsPos(); }
        else if (i == 2) { g_screenshot = true; }
        else if (i == 3) { g_frameLimit.enabled = !g_frameLimit.enabled; g_toggleFrameLimit = true; saveCfg(); }
        else if (i == 4) { g_fgEnabled = !g_fgEnabled; g_toggleFg = true; saveCfg(); }
        else if (i == 5) { g_dlssNr.enabled = !g_dlssNr.enabled; g_toggleDlssNr = true; saveCfg(); }
        else { if (IsWindowVisible(g_settings)) ShowWindow(g_settings, SW_HIDE);
          else { int sw=GetSystemMetrics(SM_CXSCREEN), sh=GetSystemMetrics(SM_CYSCREEN);
            wchar_t fpsText[16]{}; swprintf_s(fpsText, L"%d", g_frameLimit.fps); SetWindowTextW(GetDlgItem(g_settings, 206), fpsText);
            SendMessageW(GetDlgItem(g_settings, 207), CB_SETCURSEL, g_frameLimit.method, 0);
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

static LRESULT CALLBACK fpsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_NCHITTEST) return HTTRANSPARENT;
  if (m == WM_ERASEBKGND) return 1;
  return DefWindowProcW(h, m, w, l);
}

static void drawOwnerButton(DRAWITEMSTRUCT* dis, const wchar_t* label, bool checked) {
  HDC dc = dis->hDC; RECT r = dis->rcItem;
  HBRUSH bg = CreateSolidBrush(C_WIN); FillRect(dc, &r, bg); DeleteObject(bg);
  RECT box{r.left, r.top+2, r.left+16, r.top+18};
  HBRUSH b = CreateSolidBrush(checked ? C_RED : C_INPUT); FillRect(dc, &box, b); DeleteObject(b);
  HPEN p = CreatePen(PS_SOLID, 1, checked ? C_RED_HOT : C_HOVER); auto op=(HPEN)SelectObject(dc,p);
  auto ob=(HBRUSH)SelectObject(dc,GetStockObject(NULL_BRUSH)); Rectangle(dc,box.left,box.top,box.right,box.bottom);
  if(checked){MoveToEx(dc,box.left+3,box.top+8,nullptr);LineTo(dc,box.left+7,box.top+12);LineTo(dc,box.right-3,box.top+4);}
  SelectObject(dc,ob); SelectObject(dc,op); DeleteObject(p);
  drawText(dc,label,box.right+8,r.top,13,C_TEXT,false);
}

static void setSettingsTab(HWND h, bool dlssNr) {
  // Native child controls stay visible independently of the parent's custom
  // paint path. Toggle both complete control sets, not just the NR controls.
  static constexpr int generalIds[] = {
    101,102,103,104,105,106,
    201,202,203,204,205,206,207,208,
    301,302,303,401,402,403,
    601,602,603,604,605,606,607,608
  };
  for (int id : generalIds)
    if (HWND control = GetDlgItem(h, id))
      ShowWindow(control, dlssNr ? SW_HIDE : SW_SHOW);
  for (int id = 510; id <= 522; ++id)
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
    switch (dis->CtlID) {
      case 101: drawOwnerButton(dis, L"FPS", g_cfg.fps); return TRUE;
      case 102: drawOwnerButton(dis, L"Frametime", g_cfg.frametime); return TRUE;
      case 103: drawOwnerButton(dis, L"Graph", g_cfg.frame_timing); return TRUE;
      case 104: drawOwnerButton(dis, L"Resolution", g_cfg.resolution); return TRUE;
      case 105: drawOwnerButton(dis, L"Background", g_cfg.background); return TRUE;
      case 106: drawOwnerButton(dis, L"Enabled", g_frameLimit.enabled); return TRUE;
      case 510: drawOwnerButton(dis, g_dlssNr.style == 0 ? L"Style: Neutral" : g_dlssNr.style == 1 ? L"Style: Natural" : L"Style: Cinematic", true); return TRUE;
      case 516: drawOwnerButton(dis, L"Automatic skin mask", g_dlssNr.automaticSkinMask); return TRUE;
      case 517: drawOwnerButton(dis, L"Residual stabilizer (motion-only)", g_dlssNr.stabilizer); return TRUE;
       case 523: drawOwnerButton(dis, g_dlssNr.newModel ? L"Model: New (310.9.16)" : L"Model: Original (310.8.0)", g_dlssNr.newModel); return TRUE;
      case 520: {
        HDC dc = dis->hDC; RECT r = dis->rcItem;
        HBRUSH bg = CreateSolidBrush(C_RED); FillRect(dc, &r, bg); DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, C_DARK);
        HFONT font = makeFont(12, true); HFONT prior = (HFONT)SelectObject(dc, font);
        DrawTextW(dc, L"Save settings", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, prior); DeleteObject(font);
        return TRUE;
      }
      case 205: {
        HDC dc = dis->hDC; RECT r = dis->rcItem;
        HBRUSH bg = CreateSolidBrush(C_RED); FillRect(dc, &r, bg); DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, C_DARK);
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
    // FPS is a visual-only HUD: never let it consume mouse hit-tests.
    if (h == g_fps) return HTTRANSPARENT;
    POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    ScreenToClient(h, &p);
    if (p.y < 48 && !((p.x >= 244 && p.x < 336) || (p.x >= 344 && p.x < 456))) return HTCAPTION;
    return HTCLIENT;
  }
  if (m == WM_LBUTTONUP) {
    POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    if (p.y < 48 && p.x >= 244 && p.x < 336) {
      g_dlssNrTab = false;
      setSettingsTab(h, false);
      InvalidateRect(h, nullptr, TRUE); UpdateWindow(h); return 0;
    }
    if (p.y < 48 && p.x >= 344 && p.x < 456) {
      g_dlssNrTab = true;
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
    if (LOWORD(w) >= 101 && LOWORD(w) <= 106) {
      // These are BS_OWNERDRAW controls, so Windows does not maintain a
      // checkbox state for us. Toggle our actual config state directly.
      if (LOWORD(w) == 101) g_cfg.fps = !g_cfg.fps;
      if (LOWORD(w) == 102) g_cfg.frametime = !g_cfg.frametime;
      if (LOWORD(w) == 103) g_cfg.frame_timing = !g_cfg.frame_timing;
      if (LOWORD(w) == 104) g_cfg.resolution = !g_cfg.resolution;
      if (LOWORD(w) == 105) g_cfg.background = !g_cfg.background;
      if (LOWORD(w) == 106) g_frameLimit.enabled = !g_frameLimit.enabled;
      InvalidateRect(h, nullptr, FALSE);
      if (g_fpsVisible) { updateFpsPos(); renderFpsLayered(); }
      saveCfg();
      if (g_fpsVisible) updateFpsPos();
      if (LOWORD(w) == 106) { InvalidateRect(g_ui, nullptr, FALSE); UpdateWindow(g_ui); }
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
    if (LOWORD(w) >= 401 && LOWORD(w) <= 403) {
      static COLORREF cust[16] = {};
      CHOOSECOLORW cc{}; cc.lStructSize = sizeof(cc);
      cc.hwndOwner = h; cc.lpCustColors = cust;
      cc.Flags = CC_FULLOPEN | CC_RGBINIT;
      COLORREF* target = nullptr;
      if (LOWORD(w) == 401) { cc.rgbResult = g_cfg.engine_color; target = &g_cfg.engine_color; }
      if (LOWORD(w) == 402) { cc.rgbResult = g_cfg.text_color; target = &g_cfg.text_color; }
      if (LOWORD(w) == 403) { cc.rgbResult = g_cfg.frametime_color; target = &g_cfg.frametime_color; }
      if (target && ChooseColorW(&cc)) { *target = cc.rgbResult; saveCfg(); }
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
    } else if (ctrl == GetDlgItem(h, 201)) {
      g_cfg.fontSize = (int)SendMessageW(ctrl, TBM_GETPOS, 0, 0);
      wchar_t b[8]; swprintf_s(b, L"%d", g_cfg.fontSize);
      SetWindowTextW(GetDlgItem(h, 301), b); saveCfg(); updateFpsPos();
    } else if (ctrl == GetDlgItem(h, 202)) {
      int val = (int)SendMessageW(ctrl, TBM_GETPOS, 0, 0);
      g_cfg.background_alpha = (float)val / 100.0f;
      wchar_t b[8]; swprintf_s(b, L"%d%%", val);
      SetWindowTextW(GetDlgItem(h, 302), b); saveCfg(); renderFpsLayered();
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
  c.lpfnWndProc = fpsProc;     c.lpszClassName = FPS_CLS; RegisterClassExW(&c);
  c.lpfnWndProc = settingsProc; c.lpszClassName = SET_CLS; RegisterClassExW(&c);

  int barW = BAR_PAD*2 + 7*BTN_SIZE + 6*BTN_GAP;
  g_ui = CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST, UI_CLS, L"",
    WS_POPUP, 0, 0, barW, BAR_PAD*2+BTN_SIZE, nullptr, nullptr, inst, nullptr);

  // FPS window — layered for per-pixel alpha via UpdateLayeredWindow
  g_fps = CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST,
    FPS_CLS, L"", WS_POPUP, 0, 0, 200, 100, nullptr, nullptr, inst, nullptr);

  // Settings panel
  { int sw=GetSystemMetrics(SM_CXSCREEN), sh=GetSystemMetrics(SM_CYSCREEN);
    g_settings = CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST, SET_CLS, L"",
      WS_POPUP, (sw-480)/2, (sh-740)/2, 480, 740, nullptr, nullptr, inst, nullptr); }

  if (g_settings) {
    CreateWindowW(L"BUTTON", L"", WS_CHILD|WS_VISIBLE|BS_OWNERDRAW, 24, 84, 80, 22, g_settings, (HMENU)101, inst, nullptr);
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
    CreateWindowW(L"BUTTON", L"", WS_CHILD|WS_VISIBLE|BS_OWNERDRAW, 110, 84, 100, 22, g_settings, (HMENU)102, inst, nullptr);
    CreateWindowW(L"BUTTON", L"", WS_CHILD|WS_VISIBLE|BS_OWNERDRAW, 216, 84, 80, 22, g_settings, (HMENU)103, inst, nullptr);
    CreateWindowW(L"BUTTON", L"", WS_CHILD|WS_VISIBLE|BS_OWNERDRAW, 302, 84, 100, 22, g_settings, (HMENU)104, inst, nullptr);
    CreateWindowW(L"BUTTON", L"", WS_CHILD|WS_VISIBLE|BS_OWNERDRAW, 24, 112, 100, 22, g_settings, (HMENU)105, inst, nullptr);
    CreateWindowW(L"STATIC", L"Font size:", WS_CHILD|WS_VISIBLE|SS_LEFT, 24, 178, 70, 18, g_settings, (HMENU)601, inst, nullptr);
    CreateWindowW(L"STATIC", L"24", WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTER, 410, 178, 30, 18, g_settings, (HMENU)301, inst, nullptr);
    CreateWindowExW(0, L"msctls_trackbar32", L"", WS_CHILD|WS_VISIBLE|TBS_NOTICKS|TBS_AUTOTICKS, 100, 174, 300, 26, g_settings, (HMENU)201, inst, nullptr);
    SendMessageW(GetDlgItem(g_settings, 201), TBM_SETRANGE, TRUE, MAKELONG(12, 48));
    SendMessageW(GetDlgItem(g_settings, 201), TBM_SETPOS, TRUE, g_cfg.fontSize);
    CreateWindowW(L"STATIC", L"Bg alpha:", WS_CHILD|WS_VISIBLE|SS_LEFT, 24, 214, 70, 18, g_settings, (HMENU)602, inst, nullptr);
    CreateWindowW(L"STATIC", L"50%", WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTER, 410, 214, 30, 18, g_settings, (HMENU)302, inst, nullptr);
    CreateWindowExW(0, L"msctls_trackbar32", L"", WS_CHILD|WS_VISIBLE|TBS_NOTICKS|TBS_AUTOTICKS, 100, 210, 300, 26, g_settings, (HMENU)202, inst, nullptr);
    SendMessageW(GetDlgItem(g_settings, 202), TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    SendMessageW(GetDlgItem(g_settings, 202), TBM_SETPOS, TRUE, (int)(g_cfg.background_alpha*100));
    CreateWindowW(L"STATIC", L"Engine:", WS_CHILD|WS_VISIBLE|SS_LEFT, 24, 250, 50, 18, g_settings, (HMENU)603, inst, nullptr);
    CreateWindowW(L"BUTTON", L"", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 80, 246, 40, 22, g_settings, (HMENU)401, inst, nullptr);
    CreateWindowW(L"STATIC", L"Text:", WS_CHILD|WS_VISIBLE|SS_LEFT, 130, 250, 40, 18, g_settings, (HMENU)604, inst, nullptr);
    CreateWindowW(L"BUTTON", L"", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 174, 246, 40, 22, g_settings, (HMENU)402, inst, nullptr);
    CreateWindowW(L"STATIC", L"Graph:", WS_CHILD|WS_VISIBLE|SS_LEFT, 224, 250, 40, 18, g_settings, (HMENU)605, inst, nullptr);
    CreateWindowW(L"BUTTON", L"", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 268, 246, 40, 22, g_settings, (HMENU)403, inst, nullptr);
    // FSR sharpening slider belongs to overlay settings.
    CreateWindowW(L"STATIC", L"Sharpening amount", WS_CHILD|WS_VISIBLE|SS_LEFT, 24, 306, 130, 18, g_settings, (HMENU)606, inst, nullptr);
    wchar_t sharpText[16]{}; swprintf_s(sharpText, L"%.2f", g_sharpness);
    CreateWindowW(L"STATIC", sharpText, WS_CHILD|WS_VISIBLE|SS_LEFT, 410, 306, 40, 18, g_settings, (HMENU)303, inst, nullptr);
    CreateWindowExW(0, L"msctls_trackbar32", L"", WS_CHILD|WS_VISIBLE|TBS_NOTICKS|TBS_AUTOTICKS, 24, 326, 430, 26, g_settings, (HMENU)208, inst, nullptr);
    SendMessageW(GetDlgItem(g_settings, 208), TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    SendMessageW(GetDlgItem(g_settings, 208), TBM_SETPOS, TRUE, (int)std::lround(g_sharpness * 100.0f));
    // Frame limiter controls
    CreateWindowW(L"BUTTON", L"", WS_CHILD|WS_VISIBLE|BS_OWNERDRAW, 24, 376, 100, 22, g_settings, (HMENU)106, inst, nullptr);
     CreateWindowW(L"STATIC", L"Cap FPS", WS_CHILD|WS_VISIBLE|SS_LEFT, 142, 378, 52, 18, g_settings, (HMENU)607, inst, nullptr);
     CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD|WS_VISIBLE|ES_NUMBER|ES_AUTOHSCROLL, 196, 374, 58, 24, g_settings, (HMENU)206, inst, nullptr);
     HWND methodBox = CreateWindowW(L"COMBOBOX", L"", WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL, 266, 374, 188, 120, g_settings, (HMENU)207, inst, nullptr);
     SendMessageW(methodBox, CB_ADDSTRING, 0, (LPARAM)L"Early - smoother");
     SendMessageW(methodBox, CB_ADDSTRING, 0, (LPARAM)L"Late - snappier");
     SendMessageW(methodBox, CB_SETCURSEL, g_frameLimit.method, 0);
     wchar_t fpsText[16]{}; swprintf_s(fpsText, L"%d", g_frameLimit.fps); SetWindowTextW(GetDlgItem(g_settings, 206), fpsText);
     CreateWindowW(L"STATIC", L"Folder:", WS_CHILD|WS_VISIBLE|SS_LEFT, 24, 440, 60, 18, g_settings, (HMENU)608, inst, nullptr);
    g_pathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_shotPath.c_str(),
      WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL, 24, 462, 340, 24, g_settings, (HMENU)203, inst, nullptr);
    CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD|WS_VISIBLE, 374, 462, 80, 24, g_settings, (HMENU)204, inst, nullptr);
    CreateWindowW(L"BUTTON", L"", WS_CHILD|WS_VISIBLE|BS_OWNERDRAW, 342, 518, 112, 30, g_settings, (HMENU)205, inst, nullptr);
    CreateWindowW(L"BUTTON", L"", WS_CHILD|BS_OWNERDRAW, 342, 632, 112, 30, g_settings, (HMENU)520, inst, nullptr);
    CheckDlgButton(g_settings, 101, g_cfg.fps ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_settings, 102, g_cfg.frametime ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_settings, 103, g_cfg.frame_timing ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_settings, 104, g_cfg.resolution ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_settings, 105, g_cfg.background ? BST_CHECKED : BST_UNCHECKED);
    SetWindowTextW(g_pathEdit, g_shotPath.c_str());
    setSettingsTab(g_settings, false);
    ShowWindow(g_settings, SW_HIDE);
  }

  if (g_fps) {
    // Do NOT call SetLayeredWindowAttributes here. UpdateLayeredWindow owns
    // the per-pixel alpha for this window; mixing the two APIs breaks the
    // layered surface and results in an opaque/black HUD.
    if (g_fpsVisible) ShowWindow(g_fps, SW_SHOWNOACTIVATE);
  }

  g_initialized = true;
  return true;
}

void overlayShutdown() {
  saveCfg();
  if (g_fps) DestroyWindow(g_fps);
  if (g_ui) DestroyWindow(g_ui);
  if (g_settings) DestroyWindow(g_settings);
  g_fps = g_ui = g_settings = nullptr; g_initialized = false;
}

void overlaySetPresentationVisible(bool visible) {
  if (!g_fps || !IsWindow(g_fps)) return;
  if (!visible) {
    ShowWindow(g_fps, SW_HIDE);
    return;
  }
  // Recompute from the current output rectangle and restore topmost ordering;
  // never leave the HUD visible at the old desktop position during Alt-Tab.
  updateFpsPos();
  if (g_fpsVisible) {
    SetWindowPos(g_fps, HWND_TOPMOST, 0, 0, 0, 0,
      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }
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
  // Re-assert FPS window topmost when overlay toggles — applyOverlayActivation
  // can push NRLive above the FPS window.  This runs once on toggle (not
  // every frame) so no z-order fighting / stutter.
  if (g_fps && g_fpsVisible) {
    SetWindowPos(g_fps, HWND_TOPMOST, 0, 0, 0, 0,
      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }
  updateFpsPos();
}

void overlayUpdate(float fps, float ms, Size cap, Size out) {
  g_lastFps = fps; g_lastMs = ms; g_cap = cap; g_out = out;

  // MangoHud-style 500 ms sampling. This is deliberately independent from
  // the raw instantaneous FPS so a single frame spike cannot make the HUD
  // jump wildly between values.
  if (ms >= 1.0f && ms <= 1000.0f) {
    g_frameSamples.push_back(ms);
    g_sampleSumMs += ms;
    while (g_frameSamples.size() > 1 && g_sampleSumMs - g_frameSamples.front() > 500.0f) {
      g_sampleSumMs -= g_frameSamples.front();
      g_frameSamples.pop_front();
    }
    g_smoothMs = g_sampleSumMs / (float)std::max<size_t>(1, g_frameSamples.size());
    g_smoothFps = g_smoothMs > 0.001f ? 1000.0f / g_smoothMs : 0.0f;
  }

  if (g_fpsVisible) {
    g_ftBuf[g_ftIdx] = ms;
    g_ftIdx = (g_ftIdx + 1) % FT_SAMPLES;
    g_ftMin = 9999; g_ftMax = 0;
    for (int i = 0; i < FT_SAMPLES; ++i)
      if (g_ftBuf[i] > 0) { g_ftMin = std::min(g_ftMin, g_ftBuf[i]); g_ftMax = std::max(g_ftMax, g_ftBuf[i]); }
    updateFpsPos();
  }
}

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
  if (g_fgActive == active) return;
  g_fgActive = active;
  if (g_fpsVisible) renderFpsLayered();
}
const OverlayHudConfig& overlayConfig() { return g_cfg; }
std::wstring overlayScreenshotPath() { return g_shotPath; }
