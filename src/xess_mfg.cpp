#include "xess_mfg.h"

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
#include <utility>

static std::string escape(const std::string& value);
static void logEvent(const char* stage, const std::string& fields = {});

#include "runtime_payload.h"
#include "sha256_util.h"
#include "runtime_layout.h"
#include "host_unlock.h"
#include "model_port.h"
#include "shader_port.h"

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

static void logEvent(const char* stage, const std::string& fields)
{
    std::string line = "[NRLive XeMFG] ";
    line += stage ? stage : "event";
    if (!fields.empty()) line += " " + fields;
    line += "\r\n";
    OutputDebugStringA(line.c_str());

    // OutputDebugString is not normally persisted. Keep a flush-on-each-event
    // file beside NRLive.exe so native loader crashes still leave a last stage.
    wchar_t path[MAX_PATH]{};
    const DWORD count = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!count || count >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"NRLive-xess.log");
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
}

namespace {
constexpr const char* kXeFgName = "libxess_fg.dll";
constexpr const char* kXeLlName = "libxell.dll";
constexpr const char* kPayloadName = "MFGUnlocker.dll";

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
    if (FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) return 0;
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
    decltype(&::xefgSwapChainGetLastPresentStatus) getLastPresentStatus = nullptr;
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

    bool resolve(std::string& missingExport)
    {
        // These member names are local aliases. GetProcAddress must use the
        // exact exported ABI names declared by Intel's XeSS 3.0.2 headers.
#define RESOLVE_FG(member, exported) \
        member = symbol<decltype(member)>(fgModule, exported); \
        if (!member) { missingExport = std::string("libxess_fg.dll!") + exported; return false; }
#define RESOLVE_LL(member, exported) \
        member = symbol<decltype(member)>(llModule, exported); \
        if (!member) { missingExport = std::string("libxell.dll!") + exported; return false; }
        RESOLVE_FG(createContext, "xefgSwapChainD3D12CreateContext");
        RESOLVE_FG(initFromSwapChain, "xefgSwapChainD3D12InitFromSwapChain");
        RESOLVE_FG(getSwapChain, "xefgSwapChainD3D12GetSwapChainPtr");
        RESOLVE_FG(tagResource, "xefgSwapChainD3D12TagFrameResource");
        RESOLVE_FG(tagConstants, "xefgSwapChainTagFrameConstants");
        RESOLVE_FG(setEnabled, "xefgSwapChainSetEnabled");
        RESOLVE_FG(setFrameCount, "xefgSwapChainSetNumInterpolatedFrames");
        RESOLVE_FG(setPresentId, "xefgSwapChainSetPresentId");
        RESOLVE_FG(getLastPresentStatus, "xefgSwapChainGetLastPresentStatus");
        RESOLVE_FG(setLatencyReduction, "xefgSwapChainSetLatencyReduction");
        RESOLVE_FG(setLogging, "xefgSwapChainSetLoggingCallback");
        RESOLVE_FG(destroy, "xefgSwapChainDestroy");
        RESOLVE_LL(createXell, "xellD3D12CreateContext");
        RESOLVE_LL(setSleepMode, "xellSetSleepMode");
        RESOLVE_LL(sleep, "xellSleep");
        RESOLVE_LL(addMarker, "xellAddMarkerData");
        RESOLVE_LL(setXellLogging, "xellSetLoggingCallback");
        RESOLVE_LL(destroyXell, "xellDestroyContext");
#undef RESOLVE_FG
#undef RESOLVE_LL
        return true;
    }
};

struct XeMfgPatches
{
public:
    std::unique_ptr<ModelPort> model;
    std::unique_ptr<ShaderPort> shader;
    std::unique_ptr<xess_host_unlock::Unlock> unlock;

