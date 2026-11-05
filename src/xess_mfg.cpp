#include "xess_mfg.h"

#include "host_unlock.h"
#include "model_port.h"
#include "runtime_layout.h"
#include "runtime_payload.h"
#include "sha256_util.h"
#include "shader_port.h"

#include <Windows.h>
#include <bcrypt.h>
#include <xefg_swapchain_d3d12.h>
#include <xell_d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static std::string escape(const std::string& value)
{
    std::string out;
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c >= 32) out += char(c);
    }
    return out;
}

static void logEvent(const char* stage, const std::string& fields = {})
{
    std::string line = "[NRLive XeMFG] ";
    line += stage ? stage : "event";
    if (!fields.empty()) line += " " + fields;
    line += "\n";
    OutputDebugStringA(line.c_str());
}

namespace {
constexpr const char* kXeFgName = "libxess_fg.dll";
constexpr const char* kXeLlName = "libxell.dll";
constexpr const char* kPayloadName = "MFGUnlocker.dll";
constexpr const char* kSdk302FgHash = "ec5e0c65e075570c6ede72618bb666d0be0c2e10b2ea9762c0fe8cb8e375ab27";
constexpr const char* kSdk302LlHash = "d2030dcd694fda8f2ec7e044b13e6db8f0b56d4ba9113a5efad334e3f3ded8c7";

std::filesystem::path executableDirectory()
{
    std::wstring path(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) throw std::runtime_error("exe_path");
    path.resize(count);
    return std::filesystem::path(path).parent_path();
}

std::string hashFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("runtime_hash_open");
    sha256_util::Hasher hasher;
    std::array<char, 65536> buffer{};
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (file.gcount() > 0 && !hasher.Update(buffer.data(), static_cast<size_t>(file.gcount())))
            throw std::runtime_error("runtime_hash_update");
    }
    if (file.bad()) throw std::runtime_error("runtime_hash_read");
    sha256_util::Digest digest{};
    if (!hasher.Finish(digest)) throw std::runtime_error("runtime_hash_finish");
    return sha256_util::LowerHex(digest);
}

std::wstring widen(const std::string& value)
{
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return L"XeSS MFG error";
    std::wstring out(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), out.data(), count);
    return out;
}

void makeProjection(float aspect, float nearPlane, float farPlane, float verticalFov,
                    bool inverted, float out[16])
{
    if (inverted) std::swap(nearPlane, farPlane);
    const float y = 1.0f / std::tan(verticalFov * 0.5f);
    const float x = y / (aspect > 0.0f ? aspect : 1.0f);
    float depth = farPlane / (farPlane - nearPlane);
    float offset = -nearPlane * depth;
    const float matrix[16] = {
        x,0,0,0,
        0,y,0,0,
        0,0,depth,1,
        0,0,offset,0
    };
    std::memcpy(out, matrix, sizeof(matrix));
}

UINT adapterVendor(ID3D12Device* device)
{
    if (!device) return 0;
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 0;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), &adapter))) return 0;
    DXGI_ADAPTER_DESC1 desc{};
    return SUCCEEDED(adapter->GetDesc1(&desc)) ? desc.VendorId : 0;
}

void logFg(const char* message, xefg_swapchain_logging_level_t level, void*)
{
    logEvent("xefg", std::string("\"level\":") + std::to_string(static_cast<int>(level)) +
             ",\"message\":\"" + escape(message ? message : "") + "\"");
}

void logLl(const char* message, xell_logging_level_t level)
{
    logEvent("xell", std::string("\"level\":") + std::to_string(static_cast<int>(level)) +
             ",\"message\":\"" + escape(message ? message : "") + "\"");
}
}

struct XeMfgFrameGeneration::Api
{
    HMODULE fgModule = nullptr;
    HMODULE llModule = nullptr;
    HMODULE payloadModule = nullptr;

