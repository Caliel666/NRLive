#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "upscaler.h"

enum class TargetMode { Picker, Pid, Window, Process };

struct TargetSpec {
  TargetMode mode = TargetMode::Picker;
  unsigned long pid = 0;
  std::wstring text;
  int delaySeconds = 0;
  bool delayExplicit = false;
  bool help = false;
  bool noOverlay = false;
  bool front = false;
  enum class MotionMode { AmdOf, Fast };
  enum class CaptureMode { DxgiWindow, WgcWindow };
  CaptureMode captureMode = CaptureMode::WgcWindow;
  MotionMode motionMode = MotionMode::Fast;
  std::wstring motionModeText = L"fast";
  UpscalerKind upscaler = UpscalerKind::Fsr4;
  // Optional CSV trace for comparing capture, render CPU, and present cadence.
  std::wstring tracePath;
  UINT stopHotkeyModifiers = MOD_CONTROL | MOD_SHIFT;
  UINT stopHotkeyVk = 'A';
  std::wstring stopHotkeyText = L"Ctrl+Shift+A";

  // Overlay menu keys (OptiScaler / ReShade): not forwarded to the game;
  // injected into our output window so the overlay receives them.
  struct BypassKey {
    UINT modifiers = 0;
    UINT vk = 0;
  };
  std::vector<BypassKey> bindBypass;
  std::wstring bindBypassText; // human-readable
  bool bindBypassExplicit = false;

  // Steam-style NRLive overlay toggle (default Ctrl+Home).
  UINT overlayHotkeyModifiers = MOD_CONTROL;
  UINT overlayHotkeyVk = VK_HOME;
  std::wstring overlayHotkeyText = L"Ctrl+Home";
};

bool parseTargetArgs(int argc, wchar_t** argv, TargetSpec& spec, std::wstring& error);
bool resolveTargetWindow(const TargetSpec& spec, HINSTANCE inst, HWND& out, std::wstring& label);
HWND resolveFrontWindow();
std::wstring targetUsage();
