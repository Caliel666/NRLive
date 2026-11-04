#pragma once
#include <windows.h>
#include <string>
#include "graphics.h"

struct HudInfo {
  float fps = 0;
  Size capture{};
  Size output{};
  bool visible = true;
  std::wstring status;
};

HWND createOutput(HINSTANCE inst, int w, int h);
void setOutputFullscreen(HWND hwnd, HMONITOR mon);
void setOutputWindowed(HWND hwnd, int w, int h);
void setStatus(HWND hwnd, const wchar_t* text);
HWND createHud(HINSTANCE inst, HWND owner);
void updateHud(HWND hud, const HudInfo& info);

void setCaptureTarget(HWND target);
void setScaleSizes(Size capture, Size output);
void drawCursor(bool forceHideRealCursor = false);
void releaseCursorClip(); // release global clipping and restore the system cursor on focus loss
void setBindBypassVks(const UINT* vks, size_t count);

// Steam-style overlay: HUD + mouse not forwarded to game + cursor clipped
// to the presentation (game view) rectangle.
// Every way of closing (hotkey, Esc, a click outside, focus loss) goes through
// setOverlayOpen(false); returnToGame=false leaves the foreground where it is.
void setOverlayOpen(bool open, bool returnToGame = true);
bool isOverlayOpen();
bool overlayReturnsToGame(); // how the overlay was last closed
void pollOverlayFocus();     // Esc, or another window taking the foreground, closes the overlay
void setOverlayHud(HWND hud);
HWND outputHwnd(); // main presentation window
