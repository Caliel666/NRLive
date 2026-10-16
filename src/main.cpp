#include "graphics.h"
#include "capture.h"
#include "fsr.h"
#include "dlssnr.h"
#include "dlssnr_stabilizer.h"
#include "amdof.h"
#include "fastmv.h"
#include "ui.h"
#include "overlay.h"
#include "target.h"
#include "frametrace.h"
#include "frame_limiter.h"
#include "frame_timing.h"
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>
#include <stdexcept>
#include <fstream>
#include <cstring>

static std::wstring logDirectory()
{
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring dir(path);
  const auto slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) dir.resize(slash + 1);
  return dir;
}

static void logMain(const std::wstring& message)
{
  std::wofstream file(logDirectory() + L"NRLive.log", std::ios::out | std::ios::app);
  if (file) file << message << L"\n";
}

static bool hasConsole() { return GetConsoleWindow() != nullptr; }

static void printCli(const std::wstring& text)
{
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!h || h == INVALID_HANDLE_VALUE) return;
  DWORD n = 0;
  WriteConsoleW(h, text.c_str(), (DWORD)text.size(), &n, nullptr);
  WriteConsoleW(h, L"\r\n", 2, &n, nullptr);
}


static bool keyComboDown(UINT modifiers, UINT vk)
{
  auto down = [](int v) { return (GetAsyncKeyState(v) & 0x8000) != 0; };
  // Required modifiers must be down.
  if ((modifiers & MOD_CONTROL) && !down(VK_CONTROL)) return false;
  if ((modifiers & MOD_SHIFT) && !down(VK_SHIFT)) return false;
  if ((modifiers & MOD_ALT) && !down(VK_MENU)) return false;
  if ((modifiers & MOD_WIN) && !down(VK_LWIN) && !down(VK_RWIN)) return false;
  // Extra modifiers must NOT be down (so Ctrl+Home is not also "Home").
  if (!(modifiers & MOD_CONTROL) && down(VK_CONTROL)) return false;
  if (!(modifiers & MOD_SHIFT) && down(VK_SHIFT)) return false;
  if (!(modifiers & MOD_ALT) && down(VK_MENU)) return false;
  if (!(modifiers & MOD_WIN) && (down(VK_LWIN) || down(VK_RWIN))) return false;
  return down((int)vk);
}

// Rising-edge inject of overlay bypass keys into our output window.

static void pollOverlayToggle(const TargetSpec& spec)
{
  static bool prev = false;
  const bool now = keyComboDown(spec.overlayHotkeyModifiers, spec.overlayHotkeyVk);
  if (now && !prev)
    setOverlayOpen(!isOverlayOpen());
  prev = now;
}

static void pollBindBypass(HWND out, const TargetSpec& spec)
{
  // Overlay mode already focuses our window: physical keys reach OptiScaler /
  // ReShade once. Injecting again would double-toggle menus.
  if (isOverlayOpen()) {
    static bool prevOpen[64]{};
    // Keep edge state in sync so we don't fire a stale edge on close.
    const size_t n = spec.bindBypass.size() < 64 ? spec.bindBypass.size() : 64;
    for (size_t i = 0; i < n; ++i)
      prevOpen[i] = keyComboDown(spec.bindBypass[i].modifiers, spec.bindBypass[i].vk);
    return;
  }

  static bool prev[64]{};
  const size_t n = spec.bindBypass.size() < 64 ? spec.bindBypass.size() : 64;
  for (size_t i = 0; i < n; ++i) {
    const auto& b = spec.bindBypass[i];
    const bool now = keyComboDown(b.modifiers, b.vk);
    if (now && !prev[i]) {
      HWND dest = out;
      if (dest) {
        UINT scan = MapVirtualKeyW(b.vk, MAPVK_VK_TO_VSC);
        LPARAM lpDown = 1 | (LPARAM)(scan << 16);
        LPARAM lpUp = 1 | (LPARAM)(scan << 16) | (1 << 30) | (1 << 31);
        PostMessageW(dest, WM_KEYDOWN, b.vk, lpDown);
        PostMessageW(dest, WM_KEYUP, b.vk, lpUp);
      }
    }
    prev[i] = now;
  }
}