    decltype(&::xefgSwapChainD3D12CreateContext) createContext = nullptr;
    decltype(&::xefgSwapChainD3D12InitFromSwapChain) initFromSwapChain = nullptr;
    decltype(&::xefgSwapChainD3D12GetSwapChainPtr) getSwapChain = nullptr;
    decltype(&::xefgSwapChainD3D12TagFrameResource) tagResource = nullptr;
    decltype(&::xefgSwapChainTagFrameConstants) tagConstants = nullptr;
    decltype(&::xefgSwapChainSetEnabled) setEnabled = nullptr;
    decltype(&::xefgSwapChainSetNumInterpolatedFrames) setFrameCount = nullptr;
    decltype(&::xefgSwapChainSetPresentId) setPresentId = nullptr;
    decltype(&::xefgSwapChainSetLatencyReduction) setLatencyReduction = nullptr;
    decltype(&::xefgSwapChainSetLoggingCallback) setLogging = nullptr;
    decltype(&::xefgSwapChainDestroy) destroy = nullptr;
    decltype(&::xellD3D12CreateContext) createXell = nullptr;
    decltype(&::xellSetSleepMode) setSleepMode = nullptr;
    decltype(&::xellSleep) sleep = nullptr;
    decltype(&::xellAddMarkerData) addMarker = nullptr;
    decltype(&::xellSetLoggingCallback) setXellLogging = nullptr;
    decltype(&::xellDestroyContext) destroyXell = nullptr;

    template<class T> static T symbol(HMODULE module, const char* name)
    {
        return module ? reinterpret_cast<T>(GetProcAddress(module, name)) : nullptr;
    }

    bool resolve()
    {
#define RESOLVE_FG(name) name = symbol<decltype(name)>(fgModule, #name); if (!name) return false
#define RESOLVE_LL(name) name = symbol<decltype(name)>(llModule, #name); if (!name) return false
        RESOLVE_FG(createContext);
        RESOLVE_FG(initFromSwapChain);
        RESOLVE_FG(getSwapChain);
        RESOLVE_FG(tagResource);
        RESOLVE_FG(tagConstants);
        RESOLVE_FG(setEnabled);
        RESOLVE_FG(setFrameCount);
        RESOLVE_FG(setPresentId);
        RESOLVE_FG(setLatencyReduction);
        RESOLVE_FG(setLogging);
        RESOLVE_FG(destroy);
        RESOLVE_LL(createXell);
        RESOLVE_LL(setSleepMode);
        RESOLVE_LL(sleep);
        RESOLVE_LL(addMarker);
        RESOLVE_LL(setXellLogging);
        RESOLVE_LL(destroyXell);
#undef RESOLVE_FG
#undef RESOLVE_LL
        return true;
    }
};

namespace {
class XeMfgPatches
{
public:
    std::unique_ptr<ModelPort> model;
    std::unique_ptr<ShaderPort> shader;
    std::unique_ptr<xess_host_unlock::Unlock> unlock;

    void apply(HMODULE fg, HMODULE ll, HMODULE payload, UINT vendor)
    {
        model = std::make_unique<ModelPort>(fg, true, true);
        shader = std::make_unique<ShaderPort>(fg, payload, std::filesystem::path{}, vendor);
        // The AMD port uses the MFG model for every multiplier, including 2x.
        // Five interpolated frames unlock the API limit to 6x output.
        unlock = std::make_unique<xess_host_unlock::Unlock>(fg, ll, uint8_t(5), false);
    }

    void restore() noexcept
    {
        try { shader.reset(); } catch (...) {}
        try { model.reset(); } catch (...) {}
        if (unlock) {
            try { unlock->Restore(); } catch (...) { logEvent("patch_restore_failed"); }
            unlock.reset();
        }
    }
};
}

XeMfgFrameGeneration::~XeMfgFrameGeneration()
{
    shutdown();
}

