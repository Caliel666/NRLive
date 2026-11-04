#include "ui.h"
#include "overlay.h"
#include <windowsx.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <string>

static const wchar_t* OUT_CLS = L"NRLiveOutput";
static const wchar_t* HUD_CLS = L"NRLiveHud";
static const wchar_t* CURSOR_CLS = L"NRLiveCursor";
static HWND g_hudOwner = nullptr;

static UINT g_bypassVks[32]{};
static size_t g_bypassCount = 0;

void setBindBypassVks(const UINT* vks, size_t count)
{
  g_bypassCount = 0;
  if (!vks || !count) return;
  if (count > 32) count = 32;
  for (size_t i = 0; i < count; ++i)
    g_bypassVks[g_bypassCount++] = vks[i];
}

static bool isBypassVk(UINT vk)
{
  for (size_t i = 0; i < g_bypassCount; ++i)
    if (g_bypassVks[i] == vk) return true;
  return false;
}

static HWND g_target = nullptr;
static HWND g_output = nullptr;
static HWND g_overlayHud = nullptr;
static HudInfo g_hud;
static Size g_captureSize{};
static Size g_outputSize{};
static bool g_overlayOpen = false;
static bool g_returnToGame = true;
static LONG_PTR g_savedExStyle = 0;
static HWND g_fgAtOpen = nullptr;
static bool g_hadFocus = false;

void setCaptureTarget(HWND target) { g_target = target; }

void setOverlayHud(HWND hud) { g_overlayHud = hud; }

HWND outputHwnd() { return g_output; }

bool isOverlayOpen() { return g_overlayOpen; }

static void clipCursorToOutput(bool enable);

// Clip cursor to the presentation window client area (game view on screen).
static void clipCursorToOutput(bool enable)
{
  if (!enable) {
    ClipCursor(nullptr);
    return;
  }
  HWND h = g_output;
  if (!h || !IsWindow(h)) return;
  RECT rc{};
  if (!GetClientRect(h, &rc)) return;
  POINT tl{ rc.left, rc.top };
  POINT br{ rc.right, rc.bottom };
  ClientToScreen(h, &tl);
  ClientToScreen(h, &br);
  RECT screen{ tl.x, tl.y, br.x, br.y };
  if (!ClipCursor(&screen)) return;

  // Some games repeatedly clear the global ClipCursor rectangle. Reasserting
  // it each render-loop iteration is normally enough; if the pointer already
  // escaped between those calls, clamp it back into the valid client pixels.
  // This also avoids a one-frame cursor jump when focus changes.
  POINT cursor{};
  if (GetCursorPos(&cursor)) {
    const LONG maxX = std::max(screen.left, screen.right - 1);
    const LONG maxY = std::max(screen.top, screen.bottom - 1);
    const LONG x = std::clamp(cursor.x, screen.left, maxX);
    const LONG y = std::clamp(cursor.y, screen.top, maxY);
    if (x != cursor.x || y != cursor.y)
      SetCursorPos(x, y);
  }
}

// Clip cursor to the TARGET (game) window's CLIENT-AREA screen rect.
// Must match the source rect used by the cursor mapping in drawCursor()
// (which also uses GetClientRect + ClientToScreen).  Using GetWindowRect
// here would include the title bar / borders — the cursor could then sit
// in the non-client area where the mapping condition fails, leaving the
// drawn cursor at the wrong position.
static void clipCursorToTarget()
{
  if (!g_target || !IsWindow(g_target)) { ClipCursor(nullptr); return; }
  RECT rc{};
  if (!GetClientRect(g_target, &rc)) { ClipCursor(nullptr); return; }
  POINT tl{ rc.left, rc.top };
  POINT br{ rc.right, rc.bottom };
  ClientToScreen(g_target, &tl);
  ClientToScreen(g_target, &br);
  RECT screen{ tl.x, tl.y, br.x, br.y };
  ClipCursor(&screen);
}

static HWND g_cursorWindow = nullptr;
static HCURSOR g_drawnCursor = nullptr;
static int g_cursorW = 32;
static int g_cursorH = 32;
static POINT g_cursorHotspot{};

