#include "capture.h"
#include <wincodec.h>
#include <shlobj.h>
#include <windows.graphics.capture.interop.h>
#include <dwmapi.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <DispatcherQueue.h>
#include <algorithm>
#include <chrono>
#include <cmath>

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {
class PerMonitorDpiScope {
public:
  PerMonitorDpiScope() : m_previous(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
  ~PerMonitorDpiScope() { if (m_previous) SetThreadDpiAwarenessContext(m_previous); }
  PerMonitorDpiScope(const PerMonitorDpiScope&) = delete;
  PerMonitorDpiScope& operator=(const PerMonitorDpiScope&) = delete;
private:
  DPI_AWARENESS_CONTEXT m_previous;
};
}

static std::wstring hex(HRESULT hr)
{
  wchar_t b[16]{};
  swprintf_s(b, L"%08lX", (unsigned long)hr);
  return b;
}

static GraphicsCaptureItem itemFromWindow(HWND hwnd)
{
  auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
  GraphicsCaptureItem item{ nullptr };
  check_hresult(interop->CreateForWindow(
    hwnd, guid_of<GraphicsCaptureItem>(), reinterpret_cast<void**>(put_abi(item))));
  return item;
}

static IDirect3DDevice makeWinrtDevice(ID3D11Device* d)
{
  ComPtr<IDXGIDevice> gd;
  check_hresult(d->QueryInterface(IID_PPV_ARGS(gd.GetAddressOf())));
  winrt::com_ptr<::IInspectable> insp;
  check_hresult(CreateDirect3D11DeviceFromDXGIDevice(gd.Get(), insp.put()));
  return insp.as<IDirect3DDevice>();
}

Capture::~Capture()
{
  stop();
  if (m_frameArrivedEvent) { CloseHandle(m_frameArrivedEvent); m_frameArrivedEvent = nullptr; }
}

bool Capture::init(ID3D12Device* d12, ID3D12CommandQueue* q)
{
  m_d12 = d12;
  m_q = q;

  // Pure D3D11 device on the same adapter as D3D12 (Magpie style).
  ComPtr<IDXGIFactory6> factory;
  HRESULT factoryHr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
  if (FAILED(factoryHr)) {
    m_error = L"CreateDXGIFactory2 0x" + hex(factoryHr); return false;
  }
  LUID luid = m_d12->GetAdapterLuid();
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    if (desc.AdapterLuid.LowPart == luid.LowPart && desc.AdapterLuid.HighPart == luid.HighPart)
      break;
    adapter.Reset();
  }

  D3D_FEATURE_LEVEL fl{};
  ComPtr<ID3D11DeviceContext> ctx;
  HRESULT h = D3D11CreateDevice(
    adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
    D3D11_SDK_VERSION, m_d11.GetAddressOf(), &fl, ctx.GetAddressOf());
  if (FAILED(h)) {
    // Fallback without adapter bind
    h = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
      D3D11_SDK_VERSION, m_d11.GetAddressOf(), &fl, ctx.GetAddressOf());
  }
  if (FAILED(h)) { m_error = L"D3D11CreateDevice 0x" + hex(h); return false; }
  m_ctx = ctx;
  m_d11.As(&m_d11_5);

  try { m_winrtDevice = makeWinrtDevice(m_d11.Get()); }
  catch (...) { m_error = L"WinRT D3D device failed"; return false; }

  HRESULT fenceHr = m_d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_fence));
  if (FAILED(fenceHr)) {
    m_error = L"D3D12 CreateFence 0x" + hex(fenceHr); return false;
  }
  HRESULT fenceShareHr = m_d12->CreateSharedHandle(m_fence.Get(), nullptr, GENERIC_ALL, nullptr, &m_fenceHandle);
  if (FAILED(fenceShareHr)) {
    m_error = L"D3D12 CreateSharedHandle(fence) 0x" + hex(fenceShareHr); return false;
  }
  if (m_d11_5) {
    // Some drivers expose ID3D11Device5 but reject opening a D3D12-created
    // shared fence. Do not silently fall back to an unsynchronised D3D12
    // queue signal: that can present the freshly-created (black) texture
    // before the D3D11 copy has completed.
    m_d11_5->OpenSharedFence(m_fenceHandle, IID_PPV_ARGS(&m_fence11));
  }
  D3D11_QUERY_DESC queryDesc{};
  queryDesc.Query = D3D11_QUERY_EVENT;
  if (FAILED(m_d11->CreateQuery(&queryDesc, &m_copyCompleteQuery))) {
    if (!m_fence11) {
      m_error = L"Neither shared D3D11 fence nor event-query synchronization is available";
      return false;
    }
  }
  return true;
}

