#pragma once
#include <windows.h>
#include <string>
#include "graphics.h"
#include "hud_config.h"
#include "nr_build.h"

// Frame pacing settings persisted in scaleconfig.ini.
struct OverlayFrameLimitConfig {
  bool enabled = false;
  int fps = 60;
  int method = 0; // 0=early/smooth, 1=late/snappy
};

struct OverlayDlssNrConfig {
  bool enabled = false;
  bool newModel = false; // 310.9.16 model; requires dlssnr-amd/dlssnr-small.bin
  float modelScale = 1.0f;
  int style = 0; // 0=Neutral, 1=Natural, 2=Cinematic
  float structure = 1.0f;
  float intensity = 1.0f;
  float colorStrength = 0.0f; // source chroma at 0, NR chroma at 1
  int passes = 1; // neural refinement passes, 1..3
  float localTone = 1.0f;
  float maxRatio = 2.0f;
  float skinStructure = -1.0f;
  float historyStrength = 0.2f;
  bool automaticSkinMask = true;
  bool stabilizer = true; // motion-only residual stabilizer
};

// ── Overlay API ───────────────────────────────────────────────────────────
// The overlay has two parts:
//   1. Top bar (buttons: FSR / FPS / Camera / Settings) — only visible when
//      overlay mode is open.
//   2. MangoHud-style performance HUD (hud.cpp) — persists after overlay mode
//      closes, toggled by the FPS button or its own hotkey.
// Both are separate top-level windows above the NRLive output window.

bool overlayInit(HINSTANCE inst, HWND output);
void overlayShutdown();
void overlaySetOpen(bool open);             // show/hide top bar + settings
void overlaySetPresentationVisible(bool visible); // hide/re-anchor FPS HUD across focus changes
// Per-frame numbers for the HUD. Call once per presented source frame.
struct OverlayFrameStats {
  float sourceMs = 0;          // interval between captured frames
  Size capture{}, output{};
  bool fsr = false, nr = false;
  NrBuild nrBuild;             // OpenNR's network build
  float latencyMs = -1;        // frame acquired -> present call, <0 unknown
  float gpuMotionMs = -1, gpuNrMs = -1, gpuFsrMs = -1, gpuTotalMs = -1;
};
void overlayUpdate(const OverlayFrameStats& stats);
void overlaySetAdapter(LUID luid);           // GPU the HUD reports sensors for
void overlaySetCaptureExcluded(bool exclude); // hide HUD from desktop capture (DXGI mode)
bool overlayWantsGpuTimes();
// Reserved global shortcuts must not fire while a user is capturing a keybind.
void overlaySetReservedHotkeys(UINT stopModifiers, UINT stopVk, UINT overlayModifiers, UINT overlayVk);
bool overlayIsCapturingKeybind();
void overlaySetFsrEnabled(bool enabled);
// Keybinds are polled from overlayUpdate so they remain active while the game owns focus.
bool overlayConsumeFsrToggle();             // returns true once when FSR button clicked
bool overlayConsumeScreenshot();            // returns true once when Camera button clicked
bool overlayConsumeFrameLimitToggle();       // returns true once when the CAP button is clicked
OverlayFrameLimitConfig overlayFrameLimitConfig();
bool overlayConsumeFgToggle();                // returns true once when the FG button is clicked
bool overlayConsumeDlssNrToggle();            // returns true once when the NR button is clicked
const OverlayDlssNrConfig& overlayDlssNrConfig();
void overlaySetDlssNrEnabled(bool enabled);
bool overlayFgEnabled();
int overlayFgBackend(); // 0=FSR, 1=ANR; applied on the next launch
void overlaySetFgBackend(int backend);
float overlaySharpness();
void overlaySetFgEnabled(bool enabled);
void overlaySetFgActive(bool active);
const OverlayHudConfig& overlayConfig();
// The NR tab edits OpenNR's [OpenNR] settings in place when that runtime runs,
// the 0.0.6 bridge's [DLSSNR] ones otherwise.
class NrConfigWatcher;
void overlaySetNrConfig(NrConfigWatcher* config, bool openNr);
std::wstring overlayScreenshotPath();
