#include "anr_fg.h"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

struct ID3D11Resource;

using Microsoft::WRL::ComPtr;

namespace {
constexpr int kNgxVersionApi = 0x00000015;
constexpr int kNgxFeatureFrameGeneration = 11;
constexpr int kNgxSuccess = 0x00000001;
constexpr int kNgxFailureMask = 0xFFF00000;
constexpr int kNgxFailure = 0xBAD00000;
constexpr int kNgxInvalidParameter = kNgxFailure | 5;

constexpr const char* kWidth = "Width";
constexpr const char* kHeight = "Height";
constexpr const char* kCreationNodeMask = "CreationNodeMask";
constexpr const char* kVisibilityNodeMask = "VisibilityNodeMask";
constexpr const char* kDlssgWidth = "DLSSG.Width";
constexpr const char* kDlssgHeight = "DLSSG.Height";
constexpr const char* kBackbufferFormat = "DLSSG.BackbufferFormat";
constexpr const char* kInternalWidth = "DLSSG.InternalWidth";
constexpr const char* kInternalHeight = "DLSSG.InternalHeight";
constexpr const char* kDynamicResolution = "DLSSG.DynamicResolution";
constexpr const char* kMultiFrameCount = "DLSSG.MultiFrameCount";
constexpr const char* kMultiFrameIndex = "DLSSG.MultiFrameIndex";
constexpr const char* kBackbufferFrameId = "DLSSG.BackbufferFrameID";
constexpr const char* kReset = "DLSSG.Reset";
constexpr const char* kBackbuffer = "DLSSG.Backbuffer";
constexpr const char* kDepth = "DLSSG.Depth";
constexpr const char* kMotion = "DLSSG.MVecs";
constexpr const char* kHudless = "DLSSG.HUDLess";
constexpr const char* kUi = "DLSSG.UI";
constexpr const char* kUiAlpha = "DLSSG.UIAlpha";
constexpr const char* kOutputInterpolated = "DLSSG.OutputInterpolated";
constexpr const char* kOutputReal = "DLSSG.OutputReal";

std::filesystem::path executableDirectory() {
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (!length || length >= path.size())
    return {};
  path.resize(length);
  return std::filesystem::path(path).parent_path();
}

// Deliberately search only under <NRLive.exe directory>\anr.
// ANR's installer is designed for a game directory; NRLive instead has a
// private local runtime layout so it never reads or modifies the target game.
std::filesystem::path findAnrRuntime() {
  const auto exeDir = executableDirectory();
  if (exeDir.empty())
    return {}; // Never fall back to the process working directory.
  const auto root = exeDir / L"anr";
  const std::filesystem::path candidates[] = {
    root / L"Apex Neural Rendering FG" / L"dlssg" / L"nvngx_dlssg.dll",
    root / L"dlssg" / L"nvngx_dlssg.dll",
    root / L"nvngx_dlssg.dll"
  };
  std::error_code ec;
  for (const auto& candidate : candidates) {
    if (std::filesystem::is_regular_file(candidate, ec))
      return candidate;
    ec.clear();
  }
  return {};
}

std::array<float, 16> inverseMatrix(const std::array<float, 16>& matrix) {
  double augmented[4][8]{};
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 4; ++col)
      augmented[row][col] = matrix[row * 4 + col];
    augmented[row][4 + row] = 1.0;
  }

  for (int col = 0; col < 4; ++col) {
    int pivot = col;
    for (int row = col + 1; row < 4; ++row) {
      if (std::fabs(augmented[row][col]) > std::fabs(augmented[pivot][col]))
        pivot = row;
    }
    if (std::fabs(augmented[pivot][col]) < 1e-12)
      return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    for (int k = 0; k < 8; ++k)
      std::swap(augmented[col][k], augmented[pivot][k]);

    const double divisor = augmented[col][col];
    for (int k = 0; k < 8; ++k)
      augmented[col][k] /= divisor;

    for (int row = 0; row < 4; ++row) {
      if (row == col) continue;
      const double factor = augmented[row][col];
      for (int k = 0; k < 8; ++k)
        augmented[row][k] -= factor * augmented[col][k];
    }
  }

  std::array<float, 16> inverse{};
  for (int row = 0; row < 4; ++row)
    for (int col = 0; col < 4; ++col)
      inverse[row * 4 + col] = static_cast<float>(augmented[row][4 + col]);
  return inverse;
}

bool ngxFailed(int result) {
  return (result & kNgxFailureMask) == kNgxFailure;
}
} // namespace