// ── System cursor visibility ─────────────────────────────────────────────
// The public ShowCursor() is per-thread — it only affects the cursor when
// the calling thread's window is in the foreground.  In game mode NRLive
// has WS_EX_NOACTIVATE so the game owns the foreground, and ShowCursor from
// our thread has no effect.  We use the undocumented ShowSystemCursor from
// user32.dll instead — it's system-wide.  Falls back to ShowCursor if the
// function is not found.
typedef void (WINAPI* ShowSystemCursor_t)(BOOL);
static ShowSystemCursor_t g_showSystemCursor = nullptr;
static bool g_systemCursorHidden = false;
static int g_showCursorCount = 0;

static void loadShowSystemCursor()
{
  if (g_showSystemCursor) return;
  HMODULE u32 = GetModuleHandleW(L"user32.dll");
  if (u32)
    g_showSystemCursor = (ShowSystemCursor_t)GetProcAddress(u32, "ShowSystemCursor");
}

// Hide the real system cursor so only the drawn cursor is visible.
// Only call this when the game is showing its cursor (GetCursorInfo says
// CURSOR_SHOWING).  When the game hides its cursor we leave the system
// cursor hidden too — the game already did ShowCursor(FALSE) and we
// don't want to fight that.
static void hideRealCursor()
{
  loadShowSystemCursor();
  if (g_showSystemCursor) {
    if (!g_systemCursorHidden) {
      g_showSystemCursor(FALSE);
      g_systemCursorHidden = true;
    }
  } else {
    while (g_showCursorCount > -5) { ShowCursor(FALSE); --g_showCursorCount; }
  }
}

// Show the real system cursor (for overlay mode or shutdown).
static void showRealCursor()
{
  loadShowSystemCursor();
  if (g_showSystemCursor) {
    if (g_systemCursorHidden) {
      g_showSystemCursor(TRUE);
      g_systemCursorHidden = false;
    }
  } else {
    while (g_showCursorCount < 0) { ShowCursor(TRUE); ++g_showCursorCount; }
  }
}

static LRESULT CALLBACK cursorProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
  if (m == WM_NCHITTEST) return HTTRANSPARENT;
  if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  if (m == WM_ERASEBKGND) return 1;
  return DefWindowProcW(h, m, w, l);
}

static void createCursorWindow(HINSTANCE inst)
{
  if (g_cursorWindow) return;
  WNDCLASSEXW c{ sizeof(c) };
  c.hInstance = inst;
  c.lpfnWndProc = cursorProc;
  c.lpszClassName = CURSOR_CLS;
  c.hCursor = nullptr;
  c.hbrBackground = nullptr;
  RegisterClassExW(&c);

  g_cursorWindow = CreateWindowExW(
    WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
    CURSOR_CLS, L"", WS_POPUP,
    0, 0, g_cursorW, g_cursorH,
    nullptr, nullptr, inst, nullptr);

  if (g_cursorWindow)
    ShowWindow(g_cursorWindow, SW_HIDE);
}

static void hideDrawnCursor()
{
  if (g_cursorWindow)
    ShowWindow(g_cursorWindow, SW_HIDE);
}

static bool rebuildDrawnCursor(HCURSOR cursor)
{
  if (!g_cursorWindow || !cursor) return false;

  ICONINFO ii{};
  if (!GetIconInfo(cursor, &ii))
    return false;

  BITMAP bm{};
  HBITMAP shape = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
  if (!shape || !GetObjectW(shape, sizeof(bm), &bm)) {
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return false;
  }

  int w = bm.bmWidth;
  int h = bm.bmHeight;
  if (!ii.hbmColor) h /= 2;
  w = std::max(1, std::min(w, 256));
  h = std::max(1, std::min(h, 256));

  g_cursorW = w;
  g_cursorH = h;
  g_cursorHotspot.x = (LONG)ii.xHotspot;
  g_cursorHotspot.y = (LONG)ii.yHotspot;

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
  HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!mem || !dib || !bits) {
    if (dib) DeleteObject(dib);
    if (mem) DeleteDC(mem);
    if (screen) ReleaseDC(nullptr, screen);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return false;
  }

  SelectObject(mem, dib);
  memset(bits, 0, (size_t)w * (size_t)h * 4);

  DrawIconEx(mem, 0, 0, cursor, w, h, 0, nullptr, DI_NORMAL);

  POINT dst{};
  SIZE size{ w, h };
  POINT src{};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;

  BOOL ok = UpdateLayeredWindow(
    g_cursorWindow, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);

  DeleteObject(dib);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  if (ii.hbmColor) DeleteObject(ii.hbmColor);
  if (ii.hbmMask) DeleteObject(ii.hbmMask);

  if (!ok) return false;
  g_drawnCursor = cursor;
  return true;
}