bool XeMfgFrameGeneration::init(ID3D12Device* device, Size display,
                                IDXGISwapChain4* applicationSwapChain,
                                ID3D12CommandQueue* queue,
                                IDXGISwapChain4** wrappedSwapChain)
{
    shutdown();
    if (!device || !applicationSwapChain || !queue || !wrappedSwapChain) {
        m_error = L"XeSS MFG: invalid D3D12 initialization arguments";
        return false;
    }
    *wrappedSwapChain = nullptr;
    try {
        m_api = std::make_unique<Api>();
        const auto root = executableDirectory() / L"xess";
        const auto fgPath = root / kXeFgName;
        const auto llPath = root / kXeLlName;
        const auto payloadPath = root / kPayloadName;
        if (!std::filesystem::exists(fgPath) || !std::filesystem::exists(llPath) ||
            !std::filesystem::exists(payloadPath))
            throw std::runtime_error("runtime_files_missing (xess/libxess_fg.dll, xess/libxell.dll, xess/MFGUnlocker.dll)");

        m_api->llModule = LoadLibraryExW(llPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m_api->llModule) throw std::runtime_error("libxell_load");
        m_api->fgModule = LoadLibraryExW(fgPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m_api->fgModule) throw std::runtime_error("libxess_fg_load");
        m_api->payloadModule = LoadLibraryExW(payloadPath.c_str(), nullptr,
            LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (!m_api->payloadModule) throw std::runtime_error("MFGUnlocker_resource_load");

        const auto fgHash = hashFile(fgPath);
        const auto llHash = hashFile(llPath);
        logEvent("runtime_hashes", "\"libxess_fg\":\"" + fgHash + "\",\"libxell\":\"" + llHash + "\"");
        if (fgHash != kSdk302FgHash || llHash != kSdk302LlHash)
            throw std::runtime_error("unsupported_runtime_pair_expected_XeSS_SDK_3.0.2");
        if (!xess_layout::Pair(m_api->fgModule, m_api->llModule))
            throw std::runtime_error("runtime_instruction_layout_not_supported");
        if (!m_api->resolve()) throw std::runtime_error("runtime_exports_missing");

        const UINT vendor = adapterVendor(device);
        if (vendor != 0x1002) throw std::runtime_error("XeMFG_AMD_route_requires_vendor_1002");
        auto patches = std::make_unique<XeMfgPatches>();
        patches->apply(m_api->fgModule, m_api->llModule, m_api->payloadModule, vendor);
        m_patches = std::move(patches);

        xefg_swapchain_handle_t fgContext = nullptr;
        xefg_swapchain_result_t fgResult = m_api->createContext(device, &fgContext);
        if (fgResult != XEFG_SWAPCHAIN_RESULT_SUCCESS || !fgContext)
            throw std::runtime_error("xefg_create_context_" + std::to_string(static_cast<int>(fgResult)));
        m_context = fgContext;
        m_api->setLogging(fgContext, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, logFg, nullptr);

        xell_context_handle_t xellContext = nullptr;
        xell_result_t llResult = m_api->createXell(device, &xellContext);
        if (llResult != XELL_RESULT_SUCCESS || !xellContext)
            throw std::runtime_error("xell_create_context_" + std::to_string(static_cast<int>(llResult)));
        m_xell = xellContext;
        m_api->setXellLogging(xellContext, XELL_LOGGING_LEVEL_WARNING, logLl);
        xell_sleep_params_t sleep{};
        sleep.bLowLatencyMode = 1;
        llResult = m_api->setSleepMode(xellContext, &sleep);
        if (llResult != XELL_RESULT_SUCCESS)
            throw std::runtime_error("xell_set_sleep_mode_" + std::to_string(static_cast<int>(llResult)));

        fgResult = m_api->setLatencyReduction(fgContext, xellContext);
        if (fgResult != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            throw std::runtime_error("xefg_latency_reduction_" + std::to_string(static_cast<int>(fgResult)));

        xefg_swapchain_d3d12_init_params_t params{};
        params.pApplicationSwapChain = applicationSwapChain;
        params.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH;
        params.maxInterpolatedFrames = 5;
        params.creationNodeMask = 1;
        params.visibleNodeMask = 1;
        params.uiMode = XEFG_SWAPCHAIN_UI_MODE_NONE;
        fgResult = m_api->initFromSwapChain(fgContext, queue, &params);
        if (fgResult != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            throw std::runtime_error("xefg_init_swapchain_" + std::to_string(static_cast<int>(fgResult)));

        void* wrapped = nullptr;
        fgResult = m_api->getSwapChain(fgContext, IID_IDXGISwapChain4, &wrapped);
        if (fgResult != XEFG_SWAPCHAIN_RESULT_SUCCESS || !wrapped)
            throw std::runtime_error("xefg_get_swapchain_" + std::to_string(static_cast<int>(fgResult)));
        *wrappedSwapChain = static_cast<IDXGISwapChain4*>(wrapped);
        m_device = device;
        m_frameId = 1;
        m_multiplier = 2;
        m_failed = false;
        m_active = false;
        m_error = L"XeSS MFG ready (AMD 2x-6x)";
        m_api->setFrameCount(fgContext, 1);
        m_api->setEnabled(fgContext, 0);
        m_api->addMarker(xellContext, m_frameId, XELL_SIMULATION_START);
        logEvent("init_ok", "\"vendor_id\":4098,\"maximum_multiplier\":6,\"display\":\"" +
                 std::to_string(display.w) + "x" + std::to_string(display.h) + "\"");
        return true;
    } catch (const std::exception& e) {
        m_error = widen(e.what());
        logEvent("init_failed", "\"reason\":\"" + escape(e.what()) + "\"");
        shutdown();
        return false;
    } catch (...) {
        m_error = L"XeSS MFG: unknown initialization failure";
        logEvent("init_failed", "\"reason\":\"unknown\"");
        shutdown();
        return false;
    }
}

bool XeMfgFrameGeneration::prepare(ID3D12GraphicsCommandList* commandList,
                                   ID3D12Resource* depth, ID3D12Resource* motionVectors,
                                   Size render, float frameTimeMs, bool reset,
                                   bool enabled, uint32_t multiplier)
{
    if (m_failed || !m_context || !m_api) return false;
    auto fg = static_cast<xefg_swapchain_handle_t>(m_context);
    auto xell = static_cast<xell_context_handle_t>(m_xell);
    multiplier = std::clamp(multiplier, 2u, 6u);
    if (!render.w || !render.h) enabled = false;
    if (enabled && (!commandList || !depth || !motionVectors)) {
        m_error = L"XeSS MFG: depth or motion-vector input unavailable";
        return false;
    }
    const uint32_t id = m_frameId;
    xefg_swapchain_result_t result = m_api->setEnabled(fg, enabled ? 1u : 0u);
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        m_failed = true; m_active = false;
        m_error = L"XeSS MFG: enable/disable failed (" + std::to_wstring(static_cast<int>(result)) + L")";
        return false;
    }
    if (multiplier != m_multiplier) {
        result = m_api->setFrameCount(fg, multiplier - 1);
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            m_failed = true; m_active = false;
            m_error = L"XeSS MFG: multiplier change failed (" + std::to_wstring(static_cast<int>(result)) + L")";
            return false;
        }
        m_multiplier = multiplier;
    }

    if (enabled) {
        auto tag = [&](xefg_swapchain_resource_type_t type, ID3D12Resource* resource) {
            xefg_swapchain_d3d12_resource_data_t data{};
            data.type = type;
            data.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
            data.resourceBase = {0, 0};
            data.resourceSize = {render.w, render.h};
            data.pResource = resource;
            data.incomingState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            return m_api->tagResource(fg, nullptr, id, &data);
        };
        result = tag(XEFG_SWAPCHAIN_RES_DEPTH, depth);
        if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
            result = tag(XEFG_SWAPCHAIN_RES_MOTION_VECTOR, motionVectors);
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            m_failed = true; m_active = false;
            m_error = L"XeSS MFG: input resource tagging failed (" +
                      std::to_wstring(static_cast<int>(result)) + L")";
            return false;
        }

        xefg_swapchain_frame_constant_data_t constants{};
        const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        std::memcpy(constants.viewMatrix, identity, sizeof(identity));
        const float aspect = float(render.w) / float(render.h);
        makeProjection(aspect, 0.01f, 1000.0f, 1.0f, true, constants.projectionMatrix);
        constants.jitterOffsetX = 0.0f;
        constants.jitterOffsetY = 0.0f;
        constants.motionVectorScaleX = 1.0f;
        constants.motionVectorScaleY = 1.0f;
        constants.resetHistory = reset ? 1u : 0u;
        constants.frameRenderTime = std::clamp(frameTimeMs, 0.1f, 1000.0f);
        result = m_api->tagConstants(fg, id, &constants);
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            m_failed = true; m_active = false;
            m_error = L"XeSS MFG: frame constants failed (" +
                      std::to_wstring(static_cast<int>(result)) + L")";
            return false;
        }
    }

    result = m_api->setPresentId(fg, id);
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        m_failed = true; m_active = false;
        m_error = L"XeSS MFG: present ID failed (" +
                  std::to_wstring(static_cast<int>(result)) + L")";
        return false;
    }
    m_active = enabled;
    if (xell) {
        m_api->addMarker(xell, id, XELL_SIMULATION_END);
        m_api->addMarker(xell, id, XELL_RENDERSUBMIT_START);
        m_api->addMarker(xell, id, XELL_RENDERSUBMIT_END);
        m_api->addMarker(xell, id, XELL_PRESENT_START);
    }
    return true;
}

