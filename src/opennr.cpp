#include "opennr.h"
#include "nr_status.h"
#include "opennr_controls.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cwchar>

namespace {
std::filesystem::path exeDirectory()
{
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::filesystem::path(path).parent_path();
}

std::wstring iniPath() { return (exeDirectory() / L"scaleconfig.ini").wstring(); }

std::wstring readString(const wchar_t* section, const char* key, const std::wstring& ini)
{
  wchar_t wkey[96]{};
  for (size_t i = 0; key[i] && i + 1 < std::size(wkey); ++i) wkey[i] = static_cast<wchar_t>(key[i]);
  wchar_t value[64]{};
  GetPrivateProfileStringW(section, wkey, L"", value, static_cast<DWORD>(std::size(value)), ini.c_str());
  return value;
}

float readFloat(const wchar_t* section, const char* key, float def, float lo, float hi, const std::wstring& ini)
{
  const std::wstring v = readString(section, key, ini);
  if (v.empty()) return def;
  wchar_t* end = nullptr;
  const float f = std::wcstof(v.c_str(), &end);
  return (end != v.c_str() && std::isfinite(f)) ? std::clamp(f, lo, hi) : def;
}

uint32_t readUint(const wchar_t* section, const char* key, uint32_t def, uint32_t lo, uint32_t hi, const std::wstring& ini)
{
  const std::wstring v = readString(section, key, ini);
  if (v.empty()) return def;
  wchar_t* end = nullptr;
  const long n = std::wcstol(v.c_str(), &end, 10);
  return end != v.c_str() ? static_cast<uint32_t>(std::clamp<long>(n, long(lo), long(hi))) : def;
}

bool readBool(const wchar_t* section, const char* key, bool def, const std::wstring& ini)
{
  const std::wstring v = readString(section, key, ini);
  if (v.empty()) return def;
  if (_wcsicmp(v.c_str(), L"true") == 0 || _wcsicmp(v.c_str(), L"on") == 0 || v == L"1") return true;
  if (_wcsicmp(v.c_str(), L"false") == 0 || _wcsicmp(v.c_str(), L"off") == 0 || v == L"0") return false;
  return def;
}

std::wstring widen(const char* s)
{
  if (!s || !*s) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  std::wstring w(n > 1 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
  return w;
}

NrRuntimeConfig readConfig()
{
  const std::wstring ini = iniPath();
  NrRuntimeConfig c;
  wchar_t runtime[32]{};
  GetPrivateProfileStringW(L"DLSSNR", L"runtime", L"auto", runtime, 32, ini.c_str());
  c.runtime = _wcsicmp(runtime, L"opennr") == 0 ? NrRuntimeChoice::OpenNr
            : _wcsicmp(runtime, L"bridge") == 0 ? NrRuntimeChoice::Bridge : NrRuntimeChoice::Auto;
  auto& s = c.openNr;
  const wchar_t* sec = L"OpenNR";
  s.originalModel = readBool(sec, "MochizukiOriginalModel", s.originalModel, ini);
  s.int4 = readBool(sec, "MochizukiInt4", s.int4, ini);
  s.aco = readBool(sec, "MochizukiAco", s.aco, ini);
  s.temporal = readBool(sec, "MochizukiTemporal", s.temporal, ini);
  s.historyStrength = readFloat(sec, "MochizukiHistoryStrength", s.historyStrength, 0.f, 1.f, ini);
  s.styleGrade = readBool(sec, "MochizukiStyleGrade", s.styleGrade, ini);
  s.cascadeDetail = readBool(sec, "MochizukiCascadeDetail", s.cascadeDetail, ini);
  s.antiHalo = readBool(sec, "MochizukiAntiHalo", s.antiHalo, ini);
  s.antiHaloStrength = readFloat(sec, "MochizukiAntiHaloStrength", s.antiHaloStrength, 0.f, 1.f, ini);
  s.passEstimate = readBool(sec, "MochizukiPassEstimate", s.passEstimate, ini);
  s.noiseEveryFrame = readBool(sec, "MochizukiNoiseEveryFrame", s.noiseEveryFrame, ini);
  s.historyPrecision = readUint(sec, "MochizukiHistoryPrecision", s.historyPrecision, 16, 32, ini);
  s.preprocessInverse = readBool(sec, "MochizukiPreprocessInverse", s.preprocessInverse, ini);
  s.whitePoint = readFloat(sec, "MochizukiWhitePoint", s.whitePoint, 0.f, 16.f, ini);
  s.preprocess = readBool(sec, "MochizukiPreprocess", s.preprocess, ini);
  s.preprocessExposure = readUint(sec, "MochizukiPreprocessExposure", s.preprocessExposure, 0, 2, ini);
  s.preprocessBias = readFloat(sec, "MochizukiPreprocessBias", s.preprocessBias, -8.f, 8.f, ini);
  s.preprocessCurve = readUint(sec, "MochizukiPreprocessCurve", s.preprocessCurve, 0, 6, ini);
  s.preprocessContrast = readFloat(sec, "MochizukiPreprocessContrast", s.preprocessContrast, .5f, 2.f, ini);
  s.preprocessSaturation = readFloat(sec, "MochizukiPreprocessSaturation", s.preprocessSaturation, .05f, 2.f, ini);
#define MZR_F(name, def, lo, hi, group, label, member, help) s.name = readFloat(sec, #name, def, lo, hi, ini);
#define MZR_U(name, def, lo, hi, group, label, member, help) s.name = readUint(sec, #name, def, lo, hi, ini);
#define MZR_B(name, def, lo, hi, group, label, member, help) s.name = readBool(sec, #name, def, ini);
#include "MochizukiRenoKeys.inc"
#undef MZR_F
#undef MZR_U
#undef MZR_B
  // Before 0.6.0 skin structure ran from -1, the model's own; that is 1 now, and a saved -1 means it.
  auto skin = [&](const char* key, float& v) { if (readFloat(sec, key, 0.f, -1.f, 0.f, ini) < 0.f) v = 1.f; };
  skin("MochizukiRenoSkinStructure", s.MochizukiRenoSkinStructure);
  skin("MochizukiRenoPass2Skin", s.MochizukiRenoPass2Skin);
  skin("MochizukiRenoPass3Skin", s.MochizukiRenoPass3Skin);
  skin("MochizukiRenoPass4Skin", s.MochizukiRenoPass4Skin);
  return c;
}
}

uint32_t OpenNrSettings::passes() const
{
  return std::clamp(MochizukiRenoPasses, 1u, MochizukiRenoUnlockPasses ? 30u : 4u);
}

bool NrConfigWatcher::refresh()
{
  const uint64_t now = GetTickCount64();
  if (m_stamp != ~0ull && now - m_lastCheck < 500) return false;
  m_lastCheck = now;
  WIN32_FILE_ATTRIBUTE_DATA data{};
  uint64_t stamp = 0;
  if (GetFileAttributesExW(iniPath().c_str(), GetFileExInfoStandard, &data))
    stamp = (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
  if (stamp == m_stamp) return false;
  m_stamp = stamp;
  m_cfg = readConfig();
  return true;
}

void NrConfigWatcher::save()
{
  const std::wstring ini = iniPath();
  const auto& s = m_cfg.openNr;
  auto put = [&](const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(L"OpenNR", key, v.c_str(), ini.c_str());
  };
  auto b = [](bool v) { return std::wstring(v ? L"true" : L"false"); };
  auto f = [](float v) {
    wchar_t t[32]{};
    swprintf_s(t, L"%.3f", v);
    std::wstring w = t;
    while (w.size() > 1 && w.back() == L'0') w.pop_back();
    if (w.back() == L'.') w.pop_back();
    return w;
  };
  put(L"MochizukiOriginalModel", b(s.originalModel));
  put(L"MochizukiInt4", b(s.int4));
  put(L"MochizukiAco", b(s.aco));
  put(L"MochizukiTemporal", b(s.temporal));
  put(L"MochizukiHistoryStrength", f(s.historyStrength));
  put(L"MochizukiStyleGrade", b(s.styleGrade));
  put(L"MochizukiCascadeDetail", b(s.cascadeDetail));
  put(L"MochizukiAntiHalo", b(s.antiHalo));
  put(L"MochizukiAntiHaloStrength", f(s.antiHaloStrength));
  // At its default a key has no line, so the file only says what was changed.
  auto opt = [&](const wchar_t* key, const std::wstring& v, bool isDefault) {
    WritePrivateProfileStringW(L"OpenNR", key, isDefault ? nullptr : v.c_str(), ini.c_str());
  };
  const OpenNrSettings d;
  opt(L"MochizukiNoiseEveryFrame", b(s.noiseEveryFrame), s.noiseEveryFrame == d.noiseEveryFrame);
  opt(L"MochizukiHistoryPrecision", std::to_wstring(s.historyPrecision), s.historyPrecision == d.historyPrecision);
  opt(L"MochizukiPreprocessInverse", b(s.preprocessInverse), s.preprocessInverse == d.preprocessInverse);
  opt(L"MochizukiWhitePoint", f(s.whitePoint), s.whitePoint == d.whitePoint);
  opt(L"MochizukiPreprocess", b(s.preprocess), s.preprocess == d.preprocess);
  opt(L"MochizukiPreprocessExposure", std::to_wstring(s.preprocessExposure), s.preprocessExposure == d.preprocessExposure);
  opt(L"MochizukiPreprocessBias", f(s.preprocessBias), s.preprocessBias == d.preprocessBias);
  opt(L"MochizukiPreprocessCurve", std::to_wstring(s.preprocessCurve), s.preprocessCurve == d.preprocessCurve);
  opt(L"MochizukiPreprocessContrast", f(s.preprocessContrast), s.preprocessContrast == d.preprocessContrast);
  opt(L"MochizukiPreprocessSaturation", f(s.preprocessSaturation), s.preprocessSaturation == d.preprocessSaturation);
#define MZR_F(name, def, lo, hi, group, label, member, help) opt(L"" #name, f(s.name), s.name == d.name);
#define MZR_U(name, def, lo, hi, group, label, member, help) opt(L"" #name, std::to_wstring(s.name), s.name == d.name);
#define MZR_B(name, def, lo, hi, group, label, member, help) opt(L"" #name, b(s.name), s.name == d.name);
#include "MochizukiRenoKeys.inc"
#undef MZR_F
#undef MZR_U
#undef MZR_B
  // Our own write is not a change to read back.
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (GetFileAttributesExW(ini.c_str(), GetFileExInfoStandard, &data))
    m_stamp = (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}

struct OpenNrRuntime::Api {
  LmxxfNrApi table{};
  PFN_MochizukiNrSetControls setControls = nullptr;
  PFN_MochizukiNrGetInfo getInfo = nullptr;
  PFN_MochizukiNrGetControlDefaults getControlDefaults = nullptr;
  PFN_MochizukiNrSetExposure setExposure = nullptr;
  MochizukiNrControls defaults{};
  MochizukiNrControls sent{};
  bool sentValid = false;
};

OpenNrRuntime::~OpenNrRuntime()
{
  shutdown();
  delete m_api;
}

std::filesystem::path OpenNrRuntime::locate()
{
  const auto dir = exeDirectory();
  for (const auto& p : { dir / L"opennr" / L"MochizukiNrRuntime.dll", dir / L"MochizukiNrRuntime.dll" })
    if (std::filesystem::is_regular_file(p)) return p;
  return {};
}

bool OpenNrRuntime::load()
{
  if (m_module) return true;
  const auto dll = locate();
  if (dll.empty()) {
    m_error = L"MochizukiNrRuntime.dll not found (opennr\\ beside NRLive.exe); see docs/OPENNR-RUNTIME.md";
    return false;
  }
  // No "Bad Image" box over the game when Windows refuses the file.
  DWORD oldMode = 0;
  SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &oldMode);
  HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  const DWORD err = module ? 0 : GetLastError();
  SetThreadErrorMode(oldMode, nullptr);
  if (!module) {
    m_error = L"MochizukiNrRuntime.dll did not load (error " + std::to_wstring(err) + L")";
    return false;
  }
  auto getApi = reinterpret_cast<int32_t (*)(uint32_t, LmxxfNrApi*)>(GetProcAddress(module, "LmxxfNrGetApi"));
  auto* api = new Api();
  api->table.struct_size = sizeof(LmxxfNrApi);
  if (!getApi || getApi(LMXXF_NR_ABI_VERSION, &api->table) != LMXXF_NR_OK || !api->table.EnqueueHip) {
    delete api;
    FreeLibrary(module);
    m_error = L"MochizukiNrRuntime.dll has no usable LmxxfNrGetApi";
    return false;
  }
  api->setControls = reinterpret_cast<PFN_MochizukiNrSetControls>(GetProcAddress(module, "MochizukiNrSetControls"));
  api->getInfo = reinterpret_cast<PFN_MochizukiNrGetInfo>(GetProcAddress(module, "MochizukiNrGetInfo"));
  api->getControlDefaults =
      reinterpret_cast<PFN_MochizukiNrGetControlDefaults>(GetProcAddress(module, "MochizukiNrGetControlDefaults"));
  api->setExposure = reinterpret_cast<PFN_MochizukiNrSetExposure>(GetProcAddress(module, "MochizukiNrSetExposure"));
  api->defaults.struct_size = sizeof(MochizukiNrControls);
  if (api->getControlDefaults) api->getControlDefaults(&api->defaults);
  api->defaults.struct_size = sizeof(MochizukiNrControls);
  m_api = api;
  m_module = module;
  return true;
}

// The runtime reads dlssnr-amd\ beside its DLL. When it sits in opennr\, the
// models the user already extracted for NRLive (<exe>\dlssnr-amd\) are linked
// there; a hard link costs no space and leaves the originals alone.
static void linkModels(const std::filesystem::path& runtimeDir)
{
  const auto from = exeDirectory() / L"dlssnr-amd";
  const auto to = runtimeDir / L"dlssnr-amd";
  std::error_code ec;
  if (std::filesystem::equivalent(from.parent_path(), runtimeDir, ec) || !std::filesystem::is_directory(to, ec)) return;
  for (const wchar_t* name : { L"dlssnr.bin", L"dlssnr-small.bin", L"dlssnr-int4.bin" }) {
    if (std::filesystem::exists(to / name, ec) || !std::filesystem::is_regular_file(from / name, ec)) continue;
    CreateHardLinkW((to / name).c_str(), (from / name).c_str(), nullptr);
  }
}

bool OpenNrRuntime::ensureSession(ID3D12Device* device, ID3D12CommandQueue* queue, const OpenNrSettings& s)
{
  if (m_session) return true;
  if (m_failed) return false;
  if (!load()) {
    m_failed = true; // the NR toggle tries again
    return false;
  }
  const auto dir = locate().parent_path();
  std::error_code ec;
  linkModels(dir);
  const auto data = dir / L"dlssnr-amd";
  if (!std::filesystem::is_directory(data / L"shaders", ec)) {
    m_error = L"OpenNR: " + (data / L"shaders").wstring() + L" is missing";
    m_failed = true;
    return false;
  }
  if (!std::filesystem::is_regular_file(data / L"dlssnr.bin", ec) &&
      !std::filesystem::is_regular_file(data / L"dlssnr-small.bin", ec)) {
    m_error = L"OpenNR needs dlssnr-small.bin or dlssnr.bin in " + data.wstring() +
              L" (extract them from your own nvngx_dlssnr.dll into dlssnr-amd\\ beside NRLive.exe)";
    m_failed = true;
    return false;
  }
  const std::wstring assets = dir.wstring();
  LmxxfNrCreateInfo info{};
  info.struct_size = sizeof(info);
  info.device = device;
  info.queue = queue;
  info.assets_directory = assets.c_str();
  info.flags = LMXXF_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK | (s.aco ? LMXXF_NR_CREATE_FLAG_ACO : 0u);
  void* ctx = nullptr;
  int32_t rc = m_api->table.Create(&info, &ctx);
  if (rc == LMXXF_NR_INVALID_ARGUMENT && (info.flags & LMXXF_NR_CREATE_FLAG_ACO)) {
    info.flags &= ~uint32_t(LMXXF_NR_CREATE_FLAG_ACO); // a runtime from before ACO
    rc = m_api->table.Create(&info, &ctx);
  }
  if (rc == LMXXF_NR_OK && ctx && m_api->table.PrepareSession(ctx) != LMXXF_NR_OK) {
    char err[256]{};
    m_api->table.GetLastError(err, sizeof err);
    m_api->table.Destroy(ctx);
    ctx = nullptr;
    m_error = L"OpenNR session failed: " + widen(err);
  } else if (rc != LMXXF_NR_OK || !ctx) {
    char err[256]{};
    m_api->table.GetLastError(err, sizeof err);
    m_error = L"OpenNR Create failed: " + widen(err);
  }
  if (!ctx) {
    m_failed = true;
    return false;
  }
  m_session = ctx;
  m_sessionAco = (info.flags & LMXXF_NR_CREATE_FLAG_ACO) != 0;
  m_api->sentValid = false;
  m_sentExposure = -1;
  m_frame = 0;
  m_error.clear();
  return true;
}

void OpenNrRuntime::sendControls(const OpenNrSettings& s)
{
  if (const float exposure = openNrExposure(s); m_api->setExposure && exposure != m_sentExposure) {
    // Never called with the default, so a session without a white point runs as if this did not exist.
    if (exposure > 0.f || m_sentExposure > 0.f) m_api->setExposure(m_session, exposure);
    m_sentExposure = exposure;
  }
  if (!m_api->setControls) return;
  MochizukiNrControls c = m_api->defaults;
  fillOpenNrControls(s, c);
  if (m_api->sentValid && std::memcmp(&c, &m_api->sent, sizeof c) == 0) return;
  if (m_api->setControls(m_session, &c) == LMXXF_NR_OK) {
    m_api->sent = c;
    m_api->sentValid = true;
  }
}

ID3D12Resource* OpenNrRuntime::process(Graphics& graphics, ID3D12Resource* colour, D3D12_RESOURCE_STATES colourState,
                                       Size size, ID3D12Resource* motion, D3D12_RESOURCE_STATES motionState,
                                       bool reset, const OpenNrSettings& s)
{
  m_running = false;
  retire();
  if (!colour || !size.w || !size.h || !ensureSession(graphics.device(), graphics.queue(), s)) return nullptr;
  sendControls(s);

  LmxxfNrFrameInfo fi{};
  fi.struct_size = sizeof(fi);
  fi.frame_id = ++m_frame;
  fi.command_list = graphics.cmd();
  fi.color_width = size.w;
  fi.color_height = size.h;
  fi.color = colour;
  fi.color_state = static_cast<uint32_t>(colourState);
  fi.flags = openNrFrameFlags(s, m_sessionAco);
  // The RenoDX stage composes with its own strengths and scale.
  fi.transfer_strength = fi.color_strength = fi.model_scale = 1.0f;
  fi.passes = s.passes();
  if (s.temporal && motion) {
    // NRLive's vectors: full capture size, pixels, FFX convention (scale 1).
    fi.flags |= LMXXF_NR_FRAME_FLAG_TEMPORAL;
    fi.motion = motion;
    fi.motion_state = static_cast<uint32_t>(motionState);
    fi.motion_scale_x = fi.motion_scale_y = 1.0f;
  }
  fi.reset = reset ? 1u : 0u;

  LmxxfNrJob job{};
  job.struct_size = static_cast<uint32_t>(offsetof(LmxxfNrJob, first_pass_output));
  const int32_t rc = m_api->table.PrepareFrame(m_session, &fi, &job);
  if (rc != LMXXF_NR_OK || !job.handle || !job.private_output) {
    char err[256]{};
    m_api->table.GetLastError(err, sizeof err);
    // UNAVAILABLE: building its network, or declining this frame.
    m_error = (rc == LMXXF_NR_UNAVAILABLE ? L"" : L"OpenNR PrepareFrame failed: ") + widen(err);
    return nullptr;
  }
  if (m_api->table.RecordInputs(m_session, job.handle, graphics.cmd()) != LMXXF_NR_OK ||
      !graphics.submitForInterop()) {
    m_api->table.CancelUnsubmitted(m_session, job.handle);
    m_error = L"OpenNR could not record this frame";
    return nullptr;
  }
  // The network waits for the list just submitted; the continuation list waits
  // for the network. On failure the runtime hands back the frame's own colour.
  m_job = job.handle;
  if (m_api->table.EnqueueHip(m_session, job.handle, graphics.queue()) != LMXXF_NR_OK) {
    char err[256]{};
    m_api->table.GetLastError(err, sizeof err);
    m_error = L"OpenNR enqueue failed: " + widen(err);
  }
  if (m_api->table.RecordOutputs(m_session, job.handle, graphics.cmd()) != LMXXF_NR_OK) {
    m_error = L"OpenNR could not copy the result out";
    return nullptr;
  }
  auto* out = static_cast<ID3D12Resource*>(job.private_output);
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = { out, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, restState(),
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
  graphics.cmd()->ResourceBarrier(1, &b);
  m_running = true;
  return out;
}

void OpenNrRuntime::retire()
{
  if (m_job && m_session) m_api->table.Retire(m_session, m_job);
  m_job = nullptr;
}

void OpenNrRuntime::clearFailure()
{
  // Between frames: the last frame's lists are submitted and its job retired.
  if (m_sessionFailed) destroySession();
  m_sessionFailed = false;
  m_failed = false;
  m_error.clear();
}

void OpenNrRuntime::destroySession()
{
  retire();
  if (m_session) {
    if (m_api->table.Drain) m_api->table.Drain(m_session);
    m_api->table.Destroy(m_session);
  }
  m_session = nullptr;
  m_build = {};
}

void OpenNrRuntime::shutdown()
{
  // The module stays loaded: its Vulkan instance is torn down with the session,
  // and unloading a driver-heavy DLL at exit buys nothing.
  if (m_api) destroySession();
}

std::wstring OpenNrRuntime::status()
{
  const uint64_t now = GetTickCount64();
  // A build's stages go by in a second or two: asked more often while it runs.
  if (m_session && m_api->getInfo && now - m_infoTick >= (m_build.active ? 100u : 500u)) {
    m_infoTick = now;
    MochizukiNrInfo info{};
    info.struct_size = sizeof(info);
    if (m_api->getInfo(m_session, &info) == LMXXF_NR_OK) {
      info.last_error[sizeof info.last_error - 1] = 0;
      wchar_t line[256]{};
      m_build = { info.building != 0, info.build_stage, info.build_done, info.build_total, info.network };
      m_gpuMs = !info.failed && !info.building && info.frame_w ? info.gpu_ms_median : -1;
      if (info.failed) {
        m_sessionFailed = true;
        m_status = L"OpenNR failed: " + widen(info.last_error);
      } else if (info.building) {
        if (nrBuildFraction(m_build) >= 0)
          swprintf_s(line, L"OpenNR building network: %s %u/%u", nrBuildStage(m_build), info.build_done, info.build_total);
        else
          swprintf_s(line, L"OpenNR building network: %s%s", nrBuildStage(m_build),
                     nrBuildChecksPipelines(m_build) ? L" (once per driver)" : L"");
        m_status = line;
      } else if (info.frame_w) {
        const wchar_t* network = info.network == 0 ? L""
                               : info.network == 3 ? L" | RX 6000 (experimental)" : L" | RX 7000 (experimental)";
        swprintf_s(line, L"OpenNR %ux%u -> %ux%u, %.2f ms%s", info.model_w, info.model_h, info.frame_w, info.frame_h,
                   info.gpu_ms_median, network);
        m_status = line;
      } else {
        m_status = L"OpenNR session ready";
      }
    }
  }
  // While the network builds, PrepareFrame declines frames: the build stage says more than its error.
  if (!m_session || (!m_build.active && !m_running && !m_error.empty()) || m_status.empty()) return m_error;
  return m_status;
}

namespace {
std::wstring g_nrStatusLine;
}

const std::wstring& nrStatusLine() { return g_nrStatusLine; }
void setNrStatusLine(const std::wstring& status) { g_nrStatusLine = status; }
