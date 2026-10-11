// Built as a small C-ABI DLL alongside NRLive from the upstream DLSSNR-AMD
// Windows sources. Keeping the runtime on the other side of a C ABI lets the
// MSVC launcher and the upstream MinGW-built Vulkan bridge interoperate safely.
#include "nr_pe_session.hpp"
#include "nr_pe_log.hpp"
#include "nr_pe_config.hpp"
#include "dlssnr_bridge_api.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <windows.h>

namespace {
struct HostSession {
    std::unique_ptr<nr::pe::Session> session;
    std::string error;
    std::mutex mutex;
    float requestedScale = -1.0f;
    float activeScale = 1.0f;
    float pendingScale = 1.0f;
    std::chrono::steady_clock::time_point scaleChangeAt{};
    bool scaleChangePending = false;
    bool fallbackTried = false;
};

std::string utf8(const wchar_t* value) {
    if (!value || !*value) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), bytes, nullptr, nullptr);
    result.pop_back();
    return result;
}

nr::Controls makeControls(const NRLiveDlssNrSettings& s) {
    nr::Controls c{};
    c.enabled = s.enabled != 0;
    c.apply_model = true;
    c.style = std::clamp(s.style, 0, 2);
    // Intensity is applied as a post-NR blend by NRLive. Feeding it into the
    // model's internal intensity path can soften fine detail at low values.
    c.intensity = 1.0f;
    c.local_tone = std::clamp(s.local_tone, 0.0f, 2.0f);
    c.max_ratio = std::clamp(s.max_ratio, 1.0f, 8.0f);
    c.local_structure = std::clamp(s.structure, 0.0f, 2.0f);
    c.skin_structure = std::clamp(s.skin_structure, -1.0f, 2.0f);
    c.automatic_mask = s.automatic_skin_mask != 0;
    c.passes = std::clamp(s.passes, 1, 3);
    return c;
}
}

extern "C" __declspec(dllexport) void* NRLiveDlssNrCreate(const wchar_t* root) {
    try {
        // The upstream NGX/ReShade entry points call this during DLL attach,
        // but NRLive creates Session directly through its bridge. Configure
        // ACO explicitly here, before Session creates its Vulkan device or
        // builds the first network. Merely writing [Network] ACO Mode to the
        // INI cannot enable ACO unless this one-time setup is invoked.
        const std::string sessionRoot = utf8(root);
        nr::pe::configure_network(sessionRoot);
        auto* host = new HostSession();
        host->session = std::make_unique<nr::pe::Session>(sessionRoot);
        return host;
    } catch (...) {
        return nullptr;
    }
}

extern "C" __declspec(dllexport) void NRLiveDlssNrDestroy(void* opaque) {
    delete static_cast<HostSession*>(opaque);
}

