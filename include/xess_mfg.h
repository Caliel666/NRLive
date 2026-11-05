#pragma once
#include "graphics.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <memory>
#include <string>

struct XeMfgPatches;

class XeMfgFrameGeneration {
public:
    XeMfgFrameGeneration();
    ~XeMfgFrameGeneration();
    // The original app swapchain must already be released. XeSS creates a
    // replacement from this description; the scoped factory hook raises only
    // its internal native presentation chain's buffer count.
    bool init(ID3D12Device* device, Size display, HWND output, IDXGIFactory2* factory,
              const DXGI_SWAP_CHAIN_DESC1& swapChainDesc, ID3D12CommandQueue* queue,
              IDXGISwapChain4** wrappedSwapChain);
    bool prepare(ID3D12GraphicsCommandList* commandList, ID3D12Resource* depth,
                 ID3D12Resource* motionVectors, Size render, float frameTimeMs,
                 bool reset, bool enabled, uint32_t multiplier);
    // XeLL markers bracket the actual CPU submission and DXGI Present phases.
    void markSimulationEndAndRenderSubmitStart() noexcept;
    void markSubmittedAndPresentStart() noexcept;
    bool afterPresent() noexcept;
    void shutdown() noexcept;
    bool ready() const noexcept { return m_context != nullptr; }
    bool failed() const noexcept { return m_failed; }
    uint32_t framesPresented() const noexcept { return m_framesPresented; }
    const std::wstring& lastError() const noexcept { return m_error; }

private:
    struct Api;
    std::unique_ptr<Api> m_api;
    std::unique_ptr<XeMfgPatches> m_patches;
    void* m_context = nullptr;
    void* m_xell = nullptr;
    ID3D12Device* m_device = nullptr;
    uint32_t m_frameId = 0;
    uint32_t m_multiplier = 2;
    bool m_active = false;
    bool m_failed = false;
    bool m_xellFailed = false;
    std::wstring m_error;
};