// Minimal ABI-compatible implementation of NGX's parameter interface. ANR's
// public nr-replay provider path uses the same map rather than allocating NGX
// core parameters. Keeping this ABI shim local avoids redistributing NVIDIA's
// proprietary SDK headers.
struct AnrNgxParameterMap {
  using Value = std::variant<uint64_t, uint32_t, int32_t, float, double,
                             ID3D11Resource*, ID3D12Resource*, void*>;

  virtual void Set(const char* name, unsigned long long value) {
    values[name ? name : ""] = static_cast<uint64_t>(value);
  }
  virtual void Set(const char* name, float value) { values[name ? name : ""] = value; }
  virtual void Set(const char* name, double value) { values[name ? name : ""] = value; }
  virtual void Set(const char* name, unsigned int value) { values[name ? name : ""] = static_cast<uint32_t>(value); }
  virtual void Set(const char* name, int value) { values[name ? name : ""] = static_cast<int32_t>(value); }
  virtual void Set(const char* name, ID3D11Resource* value) { values[name ? name : ""] = value; }
  virtual void Set(const char* name, ID3D12Resource* value) { values[name ? name : ""] = value; }
  virtual void Set(const char* name, void* value) { values[name ? name : ""] = value; }

  virtual int Get(const char* name, unsigned long long* out) const { return getNumber(name, out); }
  virtual int Get(const char* name, float* out) const { return getNumber(name, out); }
  virtual int Get(const char* name, double* out) const { return getNumber(name, out); }
  virtual int Get(const char* name, unsigned int* out) const { return getNumber(name, out); }
  virtual int Get(const char* name, int* out) const { return getNumber(name, out); }
  virtual int Get(const char* name, ID3D11Resource** out) const { return getPointer(name, out); }
  virtual int Get(const char* name, ID3D12Resource** out) const { return getPointer(name, out); }
  virtual int Get(const char* name, void** out) const { return getPointer(name, out); }
  virtual void Reset() { values.clear(); }

private:
  template<class T>
  int getNumber(const char* name, T* out) const {
    if (!name || !out) return kNgxInvalidParameter;
    const auto it = values.find(name);
    if (it == values.end()) return kNgxInvalidParameter;
    return std::visit([&](const auto& value) -> int {
      using V = std::decay_t<decltype(value)>;
      if constexpr (std::is_arithmetic_v<V>) {
        *out = static_cast<T>(value);
        return kNgxSuccess;
      }
      return kNgxInvalidParameter;
    }, it->second);
  }

  template<class T>
  int getPointer(const char* name, T* out) const {
    if (!name || !out) return kNgxInvalidParameter;
    const auto it = values.find(name);
    if (it == values.end()) return kNgxInvalidParameter;
    return std::visit([&](const auto& value) -> int {
      using V = std::decay_t<decltype(value)>;
      if constexpr (std::is_pointer_v<V> && std::is_convertible_v<V, T>) {
        *out = static_cast<T>(value);
        return kNgxSuccess;
      }
      return kNgxInvalidParameter;
    }, it->second);
  }

  std::unordered_map<std::string, Value> values;
};

AnrFrameGeneration::AnrFrameGeneration() = default;

AnrFrameGeneration::~AnrFrameGeneration() {
  shutdown();
}

bool AnrFrameGeneration::fail(const std::wstring& message) {
  m_lastError = message;
  m_failed = true;
  return false;
}

bool AnrFrameGeneration::createOutputs() {
  if (!m_device || !m_display.w || !m_display.h)
    return fail(L"ANR FG: invalid D3D12 device or output size");

  const auto make = [&](ComPtr<ID3D12Resource>& resource) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_display.w;
    desc.Height = m_display.h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = m_format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    return SUCCEEDED(m_device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      nullptr, IID_PPV_ARGS(&resource)));
  };

  m_generated.Reset();
  m_real.Reset();
  if (!make(m_generated) || !make(m_real))
    return fail(L"ANR FG: could not allocate its generated/real output textures");

  m_outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  return true;
}