static bool stopHotkeyDown(UINT modifiers, UINT vk)
{
  auto down = [](int v) { return (GetAsyncKeyState(v) & 0x8000) != 0; };
  if ((modifiers & MOD_CONTROL) && !down(VK_CONTROL)) return false;
  if ((modifiers & MOD_SHIFT) && !down(VK_SHIFT)) return false;
  if ((modifiers & MOD_ALT) && !down(VK_MENU)) return false;
  if ((modifiers & MOD_WIN) && !down(VK_LWIN) && !down(VK_RWIN)) return false;
  return down((int)vk);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
  // Keep HWND, WGC crop, and cursor coordinates in the same physical-pixel
  // space. DPI virtualization could make the cursor miss the source bounds,
  // disabling both clipping and the source-to-presentation coordinate map.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  logMain(L"--- NRLive start ---");
  logMain(L"command line: " + std::wstring(GetCommandLineW()));

  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  TargetSpec spec;
  std::wstring err;
  bool parsed = parseTargetArgs(argc, argv, spec, err);
  LocalFree(argv);

  const bool cliMode = (argc > 1);
  if (!parsed) {
    if (cliMode && hasConsole()) printCli(err);
    else MessageBoxW(nullptr, err.c_str(), L"NRLive", MB_ICONERROR);
    return 1;
  }
  if (spec.help) {
    auto u = targetUsage();
    if (cliMode && hasConsole()) printCli(u);
    else MessageBoxW(nullptr, u.c_str(), L"NRLive", MB_OK);
    return 0;
  }

  HWND target = nullptr;
  std::wstring label;
  if (spec.delaySeconds > 0 && (spec.front || spec.mode != TargetMode::Picker))
    std::this_thread::sleep_for(std::chrono::seconds(spec.delaySeconds));

  if (!resolveTargetWindow(spec, inst, target, label)) {
    if (cliMode && hasConsole()) printCli(label);
    else MessageBoxW(nullptr, label.c_str(), L"NRLive", MB_ICONERROR);
    return 2;
  }

  HWND out = createOutput(inst, 1280, 720);
  if (!out) return 1;
  // Keep the presentation surface capturable by OBS and other desktop capture
  // tools. The DXGI capture implementation excludes NRLive by HWND/source
  // selection; WDA_EXCLUDEFROMCAPTURE also hides this output from OBS.
  SetWindowDisplayAffinity(out, WDA_NONE);

  constexpr int kStopId = 0x4653;
  bool hk = RegisterHotKey(nullptr, kStopId,
      spec.stopHotkeyModifiers | MOD_NOREPEAT, spec.stopHotkeyVk) != 0;

  // Overlay keybind bypass: these VKs are not forwarded to the game and are
  // re-injected into our output HWND so OptiScaler / ReShade menus receive them.
  {
    std::vector<UINT> vks;
    vks.reserve(spec.bindBypass.size());
    for (const auto& b : spec.bindBypass) vks.push_back(b.vk);
    setBindBypassVks(vks.data(), vks.size());
  }


  HMONITOR mon = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{ sizeof(mi) };
  GetMonitorInfoW(mon, &mi);
  Size display{
    (uint32_t)(mi.rcMonitor.right - mi.rcMonitor.left),
    (uint32_t)(mi.rcMonitor.bottom - mi.rcMonitor.top)
  };
  setOutputFullscreen(out, mon);

  RECT cr{};
  GetClientRect(target, &cr);
  Size render{
    (uint32_t)std::max<LONG>(1, cr.right - cr.left),
    (uint32_t)std::max<LONG>(1, cr.bottom - cr.top)
  };
  setCaptureTarget(target);
  setScaleSizes(render, display);

  FrameTrace trace;
  if (!spec.tracePath.empty() && !trace.open(spec.tracePath)) {
    const std::wstring msg = L"Could not open frame trace CSV: " + spec.tracePath;
    if (cliMode && hasConsole()) printCli(msg);
    else MessageBoxW(nullptr, msg.c_str(), L"NRLive", MB_ICONWARNING);
  }

  try {
    Graphics gfx;
    logMain(L"Graphics::init begin; render=" + std::to_wstring(render.w) + L"x" + std::to_wstring(render.h) + L"; display=" + std::to_wstring(display.w) + L"x" + std::to_wstring(display.h));
    if (!gfx.init(out, render, display)) {
      logMain(L"Graphics::init FAILED (D3D12 device, queue, swapchain, or presentation setup)");
      throw 1;
    }
    logMain(L"Graphics::init OK");

    Capture cap;
    const CaptureMode captureMode = spec.captureMode == TargetSpec::CaptureMode::WgcWindow
      ? CaptureMode::WgcWindow : CaptureMode::DxgiWindow;
    if (!cap.init(gfx.device(), gfx.queue()) || !cap.start(target, captureMode)) {
      std::wstring msg = L"Capture failed: " + cap.lastError();
      logMain(msg);
      if (cliMode && hasConsole()) printCli(msg);
      else MessageBoxW(nullptr, msg.c_str(), L"NRLive", MB_ICONERROR);
      if (hk) UnregisterHotKey(nullptr, kStopId);
      DestroyWindow(out);
      return 3;
    }

    // Let OptiScaler finish DXGI/D3D12/AmdExtFfxApi hooks before we touch FFX.
    // It loads as dxgi.dll and attaches on CreateDevice/CreateSwapChain (already done).
    Sleep(50);

    DlssNrRuntime dlssNr;
    DlssNrStabilizer dlssNrStabilizer;
    std::wstring lastDlssNrError;
    bool lastDlssNrStabilizerEnabled = false;
    Fsr fsr;
    bool fsrOk = fsr.init(gfx.device(), display, display);
    logMain(fsrOk ? L"FSR upscaler init OK: " + fsr.lastError() : L"FSR upscaler init FAILED: " + fsr.lastError());
    bool fsrEnabled = fsrOk;
    if (cliMode && hasConsole()) {
      if (fsrOk) printCli(L"FSR OK: " + fsr.lastError());
      else       printCli(L"FSR FAIL (raw capture only): " + fsr.lastError());
    }
    // Always show FSR path/status on the HUD so we can verify the hooked module.
    {
      std::wstring st = (fsrOk ? L"FSR " : L"FSR off: ") + fsr.lastError();
      setStatus(out, st.c_str());
    }

    AmdOf amdof;
    FastMv fastmv;
    if (fsrOk) {
      amdof.init(gfx.device(), gfx.queue(), render, false); // existing implementation
      if (spec.motionMode == TargetSpec::MotionMode::Fast)
        fastmv.init(gfx.device(), render);
    }

    overlayInit(inst, out);
    fsr.setSharpening(true, overlaySharpness());
    overlaySetFsrEnabled(fsrEnabled);
    setOverlayHud(nullptr);
    setOverlayOpen(false);
    setStatus(out, label.c_str());

    // Create the actual AMD FSR Frame Generation context. The overlay toggle
    // remains off unless the provider initializes successfully.
    FsrFrameGeneration fg;
    // Give the wrapper one owned reference, then drop every reference held by
    // Graphics before AMD destroys/recreates the swapchain. Keeping m_back[] or
    // m_swap1 alive here makes the SDK's swapchain replacement fail at runtime.
    IDXGISwapChain4* fgSwapChain = gfx.swapChain();
    if (fgSwapChain) fgSwapChain->AddRef();
    gfx.releaseSwapChainForWrap();
    bool fgOk = fg.init(gfx.device(), render, display, &fgSwapChain, gfx.queue());
    logMain(fgOk ? L"FSR frame generation init OK: " + fg.lastError() : L"FSR frame generation init FAILED: " + fg.lastError());
    if (fgOk && !gfx.adoptSwapChain(fgSwapChain)) {
      fg.shutdown();
      fgOk = false;
      logMain(L"FSR FG wrapped swapchain adoption failed; rebuilding ordinary swapchain");
    }
    if (!fgOk) {
      // Failed wrap/context creation may have consumed or invalidated the
      // original DXGI swapchain pointer. Recreate a clean ordinary swapchain
      // so capture/upscaling remain usable without frame generation.
      if (!gfx.rebuildSwapChain(display))
        throw std::runtime_error("could not rebuild DXGI swapchain after FSR FG initialization failed");
      logMain(L"Ordinary swapchain rebuilt after FSR FG failure");
    }
    bool fgEnabled = fgOk && overlayFgEnabled();
    overlaySetFgEnabled(fgEnabled);
    Size fgMaxRender = render;
    if (cliMode && hasConsole()) {
      printCli(fgOk ? fg.lastError() : L"FSR FG unavailable: " + fg.lastError());
    }

    FrameLimiter frameLimiter;
    bool reset = true, running = true;
    bool capturePausedForFocus = false;
    bool wasOverlayOpen = false;
    ULONGLONG overlayCloseGraceUntil = 0;
    bool stopLatched = false;
    float fps = 0;
    Size lastCs{};
    LARGE_INTEGER freq, lastCaptured, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&lastCaptured);

    ShowWindow(out, SW_SHOWNOACTIVATE);
    QueryPerformanceCounter(&lastCaptured); // exclude initialization from the first captured-frame interval
    /* HUD starts hidden; Ctrl+Home (or --overlaykey) toggles it */

    while (running) {
      MSG msg{};
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) running = false;
        if (msg.message == WM_HOTKEY && (int)msg.wParam == kStopId) running = false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      if (!running) break;

      pollOverlayToggle(spec);
      if (overlayConsumeDlssNrToggle()) reset = true;
      const bool stabilizerEnabledNow = overlayDlssNrConfig().stabilizer;
      if (stabilizerEnabledNow != lastDlssNrStabilizerEnabled) {
        reset = true;
        lastDlssNrStabilizerEnabled = stabilizerEnabledNow;
      }
      if (fsrOk) fsr.setSharpening(true, overlaySharpness());
      const bool overlayOpenNow = isOverlayOpen();
      // Closing the overlay is an intentional handoff back to the game.
      // The overlay's popup can briefly remain the foreground HWND while the
      // game receives focus; don't interpret that transient as Alt-Tab.
      if (wasOverlayOpen && !overlayOpenNow) {
        overlayCloseGraceUntil = GetTickCount64() + 500;
        SetForegroundWindow(target);
        setOutputFullscreen(out, mon);
        overlaySetOpen(false); // reassert FPS HUD z-order after raising output
      }
      wasOverlayOpen = overlayOpenNow;
      pollBindBypass(out, spec);
      overlayConsumeFrameLimitToggle();
      const auto limitCfg = overlayFrameLimitConfig();
      frameLimiter.configure(limitCfg.enabled, limitCfg.fps, limitCfg.method);
      if (overlayConsumeFsrToggle() && fsrOk) {
        fsrEnabled = !fsrEnabled;
        overlaySetFsrEnabled(fsrEnabled);
        reset = true;
      }
      if (overlayConsumeFgToggle()) {
        fgEnabled = fgOk && overlayFgEnabled();
        overlaySetFgEnabled(fgEnabled);
        reset = true;
      }
      // Camera requests are consumed only once a fresh capture frame is
      // available, then executed after the final FSR/blit pass below.
      bool screenshotRequested = false;

      // Single QPC per frame — measures full frame time (previous-top to
      // current-top).  Both the FPS overlay and the FSR dt use this value.
      // The previous code had a second QPC after cap.acquire which measured
      // only the render portion, making the displayed FPS incorrect.
      // Frame time is measured between successfully acquired capture frames, not
      // between busy-poll iterations. Using poll cadence here made FSR receive
      // an unrealistically tiny dt and made the FPS HUD report the polling rate.
      float loopMs = 0.0f;

      // If the target (game) window is gone, quit.  This happens when the
      // game exits — NRLive should not keep running with a dead target.
      if (!target || !IsWindow(target)) {
        running = false;
        break;
      }

      if (stopHotkeyDown(spec.stopHotkeyModifiers, spec.stopHotkeyVk)) {
        if (!stopLatched) { running = false; break; }
        stopLatched = true;
      } else stopLatched = false;

      // The output is fullscreen/topmost, so merely stopping frame acquisition
      // leaves it covering the application selected by Alt-Tab. Hide it on focus
      // loss, stop capture, then recreate the capture session on return. Overlay mode
      // is exempt because interacting with it intentionally takes focus from game.
      auto isTargetForeground = [&]() {
        HWND fg = GetForegroundWindow();
        if (!fg) return false;
        HWND fgRoot = GetAncestor(fg, GA_ROOTOWNER);
        if (!fgRoot) fgRoot = GetAncestor(fg, GA_ROOT);
        HWND targetRoot = GetAncestor(target, GA_ROOTOWNER);
        if (!targetRoot) targetRoot = GetAncestor(target, GA_ROOT);
        // The NRLive output/overlay may transiently own foreground during
        // close; it is not a real focus loss. Treat it as a handoff only while
        // the overlay was just closed (the explicit foreground restore above).
        HWND outRoot = GetAncestor(out, GA_ROOTOWNER);
        if (!outRoot) outRoot = GetAncestor(out, GA_ROOT);
        const bool outputOwnsForeground = fg == out || fgRoot == outRoot;
        return fg == target || fgRoot == targetRoot || fgRoot == target ||
               fg == targetRoot || (outputOwnsForeground && GetTickCount64() < overlayCloseGraceUntil);
      };
      const bool shouldPauseCapture = !isOverlayOpen() && !isTargetForeground();
      if (shouldPauseCapture && !capturePausedForFocus) {
        gfx.waitForGpu();
        cap.stop();
        // Soft-unscale/focus loss must not leave the global cursor trapped in
        // the fullscreen presentation bounds, and the detached HUD must hide.
        releaseCursorClip();
        overlaySetPresentationVisible(false);
        ShowWindow(out, SW_HIDE);
        capturePausedForFocus = true;
        continue;
      }
      if (shouldPauseCapture) {
        Sleep(8);
        continue;
      }
      if (capturePausedForFocus) {
        if (!cap.start(target, captureMode)) {
          Sleep(8);
          continue;
        }
        setOutputFullscreen(out, mon);
        ShowWindow(out, SW_SHOWNOACTIVATE);
        overlaySetPresentationVisible(true);
        capturePausedForFocus = false;
        reset = true;
        QueryPerformanceCounter(&lastCaptured);
        continue; // let capture deliver a fresh frame before rendering
      }

      // Only transform/hide the system cursor while the scaled presentation
      // is active. Calling drawCursor before focus-state handling re-applied
      // ClipCursor and hid the real cursor on every soft-unscale polling loop,
      // immediately undoing releaseCursorClip() and trapping the user in NRLive.
      drawCursor(cap.usingDxgi());

      if (frameLimiter.enabled() && frameLimiter.method() == 0)
        frameLimiter.wait(); // early mode: pace before capture/CPU preparation

      ComPtr<ID3D12Resource> color;
      Size cs{};
      uint64_t fenceVal = 0;
      LARGE_INTEGER acquireStart{}, acquireEnd{}, renderStart{}, renderEnd{};
      QueryPerformanceCounter(&acquireStart);
      if (!cap.acquire(color, cs, fenceVal)) {
        // No new capture frame yet — do not re-submit with stale resource states.
        // Wait for the FrameArrived event with a short timeout so stop/hotkey
        // polling remains responsive, while avoiding a busy loop that steals
        // CPU time from the game and graphics driver.
        cap.waitForFrame(1);
        continue;
      }
      QueryPerformanceCounter(&acquireEnd);
      QueryPerformanceCounter(&now);
      loopMs = (float)((now.QuadPart - lastCaptured.QuadPart) * 1000.0 / double(freq.QuadPart));
      lastCaptured = now;
      if (loopMs > 0.001f) fps = frame_timing::fpsFromFrameDeltaMs(loopMs);
      Size csForOverlay = cs;
      overlayUpdate(fps, loopMs, csForOverlay, display);
      if (cap.fence() && fenceVal)
        gfx.queue()->Wait(cap.fence(), fenceVal);
      lastCs = cs;
      setScaleSizes(cs, display);
      if (!gfx.ensureAuxTextures(cs)) throw std::runtime_error("auxiliary graphics textures could not be resized");
      if (fgOk && (cs.w > fgMaxRender.w || cs.h > fgMaxRender.h)) {
        gfx.waitForGpu();
        const bool resizedFg = fg.resize(cs, display);
        fgMaxRender = cs;
        if (!resizedFg) {
          // Keep the wrapper alive and configure it off on the next frame;
          // don't leave a stale generation callback registered.
          fgEnabled = false;
          overlaySetFgEnabled(false);
          setStatus(out, fg.lastError().c_str());
        } else {
          fgEnabled = overlayFgEnabled();
          overlaySetFgEnabled(fgEnabled);
        }
        reset = true;
      }
      QueryPerformanceCounter(&renderStart);

      // Keep a Camera click pending until a frame is available. The actual
      // PNG is still generated from the post-FSR presentation surface below.
      screenshotRequested = overlayConsumeScreenshot();

      // dt for FSR — use the frame ms from the top-of-loop QPC (stored in fps)
      float dt = frame_timing::clampFrameDeltaMs(fps > 0.001f ? 1000.0f / fps : (1000.0f / 60.0f));

      gfx.begin();
      auto* cmd = gfx.cmd();
      ID3D12Resource* back = gfx.backbuffer();
      ID3D12Resource* upscale = gfx.upscaleOutput();
      ID3D12Resource* depth = gfx.dummyDepth();
      ID3D12Resource* mv = gfx.motionVectors();
      ID3D12Resource* reactive = gfx.reactiveMask();

      ID3D12Resource* presentSrc = color.Get();
      bool usedFsr = false;
      const bool resetThisFrame = reset;

      ID3D12Resource* nrOutputForFrame = nullptr;
      if (fsrEnabled && depth && mv && upscale) {
        // ---- Batch A: pre-OF+FSR prep -------------------------------------
        //   color:   COMMON  -> PS|NPS   (FSR & AMDOF read as SRV)
        //   depth:   UAV     -> PS|NPS   (FSR reads as SRV)
        //   upscale: NPS     -> UAV      (FSR writes as UAV)
        // (mv stays in UAV — AMDOF writes it; AMDOF will leave it in PS|NPS
        //  so FSR can read it directly without an extra barrier.)
        {
          D3D12_RESOURCE_BARRIER b[3]{};
          for (int i = 0; i < 3; ++i) {
            b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          }
          b[0].Transition.pResource = color.Get();
          b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
          b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
          b[1].Transition.pResource = depth;
          b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
          b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
          b[2].Transition.pResource = upscale;
          b[2].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
          b[2].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
          cmd->ResourceBarrier(3, b);
        }

        // Motion dispatch. --mv amdof is the existing implementation; --mv fast is the
        // new quarter-resolution screen-space estimator.
        bool motionReady = false;
        bool reactiveReady = false;
        ID3D12Resource* nrStabilizedColour = nullptr;
        if (spec.motionMode == TargetSpec::MotionMode::Fast) {
          motionReady = fastmv.dispatch(cmd, color.Get(), mv, reactive, cs, reset);
          reactiveReady = motionReady;
        } else {
          amdof.dispatch(cmd, color.Get(), mv, cs, reset);
          motionReady = true;
        }

        // DLSSNR runs after the same-frame motion vectors are ready and before FSR
        // consumes colour. Its native D3D12/Vulkan bridge submits the current
        // command list, signals/waits on shared GPU fences, and resumes recording
        // on a second allocator/list; no CPU readback or GPU-completion wait.
        if (motionReady) {
          const auto& nrSettings = overlayDlssNrConfig();
          if (nrSettings.enabled) {
            const bool nrAvailable = dlssNr.available(nrSettings.newModel);
            // Never let the bridge write into the capture pool's reusable texture.
            // Capture's D3D11 producer can otherwise overwrite that same resource
            // while the previous frame's D3D12/Vulkan write-back is still in flight.
            // Snapshot each fresh frame, run NR from that immutable snapshot, and
            // write the result to a separate destination consumed by FSR.
            ID3D12Resource* nrInputColour = nrAvailable
                ? gfx.snapshotDlssNrInput(color.Get(),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                : nullptr;
            ID3D12Resource* nrOutputColour = nrAvailable ? gfx.ensureDlssNrOutput(color.Get()) : nullptr;
            ID3D12Resource* originalColour = nrInputColour;
            const auto mvState = spec.motionMode == TargetSpec::MotionMode::Fast
                ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                : (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            const bool nrApplied = nrAvailable && nrInputColour && nrOutputColour &&
                dlssNr.process(gfx, nrInputColour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    nrOutputColour, D3D12_RESOURCE_STATE_COMMON, mv, mvState, resetThisFrame, nrSettings);
            cmd = gfx.cmd(); // the bridge may have switched recording to the continuation list
            if (nrApplied) {
              D3D12_RESOURCE_BARRIER nrOutBarrier{};
              nrOutBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
              nrOutBarrier.Transition.pResource = nrOutputColour;
              nrOutBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
              nrOutBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
              nrOutBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
              cmd->ResourceBarrier(1, &nrOutBarrier);
              nrOutputForFrame = nrOutputColour;
              if (originalColour)
                nrStabilizedColour = dlssNrStabilizer.record(
                    gfx.device(), cmd, originalColour, nrOutputColour, mv, mvState,
                    resetThisFrame,
                    std::max(nrSettings.historyStrength, nrSettings.stabilizer ? 0.45f : 0.0f),
                    nrSettings.intensity, nrSettings.colorStrength, nrSettings.maxRatio);
            }
            if (!dlssNr.lastError().empty() && dlssNr.lastError() != lastDlssNrError) {
              lastDlssNrError = dlssNr.lastError();
              logMain(L"DLSSNR: " + lastDlssNrError);
            } else if (nrApplied) {
              lastDlssNrError.clear();
            }
          }
        }

        // FastMv writes both resources as UAV. FSR consumes them as SRVs.
        // Keep AMDOF's existing state contract untouched.
        {
          D3D12_RESOURCE_BARRIER mb[2]{};
          int mn = 0;
          auto tr = [&](ID3D12Resource* r) {
            mb[mn].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            mb[mn].Transition.pResource = r;
            mb[mn].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            mb[mn].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            mb[mn].Transition.StateAfter =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            ++mn;
          };
          tr(mv);
          if (reactiveReady) tr(reactive);
          cmd->ResourceBarrier(mn, mb);
        }

        // FSR dispatch (reads color/depth/mv as SRV, writes upscale as UAV).
        // No barrier needed for mv — AMDOF left it in PS|NPS.
        ID3D12Resource* fsrInputColour = nrStabilizedColour ? nrStabilizedColour :
            (nrOutputForFrame ? nrOutputForFrame : color.Get());
        usedFsr = motionReady && fsr.dispatch(cmd, fsrInputColour, depth, mv,
                                              reactiveReady ? reactive : nullptr,
                                              upscale, cs, display, dt, reset);
        reset = false;
        presentSrc = usedFsr ? upscale : fsrInputColour;

        // ---- Batch C: post-FSR cleanup + presentation prep ----------------
        //   upscale: UAV -> PS|NPS (if usedFsr, blit reads as SRV)
        //            UAV -> NPS     (if !usedFsr, idle for next frame)
        //   depth:   PS|NPS -> UAV  (next frame's FSR/AMDOF start state)
        //   mv:      PS|NPS -> UAV  (next frame's AMDOF writes it as UAV)
        //   back:    PRESENT -> RTV (clear + blit write as RTV)
        // (color stays in PS|NPS for the blit in case !usedFsr; it is
        //  returned to COMMON in batch D after the blit.)
        {
          D3D12_RESOURCE_BARRIER b[5]{};
          int n = 0;
          auto tr = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES s) {
            b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[n].Transition.pResource = r;
            b[n].Transition.StateBefore = a;
            b[n].Transition.StateAfter = s;
            b[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ++n;
          };
          tr(upscale, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             usedFsr ? (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                     : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
          tr(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          tr(mv, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          if (reactiveReady)
            tr(reactive, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          tr(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
          cmd->ResourceBarrier(n, b);
        }
      } else {
        // FSR disabled — still need to flip color COMMON→PS|NPS→COMMON and
        // backbuffer PRESENT→RTV for the blit.  Batch them.
        D3D12_RESOURCE_BARRIER b[2]{};
        int n = 0;
        auto tr = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES s) {
          b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          b[n].Transition.pResource = r;
          b[n].Transition.StateBefore = a;
          b[n].Transition.StateAfter = s;
          b[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          ++n;
        };
        tr(color.Get(), D3D12_RESOURCE_STATE_COMMON,
           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        tr(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(n, b);
      }

      // The FFX frame-interpolation swapchain owns generation and pacing.
      // Configure every output frame, but only prepare when FSR inputs are valid.
      const bool runFgThisFrame = fgEnabled && fgOk && usedFsr && depth && mv;
      bool fgPrepared = true;
      if (fgOk) {
        if (runFgThisFrame) {
          D3D12_RESOURCE_BARRIER toRead[2]{};
          ID3D12Resource* inputs[2] = { depth, mv };
          for (int i = 0; i < 2; ++i) {
            toRead[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toRead[i].Transition.pResource = inputs[i];
            toRead[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            toRead[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            toRead[i].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
          }
          cmd->ResourceBarrier(2, toRead);
          fgPrepared = fg.prepare(cmd, depth, mv, cs, display, dt, resetThisFrame, true);
          D3D12_RESOURCE_BARRIER toWrite[2]{};
          for (int i = 0; i < 2; ++i) {
            toWrite[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toWrite[i].Transition.pResource = inputs[i];
            toWrite[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            toWrite[i].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            toWrite[i].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
          }
          cmd->ResourceBarrier(2, toWrite);
        } else {
          fgPrepared = fg.prepare(cmd, nullptr, nullptr, cs, display, dt, resetThisFrame, false);
        }
        if (!fgPrepared && runFgThisFrame) {
          fgEnabled = false;
          overlaySetFgEnabled(false);
          if (cliMode && hasConsole())
            printCli(L"FSR FG disabled after prepare failure: " + fg.lastError());
          setStatus(out, fg.lastError().c_str());
        }
      }

      overlaySetFgActive(runFgThisFrame && fgPrepared && !fg.failed());

      const float clear[4] = { 0, 0, 0, 1 };
      cmd->ClearRenderTargetView(gfx.rtvHandle(), clear, 0, nullptr);
      gfx.blitToBackbuffer(presentSrc);

      // Capture the actual presentation surface, after FSR3 and the final
      // stretch blit. The overlay is a separate window, so it is excluded.
      if (screenshotRequested)
        gfx.captureBackbufferScreenshot(overlayScreenshotPath(), cap.totalFrames());

      // ---- Batch D: post-blit backbuffer + colour restore -----------------
      //   back:  RTV -> PRESENT  (always)
      //   color: PS|NPS -> COMMON  (return for next D3D11 copy; always,
      //                              whether or not FSR was used)
      {
        D3D12_RESOURCE_BARRIER b[3]{};
        int n = 0;
        auto tr = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES s) {
          b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          b[n].Transition.pResource = r;
          b[n].Transition.StateBefore = a;
          b[n].Transition.StateAfter = s;
          b[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          ++n;
        };
        tr(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        tr(color.Get(),
           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
           D3D12_RESOURCE_STATE_COMMON);
        if (nrOutputForFrame)
          tr(nrOutputForFrame,
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
             D3D12_RESOURCE_STATE_COMMON);
        cmd->ResourceBarrier(n, b);
      }

      gfx.end();
      if (frameLimiter.enabled() && frameLimiter.method() == 1)
        frameLimiter.wait(); // late mode: finish CPU command recording before pacing
      gfx.present();
      if (fgOk && fg.failed()) {
        fgEnabled = false;
        overlaySetFgEnabled(false);
        if (cliMode && hasConsole())
          printCli(L"FSR FG disabled after generation failure: " + fg.lastError());
        setStatus(out, fg.lastError().c_str());
      }
      QueryPerformanceCounter(&renderEnd);
      const double acquireMs = (acquireEnd.QuadPart - acquireStart.QuadPart) * 1000.0 / double(freq.QuadPart);
      const double renderCpuMs = (renderEnd.QuadPart - renderStart.QuadPart) * 1000.0 / double(freq.QuadPart);
      trace.record(cap.totalFrames(), loopMs, acquireMs, renderCpuMs, cs, display, spec.motionModeText, usedFsr);

      HudInfo hi;
      hi.fps = fps;
      hi.capture = cs;
      hi.output = display;
      hi.visible = !spec.noOverlay && isOverlayOpen();
      hi.status = L"frames=" + std::to_wstring(cap.totalFrames()) +
                  (usedFsr ? L" FSR" : L" blit") +
                  L" | mv=" + spec.motionModeText + L" | " + spec.stopHotkeyText + L"=quit | " + spec.overlayHotkeyText + L"=overlay | bypass=" + (spec.bindBypassText.empty() ? std::wstring(L"default") : spec.bindBypassText);
      updateHud(nullptr, hi);
    }

    // IMPORTANT: drain the GPU before tearing down any D3D12 resources.
    // The FSR context, AMDOF pipelines, capture shared textures, and
    // swapchain all hold GPU-side references.  Releasing them while the
    // GPU still has in-flight command lists is what was crashing the
    // AMD driver on quit.
    gfx.waitForGpu();

    fastmv.shutdown();
    amdof.shutdown();
    fg.shutdown();
    fsr.shutdown();
    overlayShutdown();
    cap.stop();
    if (hk) UnregisterHotKey(nullptr, kStopId);
  } catch (const std::exception& e) {
    logMain(L"Fatal std::exception: " + std::wstring(e.what(), e.what() + strlen(e.what())));
    if (hk) UnregisterHotKey(nullptr, kStopId);
    MessageBoxA(nullptr, e.what(), "NRLive", MB_ICONERROR);
  } catch (...) {
    logMain(L"Fatal unknown exception or graphics initialization failure");
    if (hk) UnregisterHotKey(nullptr, kStopId);
    MessageBoxW(nullptr, L"Fatal graphics error.", L"NRLive", MB_ICONERROR);
  }
  DestroyWindow(out);
  return 0;
}
