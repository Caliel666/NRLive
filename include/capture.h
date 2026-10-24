#pragma once
#include <windows.h>
#include <string>
#include <atomic>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.System.h>
#include "graphics.h"

using Microsoft::WRL::ComPtr;

// Capture path matches SAOG0721/Magpie GraphicsCaptureFrameSource:
//  - Direct3D11CaptureFramePool::Create (or CreateFreeThreaded)
//  - FrameArrived callback is EMPTY (only wakes the thread)
//  - TryGetNextFrame + drain on the render thread
//  - pure D3D11 CopySubresourceRegion into our output texture
//  - that texture is shared to D3D12 for FSR

enum class CaptureMode { DxgiWindow, WgcWindow };

class Capture {
public:
  bool init(ID3D12Device* d12, ID3D12CommandQueue* q);
  ~Capture();
  bool start(HWND hwnd, CaptureMode mode = CaptureMode::WgcWindow);
  // Polls the frame pool (Magpie _Update). True when a new frame was copied.
  bool acquire(ComPtr<ID3D12Resource>& out, Size& size, uint64_t& fenceValue);
  // Sleep until WGC reports a frame instead of busy-polling an empty frame pool.
  void waitForFrame(DWORD timeoutMs = 1) const;
  ID3D12Fence* fence() const { return m_fence.Get(); }
  void release(uint64_t) {} // no-op; Magpie does not need this
  bool saveScreenshot(const std::wstring& folder, uint64_t frameIndex);
  void stop();
  bool usingDxgi() const { return m_dxgiMode; }
  const std::wstring& lastError() const { return m_error; }
  uint64_t totalFrames() const { return m_frameCount.load(); }

private:
  bool createOutputTexture(Size size);
  bool startWgc(HWND hwnd);
  bool startDxgi(HWND hwnd);
  // Compute the client-area rect within the captured window and store it in
  // m_clientOffset / m_clientSize.  Called at start() and whenever the
  // captured frame size changes.  For a borderless fullscreen window the
  // client area == window area and the offset is (0,0) — the crop becomes a
  // no-op CopySubresourceRegion that covers the whole frame.
  void recomputeClientArea(HWND hwnd);

  // Pure D3D11 device (NOT 11on12) — same as Magpie DeviceResources path
  ComPtr<ID3D11Device> m_d11;
  ComPtr<ID3D11Device5> m_d11_5;
  ComPtr<ID3D11DeviceContext> m_ctx;
  ComPtr<ID3D12Device> m_d12;
  ComPtr<ID3D12CommandQueue> m_q;

  ComPtr<IDXGIOutputDuplication> m_duplication;
  bool m_dxgiMode = false;
  bool m_dxgiCursorFallbackTried = false;
  RECT m_monitorRect{};
  RECT m_captureRect{};
  winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{ nullptr };
  winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_pool{ nullptr };
  winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{ nullptr };
  winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker m_arrived;
  winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{ nullptr };

  // D3D11 output (Magpie _output) shared to D3D12
  ComPtr<ID3D11Texture2D> m_outD11;
  ComPtr<ID3D12Resource> m_outD12;
  HANDLE m_sharedHandle = nullptr;

  ComPtr<ID3D12Fence> m_fence;
  ComPtr<ID3D11Fence> m_fence11;
  // Safe fallback when the shared D3D11 fence cannot be opened/signalled.
  ComPtr<ID3D11Query> m_copyCompleteQuery;
  HANDLE m_fenceHandle = nullptr;
  uint64_t m_fenceValue = 0;

  // m_size is the CLIENT area size (what we hand to FSR).  m_windowSize is
  // the full captured window size (what WGC delivers).  m_clientOffset is
  // where the client area starts inside the captured frame.
  Size m_size{};
  Size m_windowSize{};
  UINT m_clientOffsetX = 0;
  UINT m_clientOffsetY = 0;
  HWND m_hwnd = nullptr;
  std::wstring m_error;
  std::atomic<uint64_t> m_frameCount{ 0 };
  HANDLE m_frameArrivedEvent = nullptr;
  bool m_started = false;
};