void drawCursor(bool forceHideRealCursor)
{
  HWND h = g_output;
  if (!h || !IsWindow(h)) return;

  // NOTE: We do NOT re-assert HWND_TOPMOST every frame here anymore.
  // The per-frame SetWindowPos(HWND_TOPMOST) was fighting with the FPS
  // overlay window's z-order, causing flickering.  The topmost position
  // is re-asserted on overlay toggle in applyOverlayActivation(), which
  // is sufficient — if a game jumps above NRLive during gameplay the user
  // can toggle the overlay (Ctrl+Home) to re-assert.

  // In game mode the real cursor must be confined to the TARGET (game)
  // window's screen rect, not the NRLive output window.  The game
  // window is behind our transparent (WS_EX_TRANSPARENT) presentation
  // surface.  Clipping to the game window ensures the OS routes mouse
  // events to the game — events outside the game window would otherwise
  // hit the desktop.  In overlay mode clip to the output window so the
  // cursor stays inside the NRLive presentation for menu interaction.
  if (g_overlayOpen)
    clipCursorToOutput(true);
  else
    clipCursorToTarget();

  // Overlay mode owns the real cursor. Do not put a second cursor on top of it.
  if (g_overlayOpen) {
    showRealCursor();   // make sure the real cursor is visible for menus
    hideDrawnCursor();
    return;
  }

  CURSORINFO ci{};
  ci.cbSize = sizeof(ci);
  const bool cursorInfoValid = GetCursorInfo(&ci) != FALSE;
  const bool cursorShowing = cursorInfoValid &&
    (ci.flags & CURSOR_SHOWING) && ci.hCursor;

  // DXGI Desktop Duplication can include the hardware pointer in the
  // duplicated desktop image independently of WGC's cursor-capture setting.
  // Keep the real pointer suppressed for the entire DXGI game-mode interval;
  // the separate layered cursor window below is the only visible cursor.
  if (forceHideRealCursor)
    hideRealCursor();

  if (!cursorShowing) {
    // The game has hidden its cursor. In DXGI mode still suppress the desktop
    // pointer plane, but don't draw a replacement cursor.
    hideDrawnCursor();
    return;
  }

  // The game is showing its cursor. Draw our mapped cursor and suppress the
  // real system cursor so only the transformed cursor is visible.
  if (!forceHideRealCursor)
    hideRealCursor();

  if (ci.hCursor != g_drawnCursor && !rebuildDrawnCursor(ci.hCursor)) {
    hideDrawnCursor();
    return;
  }

  RECT out{};
  if (!GetWindowRect(h, &out)) {
    hideDrawnCursor();
    return;
  }

  // Map the source-window cursor into the fullscreen presentation rectangle.
  // This is the important distinction from merely drawing the system cursor:
  // a 1280x720 source scaled to 1920x1080 needs the cursor transformed by the
  // same source->destination mapping, while the cursor bitmap itself stays
  // native-resolution and is therefore not blurred by FSR.
  POINT visualPos = ci.ptScreenPos;
  if (g_target && IsWindow(g_target)) {
    RECT src{};
    if (GetClientRect(g_target, &src)) {
      POINT srcTopLeft{ src.left, src.top };
      POINT srcBottomRight{ src.right, src.bottom };
      ClientToScreen(g_target, &srcTopLeft);
      ClientToScreen(g_target, &srcBottomRight);

      const int sw = srcBottomRight.x - srcTopLeft.x;
      const int sh = srcBottomRight.y - srcTopLeft.y;
      const int dw = out.right - out.left;
      const int dh = out.bottom - out.top;

      if (sw > 1 && sh > 1 && dw > 1 && dh > 1) {
        // Clamp instead of skipping the transform when Win32 reports a
        // cursor just outside the client rectangle (often during focus/DPI
        // transitions). Skipping it made the cursor jump to raw desktop
        // coordinates and appear offset from the upscaled game.
        const LONG sourceX = std::clamp(ci.ptScreenPos.x, srcTopLeft.x, srcBottomRight.x - 1);
        const LONG sourceY = std::clamp(ci.ptScreenPos.y, srcTopLeft.y, srcBottomRight.y - 1);
        const double nx = double(sourceX - srcTopLeft.x) / double(sw - 1);
        const double ny = double(sourceY - srcTopLeft.y) / double(sh - 1);
        visualPos.x = out.left + (LONG)std::lround(nx * double(dw - 1));
        visualPos.y = out.top  + (LONG)std::lround(ny * double(dh - 1));
      }
    }
  }

  const int x = visualPos.x - (int)g_cursorHotspot.x;
  const int y = visualPos.y - (int)g_cursorHotspot.y;

  SetWindowPos(g_cursorWindow, HWND_TOPMOST,
               x, y, g_cursorW, g_cursorH,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void applyOverlayActivation(bool open)
{
  HWND h = g_output;
  if (!h || !IsWindow(h)) return;

  if (open) {
    LONG_PTR style = GetWindowLongPtrW(h, GWL_EXSTYLE);
    // Overlay mode: REMOVE WS_EX_LAYERED | WS_EX_TRANSPARENT so NRLive
    // receives mouse events for OptiScaler / ReShade menus.
    SetWindowLongPtrW(h, GWL_EXSTYLE,
      style & ~(WS_EX_TRANSPARENT | WS_EX_LAYERED));
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    // OptiScaler / ReShade need a real activatable, focused window.
    g_savedExStyle = GetWindowLongPtrW(h, GWL_EXSTYLE);
    SetWindowLongPtrW(h, GWL_EXSTYLE,
      (g_savedExStyle & ~WS_EX_NOACTIVATE) | WS_EX_TOPMOST);
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    AllowSetForegroundWindow(ASFW_ANY);
    // Attach input so SetForegroundWindow succeeds from our thread.
    const DWORD fgTid = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD ourTid = GetCurrentThreadId();
    if (fgTid && fgTid != ourTid)
      AttachThreadInput(fgTid, ourTid, TRUE);
    SetForegroundWindow(h);
    BringWindowToTop(h);
    SetActiveWindow(h);
    SetFocus(h);
    if (fgTid && fgTid != ourTid)
      AttachThreadInput(fgTid, ourTid, FALSE);

    clipCursorToOutput(true);
    showRealCursor();   // overlay mode: make the real cursor visible for menus
  } else {
    ClipCursor(nullptr);
    if (GetCapture()) ReleaseCapture();
    // Game mode: make the window transparent to mouse input so the OS
    // routes events to the game window behind us.  WS_EX_TRANSPARENT alone
    // does NOT work for top-level windows — you also need WS_EX_LAYERED
    // + SetLayeredWindowAttributes(alpha=255) for the hit-test skip to
    // take effect.  The window stays fully opaque (alpha 255); only mouse
    // passes through.
    LONG_PTR ex = g_savedExStyle ? g_savedExStyle
      : (WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    SetWindowLongPtrW(h, GWL_EXSTYLE,
      ex | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED);
    SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);

    // Return focus toward the game FIRST — this lets the game receive
    // keyboard input.  SetForegroundWindow can bring the game window to
    // the top of the z-order, so we re-assert NRLive's HWND_TOPMOST
    // AFTER it to guarantee we stay above the game.
    if (g_returnToGame && g_target && IsWindow(g_target)) {
      AllowSetForegroundWindow(ASFW_ANY);
      SetForegroundWindow(g_target);
    }

    // Re-assert HWND_TOPMOST AFTER SetForegroundWindow so the game window
    // cannot end up above NRLive.  This is the critical ordering fix.
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  }
}

void releaseCursorClip()
{
  ClipCursor(nullptr);
  if (g_output && GetCapture() == g_output) ReleaseCapture();
  showRealCursor();
  hideDrawnCursor();
}

void setOverlayOpen(bool open, bool returnToGame)
{
  if (g_overlayOpen == open) {
    if (!open && GetCapture() == g_output) ReleaseCapture();
    return;
  }
  g_overlayOpen = open;
  g_returnToGame = open || returnToGame;

  if (g_overlayHud && IsWindow(g_overlayHud)) {
    ShowWindow(g_overlayHud, open ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (open) InvalidateRect(g_overlayHud, nullptr, FALSE);
  }

  // IMPORTANT: applyOverlayActivation FIRST (sets NRLive to HWND_TOPMOST),
  // THEN overlaySetOpen (shows top bar + FPS above NRLive).  If we do it in
  // the other order, NRLive's topmost push puts it above the overlay windows
  // and they become invisible.
  applyOverlayActivation(open);
  overlaySetOpen(open);
  if (open) {
    g_fgAtOpen = GetForegroundWindow();
    DWORD pid = 0;
    if (g_fgAtOpen) GetWindowThreadProcessId(g_fgAtOpen, &pid);
    g_hadFocus = pid == GetCurrentProcessId();
  }
}

bool overlayReturnsToGame() { return g_returnToGame; }

void pollOverlayFocus()
{
  // Esc closes it too. Polled: when the game kept the keyboard focus the key
  // never reaches the output window.
  static bool escWasDown = false;
  const bool escDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
  const bool escPressed = escDown && !escWasDown;
  escWasDown = escDown;
  if (!g_overlayOpen) return;
  if (escPressed) { setOverlayOpen(false); return; }
  HWND fg = GetForegroundWindow();
  if (!fg) return; // between activations
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  if (pid == GetCurrentProcessId()) { g_hadFocus = true; return; }
  // Our activation did not take and the game kept the foreground: still open.
  if (!g_hadFocus && fg == g_fgAtOpen) return;
  // The game (a click that reached it) or another application took the
  // foreground: close as the hotkey does, handing focus back only to the game.
  HWND root = GetAncestor(fg, GA_ROOTOWNER);
  HWND targetRoot = g_target ? GetAncestor(g_target, GA_ROOTOWNER) : nullptr;
  const bool game = fg == g_target || (root && root == targetRoot);
  setOverlayOpen(false, game);
}

// OptiScaler or ReShade loaded beside NRLive draw their menus on the output
// window and need its clicks; without them a click there closes the overlay.
static bool hostMenuLoaded()
{
  static int cached = -1;
  if (cached >= 0) return cached != 0;
  wchar_t exe[MAX_PATH]{};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir = exe;
  dir.resize(dir.find_last_of(L"\\/") + 1);
  bool found = GetModuleHandleW(L"ReShade64.dll") || GetModuleHandleW(L"OptiScaler.dll");
  for (const wchar_t* name : { L"dxgi.dll", L"d3d12.dll", L"winmm.dll", L"version.dll", L"dbghelp.dll" }) {
    HMODULE m = GetModuleHandleW(name);
    wchar_t path[MAX_PATH]{};
    if (m && GetModuleFileNameW(m, path, MAX_PATH) && _wcsnicmp(path, dir.c_str(), dir.size()) == 0) found = true;
  }
  cached = found ? 1 : 0;
  return found;
}

void setScaleSizes(Size capture, Size output)
{
  g_captureSize = capture;
  g_outputSize = output;
}


static void registerClass(const wchar_t* name, WNDPROC proc)
{
  WNDCLASSEXW c{ sizeof(c) };
  c.hInstance = GetModuleHandleW(nullptr);
  c.lpfnWndProc = proc;
  c.lpszClassName = name;
  c.hCursor = LoadCursor(nullptr, IDC_ARROW);
  c.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
  RegisterClassExW(&c);
}

static LRESULT CALLBACK outProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
  switch (m) {
  case WM_NCHITTEST:
    if (!g_overlayOpen) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
  case WM_MOUSEACTIVATE:
    // Game mode: never steal activation. Overlay mode: accept activation so
    // OptiScaler / ReShade ImGui can take mouse + keyboard.
    // Without them the click only closes the overlay; the game keeps focus.
    if (g_overlayOpen && hostMenuLoaded()) return MA_ACTIVATE;
    return MA_NOACTIVATE;
  case WM_MOUSEMOVE:
  case WM_LBUTTONDOWN: case WM_LBUTTONUP:
  case WM_RBUTTONDOWN: case WM_RBUTTONUP:
  case WM_MBUTTONDOWN: case WM_MBUTTONUP:
  case WM_MOUSEWHEEL:
    if (g_overlayOpen) {
      if (m == WM_MOUSEWHEEL) {
        // The settings panel never takes focus; its wheel arrives here.
        POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        HWND under = WindowFromPoint(p);
        if (under && under != h) { PostMessageW(under, m, w, l); return 0; }
      }
      if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN)
        SetCapture(h);
      if (m == WM_LBUTTONUP || m == WM_RBUTTONUP || m == WM_MBUTTONUP) {
        ReleaseCapture();
        // A click outside the overlay's windows goes back to the game.
        if (!hostMenuLoaded()) { setOverlayOpen(false); return 0; }
      }
      clipCursorToOutput(true);
      return DefWindowProcW(h, m, w, l);
    }
    // Game mode: this window must NEVER own mouse capture.
    if (GetCapture() == h) ReleaseCapture();
    return 0;
  case WM_CAPTURECHANGED:
    if (!g_overlayOpen && GetCapture() == h) ReleaseCapture();
    return 0;
  case WM_SETCURSOR:
    // Never overwrite the game's cursor from the presentation window.
    if (!g_overlayOpen)
      return FALSE;
    return DefWindowProcW(h, m, w, l);

  case WM_KEYDOWN: case WM_KEYUP:
  case WM_SYSKEYDOWN: case WM_SYSKEYUP:
  case WM_CHAR: case WM_SYSCHAR:
    // Overlay mode or bypass keys: keep on this window for OptiScaler/ReShade.
    if (g_overlayOpen || isBypassVk((UINT)w))
      return DefWindowProcW(h, m, w, l);
    if (g_target && IsWindow(g_target))
      PostMessageW(g_target, m, w, l);
    return DefWindowProcW(h, m, w, l);
  case WM_CLOSE:
    // Standard pattern: WM_CLOSE → DestroyWindow → WM_DESTROY → PostQuitMessage.
    // DestroyWindow sends WM_DESTROY which does the cleanup (release capture,
    // restore cursor, destroy cursor window, etc.) and then PostQuitMessage.
    DestroyWindow(h);
    return 0;
  case WM_DESTROY:
    if (GetCapture() == h) ReleaseCapture();
    ClipCursor(nullptr);
    showRealCursor();   // restore the system cursor on exit
    hideDrawnCursor();
    if (g_cursorWindow) { DestroyWindow(g_cursorWindow); g_cursorWindow = nullptr; }
    PostQuitMessage(0);
    return 0;
  default:
    return DefWindowProcW(h, m, w, l);
  }
}

static LRESULT CALLBACK hudProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
  if (m == WM_PAINT) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(h, &ps);
    RECT rc{}; GetClientRect(h, &rc);
    HBRUSH br = CreateSolidBrush(RGB(16, 18, 24));
    FillRect(dc, &rc, br);
    DeleteObject(br);
    if (g_hud.visible) {
      SetBkMode(dc, TRANSPARENT);
      wchar_t head[256]{};
      swprintf_s(head, L"%.1f fps   %ux%u -> %ux%u",
                 (double)g_hud.fps,
                 (unsigned)g_hud.capture.w, (unsigned)g_hud.capture.h,
                 (unsigned)g_hud.output.w, (unsigned)g_hud.output.h);
      SetTextColor(dc, RGB(0, 255, 170));
      RECT r1{ 10, 6, 800, 28 };
      DrawTextW(dc, head, -1, &r1, DT_LEFT | DT_TOP | DT_SINGLELINE);
      if (!g_hud.status.empty()) {
        SetTextColor(dc, RGB(255, 215, 120));
        RECT r2{ 10, 28, 800, 70 };
        DrawTextW(dc, g_hud.status.c_str(), -1, &r2, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
      }
    }
    EndPaint(h, &ps);
    return 0;
  }
  if (m == WM_NCHITTEST) return HTTRANSPARENT;
  return DefWindowProcW(h, m, w, l);
}

