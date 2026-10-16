#include "dlssnr.h"
#include "overlay.h"
#include <algorithm>
#include <filesystem>

namespace {
struct FlushContext {
  Graphics* graphics{};
  bool submitted{};
};
void flushForInterop(void* opaque) {
  auto* ctx = static_cast<FlushContext*>(opaque);
  if (ctx && ctx->graphics)
    ctx->submitted = ctx->graphics->submitForInterop();
}
std::wstring appDirectory() {
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring result(path);
  const auto slash = result.find_last_of(L"\\/");
  if (slash != std::wstring::npos) result.resize(slash);
  return result;
}
}

bool DlssNrRuntime::ensureLoaded(bool newModel) {
  if (m_session && m_module) return true;
  const std::filesystem::path root(appDirectory());
  const auto model = root / L"dlssnr-amd" / (newModel ? L"dlssnr-small.bin" : L"dlssnr.bin");
  const auto bridge = root / L"NRLiveDlssNrBridge.dll";
  if (!std::filesystem::is_regular_file(model)) {
    if (!m_reportedMissingModel) {
      m_error = newModel
          ? L"The new DLSSNR model needs dlssnr-amd\\dlssnr-small.bin. Extract both models from your own nvngx_dlssnr.dll 310.9.16."
          : L"DLSSNR needs dlssnr-amd\\dlssnr.bin. Use the upstream extractor with your own compatible nvngx_dlssnr.dll.";
      m_reportedMissingModel = true;
    }
    return false;
  }
  m_module = LoadLibraryW(bridge.c_str());
  if (!m_module) {
    m_error = L"NRLiveDlssNrBridge.dll could not be loaded.";
    return false;
  }
  m_create = reinterpret_cast<CreateFn>(GetProcAddress(m_module, "NRLiveDlssNrCreate"));
  m_destroy = reinterpret_cast<DestroyFn>(GetProcAddress(m_module, "NRLiveDlssNrDestroy"));
  m_process = reinterpret_cast<ProcessFn>(GetProcAddress(m_module, "NRLiveDlssNrProcess"));
  m_getError = reinterpret_cast<ErrorFn>(GetProcAddress(m_module, "NRLiveDlssNrLastError"));
  if (!m_create || !m_destroy || !m_process || !m_getError) {
    m_error = L"DLSSNR bridge is missing required exports.";
    FreeLibrary(m_module); m_module = nullptr;
    return false;
  }
  SetEnvironmentVariableW(L"DLSSNR_MODEL", newModel ? L"small" : L"original");
  m_session = m_create(root.c_str());
  if (!m_session) {
    m_error = L"DLSSNR runtime session could not be initialized.";
    FreeLibrary(m_module); m_module = nullptr;
    return false;
  }
  m_activeNewModel = newModel;
  m_error.clear();
  return true;
}

bool DlssNrRuntime::process(Graphics& graphics, ID3D12Resource* inputColour,
                            D3D12_RESOURCE_STATES inputColourState, ID3D12Resource* outputColour,
                            D3D12_RESOURCE_STATES outputColourState, ID3D12Resource* motion,
                            D3D12_RESOURCE_STATES motionState, bool reset,
                            const OverlayDlssNrConfig& settings) {
  if (!settings.enabled || !inputColour || !outputColour || !motion) return false;
  if (m_session && m_module && m_activeNewModel != settings.newModel) shutdown();
  if (!ensureLoaded(settings.newModel)) return false;

  NRLiveDlssNrSettings s{};
  s.enabled = settings.enabled ? 1 : 0;
  s.style = std::clamp(settings.style, 0, 2);
  s.model_scale = std::clamp(settings.modelScale, 0.25f, 1.0f);
  s.intensity = std::clamp(settings.intensity, 0.0f, 2.0f);
  s.local_tone = std::clamp(settings.localTone, 0.0f, 2.0f);
  s.max_ratio = std::clamp(settings.maxRatio, 1.0f, 8.0f);
  s.structure = std::clamp(settings.structure, 0.0f, 2.0f);
  s.skin_structure = std::clamp(settings.skinStructure, -1.0f, 2.0f);
  s.history_strength = std::clamp(settings.historyStrength, 0.0f, 1.0f);
  s.automatic_skin_mask = settings.automaticSkinMask ? 1 : 0;
  s.passes = std::clamp(settings.passes, 1, 3);

  FlushContext flush{&graphics, false};
  const int applied = m_process(m_session, graphics.device(), graphics.queue(),
      inputColour, static_cast<uint32_t>(inputColourState),
      outputColour, static_cast<uint32_t>(outputColourState), motion,
      static_cast<uint32_t>(motionState), reset ? 1 : 0, &s, flushForInterop, &flush);
  if (flush.submitted && applied) {
    m_error.clear();
    return true;
  }
  if (m_getError && m_session) {
    const char* e = m_getError(m_session);
    if (e && *e) {
      const int n = MultiByteToWideChar(CP_UTF8, 0, e, -1, nullptr, 0);
      if (n > 1) {
        m_error.resize(n);
        MultiByteToWideChar(CP_UTF8, 0, e, -1, m_error.data(), n);
        m_error.pop_back();
      }
    }
  }
  if (m_error.empty() && flush.submitted && !applied)
    m_error = L"DLSSNR could not process this frame; FSR is continuing with the original colour.";
  return false;
}

void DlssNrRuntime::shutdown() {
  if (m_session && m_destroy) m_destroy(m_session);
  m_session = nullptr;
  if (m_module) FreeLibrary(m_module);
  m_module = nullptr;
  m_create = nullptr; m_destroy = nullptr; m_process = nullptr; m_getError = nullptr;
}
