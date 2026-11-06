#pragma once
#include <windows.h>
#include "graphics.h"
#include "hud_config.h"
#include "nr_build.h"

// Performance HUD drawn into a layered window on top of the NRLive output.
// The window lives on its own thread: drawing (GDI+) and sensor reads never
// run on the capture/present loop, which only pushes a few numbers per frame.
struct HudFrame {
  float sourceMs = 0;          // interval between captured frames
  Size capture{}, output{};
  bool fsr = false, nr = false, fg = false;
  uint32_t fgMultiplier = 2;
  NrBuild nrBuild;
  float latencyMs = -1;        // frame acquired -> present call
  float gpuMotionMs = -1, gpuNrMs = -1, gpuFsrMs = -1, gpuTotalMs = -1;
};

bool hudStart(HINSTANCE inst, HWND output);
void hudStop();
void hudSetConfig(const OverlayHudConfig& cfg);
void hudSetEnabled(bool enabled);            // user toggle (FPS button / hotkey)
void hudSetPresentationVisible(bool visible); // output shown (focus handling)
void hudSetAdapter(LUID luid);
void hudSetCaptureExcluded(bool exclude);
void hudReassertTopmost();
void hudPushFrame(const HudFrame& f);
