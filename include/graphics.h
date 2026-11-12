#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <cstdint>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;

struct Size { uint32_t w = 0, h = 0; };

class Graphics {
public:
  Graphics() = default;
  ~Graphics();
  Graphics(const Graphics&) = delete;
  Graphics& operator=(const Graphics&) = delete;

  bool init(HWND output, Size render, Size display);
  bool resize(Size display);
  // Release all app-owned references before AMD replaces the swapchain.
  void releaseSwapChainForWrap();
  bool rebuildSwapChain(Size display);
  bool adoptSwapChain(IDXGISwapChain4* wrapped);
  // Recreate the ordinary swapchain with DXGI's frame-latency waitable object.
  // Used by ANR to keep generated-frame presentation queue depth at one.
  bool enableFramePacing();
  void waitForFrameLatency();
  bool begin(bool clearSyntheticDepth = true);
  // Submit the current prep command list and open a second list for the same frame.
  // Used to place the cross-API DLSSNR pass between motion generation and FSR without a CPU wait.
  bool submitForInterop();
  ID3D12Resource* snapshotDlssNrInput(ID3D12Resource* colour, D3D12_RESOURCE_STATES colourState);
  ID3D12Resource* ensureDlssNrOutput(ID3D12Resource* colour);
  ID3D12Resource* dlssNrOutput() const { return m_dlssNrOutput.Get(); }
  void end();
  ID3D12Device* device() const { return m_dev.Get(); }
  IDXGISwapChain4* swapChain() const { return m_swap.Get(); }
  ID3D12CommandQueue* queue() const { return m_queue.Get(); }
  ID3D12GraphicsCommandList* cmd() const { return (m_interopContinuation ? m_cmdContinuation : m_cmd).Get(); }
  ID3D12Resource* backbuffer() const { return m_back[m_index].Get(); }
  ID3D12Resource* upscaleOutput() const { return m_upscaleOutput.Get(); }
  ID3D12Resource* dummyDepth() const { return m_dummyDepth.Get(); }
  ID3D12Resource* motionVectors() const { return m_motionVectors.Get(); }
  ID3D12Resource* reactiveMask() const { return m_reactiveMask.Get(); }
  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle() const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtvBase;
    h.ptr += (SIZE_T)m_index * m_rtvStride;
    return h;
  }
  uint32_t index() const { return m_index; }
  Size display() const { return m_display; }
  Size renderSize() const { return m_render; }
  bool present();

  // Drain the GPU: signal the frame fence and CPU-wait until the GPU has
  // finished everything queued up to this point.  Call this BEFORE tearing
  // down FSR / AMDOF / capture so that the driver does not crash trying to
  // clean up resources still referenced by in-flight command lists.
  bool waitForGpu();

  bool ensureAuxTextures(Size render);

  // Stretch-blit src (any size, PIXEL/NON_PIXEL SRV state) onto the current
  // backbuffer (must already be in RENDER_TARGET state).
  void blitToBackbuffer(ID3D12Resource* src, UINT srvSlot = 0);
  // Return an RGBA8 display-sized color texture for NGX providers. When the
  // source already matches that contract, no extra blit is recorded.
  ID3D12Resource* prepareAnrInput(ID3D12Resource* src);

  // Queue a PNG readback of the current backbuffer. Call after the final
  // FSR/blit pass and before end()/present(). present() completes the
  // readback only when a screenshot was requested.
  bool captureBackbufferScreenshot(const std::wstring& folder, uint64_t frameIndex);

private:
  bool buildSwapChain(Size display);
  bool createAuxTextures(Size render);
  bool createBlitPipeline();
  void closeFrameLatencyHandle();

  ComPtr<ID3D12Device> m_dev;
  ComPtr<ID3D12CommandQueue> m_queue;
  ComPtr<IDXGIFactory6> m_factory;
  ComPtr<IDXGISwapChain1> m_swap1;
  ComPtr<IDXGISwapChain4> m_swap;
  HANDLE m_frameLatencyWaitableObject = nullptr;
  bool m_framePacingEnabled = false;
  ComPtr<ID3D12DescriptorHeap> m_rtv;
  ComPtr<ID3D12DescriptorHeap> m_srvHeap;
  ComPtr<ID3D12CommandAllocator> m_alloc[3];
  ComPtr<ID3D12CommandAllocator> m_allocContinuation[3];
  ComPtr<ID3D12GraphicsCommandList> m_cmd;
  ComPtr<ID3D12GraphicsCommandList> m_cmdContinuation;
  bool m_interopContinuation = false;
  ComPtr<ID3D12Resource> m_dlssNrInput;
  std::vector<ComPtr<ID3D12Resource>> m_retiredDlssNrInputs;
  D3D12_RESOURCE_STATES m_dlssNrInputState = D3D12_RESOURCE_STATE_COMMON;
  ComPtr<ID3D12Resource> m_dlssNrOutput;
  std::vector<ComPtr<ID3D12Resource>> m_retiredDlssNrOutputs;
  ComPtr<ID3D12Fence> m_fence;
  HANDLE m_fenceEvent = nullptr;
  uint64_t m_fenceValue = 0;
  uint64_t m_frameSlotFence[3]{};
  uint32_t m_frameSlot = 0;
  ComPtr<ID3D12Resource> m_back[3];
  uint32_t m_index = 0;
  UINT m_rtvStride = 0;
  UINT m_srvStride = 0;
  D3D12_CPU_DESCRIPTOR_HANDLE m_rtvBase{};
  Size m_display{};
  Size m_render{};
  HWND m_output = nullptr;

  ComPtr<ID3D12Resource> m_anrInput;
  ComPtr<ID3D12DescriptorHeap> m_anrInputRtvHeap;
  D3D12_RESOURCE_STATES m_anrInputState = D3D12_RESOURCE_STATE_COMMON;

  ComPtr<ID3D12Resource> m_upscaleOutput;
  ComPtr<ID3D12Resource> m_dummyDepth;
  ComPtr<ID3D12Resource> m_motionVectors;
  ComPtr<ID3D12Resource> m_reactiveMask;
  ComPtr<ID3D12DescriptorHeap> m_clearGpuHeap;
  ComPtr<ID3D12DescriptorHeap> m_clearCpuHeap;

  // Fullscreen stretch blit
  ComPtr<ID3D12RootSignature> m_blitRs;
  ComPtr<ID3D12PipelineState> m_blitPso;

  // Cached SRVs use separate slots for the normal blit, ANR's generated
  // and real outputs, and per-frame ANR input conversions. This avoids
  // rewriting descriptors still referenced by queued command lists.
  ID3D12Resource* m_lastBlitSrc[8]{};
  DXGI_FORMAT m_lastBlitFmt[8] = {
    DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
    DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN
  };

  // One-shot screenshot readback. Kept alive until present() has submitted
  // the command list and waited for its fence.
  ComPtr<ID3D12Resource> m_screenshotReadback;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_screenshotFootprint{};
  UINT m_screenshotWidth = 0;
  UINT m_screenshotHeight = 0;
  std::wstring m_screenshotFolder;
  uint64_t m_screenshotFrame = 0;
  bool m_screenshotPending = false;
};