bool AnrFrameGeneration::init(ID3D12Device* device, Size render, Size display) {
  shutdown();
  m_failed = false;
  m_lastError.clear();
  m_device = device;
  m_render = render;
  m_display = display;
  updateCameraMatrices();
  if (!m_device)
    return fail(L"ANR FG: no D3D12 device");

  const auto runtime = findAnrRuntime();
  if (runtime.empty())
    return fail(L"ANR FG runtime not found. Place the ANR package under NRLive\\anr\\Apex Neural Rendering FG\\.");

  m_module = LoadLibraryExW(runtime.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!m_module)
    return fail(L"ANR FG: LoadLibraryExW failed for " + runtime.wstring() +
                L" (Win32 " + std::to_wstring(GetLastError()) + L")");

  const auto get = [&](const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(m_module, name));
  };
  m_init = reinterpret_cast<InitFn>(get("NVSDK_NGX_D3D12_Init_Ext"));
  m_populate = reinterpret_cast<PopulateFn>(get("NVSDK_NGX_D3D12_PopulateParameters_Impl"));
  m_create = reinterpret_cast<CreateFn>(get("NVSDK_NGX_D3D12_CreateFeature"));
  m_evaluate = reinterpret_cast<EvaluateFn>(get("NVSDK_NGX_D3D12_EvaluateFeature"));
  m_release = reinterpret_cast<ReleaseFn>(get("NVSDK_NGX_D3D12_ReleaseFeature"));
  m_shutdown1 = reinterpret_cast<Shutdown1Fn>(get("NVSDK_NGX_D3D12_Shutdown1"));
  m_shutdown = reinterpret_cast<ShutdownFn>(get("NVSDK_NGX_D3D12_Shutdown"));

  if (!m_init || !m_create || !m_evaluate || !m_release)
    return fail(L"ANR FG: the DLL is missing one or more required NGX D3D12 exports");

  wchar_t temp[MAX_PATH]{};
  if (!GetTempPathW(MAX_PATH, temp))
    return fail(L"ANR FG: GetTempPathW failed");

  // ANR's public nr-replay provider path initializes the provider with a
  // parameter map and a null FeatureCommonInfo pointer.
  const int initResult = m_init(1ull, temp, m_device.Get(), kNgxVersionApi, nullptr);
  if (ngxFailed(initResult))
    return fail(L"ANR FG: NGX Init_Ext failed (result " +
                std::to_wstring(static_cast<unsigned>(initResult)) + L")");

  m_initialized = true; // also ensures Shutdown is called if later setup fails
  m_parameters = std::make_unique<AnrNgxParameterMap>();
  if (m_populate) {
    const int populateResult = m_populate(m_parameters.get());
    if (ngxFailed(populateResult))
      return fail(L"ANR FG: NGX PopulateParameters_Impl failed");
  }

  if (!createOutputs())
    return false;

  m_needsCreate = true;
  m_frameId = 0;
  m_lastError = L"ANR FG provider loaded; waiting for D3D12 frame-generation feature creation";
  return true;
}

void AnrFrameGeneration::updateCameraMatrices() {
  constexpr float fov = 1.0f;
  constexpr float nearPlane = 0.1f;
  constexpr float farPlane = 1000.0f;
  const float aspect = m_display.h
      ? static_cast<float>(m_display.w) / static_cast<float>(m_display.h)
      : 1.0f;
  const float yScale = 1.0f / std::tan(fov * 0.5f);

  // NRLive's synthetic depth uses reversed-Z (0 is far, 1 is near).
  const float projectionNear = farPlane;
  const float projectionFar = nearPlane;
  const std::array<float, 16> projection = {
      yScale / aspect, 0, 0, 0,
      0, yScale, 0, 0,
      0, 0, projectionFar / (projectionFar - projectionNear), 1,
      0, 0, -projectionNear * projectionFar / (projectionFar - projectionNear), 0};
  const std::array<float, 16> inverse = inverseMatrix(projection);
  std::copy(projection.begin(), projection.end(), m_viewToClip);
  std::copy(inverse.begin(), inverse.end(), m_clipToView);
}

bool AnrFrameGeneration::resize(Size render, Size display) {
  if (render.w == m_render.w && render.h == m_render.h &&
      display.w == m_display.w && display.h == m_display.h)
    return true;

  releaseFeature();
  m_render = render;
  m_display = display;
  updateCameraMatrices();
  m_failed = false;
  m_lastError.clear();
  if (!createOutputs())
    return false;
  m_needsCreate = true;
  m_frameId = 0;
  return true;
}

bool AnrFrameGeneration::backbufferFormatMatches(ID3D12Resource* backbuffer) const {
  if (!backbuffer) return false;
  const DXGI_FORMAT format = backbuffer->GetDesc().Format;
  return format == m_format;
}

bool AnrFrameGeneration::setBackbufferFormat(ID3D12Resource* backbuffer) {
  if (!backbuffer)
    return fail(L"ANR FG: no color resource was supplied to configure the backbuffer format");

  const DXGI_FORMAT format = backbuffer->GetDesc().Format;
  if (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM)
    return fail(L"ANR FG: unsupported color format " + std::to_wstring(static_cast<unsigned>(format)));

  if (format == m_format)
    return true;

  releaseFeature();
  m_format = format;
  m_frameId = 0;
  m_failed = false;
  m_lastError.clear();
  if (!createOutputs())
    return false;
  m_needsCreate = true;
  return true;
}