bool Capture::createOutputTexture(Size size)
{
  m_size = size;
  m_outD11.Reset();
  m_outD12.Reset();
  if (m_sharedHandle) { CloseHandle(m_sharedHandle); m_sharedHandle = nullptr; }

  // D3D11 texture with NT shared handle → open on D3D12
  D3D11_TEXTURE2D_DESC td{};
  td.Width = size.w;
  td.Height = size.h;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

  HRESULT h = m_d11->CreateTexture2D(&td, nullptr, m_outD11.GetAddressOf());
  if (FAILED(h)) { m_error = L"D3D11 out tex 0x" + hex(h); return false; }

  ComPtr<IDXGIResource1> res1;
  HRESULT resHr = m_outD11.As(&res1);
  if (FAILED(resHr)) { m_error = L"IDXGIResource1 query 0x" + hex(resHr); return false; }
  HRESULT sharedHr = res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                      nullptr, &m_sharedHandle);
  if (FAILED(sharedHr)) {
    m_error = L"IDXGIResource1 CreateSharedHandle 0x" + hex(sharedHr); return false;
  }

  // Open on D3D12
  HRESULT openHr = m_d12->OpenSharedHandle(m_sharedHandle, IID_PPV_ARGS(&m_outD12));
  if (FAILED(openHr)) {
    m_error = L"D3D12 OpenSharedHandle 0x" + hex(openHr); return false;
  }
  return true;
}

bool Capture::start(HWND hwnd, CaptureMode mode)
{
  m_dxgiCursorFallbackTried = false;
  if (mode == CaptureMode::WgcWindow)
    return startWgc(hwnd);
  return startDxgi(hwnd);
}

bool Capture::startDxgi(HWND hwnd)
{
  // Desktop Duplication coordinates are physical pixels. Keep all HWND and
  // monitor geometry queries in the same per-monitor-aware coordinate space.
  PerMonitorDpiScope dpiScope;
  stop();
  m_hwnd = hwnd;
  m_dxgiMode = true;
  m_monitorRect = {};
  m_captureRect = {};
  if (!hwnd || !IsWindow(hwnd)) { m_error = L"Invalid target window"; return false; }

  HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{ sizeof(mi) };
  if (!monitor || !GetMonitorInfoW(monitor, &mi)) {
    m_error = L"Could not query target monitor";
    return false;
  }
  m_monitorRect = mi.rcMonitor;

  RECT client{};
  POINT origin{0, 0};
  if (!GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin)) {
    m_error = L"Could not query target client area";
    return false;
  }
  m_captureRect = {
    origin.x, origin.y,
    origin.x + (client.right - client.left),
    origin.y + (client.bottom - client.top)
  };
  RECT clipped{};
  if (!IntersectRect(&clipped, &m_captureRect, &m_monitorRect)) {
    m_error = L"Target client area is outside the selected monitor";
    return false;
  }
  m_captureRect = clipped;

  const LONG width = m_captureRect.right - m_captureRect.left;
  const LONG height = m_captureRect.bottom - m_captureRect.top;
  if (width <= 0 || height <= 0) { m_error = L"Capture region has zero size"; return false; }
  m_size = { (uint32_t)width, (uint32_t)height };
  m_windowSize = m_size;

  ComPtr<IDXGIDevice> dxgiDevice;
  HRESULT h = m_d11.As(&dxgiDevice);
  if (FAILED(h)) { m_error = L"Query IDXGIDevice 0x" + hex(h); return false; }
  ComPtr<IDXGIAdapter> adapter;
  h = dxgiDevice->GetAdapter(&adapter);
  if (FAILED(h)) { m_error = L"IDXGIDevice::GetAdapter 0x" + hex(h); return false; }

  bool foundOutput = false;
  for (UINT i = 0; ; ++i) {
    ComPtr<IDXGIOutput> output;
    h = adapter->EnumOutputs(i, &output);
    if (h == DXGI_ERROR_NOT_FOUND) break;
    if (FAILED(h)) { m_error = L"IDXGIAdapter::EnumOutputs 0x" + hex(h); return false; }
    DXGI_OUTPUT_DESC desc{};
    if (FAILED(output->GetDesc(&desc))) continue;
    if (desc.Monitor != monitor) continue;
    ComPtr<IDXGIOutput1> output1;
    h = output.As(&output1);
    if (FAILED(h)) { m_error = L"Query IDXGIOutput1 0x" + hex(h); return false; }
    h = output1->DuplicateOutput(m_d11.Get(), &m_duplication);
    if (FAILED(h)) { m_error = L"DXGI DuplicateOutput 0x" + hex(h); return false; }
    foundOutput = true;
    break;
  }
  if (!foundOutput) { m_error = L"No DXGI output matches the target monitor"; return false; }
  if (!createOutputTexture(m_size)) return false;
  m_started = true;
  return true;
}