void XeMfgFrameGeneration::afterPresent() noexcept
{
    if (!m_api || !m_xell || !m_context || m_failed) return;
    auto xell = static_cast<xell_context_handle_t>(m_xell);
    const uint32_t presented = m_frameId;
    const uint32_t next = presented + 1;
    m_api->addMarker(xell, presented, XELL_PRESENT_END);
    const xell_result_t slept = m_api->sleep(xell, next);
    if (slept != XELL_RESULT_SUCCESS) {
        m_failed = true;
        m_active = false;
        m_error = L"XeSS MFG: XeLL pacing failed (" +
                  std::to_wstring(static_cast<int>(slept)) + L")";
        logEvent("xell_sleep_failed", "\"result\":" + std::to_string(static_cast<int>(slept)));
        return;
    }
    m_frameId = next ? next : 1;
    m_api->addMarker(xell, m_frameId, XELL_SIMULATION_START);
}

void XeMfgFrameGeneration::shutdown() noexcept
{
    m_active = false;
    if (m_context && m_api && m_api->destroy) {
        const auto result = m_api->destroy(static_cast<xefg_swapchain_handle_t>(m_context));
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            logEvent("destroy_context_failed", "\"result\":" + std::to_string(static_cast<int>(result)));
    }
    m_context = nullptr;
    if (m_xell && m_api && m_api->destroyXell) {
        const auto result = m_api->destroyXell(static_cast<xell_context_handle_t>(m_xell));
        if (result != XELL_RESULT_SUCCESS)
            logEvent("destroy_xell_failed", "\"result\":" + std::to_string(static_cast<int>(result)));
    }
    m_xell = nullptr;
    if (m_patches) {
        m_patches->restore();
        m_patches.reset();
    }
    if (m_api) {
        if (m_api->payloadModule) FreeLibrary(m_api->payloadModule);
        // Runtime modules are pinned by xess_layout after validation. Do not unload code
        // while function pointers or runtime-owned objects could still reference it.
        m_api->payloadModule = nullptr;
        m_api.reset();
    }
    m_device = nullptr;
}