HWND createOutput(HINSTANCE i, int w, int h)
{
  registerClass(OUT_CLS, outProc);
  HWND hwnd = CreateWindowExW(
    WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
    OUT_CLS, L"NRLive",
    WS_POPUP, 0, 0, w, h, nullptr, nullptr, i, nullptr);
  if (hwnd) {
    g_output = hwnd;
    createCursorWindow(i);
    // Keep the presentation window eligible for Windows Graphics Capture,
    // Snipping Tool, Game Bar, and OBS. WDA_EXCLUDEFROMCAPTURE made the whole
    // upscaled surface disappear from external capture tools.
  }
  return hwnd;
}

void setOutputFullscreen(HWND h, HMONITOR mon)
{
  MONITORINFO mi{ sizeof(mi) };
  GetMonitorInfoW(mon, &mi);
  SetWindowLongPtrW(h, GWL_STYLE, WS_POPUP);
  SetWindowLongPtrW(h, GWL_EXSTYLE,
    WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
    (g_overlayOpen ? 0 : (WS_EX_TRANSPARENT | WS_EX_LAYERED)));
  if (!g_overlayOpen) SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
  SetWindowPos(h, HWND_TOPMOST,
               mi.rcMonitor.left, mi.rcMonitor.top,
               mi.rcMonitor.right - mi.rcMonitor.left,
               mi.rcMonitor.bottom - mi.rcMonitor.top,
               SWP_SHOWWINDOW | SWP_FRAMECHANGED | SWP_NOACTIVATE);
  // Do not initialize, reposition, or clip the real cursor here.
  // The target game owns cursor/input state.
}