extern "C" __declspec(dllexport) int NRLiveDlssNrProcess(
    void* opaque, ID3D12Device* device, ID3D12CommandQueue* queue,
    ID3D12Resource* input_colour, uint32_t input_colour_state,
    ID3D12Resource* output_colour, uint32_t output_colour_state,
    ID3D12Resource* motion,
    uint32_t motion_state, int reset_history, const NRLiveDlssNrSettings* settings,
    NRLiveDlssNrFlushCallback flush, void* flush_user_data) {
    auto* host = static_cast<HostSession*>(opaque);
    if (!host || !host->session || !device || !queue || !input_colour || !output_colour || !settings ||
        !settings->enabled || !flush)
        return 0;

    std::lock_guard lock(host->mutex);
    try {
        const float requestedScale = std::clamp(settings->model_scale, 0.25f, 1.0f);
        const auto now = std::chrono::steady_clock::now();
        if (host->requestedScale < 0.0f) {
            // Apply the initial scale immediately; only interactive changes are debounced.
            host->requestedScale = requestedScale;
            host->activeScale = requestedScale;
            host->fallbackTried = false;
            host->error.clear();
        } else if (std::abs(requestedScale - host->requestedScale) > 0.001f) {
            host->requestedScale = requestedScale;
            host->pendingScale = requestedScale;
            host->scaleChangeAt = now;
            host->scaleChangePending = true;
            host->fallbackTried = false;
            host->error.clear();
        }
        // A trackbar sends a new value for every thumb movement. Waiting until
        // it has been stable for 500 ms prevents one Vulkan network allocation
        // per slider tick and avoids keeping many obsolete scales resident.
        if (host->scaleChangePending &&
            now - host->scaleChangeAt >= std::chrono::milliseconds(500)) {
            host->activeScale = host->pendingScale;
            host->scaleChangePending = false;
        }
        host->session->set_model_scale(host->activeScale);
        host->session->set_history_strength(std::clamp(settings->history_strength, 0.0f, 1.0f));
        host->session->set_max_passes(std::clamp(settings->passes, 1, 3));

        nr::pe::Session::D3D12QueueFrame frame{};
        frame.device = device;
        frame.queue = queue;
        frame.flush = [flush, flush_user_data] { flush(flush_user_data); };
        frame.target = output_colour;
        frame.target_state = static_cast<D3D12_RESOURCE_STATES>(output_colour_state);
        frame.source = input_colour;
        frame.source_state = static_cast<D3D12_RESOURCE_STATES>(input_colour_state);
        frame.motion = motion;
        frame.motion_state = static_cast<D3D12_RESOURCE_STATES>(motion_state);
        // The upstream runtime expects motion vectors in normalized texture
        // coordinates, not pixel units. Match the upstream ReShade adapter:
        // sx=1/colour width, sy=1/colour height. Leaving the D3D12QueueFrame
        // defaults at 1.0 turns ordinary pixel motion into enormous UV offsets,
        // effectively invalidating temporal reprojection on almost every frame.
        const D3D12_RESOURCE_DESC colourDesc = input_colour->GetDesc();
        if (colourDesc.Width > 0 && colourDesc.Height > 0) {
            frame.motion_scale_x = 1.0f / static_cast<float>(colourDesc.Width);
            frame.motion_scale_y = 1.0f / static_cast<float>(colourDesc.Height);
        }
        frame.depth = nullptr; // NRLive does not currently capture real game depth.
        frame.reset = reset_history != 0;
        auto controls = makeControls(*settings);
        bool ok = host->session->run_d3d12_queue(frame, controls);
        // Allocation checks can reject scale 1.0 by only a few MB even when
        // there is ample total VRAM. Retry once per user-selected scale at
        // progressively smaller model sizes rather than leaving NR permanently
        // unavailable until restart. Keep the successful scale for later frames.
        if (!ok && !host->fallbackTried) {
            host->fallbackTried = true;
            const float requested = host->activeScale;
            for (float candidate = std::floor((requested - 0.001f) * 10.0f) / 10.0f;
                 candidate >= 0.25f && !ok; candidate = std::round((candidate - 0.1f) * 100.0f) / 100.0f) {
                host->session->set_model_scale(candidate);
                ok = host->session->run_d3d12_queue(frame, controls);
                if (ok) {
                    host->activeScale = candidate;
                    host->error = "DLSSNR memory guard rejected model scale " +
                        std::to_string(requested) + "; running at fallback scale " +
                        std::to_string(candidate) + ". Increase available RAM/page file or lower Model scale to restore the requested setting.";
                }
            }
        }
        if (!ok) host->error = "DLSSNR did not run this frame; inspect dlssnr-amd.log for the runtime reason.";
        else if (host->error.empty()) host->error.clear();
        return ok ? 1 : 0;
    } catch (const std::exception& e) {
        host->error = e.what();
        return 0;
    } catch (...) {
        host->error = "unknown DLSSNR runtime error";
        return 0;
    }
}

extern "C" __declspec(dllexport) const char* NRLiveDlssNrLastError(void* opaque) {
    auto* host = static_cast<HostSession*>(opaque);
    if (!host) return "DLSSNR session is not initialized";
    std::lock_guard lock(host->mutex);
    return host->error.c_str();
}