bool Capture::startWgc(HWND hwnd)
{
  stop();
  if (m_frameArrivedEvent) { CloseHandle(m_frameArrivedEvent); m_frameArrivedEvent = nullptr; }
  m_frameArrivedEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!m_frameArrivedEvent) { m_error = L"Failed to create WGC frame event"; return false; }
  m_frameCount = 0;
  m_fenceValue = 0;
  m_started = false;
  m_hwnd = hwnd;

  try {
    m_item = itemFromWindow(hwnd);
    auto itemSize = m_item.Size();
    if (itemSize.Width <= 0 || itemSize.Height <= 0) {
      m_error = L"Window reports zero size";
      return false;
    }
    m_windowSize = { (uint32_t)itemSize.Width, (uint32_t)itemSize.Height };
    // Compute the client-area crop rect for this window so we can exclude
    // the title bar / borders when scaling a windowed-mode target.
    recomputeClientArea(hwnd);
    if (m_size.w == 0 || m_size.h == 0) {
      m_error = L"Client area is zero"; return false;
    }
    if (!createOutputTexture(m_size)) return false;

    // Keep the four-buffer WGC pool, but use FrameArrived to wake the render
    // thread. The old empty callback plus SwitchToThread loop burned CPU while
    // waiting for the next game frame, competing with the game and GPU driver.
    m_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
      m_winrtDevice,
      DirectXPixelFormat::B8G8R8A8UIntNormalized,
      4,
      itemSize);

    m_arrived = m_pool.FrameArrived(winrt::auto_revoke,
      [this](auto&&, auto&&) {
        HANDLE event = m_frameArrivedEvent;
        if (event) SetEvent(event);
      });

    m_session = m_pool.CreateCaptureSession(m_item);

    if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
          winrt::name_of<GraphicsCaptureSession>(), L"IsCursorCaptureEnabled")) {
      m_session.IsCursorCaptureEnabled(false);
    }
    if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
          winrt::name_of<GraphicsCaptureSession>(), L"IsBorderRequired")) {
      m_session.IsBorderRequired(false);
    }
    // Win11 24H2: required for >60fps capture (Magpie does this)
    if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
          winrt::name_of<GraphicsCaptureSession>(), L"MinUpdateInterval")) {
      try {
        m_session.MinUpdateInterval(std::chrono::milliseconds(1));
      } catch (...) {}
    }

    m_session.StartCapture();
    m_started = true;
    return true;
  } catch (const winrt::hresult_error& e) {
    m_error = L"WGC start 0x" + hex(e.code()) + L" " + std::wstring(e.message());
    return false;
  } catch (...) {
    m_error = L"WGC start failed";
    return false;
  }
}

