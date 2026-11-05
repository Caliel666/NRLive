#include "fsr.h"
#include "graphics.h"
#include <algorithm>
#include <atomic>
#include <ffx_api_loader.h>
#include <cfloat>
#include <mutex>
#include <string>
#include <fstream>
#include <vector>

// CRITICAL for OptiScaler:
// Call ffxCreateContext / ffxDispatch ONLY through the module OptiScaler hooks
// (amd_fidelityfx_loader_dx12.dll). Do NOT import amd_fidelityfx_dx12.lib —
// that binds a second unhooked copy of the API.

static ffxFunctions g_ffx{};
static HMODULE g_mod = nullptr;
static std::once_flag g_once;
static std::wstring g_info;

static std::wstring hex(uint32_t v)
{
  wchar_t b[16]{};
  swprintf_s(b, L"%08X", v);
  return b;
}

static std::wstring exeDir()
{
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring d(path);
  auto s = d.find_last_of(L"\\/");
  if (s != std::wstring::npos) d.resize(s + 1);
  return d;
}

static std::mutex g_logMutex;
static void logFfx(const std::wstring& message)
{
  std::lock_guard<std::mutex> lock(g_logMutex);
  std::wofstream file(exeDir() + L"NRLive-fg.log", std::ios::out | std::ios::app);
  if (file) file << message << L"\n";
}

static bool resolve(HMODULE m)
{
  if (!m) return false;
  ffxFunctions loaded{};
  ffxLoadFunctions(&loaded, m);
  g_ffx = loaded;

  // Some loader builds expose the core entry points while the effect module
  // exposes additional API calls. Fill only missing pointers; never overwrite
  // functions already provided by the selected runtime.
  HMODULE up = GetModuleHandleW(L"amd_fidelityfx_upscaler_dx12.dll");
  if (!up) {
    const std::wstring dir = exeDir();
    const std::wstring candidate = dir + L"amd_fidelityfx_upscaler_dx12.dll";
    if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
      up = LoadLibraryW(candidate.c_str());
  }
  if (up) {
    ffxFunctions upFn{};
    ffxLoadFunctions(&upFn, up);
    if (!g_ffx.CreateContext) g_ffx.CreateContext = upFn.CreateContext;
    if (!g_ffx.DestroyContext) g_ffx.DestroyContext = upFn.DestroyContext;
    if (!g_ffx.Dispatch) g_ffx.Dispatch = upFn.Dispatch;
    if (!g_ffx.Query) g_ffx.Query = upFn.Query;
    if (!g_ffx.Configure) g_ffx.Configure = upFn.Configure;
  }
  return g_ffx.CreateContext && g_ffx.DestroyContext &&
         g_ffx.Dispatch && g_ffx.Configure && g_ffx.Query;
}

static bool loadFfx()
{
  // Prefer modules OptiScaler already loaded into the process.
  const wchar_t* prefer[] = {
    L"amd_fidelityfx_loader_dx12.dll",
    L"amd_fidelityfx_dx12.dll",
    L"amd_fidelityfx_upscaler_dx12.dll",
  };
  for (auto* n : prefer) {
    HMODULE m = GetModuleHandleW(n);
    if (m && resolve(m)) {
      g_mod = m;
      g_info = std::wstring(L"hooked/") + n;
      return true;
    }
  }

  const std::wstring dir = exeDir();
  const wchar_t* subs[] = { L"OptiScaler\\", L"optiscaler\\", L"" };

  for (auto* n : prefer) {
    for (auto* sub : subs) {
      std::wstring full = dir + sub + n;
      if (GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES)
        continue;
      HMODULE m = LoadLibraryW(full.c_str());
      if (m && resolve(m)) {
        g_mod = m;
        g_info = full;
        return true;
      }
    }
  }

  for (auto* n : prefer) {
    HMODULE m = LoadLibraryW(n);
    if (m && resolve(m)) {
      g_mod = m;
      g_info = std::wstring(L"PATH/") + n;
      return true;
    }
  }

  g_info = L"no FFX DLL";
  return false;
}