void setOutputWindowed(HWND h, int w, int t)
{
  SetWindowLongPtrW(h, GWL_STYLE, WS_POPUP | WS_BORDER);
  SetWindowLongPtrW(h, GWL_EXSTYLE, WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
  SetWindowPos(h, HWND_TOPMOST, 80, 80, w, t, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
}

void setStatus(HWND h, const wchar_t* t)
{
  if (!t) return;
  if (h) SetWindowTextW(h, t);
}

HWND createHud(HINSTANCE inst, HWND owner)
{
  registerClass(HUD_CLS, hudProc);
  g_hudOwner = owner;
  RECT r{}; GetWindowRect(owner, &r);
  return CreateWindowExW(
    WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
    HUD_CLS, L"", WS_POPUP,
    r.left + 12, r.top + 12, 820, 76,
    owner, nullptr, inst, nullptr);
}

void updateHud(HWND hud, const HudInfo& info)
{
  // Always update the cached info (used by WM_PAINT when it does fire).
  g_hud = info;
  if (!hud) return;
  // Steam-style: HUD only paints while the NRLive overlay is open.
  if (!g_overlayOpen) return;

  // Throttle HUD repaints to ~5 Hz (200 ms).  The overlay text is informational
  // only — frame-by-frame redraws force a GDI InvalidateRect + BeginPaint /
  // EndPaint round-trip every frame and were a measurable stutter source on
  // integrated GPUs where the desktop compositor interacts with our swapchain.
  static LONGLONG s_lastHudTicks = 0;
  static LONGLONG s_tickFreq = 0;
  if (s_tickFreq == 0) {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    s_tickFreq = f.QuadPart;
  }
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  const LONGLONG elapsed = now.QuadPart - s_lastHudTicks;
  const bool due = (s_tickFreq != 0) && (elapsed >= (s_tickFreq / 5));  // 5 Hz
  if (!due && info.visible) return;
  s_lastHudTicks = now.QuadPart;

  // Cache the owner window rect — it only changes when the user moves the
  // borderless output window, so skip SetWindowPos on the common steady-state
  // path.  The previous code called GetWindowRect + SetWindowPos every frame.
  static RECT s_lastOwnerRect{};
  RECT o{};
  if (!GetWindowRect(g_hudOwner, &o)) return;
  if (o.left != s_lastOwnerRect.left || o.top != s_lastOwnerRect.top) {
    s_lastOwnerRect = o;
    SetWindowPos(hud, HWND_TOPMOST, o.left + 12, o.top + 12, 820, 76,
                 SWP_NOACTIVATE | SWP_NOSIZE | (g_overlayOpen ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
  }
  InvalidateRect(hud, nullptr, FALSE);
}
