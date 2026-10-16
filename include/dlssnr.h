#pragma once
#include "dlssnr_bridge_api.h"
#include "graphics.h"
#include <string>

struct OverlayDlssNrConfig;

// Lazy-loaded bridge to the upstream DLSSNR-AMD Vulkan runtime. Missing model/runtime
// is a fail-open condition: the original captured frame continues through FSR unchanged.
class DlssNrRuntime {
public:
  ~DlssNrRuntime() { shutdown(); }
  bool available() { return ensureLoaded(); }
  bool process(Graphics& graphics, ID3D12Resource* inputColour,
               D3D12_RESOURCE_STATES inputColourState, ID3D12Resource* outputColour,
               D3D12_RESOURCE_STATES outputColourState, ID3D12Resource* motion,
               D3D12_RESOURCE_STATES motionState, bool reset,
               const OverlayDlssNrConfig& settings);
  void shutdown();
  const std::wstring& lastError() const { return m_error; }

private:
  using CreateFn = void* (*)(const wchar_t*);
  using DestroyFn = void (*)(void*);
  using ProcessFn = int (*)(void*, ID3D12Device*, ID3D12CommandQueue*,
      ID3D12Resource*, uint32_t, ID3D12Resource*, uint32_t,
      ID3D12Resource*, uint32_t, int,
      const NRLiveDlssNrSettings*, NRLiveDlssNrFlushCallback, void*);
  using ErrorFn = const char* (*)(void*);

  bool ensureLoaded(bool newModel = false);
  HMODULE m_module = nullptr;
  void* m_session = nullptr;
  CreateFn m_create = nullptr;
  DestroyFn m_destroy = nullptr;
  ProcessFn m_process = nullptr;
  ErrorFn m_getError = nullptr;
  std::wstring m_error;
  bool m_reportedMissingModel = false;
  bool m_activeNewModel = false;
};