    void apply(HMODULE fg, HMODULE ll, HMODULE payload, UINT vendor)
    {
        logEvent("model_port_begin");
        model = std::make_unique<ModelPort>(fg, true, true);
        logEvent("model_port_ready");
        logEvent("shader_port_begin");
        shader = std::make_unique<ShaderPort>(fg, payload, std::filesystem::path{}, vendor);
        logEvent("shader_port_ready");
        // The AMD port uses the MFG model for every multiplier, including 2x.
        // Five interpolated frames unlock the API limit to 6x output.
        logEvent("host_unlock_begin");
        unlock = std::make_unique<xess_host_unlock::Unlock>(fg, ll, uint8_t(5), false);
        logEvent("host_unlock_ready");
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

XeMfgFrameGeneration::XeMfgFrameGeneration() = default;

XeMfgFrameGeneration::~XeMfgFrameGeneration()
{
    shutdown();
}

bool XeMfgFrameGeneration::init(ID3D12Device* device, Size display,
                                IDXGISwapChain4*& applicationSwapChain,
                                ID3D12CommandQueue* queue,
                                IDXGISwapChain4** wrappedSwapChain)
{
    logEvent("init_begin");
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

        logEvent("runtime_files_found");
        m_api->llModule = LoadLibraryExW(llPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m_api->llModule) throw std::runtime_error("libxell_load_win32_" + std::to_string(GetLastError()));
        logEvent("libxell_loaded");
        m_api->fgModule = LoadLibraryExW(fgPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m_api->fgModule) throw std::runtime_error("libxess_fg_load_win32_" + std::to_string(GetLastError()));
        logEvent("libxess_fg_loaded");
        m_api->payloadModule = LoadLibraryExW(payloadPath.c_str(), nullptr,
            LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (!m_api->payloadModule) throw std::runtime_error("MFGUnlocker_resource_load_win32_" + std::to_string(GetLastError()));
        logEvent("MFGUnlocker_resource_loaded");

        const auto fgHash = hashFile(fgPath);
        const auto llHash = hashFile(llPath);
        logEvent("runtime_hashes", "\"libxess_fg\":\"" + fgHash + "\",\"libxell\":\"" + llHash + "\"");
        logEvent("runtime_layout_validation_begin");
        if (!xess_layout::Pair(m_api->fgModule, m_api->llModule))
            throw std::runtime_error("runtime_instruction_layout_not_supported");
        logEvent("runtime_layout_validation_ok");
        std::string missingExport;
        if (!m_api->resolve(missingExport))
            throw std::runtime_error("runtime_export_missing:" + missingExport);
        logEvent("runtime_exports_resolved");

        const UINT vendor = adapterVendor(device);
        if (vendor != 0x1002) throw std::runtime_error("XeMFG_AMD_route_requires_vendor_1002");
        logEvent("compatibility_patches_begin", "\"vendor_id\":" + std::to_string(vendor));
        auto patches = std::make_unique<XeMfgPatches>();
        patches->apply(m_api->fgModule, m_api->llModule, m_api->payloadModule, vendor);
        m_patches = std::move(patches);
        logEvent("compatibility_patches_ready");

        xefg_swapchain_handle_t fgContext = nullptr;
        logEvent("xefg_create_context_begin");
        xefg_swapchain_result_t fgResult = m_api->createContext(device, &fgContext);
        if (fgResult != XEFG_SWAPCHAIN_RESULT_SUCCESS || !fgContext)
            throw std::runtime_error("xefg_create_context_" + std::to_string(static_cast<int>(fgResult)));
        m_context = fgContext;
        fgResult = m_api->setLogging(fgContext, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, logFg, nullptr);
        if (fgResult != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            throw std::runtime_error("xefg_set_logging_" + std::to_string(static_cast<int>(fgResult)));

        xell_context_handle_t xellContext = nullptr;
        logEvent("xell_create_context_begin");
        xell_result_t llResult = m_api->createXell(device, &xellContext);
        if (llResult != XELL_RESULT_SUCCESS || !xellContext)
            throw std::runtime_error("xell_create_context_" + std::to_string(static_cast<int>(llResult)));
        m_xell = xellContext;
        llResult = m_api->setXellLogging(xellContext, XELL_LOGGING_LEVEL_WARNING, logLl);
        if (llResult != XELL_RESULT_SUCCESS)
            throw std::runtime_error("xell_set_logging_" + std::to_string(static_cast<int>(llResult)));
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
        logEvent("xefg_init_swapchain_begin");
        // XeSS InitFromSwapChain consumes the caller's one owned swapchain
        // reference on success. Clear the caller's pointer immediately after
        // that successful transfer so it cannot Release an invalid COM object.
        fgResult = m_api->initFromSwapChain(fgContext, queue, &params);
        if (fgResult != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            throw std::runtime_error("xefg_init_swapchain_" + std::to_string(static_cast<int>(fgResult)));
        applicationSwapChain = nullptr;
        logEvent("application_swapchain_ownership_transferred");

        void* wrapped = nullptr;
        logEvent("xefg_get_wrapped_swapchain_begin");
        fgResult = m_api->getSwapChain(fgContext, IID_IDXGISwapChain4, &wrapped);
        if (fgResult != XEFG_SWAPCHAIN_RESULT_SUCCESS || !wrapped)
            throw std::runtime_error("xefg_get_swapchain_" + std::to_string(static_cast<int>(fgResult)));
        *wrappedSwapChain = static_cast<IDXGISwapChain4*>(wrapped);
        m_device = device;
        m_frameId = 1;
        m_multiplier = 2;
        m_failed = false;
        m_xellFailed = false;
        m_active = false;
        m_error = L"XeSS MFG ready (AMD 2x-6x)";
        fgResult = m_api->setFrameCount(fgContext, 1);
        if (fgResult != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            throw std::runtime_error("xefg_set_initial_frame_count_" + std::to_string(static_cast<int>(fgResult)));
        fgResult = m_api->setEnabled(fgContext, 0);
        if (fgResult != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            throw std::runtime_error("xefg_set_initial_enabled_" + std::to_string(static_cast<int>(fgResult)));
        // XeLL requires Sleep to be the first API call for each newly
        // assigned frame ID. Start frame 1 before submitting its markers.
        llResult = m_api->sleep(xellContext, m_frameId);
        if (llResult != XELL_RESULT_SUCCESS)
            throw std::runtime_error("xell_initial_sleep_" + std::to_string(static_cast<int>(llResult)));
        llResult = m_api->addMarker(xellContext, m_frameId, XELL_SIMULATION_START);
        if (llResult != XELL_RESULT_SUCCESS)
            throw std::runtime_error("xell_initial_simulation_start_" + std::to_string(static_cast<int>(llResult)));
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
    if (!m_api || !m_context || !m_xell) {
        m_active = false;
        return false;
    }

    const uint32_t id = m_frameId;
    auto fg = static_cast<xefg_swapchain_handle_t>(m_context);
    auto xell = static_cast<xell_context_handle_t>(m_xell);
    multiplier = std::clamp(multiplier, 2u, 6u);

    bool frameInputsValid = true;
    if (!render.w || !render.h)
        enabled = false;
    if (enabled && (!commandList || !depth || !motionVectors)) {
        m_error = L"XeSS MFG: depth or motion-vector input unavailable";
        enabled = false;
        frameInputsValid = false;
    }

    bool fgCallOk = !m_failed;
    auto failFg = [&](const char* operation, const wchar_t* label, int result) {
        m_failed = true;
        m_active = false;
        fgCallOk = false;
        m_error = std::wstring(L"XeSS MFG: ") + label + L" failed (" +
                  std::to_wstring(result) + L")";
        logEvent("fg_call_failed",
            "\"operation\":\"" + std::string(operation) +
            "\",\"result\":" + std::to_string(result) +
            ",\"present_id\":" + std::to_string(id));
    };

    if (fgCallOk) {
        const auto result = m_api->setEnabled(fg, enabled ? 1u : 0u);
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            failFg("enable_disable", L"enable/disable", static_cast<int>(result));
    }
    if (fgCallOk && multiplier != m_multiplier) {
        const auto result = m_api->setFrameCount(fg, multiplier - 1);
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            failFg("multiplier_change", L"multiplier change", static_cast<int>(result));
        else
            m_multiplier = multiplier;
    }

    if (fgCallOk && enabled) {
        auto tag = [&](xefg_swapchain_resource_type_t type, ID3D12Resource* resource) {
            xefg_swapchain_d3d12_resource_data_t data{};
            data.type = type;
            data.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
            data.resourceBase = {0, 0};
            data.resourceSize = {render.w, render.h};
            data.pResource = resource;
            data.incomingState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            return m_api->tagResource(fg, commandList, id, &data);
        };
        auto result = tag(XEFG_SWAPCHAIN_RES_DEPTH, depth);
        if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
            result = tag(XEFG_SWAPCHAIN_RES_MOTION_VECTOR, motionVectors);
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            failFg("input_resource_tagging", L"input resource tagging", static_cast<int>(result));
        } else {
            xefg_swapchain_frame_constant_data_t constants{};
            const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            std::memcpy(constants.viewMatrix, identity, sizeof(identity));
            const float aspect = float(render.w) / float(render.h);
            makeProjection(aspect, 0.1f, 10000.0f, 1.0471976f, true, constants.projectionMatrix);
            constants.jitterOffsetX = 0.0f;
            constants.jitterOffsetY = 0.0f;
            constants.motionVectorScaleX = 1.0f;
            constants.motionVectorScaleY = 1.0f;
            constants.resetHistory = reset ? 1u : 0u;
            constants.frameRenderTime = std::clamp(frameTimeMs, 0.1f, 1000.0f);
            result = m_api->tagConstants(fg, id, &constants);
            if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
                failFg("frame_constants", L"frame constants", static_cast<int>(result));
        }
    }

    if (fgCallOk) {
        const auto result = m_api->setPresentId(fg, id);
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            failFg("present_id", L"present ID", static_cast<int>(result));
    }

    // XeLL markers are frame bookkeeping, not conditional on interpolation.
    // They must be supplied on warm-up, warning, and FG-disabled frames too.
    bool markerOk = true;
    if (!m_xellFailed) {
        const std::pair<xell_latency_marker_type_t, const char*> markers[] = {
            {XELL_SIMULATION_END, "simulation_end"},
            {XELL_RENDERSUBMIT_START, "rendersubmit_start"},
            {XELL_RENDERSUBMIT_END, "rendersubmit_end"},
            {XELL_PRESENT_START, "present_start"}
        };
        for (const auto& [marker, name] : markers) {
            const auto result = m_api->addMarker(xell, id, marker);
            if (result != XELL_RESULT_SUCCESS) {
                markerOk = false;
                m_xellFailed = true;
                m_failed = true;
                m_active = false;
                m_error = L"XeSS MFG: XeLL marker failed (" +
                          std::to_wstring(static_cast<int>(result)) + L")";
                logEvent("xell_marker_failed",
                    "\"marker\":\"" + std::string("\\"marker\\":\\"") + name + "\",\"result\":" +
                    std::to_string(static_cast<int>(result)) +
                    ",\"present_id\":" + std::to_string(id));
                break;
            }
        }
    }

    m_active = fgCallOk && enabled && frameInputsValid && markerOk;
    return fgCallOk && frameInputsValid && markerOk;
}

bool XeMfgFrameGeneration::afterPresent() noexcept
{
    if (!m_api || !m_xell || !m_context) {
        m_active = false;
        return false;
    }

    xefg_swapchain_present_status_t status{};
    bool haveStatus = false;
    if (!m_failed) {
        const auto statusResult = m_api->getLastPresentStatus(
            static_cast<xefg_swapchain_handle_t>(m_context), &status);
        if (statusResult == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            haveStatus = true;
        } else {
            m_failed = true;
            m_active = false;
            m_error = L"XeSS MFG: present status query failed (" +
                      std::to_wstring(static_cast<int>(statusResult)) + L")";
            logEvent("present_status_failed", "\"result\":" +
                     std::to_string(static_cast<int>(statusResult)));
        }
    }

    // Result 3 is WARNING_TOO_FEW_FRAMES, a nonfatal warm-up/reset warning.
    // It means this Present produced no interpolated frames, not that XeLL's
    // frame timeline should stop advancing.
    m_active = haveStatus && status.isFrameGenEnabled != 0 &&
               status.frameGenResult == XEFG_SWAPCHAIN_RESULT_SUCCESS &&
               status.framesPresented > 1;

    static uint64_t statusFrame = 0;
    static uint32_t lastEnabled = UINT32_MAX;
    static uint32_t lastResult = UINT32_MAX;
    static uint32_t lastPresented = UINT32_MAX;
    ++statusFrame;
    if (haveStatus &&
        (status.isFrameGenEnabled != lastEnabled ||
         static_cast<uint32_t>(status.frameGenResult) != lastResult ||
         status.framesPresented != lastPresented || (statusFrame % 120) == 0)) {
        logEvent("present_status",
            "\"enabled\":" + std::to_string(status.isFrameGenEnabled) +
            ",\"frames_presented\":" + std::to_string(status.framesPresented) +
            ",\"frame_gen_result\":" + std::to_string(static_cast<int>(status.frameGenResult)) +
            ",\"present_id\":" + std::to_string(m_frameId));
        if (status.frameGenResult > XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            logEvent("frame_generation_warning",
                "\"warning\":" + std::to_string(static_cast<int>(status.frameGenResult)) +
                ",\"present_id\":" + std::to_string(m_frameId));
        }
        lastEnabled = status.isFrameGenEnabled;
        lastResult = static_cast<uint32_t>(status.frameGenResult);
        lastPresented = status.framesPresented;
    }

    const uint32_t presented = m_frameId;
    const uint32_t next = presented + 1 ? presented + 1 : 1;
    auto xell = static_cast<xell_context_handle_t>(m_xell);

    // Always close PRESENT for the ID submitted to XeFG/XeLL. A skipped
    // interpolation frame still consumed a real Present and must not create a
    // hole in XeLL's monotonically increasing frame IDs.
    if (!m_xellFailed) {
        const auto ended = m_api->addMarker(xell, presented, XELL_PRESENT_END);
        if (ended != XELL_RESULT_SUCCESS) {
            m_xellFailed = true;
            m_failed = true;
            m_active = false;
            m_error = L"XeSS MFG: XeLL present-end marker failed (" +
                      std::to_wstring(static_cast<int>(ended)) + L")";
            logEvent("xell_present_end_failed",
                     "\"result\":" + std::to_string(static_cast<int>(ended)) +
                     ",\"present_id\":" + std::to_string(presented));
        } else {
            // Sleep begins the NEXT frame. Avoid a synchronous disk flush on
            // every source frame; errors are logged below, status is sampled
            // at a low rate above.
            const xell_result_t slept = m_api->sleep(xell, next);
            if (slept != XELL_RESULT_SUCCESS) {
                m_xellFailed = true;
                m_failed = true;
                m_active = false;
                m_error = L"XeSS MFG: XeLL pacing failed (" +
                          std::to_wstring(static_cast<int>(slept)) + L")";
                logEvent("xell_sleep_failed",
                         "\"result\":" + std::to_string(static_cast<int>(slept)) +
                         ",\"requested_frame\":" + std::to_string(next));
            } else {
                const auto started = m_api->addMarker(xell, next, XELL_SIMULATION_START);
                if (started != XELL_RESULT_SUCCESS) {
                    m_xellFailed = true;
                    m_failed = true;
                    m_active = false;
                    m_error = L"XeSS MFG: XeLL simulation-start marker failed (" +
                              std::to_wstring(static_cast<int>(started)) + L")";
                    logEvent("xell_simulation_start_failed",
                             "\"result\":" + std::to_string(static_cast<int>(started)) +
                             ",\"frame_id\":" + std::to_string(next));
                }
            }
        }
    }

    m_frameId = next;
    return m_active;
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
    m_failed = false;
    m_xellFailed = false;
    m_frameId = 0;
}