static bool ensure()
{
  std::call_once(g_once, [] { loadFfx(); });
  return g_ffx.CreateContext && g_ffx.Dispatch;
}

static std::wstring widen(const char* s)
{
  std::wstring w;
  for (; s && *s; ++s) w += static_cast<wchar_t>(static_cast<unsigned char>(*s));
  return w;
}

bool Fsr::init(ID3D12Device* device, Size maxRender, Size maxDisplay, const UpscalerInfo& choice)
{
  m_provider.clear();
  if (!device) { m_error = L"null device"; return false; }
  if (!choice.ffx) { m_error = L"no FFX upscaler for this choice"; return false; }
  if (!ensure()) {
    m_error = L"FFX missing (" + g_info + L"). Place amd_fidelityfx_dx12.dll "
              L"or OptiScaler\\amd_fidelityfx_loader_dx12.dll next to the EXE.";
    return false;
  }

  // Ask the loader which upscaler providers it has and pick the wanted one.
  // An older or hooked loader that cannot list them keeps its default.
  ffxOverrideVersion override{};
  override.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
  std::wstring offered;
  bool picked = false;
  {
    uint64_t count = 0;
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = device;
    q.outputCount = &count;
    if (g_ffx.Query && g_ffx.Query(nullptr, &q.header) == FFX_API_RETURN_OK && count) {
      std::vector<uint64_t> ids(count);
      std::vector<const char*> names(count);
      q.versionIds = ids.data();
      q.versionNames = names.data();
      if (g_ffx.Query(nullptr, &q.header) == FFX_API_RETURN_OK) {
        for (uint64_t i = 0; i < count; ++i) {
          if (!offered.empty()) offered += L", ";
          offered += widen(names[i]);
          if (!picked && upscalerMatchesProvider(choice, names[i])) {
            override.versionId = ids[i];
            picked = true;
          }
        }
      }
    }
    logFfx(L"FFX upscaler providers: " + (offered.empty() ? std::wstring(L"(not listed)") : offered) +
           L"; wanted " + choice.label + (picked ? L"" : L" (not offered, using the loader's default)"));
  }

  ffxCreateBackendDX12Desc backend{};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  backend.header.pNext = nullptr;
  backend.device = device;

  ffxCreateContextDescUpscale up{};
  up.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  ffxCreateContextDescUpscaleVersion upVersion{};
  upVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
  upVersion.version = FFX_UPSCALER_VERSION;
  upVersion.header.pNext = &backend.header;
  if (picked) {
    override.header.pNext = &backend.header;
    upVersion.header.pNext = &override.header;
  }
  up.header.pNext = &upVersion.header;
  up.maxRenderSize = { maxRender.w, maxRender.h };
  up.maxUpscaleSize = { maxDisplay.w, maxDisplay.h };
  up.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
             FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
             FFX_UPSCALE_ENABLE_DEPTH_INFINITE;

  ffxReturnCode_t rc = g_ffx.CreateContext(&m_ctx, &up.header, nullptr);
  if (rc != FFX_API_RETURN_OK || !m_ctx) {
    m_error = L"ffxCreateContext 0x" + hex((uint32_t)rc) + L" [" + g_info + L"]";
    m_ctx = nullptr;
    return false;
  }

  if (g_ffx.Query) {
    ffxQueryDescUpscaleGetJitterPhaseCount phases{};
    phases.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    int32_t phaseCount = 0;
    phases.renderWidth = maxRender.w;
    phases.displayWidth = maxDisplay.w;
    phases.pOutPhaseCount = &phaseCount;
    if (g_ffx.Query(&m_ctx, &phases.header) == FFX_API_RETURN_OK)
      m_jitterPhases = phaseCount;
  }

  if (g_ffx.Query) {
    ffxQueryGetProviderVersion pv{};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (g_ffx.Query(&m_ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName)
      m_provider = widen(pv.versionName);
  }
  m_error = L"OK " + g_info + (m_provider.empty() ? L"" : L" [" + m_provider + L"]") +
            (picked ? L"" : std::wstring(L" (") + choice.label + L" not offered)");
  logFfx(L"FFX upscaler context on " + (m_provider.empty() ? std::wstring(L"unknown provider") : m_provider));
  return true;
}

void Fsr::setQuality(int quality)
{
  if (quality < 0) quality = 0;
  if (quality > 4) quality = 4;
  m_quality = quality;
}

void Fsr::setSharpening(bool on, float amount)
{
  m_sharpness = std::clamp(amount, 0.0f, 1.0f);
  m_sharpen = on && m_sharpness > 0.0f;
}

static FfxApiResource apiRes(ID3D12Resource* r, uint32_t state)
{
  return ffxApiGetResourceDX12(r, state);
}

bool Fsr::dispatch(ID3D12GraphicsCommandList* cmd,
                   ID3D12Resource* color,
                   ID3D12Resource* depth,
                   ID3D12Resource* motionVectors,
                   ID3D12Resource* reactive,
                   ID3D12Resource* output,
                   Size render, Size display,
                   float dt, bool reset)
{
  if (!m_ctx || !g_ffx.Dispatch || !cmd || !color || !output)
    return false;

  ffxDispatchDescUpscale d{};
  d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  d.header.pNext = nullptr;
  d.commandList = cmd;
  d.color = apiRes(color, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  d.output = apiRes(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
  if (depth)
    d.depth = apiRes(depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  if (motionVectors)
    d.motionVectors = apiRes(motionVectors, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  if (reactive)
    d.reactive = apiRes(reactive, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);

  d.renderSize = { render.w, render.h };
  d.upscaleSize = { display.w, display.h };
  // MVs from our OF are in full-res pixel units → scale must be (1,1).
  // Using (w,h) made OptiScaler treat them as UV and reject/mis-scale them.
  d.motionVectorScale = { 1.f, 1.f };
  d.jitterOffset = { 0.f, 0.f };
  d.frameTimeDelta = (dt > 0.f) ? dt : (1000.f / 60.f);
  d.preExposure = 1.f;
  d.reset = reset ? 1 : 0;
  d.cameraNear = 0.01f;
  d.cameraFar = 1000.f;
  d.cameraFovAngleVertical = 1.0f;
  d.viewSpaceToMetersFactor = 1.f;
  // FSR 3.1 has a separate enable switch; setting sharpness alone is not enough.
  d.enableSharpening = m_sharpen && m_sharpness > 0.0f;
  d.sharpness = d.enableSharpening ? m_sharpness : 0.0f;
  d.flags = 0;

  ffxReturnCode_t rc = g_ffx.Dispatch(&m_ctx, &d.header);
  if (rc != FFX_API_RETURN_OK) {
    m_error = L"ffxDispatch 0x" + hex((uint32_t)rc);
    return false;
  }
  return true;
}

void Fsr::shutdown()
{
  if (m_ctx && g_ffx.DestroyContext)
    g_ffx.DestroyContext(&m_ctx, nullptr);
  m_ctx = nullptr;
}


// ---------------------------------------------------------------------------
// AMD FSR Frame Generation with the SDK's frame-interpolation swapchain.
// The swapchain owns pacing and invokes generationCallback during Present.
bool FsrFrameGeneration::init(ID3D12Device* device, Size maxRender, Size display,
                              IDXGISwapChain4** swapChain, ID3D12CommandQueue* queue)
{
  shutdown();
  if (!device || !swapChain || !*swapChain || !queue || !ensure() ||
      !g_ffx.CreateContext || !g_ffx.Dispatch || !g_ffx.DestroyContext ||
      !g_ffx.Configure || !g_ffx.Query) {
    m_error = L"FSR FG API unavailable (missing FFX context, query, dispatch or configure entry points)";
    logFfx(L"FG init failed: " + m_error + L"; selected FFX module=" + g_info);
    // The caller passes one owned reference specifically for wrapping.
    if (swapChain && *swapChain) { (*swapChain)->Release(); *swapChain = nullptr; }
    return false;
  }
  ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrap{};
  wrap.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
  ffxCreateContextDescFrameGenerationSwapChainVersionDX12 wrapVersion{};
  wrapVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
  wrapVersion.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
  wrapVersion.header.pNext = nullptr;
  wrap.header.pNext = &wrapVersion.header;
  wrap.swapchain = swapChain;
  wrap.gameQueue = queue;
  ffxReturnCode_t rc = g_ffx.CreateContext(&m_swapChainCtx, &wrap.header, nullptr);
  if (rc != FFX_API_RETURN_OK || !m_swapChainCtx || !*swapChain) {
    m_error = L"FSR FG swapchain wrapping failed 0x" + hex((uint32_t)rc);
    logFfx(L"FG init failed: " + m_error + L"; selected FFX module=" + g_info);
    if (m_swapChainCtx) g_ffx.DestroyContext(&m_swapChainCtx, nullptr);
    m_swapChainCtx = nullptr;
    // The SDK may already have released the original chain while attempting
    // replacement. Do not let the caller reuse that pointer after failure.
    *swapChain = nullptr;
    return false;
  }
  m_swapChain = *swapChain;
  m_device = device;

  ffxCreateBackendDX12Desc backend{};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  backend.header.pNext = nullptr;
  backend.device = device;
  ffxCreateContextDescFrameGeneration fg{};
  fg.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
  ffxCreateContextDescFrameGenerationVersion fgVersion{};
  fgVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
  fgVersion.version = FFX_FRAMEGENERATION_VERSION;
  fgVersion.header.pNext = &backend.header;
  fg.header.pNext = &fgVersion.header;
  fg.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED |
             FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
  fg.displaySize = { display.w, display.h };
  fg.maxRenderSize = { maxRender.w, maxRender.h };
  fg.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
  rc = g_ffx.CreateContext(&m_ctx, &fg.header, nullptr);
  if (rc != FFX_API_RETURN_OK || !m_ctx) {
    m_error = L"FSR FG context creation failed 0x" + hex((uint32_t)rc);
    logFfx(L"FG init failed: " + m_error + L"; selected FFX module=" + g_info);
    if (m_swapChainCtx) g_ffx.DestroyContext(&m_swapChainCtx, nullptr);
    m_swapChainCtx = nullptr;
    m_swapChain = nullptr;
    m_ctx = nullptr;
    // Destroying the wrapper context may destroy its replacement swapchain.
    *swapChain = nullptr;
    return false;
  }
  m_maxRender = maxRender;
  m_display = display;
  m_frameId = 0;
  m_callbackEnabled = false;
  m_pendingReset = true;
  m_failed = false;
  m_error = L"FSR FG ready; AMD frame-interpolation swapchain active [" + g_info + L"]";
  logFfx(L"FG init OK; selected FFX module=" + g_info);
  return true;
}

ffxReturnCode_t FsrFrameGeneration::generationCallback(
    ffxDispatchDescFrameGeneration* params, void* userCtx)
{
  auto* self = static_cast<FsrFrameGeneration*>(userCtx);
  if (!self || !params || !g_ffx.Dispatch)
    return FFX_API_RETURN_ERROR_PARAMETER;
  // The proxy-swapchain Present is entered through present(), which holds
  // m_mutex for the whole call. AMD invokes this callback from inside Present;
  // taking the same non-recursive mutex here would deadlock.
  if (!self->m_ctx)
    return FFX_API_RETURN_ERROR_PARAMETER;
  // A provider can race a failure/toggle with Present. If generation was
  // disabled after configuration, leave the ordinary present path intact.
  if (!self->m_callbackEnabled)
    return FFX_API_RETURN_OK;
  params->reset = params->reset || self->m_pendingReset;
  static std::atomic<uint64_t> callbackCount{0};
  const uint64_t sequence = ++callbackCount;
  if (sequence <= 3 || sequence % 120 == 0)
    logFfx(L"FG callback dispatch begin; frameId=" + std::to_wstring(params->frameID));
  const ffxReturnCode_t rc = g_ffx.Dispatch(&self->m_ctx, &params->header);
  if (rc == FFX_API_RETURN_OK) {
    self->m_pendingReset = false;
    if (sequence <= 3 || sequence % 120 == 0)
      logFfx(L"FG callback dispatch OK; frameId=" + std::to_wstring(params->frameID));
  }
  if (rc != FFX_API_RETURN_OK) {
    self->m_failed = true;
    self->m_callbackEnabled = false;
    self->m_error = L"FSR FG generation callback failed 0x" + hex((uint32_t)rc);
    logFfx(self->m_error + L"; frameId=" + std::to_wstring(params->frameID));
  }
  return rc;
}

bool FsrFrameGeneration::resize(Size maxRender, Size display)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_swapChainCtx || !g_ffx.CreateContext || !g_ffx.DestroyContext || !m_device) {
    m_error = L"FSR FG resize failed: swapchain context or device unavailable";
    return false;
  }

  const bool wasEnabled = m_callbackEnabled;
  ffxCreateBackendDX12Desc backend{};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  backend.header.pNext = nullptr;
  backend.device = m_device;
  ffxCreateContextDescFrameGeneration fg{};
  fg.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
  ffxCreateContextDescFrameGenerationVersion fgVersion{};
  fgVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
  fgVersion.version = FFX_FRAMEGENERATION_VERSION;
  fgVersion.header.pNext = &backend.header;
  fg.header.pNext = &fgVersion.header;
  fg.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED |
             FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
  fg.displaySize = { display.w, display.h };
  fg.maxRenderSize = { maxRender.w, maxRender.h };
  fg.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;

  // Create the replacement first. If allocation fails, the old context remains
  // valid so the next frame can still configure the wrapper for safe fallback.
  ffxContext resizedCtx = nullptr;
  const ffxReturnCode_t rc = g_ffx.CreateContext(&resizedCtx, &fg.header, nullptr);
  if (rc != FFX_API_RETURN_OK || !resizedCtx) {
    m_callbackEnabled = wasEnabled;
    m_error = L"FSR FG resize context creation failed 0x" + hex((uint32_t)rc);
    return false;
  }
  if (m_ctx && g_ffx.DestroyContext)
    g_ffx.DestroyContext(&m_ctx, nullptr);
  m_ctx = resizedCtx;
  m_callbackEnabled = false;
  m_pendingReset = true;
  m_failed = false;
  m_maxRender = maxRender;
  m_display = display;
  m_error = L"FSR FG context resized";
  return true;
}

bool FsrFrameGeneration::prepare(ID3D12GraphicsCommandList* cmd,
                                 ID3D12Resource* depth,
                                 ID3D12Resource* motionVectors,
                                 Size render, Size display,
                                 float dt, bool reset, bool enabled)
{
  bool priorFailure = failed();
  if (priorFailure) enabled = false;
  if (!m_ctx || !m_swapChainCtx || !g_ffx.Configure || !g_ffx.Dispatch || !cmd) {
    m_error = L"FSR FG prepare skipped: runtime or swapchain unavailable";
    return false;
  }
  if ((enabled && (render.w > m_maxRender.w || render.h > m_maxRender.h)) ||
      display.w != m_display.w || display.h != m_display.h) {
    m_error = L"FSR FG prepare skipped: resolution changed; recreate context";
    return false;
  }
  if (enabled && (!depth || !motionVectors)) {
    m_error = L"FSR FG prepare skipped: depth or motion vectors unavailable";
    enabled = false;
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  static std::atomic<uint64_t> prepareCount{0};
  const uint64_t prepareSequence = ++prepareCount;
  const bool logPrepare = prepareSequence <= 3 || prepareSequence % 120 == 0;
  if (logPrepare)
    logFfx(L"FG prepare begin; frameId=" + std::to_wstring(m_frameId) +
           L"; enabled=" + (enabled ? L"1" : L"0") +
           L"; render=" + std::to_wstring(render.w) + L"x" + std::to_wstring(render.h));
  // Recheck under the same lock used by the Present callback so a late
  // callback failure cannot race with a new frame re-enabling generation.
  if (m_failed) {
    priorFailure = true;
    enabled = false;
  }
  const uint64_t frameId = m_frameId;
  ffxConfigureDescFrameGeneration config{};
  config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
  config.header.pNext = nullptr;
  config.swapChain = m_swapChain;
  config.presentCallback = nullptr;
  config.presentCallbackUserContext = nullptr;
  config.frameGenerationCallback = enabled ? &FsrFrameGeneration::generationCallback : nullptr;
  config.frameGenerationCallbackUserContext = enabled ? this : nullptr;
  config.frameGenerationEnabled = enabled;
  config.allowAsyncWorkloads = false;
  config.HUDLessColor = {};
  config.flags = 0;
  config.onlyPresentGenerated = false;
  config.generationRect = { 0, 0, (int32_t)display.w, (int32_t)display.h };
  config.frameID = frameId;
  ffxReturnCode_t rc = g_ffx.Configure(&m_ctx, &config.header);
  if (rc != FFX_API_RETURN_OK) {
    m_error = L"FSR FG configure failed 0x" + hex((uint32_t)rc);
    logFfx(m_error + L"; frameId=" + std::to_wstring(frameId));
    m_callbackEnabled = false;
    return false;
  }

  if (logPrepare)
    logFfx(L"FG configure OK; frameId=" + std::to_wstring(frameId));
  m_callbackEnabled = false;
  m_pendingReset = enabled && reset;
  if (enabled) {
    ffxDispatchDescFrameGenerationPrepareV2 prep{};
    prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prep.header.pNext = nullptr;
    prep.frameID = frameId;
    prep.flags = 0;
    prep.commandList = cmd;
    prep.renderSize = { render.w, render.h };
    prep.jitterOffset = { 0.0f, 0.0f };
    prep.motionVectorScale = { 1.0f, 1.0f };
    prep.frameTimeDelta = dt > 0.0f ? dt : (1000.0f / 60.0f);
    prep.reset = reset;
    prep.cameraNear = 0.01f;
    prep.cameraFar = 1000.0f;
    prep.cameraFovAngleVertical = 1.0f;
    prep.viewSpaceToMetersFactor = 1.0f;
    prep.cameraPosition[0] = prep.cameraPosition[1] = prep.cameraPosition[2] = 0.0f;
    prep.cameraUp[0] = 0.0f; prep.cameraUp[1] = 1.0f; prep.cameraUp[2] = 0.0f;
    prep.cameraRight[0] = 1.0f; prep.cameraRight[1] = 0.0f; prep.cameraRight[2] = 0.0f;
    prep.cameraForward[0] = 0.0f; prep.cameraForward[1] = 0.0f; prep.cameraForward[2] = 1.0f;
    prep.depth = apiRes(depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    prep.motionVectors = apiRes(motionVectors, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    rc = g_ffx.Dispatch(&m_ctx, &prep.header);
    if (rc != FFX_API_RETURN_OK) {
      config.frameGenerationEnabled = false;
      config.frameGenerationCallback = nullptr;
      config.frameGenerationCallbackUserContext = nullptr;
      g_ffx.Configure(&m_ctx, &config.header);
      ++m_frameId;
      m_error = L"FSR FG prepare dispatch failed 0x" + hex((uint32_t)rc);
      return false;
    }
    m_callbackEnabled = true;
    if (logPrepare)
      logFfx(L"FG PrepareV2 dispatch OK; frameId=" + std::to_wstring(frameId));
  } else if (logPrepare) {
    logFfx(L"FG PrepareV2 skipped; frameId=" + std::to_wstring(frameId));
  }
  ++m_frameId;
  m_error = priorFailure ? L"FSR FG disabled after generation callback failure" :
            (enabled ? L"FSR FG prepared for paced Present" : L"FSR FG disabled for this frame");
  return true;
}

bool FsrFrameGeneration::present(Graphics& graphics)
{
  // AMD requires Present to be externally synchronized with PrepareV2 and
  // context destruction when a frame-generation callback is registered.
  // The callback executes synchronously from this Present call and therefore
  // deliberately does not acquire m_mutex a second time.
  std::lock_guard<std::mutex> lock(m_mutex);
  static std::atomic<uint64_t> presentCount{0};
  const uint64_t sequence = ++presentCount;
  const bool logFrame = sequence <= 3 || sequence % 120 == 0;
  if (logFrame)
    logFfx(L"FG proxy Present begin; frameId=" + std::to_wstring(m_frameId) +
           L"; callbackEnabled=" + (m_callbackEnabled ? L"1" : L"0"));
  const bool ok = graphics.present();
  if (logFrame)
    logFfx(L"FG proxy Present returned; frameId=" + std::to_wstring(m_frameId) +
           L"; result=" + (ok ? L"OK" : L"FAILED"));
  return ok;
}

bool FsrFrameGeneration::failed() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_failed;
}

void FsrFrameGeneration::shutdown()
{
  // The caller stops the render loop and drains NRLive's queue before entering
  // shutdown. Acquire the same lock as generationCallback so no callback can
  // be executing against m_ctx while we disable the proxy and destroy contexts.
  std::lock_guard<std::mutex> lock(m_mutex);
  const bool wasCallbackEnabled = m_callbackEnabled;
  m_callbackEnabled = false;
  m_pendingReset = true;
  m_failed = false;

  // Avoid a provider Configure/flush on the common disabled path. If generation
  // was active, explicitly disable it before destroying the effect context.
  if (m_ctx && m_swapChain && g_ffx.Configure && wasCallbackEnabled) {
    // Required by FidelityFX: disabling through the proxy flushes interpolation
    // and UI/present work that may still reference resources owned by m_ctx.
    // DestroyContext alone does not provide this synchronization and can stall
    // or trip D3D12 "object deleted while still in use" validation on exit.
    ffxConfigureDescFrameGeneration config{};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    config.header.pNext = nullptr;
    config.swapChain = m_swapChain;
    config.presentCallback = nullptr;
    config.presentCallbackUserContext = nullptr;
    config.frameGenerationCallback = nullptr;
    config.frameGenerationCallbackUserContext = nullptr;
    config.frameGenerationEnabled = false;
    config.allowAsyncWorkloads = false;
    config.HUDLessColor = {};
    config.flags = 0;
    config.onlyPresentGenerated = false;
    config.generationRect = { 0, 0, (int32_t)m_display.w, (int32_t)m_display.h };
    config.frameID = m_frameId;
    const ffxReturnCode_t rc = g_ffx.Configure(&m_ctx, &config.header);
    if (rc != FFX_API_RETURN_OK) {
      m_error = L"FSR FG shutdown configure failed 0x" + hex((uint32_t)rc);
      logFfx(m_error);
    } else {
      logFfx(L"FG shutdown configure succeeded; contexts will now be destroyed");
    }
  }

  // Destroy the effect context before the wrapper context, then let Graphics
  // release the wrapped IDXGISwapChain after this method returns.
  if (m_ctx && g_ffx.DestroyContext)
    g_ffx.DestroyContext(&m_ctx, nullptr);
  m_ctx = nullptr;
  if (m_swapChainCtx && g_ffx.DestroyContext)
    g_ffx.DestroyContext(&m_swapChainCtx, nullptr);
  m_swapChainCtx = nullptr;
  m_swapChain = nullptr;
  m_device = nullptr;
  m_maxRender = {};
  m_display = {};
  m_frameId = 0;
}