// Compute the client area of `hwnd` and store its offset inside the captured
// frame plus its size.  For a borderless fullscreen window the offset is
// (0,0) and the client size equals the window size, so the per-frame copy
// degrades to a full-frame CopySubresourceRegion (same as before).
void Capture::recomputeClientArea(HWND hwnd)
{
  PerMonitorDpiScope dpiScope;
  m_clientOffsetX = 0;
  m_clientOffsetY = 0;
  m_size = m_windowSize;
  if (!hwnd || !IsWindow(hwnd) || m_windowSize.w == 0 || m_windowSize.h == 0)
    return;

  RECT clientRect{}, frameRect{};
  if (!GetClientRect(hwnd, &clientRect))
    return;

  // GetWindowRect includes invisible resize borders on modern Windows. WGC
  // captures the visible window frame, so map the client area against DWM's
  // visible bounds instead of that larger, invisible rectangle.
  if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                   &frameRect, sizeof(frameRect)))) {
    if (!GetWindowRect(hwnd, &frameRect))
      return;
  }

  const int cw = std::max(1L, clientRect.right - clientRect.left);
  const int ch = std::max(1L, clientRect.bottom - clientRect.top);
  const int fw = std::max(1L, frameRect.right - frameRect.left);
  const int fh = std::max(1L, frameRect.bottom - frameRect.top);
  const double frameAspect = double(m_windowSize.w) / double(m_windowSize.h);
  const double clientAspect = double(cw) / double(ch);
  const double visibleFrameAspect = double(fw) / double(fh);

  // CreateForWindow can already return a client-only texture on some Windows
  // versions / window types. Only crop when the captured texture's aspect
  // matches the visible outer frame but not the client area; otherwise a
  // second crop trims actual game pixels along the bottom or sides.
  const bool matchesClient = std::abs(frameAspect / clientAspect - 1.0) < 0.005;
  const bool matchesVisibleFrame = std::abs(frameAspect / visibleFrameAspect - 1.0) < 0.005;
  if (matchesClient || !matchesVisibleFrame)
    return;

  POINT clientOrigin{0, 0};
  if (!ClientToScreen(hwnd, &clientOrigin))
    return;

  const double sx = double(m_windowSize.w) / double(fw);
  const double sy = double(m_windowSize.h) / double(fh);
  const int ox = clientOrigin.x - frameRect.left;
  const int oy = clientOrigin.y - frameRect.top;
  const int offsetX = (int)std::lround(ox * sx);
  const int offsetY = (int)std::lround(oy * sy);
  const int rightInset = (int)std::lround((frameRect.right -
    (clientOrigin.x + cw)) * sx);
  const int bottomInset = (int)std::lround((frameRect.bottom -
    (clientOrigin.y + ch)) * sy);

  m_clientOffsetX = (UINT)std::clamp(offsetX, 0, (int)m_windowSize.w - 1);
  m_clientOffsetY = (UINT)std::clamp(offsetY, 0, (int)m_windowSize.h - 1);
  const int clientW = (int)m_windowSize.w - (int)m_clientOffsetX -
    std::max(0, rightInset);
  const int clientH = (int)m_windowSize.h - (int)m_clientOffsetY -
    std::max(0, bottomInset);
  if (clientW > 0 && clientH > 0)
    m_size = {(uint32_t)clientW, (uint32_t)clientH};
}
void Capture::waitForFrame(DWORD timeoutMs) const
{
  if (m_dxgiMode) {
    // Desktop Duplication has no FrameArrived event. AcquireNextFrame uses
    // a short bounded wait; yield once here rather than spinning continuously
    // and competing with the game and graphics driver.
    SwitchToThread();
    return;
  }
  if (m_frameArrivedEvent)
    WaitForSingleObject(m_frameArrivedEvent, timeoutMs);
}

