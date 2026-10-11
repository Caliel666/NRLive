#pragma once
#include <windows.h>
#include <string>
#include "graphics.h"

// ── MangoHud-style overlay config ─────────────────────────────────────────
struct OverlayHudConfig {
  bool fps = true;              // show FPS number
  bool frametime = true;        // show frametime ms
  bool frame_timing = true;    // show frametime graph
  bool resolution = true;      // show resolution
  bool background = true;      // show semi-transparent background
  bool text_outline = true;   // draw black outline around text
  int  fontSize = 24;          // main font size (px)
  int  position = 0;           // 0=TL 1=TR 2=BL 3=BR
  float background_alpha = 0.5f; // 0..1
  // MangoHud default colors (RRGGBB → RGB)
  COLORREF engine_color = RGB(235,91,91);    // #EB5B5B red
  COLORREF text_color = RGB(255,255,255);   // #FFFFFF white
  COLORREF frametime_color = RGB(0,255,0);  // #00FF00 green
};

// Frame pacing settings persisted in scaleconfig.ini.
struct OverlayFrameLimitConfig {
  bool enabled = false;
  int fps = 60;
  int method = 0; // 0=early/smooth, 1=late/snappy
};

struct OverlayDlssNrConfig {
  bool enabled = false;
  float modelScale = 1.0f;
  int style = 0; // 0=Neutral, 1=Natural, 2=Cinematic
  float structure = 1.0f;
  float intensity = 1.0f;
  float colorStrength = 0.0f; // source chroma at 0, NR chroma at 1
  int passes = 1; // neural refinement passes, 1..3
  float localTone = 1.0f;
  float maxRatio = 2.0f;
  float skinStructure = -1.0f;
  float historyStrength = 0.8f;
  bool automaticSkinMask = true;
  bool stabilizer = false; // motion-only residual stabilizer
  bool acoMode = false; // upstream ACO network machine code; restart required
};

// ── Overlay API ───────────────────────────────────────────────────────────
// The overlay has two parts:
//   1. Top bar (buttons: FSR / FPS / Camera / Settings) — only visible when
//      overlay mode is open.
//   2. MangoHud-style FPS overlay — persists even after overlay mode closes,
//      toggled by the FPS button.
// Both are GDI child windows of the main NRLive output window.

bool overlayInit(HINSTANCE inst, HWND output);
void overlayShutdown();
void overlaySetOpen(bool open);             // show/hide top bar + settings
void overlaySetPresentationVisible(bool visible); // hide/re-anchor FPS HUD across focus changes
void overlayUpdate(float fps, float frametimeMs, Size capture, Size output);
void overlaySetFsrEnabled(bool enabled);
bool overlayConsumeFsrToggle();             // returns true once when FSR button clicked
bool overlayConsumeScreenshot();            // returns true once when Camera button clicked
bool overlayConsumeFrameLimitToggle();       // returns true once when the CAP button is clicked
OverlayFrameLimitConfig overlayFrameLimitConfig();
bool overlayConsumeFgToggle();                // returns true once when the FG button is clicked
bool overlayConsumeDlssNrToggle();            // returns true once when the NR button is clicked
const OverlayDlssNrConfig& overlayDlssNrConfig();
void overlaySetDlssNrEnabled(bool enabled);
bool overlayFgEnabled();
float overlaySharpness();
void overlaySetFgEnabled(bool enabled);
void overlaySetFgActive(bool active);
const OverlayHudConfig& overlayConfig();
std::wstring overlayScreenshotPath();