void AnrFrameGeneration::releaseFeature() {
  if (m_handle && m_release)
    m_release(m_handle);
  m_handle = nullptr;
}

bool AnrFrameGeneration::createFeature(ID3D12GraphicsCommandList* commandList) {
  if (!m_initialized || !m_parameters || !m_create || !commandList)
    return fail(L"ANR FG: feature creation was requested before provider initialization");

  m_parameters->Set(kCreationNodeMask, 1u);
  m_parameters->Set(kVisibilityNodeMask, 1u);
  m_parameters->Set(kWidth, m_display.w);
  m_parameters->Set(kHeight, m_display.h);
  m_parameters->Set(kDlssgWidth, m_display.w);
  m_parameters->Set(kDlssgHeight, m_display.h);
  m_parameters->Set(kBackbufferFormat, static_cast<unsigned int>(m_format));
  m_parameters->Set(kInternalWidth, m_render.w);
  m_parameters->Set(kInternalHeight, m_render.h);
  m_parameters->Set(kDynamicResolution, 0u);

  void* handle = nullptr;
  const int result = m_create(commandList, kNgxFeatureFrameGeneration, m_parameters.get(), &handle);
  if (ngxFailed(result) || !handle)
    return fail(L"ANR FG: NGX CreateFeature failed (result " +
                std::to_wstring(static_cast<unsigned>(result)) + L")");

  m_handle = handle;
  m_needsCreate = false;
  m_lastError = L"ANR FG D3D12 feature created";
  return true;
}