bool Capture::acquire(ComPtr<ID3D12Resource>& out, Size& size, uint64_t& fenceValue)
{
  if (!m_started) return false;

  if (m_dxgiMode) {
    if (!m_duplication) return false;
    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    ComPtr<IDXGIResource> desktopResource;
    HRESULT acquired = m_duplication->AcquireNextFrame(1, &frameInfo, &desktopResource);
    if (acquired == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (FAILED(acquired)) {
      m_error = L"DXGI AcquireNextFrame 0x" + hex(acquired);
      return false;
    }

    // DXGI Desktop Duplication has no cursor-capture toggle: depending on the
    // driver, the pointer may be baked into the desktop texture or overlaid
    // separately. If the visible system cursor overlaps the target region,
    // switch to WGC, which can explicitly exclude cursor pixels from capture.
    if (!m_dxgiCursorFallbackTried) {
      CURSORINFO cursorInfo{ sizeof(cursorInfo) };
      POINT physicalCursor{};
      const bool cursorInCapturedRegion =
        GetCursorInfo(&cursorInfo) && (cursorInfo.flags & CURSOR_SHOWING) && cursorInfo.hCursor &&
        GetPhysicalCursorPos(&physicalCursor) &&
        physicalCursor.x >= m_captureRect.left && physicalCursor.x < m_captureRect.right &&
        physicalCursor.y >= m_captureRect.top && physicalCursor.y < m_captureRect.bottom;
      if (cursorInCapturedRegion) {
        m_dxgiCursorFallbackTried = true;
        m_duplication->ReleaseFrame();
        HWND target = m_hwnd;
        if (startWgc(target))
          return false; // WGC's FrameArrived event will wake the next acquire.

        // If WGC cannot start for this target, keep capture alive in DXGI
        // rather than leaving the main loop waiting forever for frames.
        const std::wstring fallbackError = m_error;
        if (startDxgi(target))
          m_error = L"Cursor-safe WGC fallback failed; continuing with DXGI: " + fallbackError;
        return false;
      }
    }

    ComPtr<ID3D11Texture2D> src;
    HRESULT query = desktopResource.As(&src);
    if (FAILED(query)) {
      m_duplication->ReleaseFrame();
      m_error = L"DXGI desktop texture query 0x" + hex(query);
      return false;
    }

    D3D11_BOX srcBox{};
    srcBox.left = (UINT)(m_captureRect.left - m_monitorRect.left);
    srcBox.top = (UINT)(m_captureRect.top - m_monitorRect.top);
    srcBox.right = srcBox.left + m_size.w;
    srcBox.bottom = srcBox.top + m_size.h;
    srcBox.front = 0;
    srcBox.back = 1;
    m_ctx->CopySubresourceRegion(m_outD11.Get(), 0, 0, 0, 0, src.Get(), 0, &srcBox);
    m_duplication->ReleaseFrame();

    const uint64_t fv = ++m_fenceValue;
    bool d3d11FenceSignalled = false;
    if (m_fence11) {
      ComPtr<ID3D11DeviceContext4> ctx4;
      if (SUCCEEDED(m_ctx.As(&ctx4)) &&
          SUCCEEDED(ctx4->Signal(m_fence11.Get(), fv))) {
        d3d11FenceSignalled = true;
      }
    }
    if (!d3d11FenceSignalled) {
      if (!m_copyCompleteQuery) {
        m_error = L"Shared fence signal failed and no event-query fallback exists";
        return false;
      }
      m_ctx->End(m_copyCompleteQuery.Get());
      m_ctx->Flush();
      HRESULT queryResult = S_FALSE;
      while (queryResult == S_FALSE) {
        queryResult = m_ctx->GetData(m_copyCompleteQuery.Get(), nullptr, 0, 0);
        if (queryResult == S_FALSE) SwitchToThread();
      }
      if (FAILED(queryResult)) {
        m_error = L"D3D11 event-query synchronization failed";
        return false;
      }
      m_q->Signal(m_fence.Get(), fv);
    }

    out = m_outD12;
    size = m_size;
    fenceValue = fv;
    m_frameCount.fetch_add(1);
    return true;
  }

  if (!m_pool) return false;

  // Magpie _Update(): poll + drain to the newest frame
  Direct3D11CaptureFrame frame{ nullptr };
  try {
    frame = m_pool.TryGetNextFrame();
  } catch (...) {
    return false;
  }
  if (!frame) return false;

  try {
    while (true) {
      auto next = m_pool.TryGetNextFrame();
      if (!next) break;
      frame = std::move(next);
    }
  } catch (...) {}

  ComPtr<ID3D11Texture2D> src;
  try {
    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    if (FAILED(access->GetInterface(IID_PPV_ARGS(&src)))) return false;
  } catch (...) {
    return false;
  }

  D3D11_TEXTURE2D_DESC td{};
  src->GetDesc(&td);

  // td describes the captured frame's size, which is the FULL window size
  // (including title bar / borders).  We track that separately as
  // m_windowSize so we can detect when the window was resized.
  if (td.Width != m_windowSize.w || td.Height != m_windowSize.h) {
    // The captured window changed size; recalculate the client-area crop.
    m_windowSize = { (uint32_t)std::max(1u, td.Width), (uint32_t)std::max(1u, td.Height) };
    recomputeClientArea(m_hwnd);
    if (m_size.w == 0 || m_size.h == 0) return false;
    if (!createOutputTexture(m_size)) return false;
    try {
      m_pool.Recreate(m_winrtDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 4,
                      { (int32_t)m_windowSize.w, (int32_t)m_windowSize.h });
    } catch (...) {}
    // Drop this frame; next acquire uses new sizes.
    return false;
  }

  // Copy only the client-area portion of the captured frame into our output
  // texture.  For a borderless fullscreen window the offset is (0,0) and the
  // box covers the whole frame, so this is equivalent to the old
  // CopyResource path.  For a windowed-mode window this crops out the
  // title bar / borders so FSR only sees the game content.
  D3D11_BOX srcBox{};
  srcBox.left   = m_clientOffsetX;
  srcBox.top    = m_clientOffsetY;
  srcBox.right  = m_clientOffsetX + m_size.w;
  srcBox.bottom = m_clientOffsetY + m_size.h;
  srcBox.front  = 0;
  srcBox.back   = 1;
  // NULL src box == full resource.  When the client area covers the entire
  // captured frame (fullscreen), CopySubresourceRegion with an explicit box
  // that covers everything is equivalent to CopyResource — no perf penalty.
  if (m_clientOffsetX == 0 && m_clientOffsetY == 0 &&
      m_size.w == td.Width && m_size.h == td.Height) {
    // Preserve the fast full-resource copy path for borderless/full-frame
    // capture; only use a boxed copy when a real crop is needed.
    m_ctx->CopyResource(m_outD11.Get(), src.Get());
  } else {
    m_ctx->CopySubresourceRegion(m_outD11.Get(), 0, 0, 0, 0,
                                 src.Get(), 0, &srcBox);
  }

  // Publish the copy to D3D12. A shared D3D11 fence is the fast path.
  // The old fallback only called Flush() and then signalled the fence from
  // the D3D12 queue; Flush submits work but does NOT wait for its completion,
  // so D3D12 could read the still-black shared texture. Use a D3D11 event
  // query in that case and only release the D3D12 queue after the copy ends.
  const uint64_t fv = ++m_fenceValue;
  bool d3d11FenceSignalled = false;
  if (m_fence11) {
    ComPtr<ID3D11DeviceContext4> ctx4;
    if (SUCCEEDED(m_ctx.As(&ctx4)) &&
        SUCCEEDED(ctx4->Signal(m_fence11.Get(), fv))) {
      d3d11FenceSignalled = true;
    }
  }
  if (!d3d11FenceSignalled) {
    if (!m_copyCompleteQuery) {
      m_error = L"Shared fence signal failed and no event-query fallback exists";
      return false;
    }
    m_ctx->End(m_copyCompleteQuery.Get());
    m_ctx->Flush();
    HRESULT queryResult = S_FALSE;
    while (queryResult == S_FALSE) {
      queryResult = m_ctx->GetData(m_copyCompleteQuery.Get(), nullptr, 0, 0);
      if (queryResult == S_FALSE) SwitchToThread();
    }
    if (FAILED(queryResult)) {
      m_error = L"D3D11 event-query synchronization failed";
      return false;
    }
    // The D3D11 copy is now complete; this signal safely orders the caller's
    // subsequent queue wait without racing the independent D3D11 queue.
    m_q->Signal(m_fence.Get(), fv);
  }

  out = m_outD12;
  size = m_size;
  fenceValue = fv;
  m_frameCount.fetch_add(1);
  // frame destroyed here → returns buffer to the pool
  return true;
}

void Capture::stop()
{
  m_started = false;
  m_dxgiMode = false;
  m_duplication.Reset();
  m_arrived.revoke();
  try { if (m_session) m_session.Close(); } catch (...) {}
  m_session = nullptr;
  try { if (m_pool) m_pool.Close(); } catch (...) {}
  m_pool = nullptr;
  if (m_ctx) m_ctx->Flush();
  m_outD11.Reset();
  m_outD12.Reset();
  if (m_sharedHandle) { CloseHandle(m_sharedHandle); m_sharedHandle = nullptr; }
  m_item = nullptr;
  m_hwnd = nullptr;
  // Keep the init-owned event alive across stop/start cycles for focus resume.
  m_size = {};
  m_windowSize = {};
  m_clientOffsetX = 0;
  m_clientOffsetY = 0;
}

bool Capture::saveScreenshot(const std::wstring& folder, uint64_t frameIndex)
{
  if (!m_outD11 || !m_d11 || !m_ctx) return false;
  D3D11_TEXTURE2D_DESC td{}; m_outD11->GetDesc(&td);
  td.Usage=D3D11_USAGE_STAGING; td.BindFlags=0; td.CPUAccessFlags=D3D11_CPU_ACCESS_READ; td.MiscFlags=0;
  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(m_d11->CreateTexture2D(&td,nullptr,&staging))) return false;
  m_ctx->CopyResource(staging.Get(),m_outD11.Get());
  m_ctx->Flush();
  D3D11_MAPPED_SUBRESOURCE map{};
  if (FAILED(m_ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&map))) return false;
  std::wstring dir=folder;
  if(dir.empty()){ wchar_t p[MAX_PATH*4]{}; SHGetFolderPathW(nullptr,CSIDL_MYPICTURES,nullptr,SHGFP_TYPE_CURRENT,p); dir=p; if(!dir.empty()&&dir.back()!=L'\\')dir+=L'\\'; dir+=L"NRLive"; }
  CreateDirectoryW(dir.c_str(),nullptr);
  SYSTEMTIME st{}; GetLocalTime(&st); wchar_t name[128]{};
  swprintf_s(name,L"NRLive_%04u%02u%02u_%02u%02u%02u_%llu.png",st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond,(unsigned long long)frameIndex);
  std::wstring file=dir+L"\\"+name;
  HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  bool uninit=SUCCEEDED(hr);
  ComPtr<IWICImagingFactory> fac; ComPtr<IWICBitmap> bmp; ComPtr<IWICStream> stream; ComPtr<IWICBitmapEncoder> enc; ComPtr<IWICBitmapFrameEncode> frame;
  bool ok=false;
  do {
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&fac))))break;
    if(FAILED(fac->CreateBitmapFromMemory(td.Width,td.Height,GUID_WICPixelFormat32bppBGRA,map.RowPitch,td.Height*map.RowPitch,(BYTE*)map.pData,&bmp)))break;
    if(FAILED(fac->CreateStream(&stream)))break;
    if(FAILED(stream->InitializeFromFilename(file.c_str(),GENERIC_WRITE)))break;
    if(FAILED(fac->CreateEncoder(GUID_ContainerFormatPng,nullptr,&enc)))break;
    if(FAILED(enc->Initialize(stream.Get(),WICBitmapEncoderNoCache)))break;
    if(FAILED(enc->CreateNewFrame(&frame,nullptr)))break;
    if(FAILED(frame->Initialize(nullptr)))break;
    if(FAILED(frame->WriteSource(bmp.Get(),nullptr)))break;
    if(FAILED(frame->Commit()))break;
    if(FAILED(enc->Commit()))break;
    ok=true;
  } while(false);
  m_ctx->Unmap(staging.Get(),0);
  if(uninit) CoUninitialize();
  return ok;
}