bool AnrFrameGeneration::evaluate(ID3D12GraphicsCommandList* commandList,
                                  ID3D12Resource* backbuffer,
                                  ID3D12Resource* depth,
                                  ID3D12Resource* motion,
                                  bool reset,
                                  uint32_t multiFrameCount,
                                  uint32_t multiFrameIndex) {
  if (!ready() || !commandList || !backbuffer || !depth || !motion)
    return fail(L"ANR FG: missing feature, command list, color, depth, or motion-vector input");

  if (m_outputState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    D3D12_RESOURCE_BARRIER barriers[2]{};
    for (int i = 0; i < 2; ++i) {
      barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barriers[i].Transition.pResource = i == 0 ? m_generated.Get() : m_real.Get();
      barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      barriers[i].Transition.StateBefore = m_outputState;
      barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    commandList->ResourceBarrier(2, barriers);
    m_outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  }

  m_parameters->Set(kWidth, m_display.w);
  m_parameters->Set(kHeight, m_display.h);
  m_parameters->Set(kDlssgWidth, m_display.w);
  m_parameters->Set(kDlssgHeight, m_display.h);
  m_parameters->Set(kBackbufferFormat, static_cast<unsigned int>(m_format));
  m_parameters->Set(kInternalWidth, m_render.w);
  m_parameters->Set(kInternalHeight, m_render.h);
  multiFrameCount = std::clamp(multiFrameCount, 1u, 5u);
  multiFrameIndex = std::clamp(multiFrameIndex, 1u, multiFrameCount);
  m_parameters->Set(kReset, reset ? 1u : 0u);
  m_parameters->Set(kMultiFrameCount, multiFrameCount);
  m_parameters->Set(kMultiFrameIndex, multiFrameIndex);
  if (multiFrameIndex == 1)
    ++m_frameId;
  m_parameters->Set(kBackbufferFrameId, m_frameId);

  m_parameters->Set(kBackbuffer, backbuffer);
  m_parameters->Set(kDepth, depth);
  m_parameters->Set(kMotion, motion);
  m_parameters->Set(kHudless, static_cast<ID3D12Resource*>(nullptr));
  m_parameters->Set(kUi, static_cast<ID3D12Resource*>(nullptr));
  m_parameters->Set(kUiAlpha, static_cast<ID3D12Resource*>(nullptr));
  m_parameters->Set(kOutputInterpolated, m_generated.Get());
  m_parameters->Set(kOutputReal, m_real.Get());

  // NRLive currently has optical-flow vectors in render-pixel units and no
  // source camera matrices. Supply explicit, conservative DLSS-G metadata
  // instead of leaving these parameters uninitialized.
  m_parameters->Set("DLSSG.ColorBuffersHDR", 0u);
  // NRLive clears its synthetic depth texture to 0 as the far plane, so it
  // uses reversed-Z semantics (near=1, far=0).
  m_parameters->Set("DLSSG.DepthInverted", 1u);
  m_parameters->Set("DLSSG.CameraMotionIncluded", 1u);
  m_parameters->Set("DLSSG.MvecJittered", 0u);
  m_parameters->Set("DLSSG.MvecDilated", 0u);
  // FastMV/AMDOF output pixel-space displacements; NGX expects these scaled
  // into its normalized motion-vector range.
  m_parameters->Set("DLSSG.MvecScaleX", 1.0f / static_cast<float>(m_render.w));
  m_parameters->Set("DLSSG.MvecScaleY", 1.0f / static_cast<float>(m_render.h));
  m_parameters->Set("DLSSG.JitterOffsetX", 0.0f);
  m_parameters->Set("DLSSG.JitterOffsetY", 0.0f);
  m_parameters->Set("DLSSG.CameraNear", 0.1f);
  m_parameters->Set("DLSSG.CameraFar", 1000.0f);
  m_parameters->Set("DLSSG.CameraFOV", 1.0f);
  m_parameters->Set("DLSSG.CameraAspectRatio",
                    m_display.h ? static_cast<float>(m_display.w) / static_cast<float>(m_display.h) : 1.0f);
  m_parameters->Set("DLSSG.CameraUpX", 0.0f);
  m_parameters->Set("DLSSG.CameraUpY", 1.0f);
  m_parameters->Set("DLSSG.CameraUpZ", 0.0f);
  m_parameters->Set("DLSSG.CameraRightX", 1.0f);
  m_parameters->Set("DLSSG.CameraRightY", 0.0f);
  m_parameters->Set("DLSSG.CameraRightZ", 0.0f);
  m_parameters->Set("DLSSG.CameraFwdX", 0.0f);
  m_parameters->Set("DLSSG.CameraFwdY", 0.0f);
  m_parameters->Set("DLSSG.CameraFwdZ", 1.0f);
  m_parameters->Set("DLSSG.CameraPinholeOffsetX", 0.0f);
  m_parameters->Set("DLSSG.CameraPinholeOffsetY", 0.0f);
  m_parameters->Set("DLSSG.CameraPosX", 0.0f);
  m_parameters->Set("DLSSG.CameraPosY", 0.0f);
  m_parameters->Set("DLSSG.CameraPosZ", 0.0f);
  m_parameters->Set("DLSSG.NotRenderingGameFrames", 0u);
  m_parameters->Set("DLSSG.OrthoProjection", 0u);

  // NRLive has no camera matrices from the game capture path. Supply a
  // consistent synthetic perspective camera rather than leaving NGX matrix
  // parameters unset; the optical-flow vectors still carry frame-to-frame motion.
  static const float identity[16] = {
      1, 0, 0, 0,
      0, 1, 0, 0,
      0, 0, 1, 0,
      0, 0, 0, 1};
  m_parameters->Set("DLSSG.CameraViewToClip", static_cast<void*>(m_viewToClip));
  m_parameters->Set("DLSSG.ClipToCameraView", static_cast<void*>(m_clipToView));
  m_parameters->Set("DLSSG.ClipToPrevClip", static_cast<void*>(const_cast<float*>(identity)));
  m_parameters->Set("DLSSG.PrevClipToClip", static_cast<void*>(const_cast<float*>(identity)));

  const int result = m_evaluate(commandList, m_handle, m_parameters.get(), nullptr);
  if (ngxFailed(result))
    return fail(L"ANR FG: NGX EvaluateFeature failed (result " +
                std::to_wstring(static_cast<unsigned>(result)) + L")");

  D3D12_RESOURCE_BARRIER barriers[2]{};
  for (int i = 0; i < 2; ++i) {
    barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[i].Transition.pResource = i == 0 ? m_generated.Get() : m_real.Get();
    barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  }
  commandList->ResourceBarrier(2, barriers);
  m_outputState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  return true;
}

void AnrFrameGeneration::shutdown() {
  releaseFeature();
  if (m_initialized) {
    if (m_shutdown1)
      m_shutdown1(m_device.Get());
    else if (m_shutdown)
      m_shutdown();
  }
  m_shutdown = nullptr;
  m_shutdown1 = nullptr;
  m_initialized = false;
  m_needsCreate = false;
  m_generated.Reset();
  m_real.Reset();
  m_parameters.reset();
  m_device.Reset();
  if (m_module) {
    FreeLibrary(m_module);
    m_module = nullptr;
  }
  m_init = nullptr;
  m_populate = nullptr;
  m_create = nullptr;
  m_evaluate = nullptr;
  m_release = nullptr;
}
