#include "graphics.h"
#include <d3dcompiler.h>
#include <wincodec.h>
#include <shlobj.h>
#include <filesystem>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <vector>

static void hr(HRESULT x)
{
  if (FAILED(x)) {
    char message[64]{};
    sprintf_s(message, "D3D12 failure HRESULT=0x%08lX", static_cast<unsigned long>(x));
    throw std::runtime_error(message);
  }
}

// Fullscreen triangle, sample texture, write to RTV. Handles any src size.
static const char* kBlitHlsl = R"(
Texture2D    Tex : register(t0);
SamplerState Samp : register(s0);

struct VSOut {
  float4 pos : SV_Position;
  float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
  VSOut o;
  o.uv  = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}

float4 PSMain(VSOut i) : SV_Target {
  return Tex.SampleLevel(Samp, i.uv, 0);
}
)";

bool Graphics::createBlitPipeline()
{
  ComPtr<ID3DBlob> vs, ps, err, rsBlob;
  UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
  if (FAILED(D3DCompile(kBlitHlsl, strlen(kBlitHlsl), nullptr, nullptr, nullptr,
                        "VSMain", "vs_5_0", flags, 0, &vs, &err)))
    return false;
  if (FAILED(D3DCompile(kBlitHlsl, strlen(kBlitHlsl), nullptr, nullptr, nullptr,
                        "PSMain", "ps_5_0", flags, 0, &ps, &err)))
    return false;

  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  range.BaseShaderRegister = 0;

  D3D12_ROOT_PARAMETER param{};
  param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  param.DescriptorTable.NumDescriptorRanges = 1;
  param.DescriptorTable.pDescriptorRanges = &range;
  param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_STATIC_SAMPLER_DESC samp{};
  samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samp.ShaderRegister = 0;
  samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC rsd{};
  rsd.NumParameters = 1;
  rsd.pParameters = &param;
  rsd.NumStaticSamplers = 1;
  rsd.pStaticSamplers = &samp;
  rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  hr(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, nullptr));
  hr(m_dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                IID_PPV_ARGS(&m_blitRs)));

  D3D12_GRAPHICS_PIPELINE_STATE_DESC psd{};
  psd.pRootSignature = m_blitRs.Get();
  psd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
  psd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
  psd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0xF;
  psd.SampleMask = UINT_MAX;
  psd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  psd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  psd.DepthStencilState.DepthEnable = FALSE;
  psd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  psd.NumRenderTargets = 1;
  psd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  psd.SampleDesc.Count = 1;
  hr(m_dev->CreateGraphicsPipelineState(&psd, IID_PPV_ARGS(&m_blitPso)));

  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 8;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  hr(m_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_srvHeap)));
  m_srvStride = m_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  return true;
}

ID3D12Resource* Graphics::prepareAnrInput(ID3D12Resource* src)
{
  if (!src || !m_dev || !cmd() || !m_blitPso || !m_srvHeap)
    return nullptr;

  const D3D12_RESOURCE_DESC srcDesc = src->GetDesc();
  if (srcDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      !srcDesc.Width || !srcDesc.Height || srcDesc.SampleDesc.Count != 1)
    return nullptr;

  // NGX's DLSS-G input and outputs use a single matching display-size format.
  // FSR's output is already RGBA8 at display resolution, so it can be consumed
  // directly. Capture surfaces are BGRA8 and may be render-size, so normalize
  // them through this fullscreen blit before passing them to ANR.
  if (srcDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
      srcDesc.Width == m_display.w && srcDesc.Height == m_display.h)
    return src;

  if (!m_anrInput || m_anrInput->GetDesc().Width != m_display.w ||
      m_anrInput->GetDesc().Height != m_display.h) {
    m_anrInput.Reset();
    m_anrInputRtvHeap.Reset();
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_display.w;
    desc.Height = m_display.h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    if (FAILED(m_dev->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(&m_anrInput))))
      return nullptr;

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = 1;
    if (FAILED(m_dev->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_anrInputRtvHeap)))) {
      m_anrInput.Reset();
      return nullptr;
    }
    m_dev->CreateRenderTargetView(m_anrInput.Get(), nullptr,
                                  m_anrInputRtvHeap->GetCPUDescriptorHandleForHeapStart());
    m_anrInputState = D3D12_RESOURCE_STATE_COMMON;
  }

  // Reuse the normal blit's first SRV descriptor.
  const UINT inputSrvSlot = 3 + m_frameSlot;
  if (src != m_lastBlitSrc[inputSrvSlot] || srcDesc.Format != m_lastBlitFmt[inputSrvSlot]) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = srcDesc.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(inputSrvSlot) * m_srvStride;
    m_dev->CreateShaderResourceView(src, &srv, cpu);
    m_lastBlitSrc[inputSrvSlot] = src;
    m_lastBlitFmt[inputSrvSlot] = srcDesc.Format;
  }

  D3D12_RESOURCE_BARRIER toTarget{};
  toTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  toTarget.Transition.pResource = m_anrInput.Get();
  toTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  toTarget.Transition.StateBefore = m_anrInputState;
  toTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  cmd()->ResourceBarrier(1, &toTarget);

  ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
  cmd()->SetDescriptorHeaps(1, heaps);
  cmd()->SetGraphicsRootSignature(m_blitRs.Get());
  cmd()->SetPipelineState(m_blitPso.Get());
  D3D12_GPU_DESCRIPTOR_HANDLE inputGpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
  inputGpu.ptr += static_cast<UINT64>(inputSrvSlot) * m_srvStride;
  cmd()->SetGraphicsRootDescriptorTable(0, inputGpu);

  D3D12_VIEWPORT vp{ 0, 0, static_cast<float>(m_display.w), static_cast<float>(m_display.h), 0, 1 };
  D3D12_RECT sc{ 0, 0, static_cast<LONG>(m_display.w), static_cast<LONG>(m_display.h) };
  cmd()->RSSetViewports(1, &vp);
  cmd()->RSSetScissorRects(1, &sc);
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_anrInputRtvHeap->GetCPUDescriptorHandleForHeapStart();
  cmd()->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  cmd()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd()->DrawInstanced(3, 1, 0, 0);

  D3D12_RESOURCE_BARRIER toShaderRead{};
  toShaderRead.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  toShaderRead.Transition.pResource = m_anrInput.Get();
  toShaderRead.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  toShaderRead.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  toShaderRead.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  cmd()->ResourceBarrier(1, &toShaderRead);
  m_anrInputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  return m_anrInput.Get();
}

void Graphics::blitToBackbuffer(ID3D12Resource* src, UINT srvSlot)
{
  if (!src || !m_blitPso || srvSlot >= 8) return;

  // Keep the normal FSR path on slot 0. ANR's generated and real frame blits
  // use dedicated slots because both lists may be in flight simultaneously.
  D3D12_RESOURCE_DESC srcDesc = src->GetDesc();
  if (src != m_lastBlitSrc[srvSlot] || srcDesc.Format != m_lastBlitFmt[srvSlot]) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = srcDesc.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(srvSlot) * m_srvStride;
    m_dev->CreateShaderResourceView(src, &srv, cpu);
    m_lastBlitSrc[srvSlot] = src;
    m_lastBlitFmt[srvSlot] = srcDesc.Format;
  }

  ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
  cmd()->SetDescriptorHeaps(1, heaps);
  cmd()->SetGraphicsRootSignature(m_blitRs.Get());
  cmd()->SetPipelineState(m_blitPso.Get());
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += static_cast<UINT64>(srvSlot) * m_srvStride;
  cmd()->SetGraphicsRootDescriptorTable(0, gpu);

  D3D12_VIEWPORT vp{ 0, 0, (float)m_display.w, (float)m_display.h, 0, 1 };
  D3D12_RECT sc{ 0, 0, (LONG)m_display.w, (LONG)m_display.h };
  cmd()->RSSetViewports(1, &vp);
  cmd()->RSSetScissorRects(1, &sc);

  auto rtv = rtvHandle();
  cmd()->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  cmd()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd()->DrawInstanced(3, 1, 0, 0);
}

bool Graphics::createAuxTextures(Size render)
{
  if (render.w == 0 || render.h == 0) return false;
  m_render = render;
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  hp.CreationNodeMask = 1;
  hp.VisibleNodeMask = 1;

  m_dummyDepth.Reset();
  {
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = render.w; rd.Height = render.h;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr(m_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_dummyDepth)));
  }
  m_motionVectors.Reset();
  {
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = render.w; rd.Height = render.h;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R16G16_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr(m_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_motionVectors)));
  }
  {
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = render.w; rd.Height = render.h;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr(m_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_reactiveMask)));
  }

  if (!m_clearGpuHeap) {
    D3D12_DESCRIPTOR_HEAP_DESC gpu{};
    gpu.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    gpu.NumDescriptors = 1;
    gpu.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr(m_dev->CreateDescriptorHeap(&gpu, IID_PPV_ARGS(&m_clearGpuHeap)));

    D3D12_DESCRIPTOR_HEAP_DESC cpu = gpu;
    cpu.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr(m_dev->CreateDescriptorHeap(&cpu, IID_PPV_ARGS(&m_clearCpuHeap)));
  }

  D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
  uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  uv.Format = DXGI_FORMAT_R32_FLOAT;
  m_dev->CreateUnorderedAccessView(m_dummyDepth.Get(), nullptr, &uv,
                                   m_clearGpuHeap->GetCPUDescriptorHandleForHeapStart());
  m_dev->CreateUnorderedAccessView(m_dummyDepth.Get(), nullptr, &uv,
                                   m_clearCpuHeap->GetCPUDescriptorHandleForHeapStart());

  return true;
}

bool Graphics::ensureAuxTextures(Size render)
{
  if (m_dummyDepth && m_motionVectors && m_render.w == render.w && m_render.h == render.h)
    return true;
  return createAuxTextures(render);
}

Graphics::~Graphics()
{
  closeFrameLatencyHandle();
}

void Graphics::closeFrameLatencyHandle()
{
  if (m_frameLatencyWaitableObject) {
    CloseHandle(m_frameLatencyWaitableObject);
    m_frameLatencyWaitableObject = nullptr;
  }
}

bool Graphics::buildSwapChain(Size display)
{
  DXGI_SWAP_CHAIN_DESC1 s{};
  s.Width = display.w; s.Height = display.h;
  s.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  // FSR's interpolation swapchain recommends two application backbuffers; it
  // maintains its own real presentation swapchain internally. Three here adds
  // latency and can make wrapping less compatible with providers.
  s.BufferCount = 2;
  s.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  s.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  s.SampleDesc.Count = 1;
  s.Scaling = DXGI_SCALING_STRETCH;
  s.Flags = m_framePacingEnabled ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0;

  ComPtr<IDXGISwapChain1> sw;
  hr(m_factory->CreateSwapChainForHwnd(m_queue.Get(), m_output, &s, nullptr, nullptr, &sw));
  m_swap1 = sw;
  hr(m_swap1.As(&m_swap));
  closeFrameLatencyHandle();
  if (m_framePacingEnabled) {
    ComPtr<IDXGISwapChain2> latencySwap;
    if (SUCCEEDED(m_swap.As(&latencySwap)) &&
        SUCCEEDED(latencySwap->SetMaximumFrameLatency(1))) {
      m_frameLatencyWaitableObject = latencySwap->GetFrameLatencyWaitableObject();
    }
  }
  m_factory->MakeWindowAssociation(m_output, DXGI_MWA_NO_ALT_ENTER);

  m_display = display;
  m_index = m_swap->GetCurrentBackBufferIndex();

  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.NumDescriptors = 3;
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  if (!m_rtv) hr(m_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_rtv)));
  m_rtvStride = m_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  m_rtvBase = m_rtv->GetCPUDescriptorHandleForHeapStart();

  for (int i = 0; i < 2; i++) {
    m_back[i].Reset();
    hr(m_swap->GetBuffer(i, IID_PPV_ARGS(&m_back[i])));
    auto h = m_rtvBase; h.ptr += (SIZE_T)i * m_rtvStride;
    m_dev->CreateRenderTargetView(m_back[i].Get(), nullptr, h);
    if (!m_alloc[i])
      hr(m_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_alloc[i])));
    if (!m_allocContinuation[i])
      hr(m_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_allocContinuation[i])));
  }
  // Allocator slots are independent of the swapchain's two backbuffers.
  // Three slots let the CPU queue frames without resetting an allocator that
  // the GPU may still be using.
  for (int i = 2; i < 3; ++i) {
    if (!m_alloc[i])
      hr(m_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_alloc[i])));
    if (!m_allocContinuation[i])
      hr(m_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_allocContinuation[i])));
  }

  m_upscaleOutput.Reset();
  {
    D3D12_RESOURCE_DESC od{};
    od.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    od.Width = display.w; od.Height = display.h;
    od.DepthOrArraySize = 1; od.MipLevels = 1;
    od.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    od.SampleDesc.Count = 1;
    od.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES ohp{};
    ohp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ohp.CreationNodeMask = 1; ohp.VisibleNodeMask = 1;
    hr(m_dev->CreateCommittedResource(&ohp, D3D12_HEAP_FLAG_NONE, &od,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_upscaleOutput)));
  }
  return true;
}

void Graphics::releaseSwapChainForWrap()
{
  closeFrameLatencyHandle();
  // AMD's frame-interpolation wrapper releases the original swapchain and
  // recreates it for the same HWND. DXGI requires every backbuffer reference
  // and every app-owned swapchain interface to be released first.
  for (auto& b : m_back) b.Reset();
  m_swap1.Reset();
  m_swap.Reset();
  m_index = 0;
  for (UINT i = 0; i < 8; ++i) {
    m_lastBlitSrc[i] = nullptr;
    m_lastBlitFmt[i] = DXGI_FORMAT_UNKNOWN;
  }
}

bool Graphics::rebuildSwapChain(Size display)
{
  releaseSwapChainForWrap();
  return buildSwapChain(display);
}

bool Graphics::enableFramePacing()
{
  if (m_framePacingEnabled && m_frameLatencyWaitableObject)
    return true;
  if (!m_swap || !m_factory || !m_queue || !waitForGpu())
    return false;

  m_framePacingEnabled = true;
  releaseSwapChainForWrap();
  if (!buildSwapChain(m_display)) {
    m_framePacingEnabled = false;
    return false;
  }
  if (m_frameLatencyWaitableObject)
    return true;

  // Older drivers may expose IDXGISwapChain2 but not the waitable handle.
  // Rebuild a normal chain and keep ANR functional without the pacing aid.
  m_framePacingEnabled = false;
  releaseSwapChainForWrap();
  buildSwapChain(m_display);
  return false;
}

bool Graphics::adoptSwapChain(IDXGISwapChain4* wrapped)
{
  if (!wrapped || !m_rtv || !m_dev) return false;
  for (auto& b : m_back) b.Reset();
  m_swap1.Reset();
  m_swap.Reset();
  m_swap = wrapped;
  if (FAILED(m_swap.As(&m_swap1))) return false;
  m_index = m_swap->GetCurrentBackBufferIndex();
  for (UINT i = 0; i < 2; ++i) {
    if (FAILED(m_swap->GetBuffer(i, IID_PPV_ARGS(&m_back[i]))))
      return false;
    auto h = m_rtvBase;
    h.ptr += (SIZE_T)i * m_rtvStride;
    m_dev->CreateRenderTargetView(m_back[i].Get(), nullptr, h);
  }
  return true;
}

bool Graphics::resize(Size display)
{
  if (display.w == m_display.w && display.h == m_display.h) return true;
  closeFrameLatencyHandle();
  for (auto& b : m_back) b.Reset();
  m_upscaleOutput.Reset();
  m_swap1.Reset(); m_swap.Reset();
  for (UINT i = 0; i < 8; ++i) {
    m_lastBlitSrc[i] = nullptr;
    m_lastBlitFmt[i] = DXGI_FORMAT_UNKNOWN;
  } // cached SRVs are stale after swapchain rebuild
  return buildSwapChain(display);
}

bool Graphics::init(HWND output, Size render, Size display)
{
  m_output = output;
  ComPtr<IDXGIFactory6> f;
  hr(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)));
  m_factory = f;

  ComPtr<IDXGIAdapter1> a;
  for (UINT i = 0;
       f->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)) != DXGI_ERROR_NOT_FOUND;
       ++i) {
    DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
    if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
    a.Reset();
  }
  hr(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_dev)));

  D3D12_COMMAND_QUEUE_DESC q{};
  q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  hr(m_dev->CreateCommandQueue(&q, IID_PPV_ARGS(&m_queue)));

  buildSwapChain(display);
  createAuxTextures(render.w ? render : display);
  createBlitPipeline();

  hr(m_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc[0].Get(),
                              nullptr, IID_PPV_ARGS(&m_cmd)));
  hr(cmd()->Close());
  hr(m_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocContinuation[0].Get(),
                              nullptr, IID_PPV_ARGS(&m_cmdContinuation)));
  hr(m_cmdContinuation->Close());
  hr(m_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
  m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  return true;
}

void Graphics::waitForFrameLatency()
{
  if (!m_frameLatencyWaitableObject)
    return;
  const DWORD waitResult = WaitForSingleObject(m_frameLatencyWaitableObject, 1000);
  if (waitResult == WAIT_FAILED) {
    closeFrameLatencyHandle();
    m_framePacingEnabled = false;
  }
}

bool Graphics::begin(bool clearSyntheticDepth)
{
  // Wait only when this CPU command-allocator slot is about to be reused.
  // Do not key allocator lifetime to the wrapped swapchain's backbuffer index:
  // frame interpolation can advance presentation independently of base frames.
  const uint64_t slotFence = m_frameSlotFence[m_frameSlot];
  if (slotFence && m_fence->GetCompletedValue() < slotFence) {
    m_fence->SetEventOnCompletion(slotFence, m_fenceEvent);
    WaitForSingleObject(m_fenceEvent, INFINITE);
  }
  m_interopContinuation = false;
  m_alloc[m_frameSlot]->Reset();
  cmd()->Reset(m_alloc[m_frameSlot].Get(), nullptr);

  // The capture pipeline has no real game depth. Always clear the synthetic
  // depth to deterministic far depth before FSR sees it; an uninitialized
  // UAV here was a major source of temporal instability/smearing.
  if (clearSyntheticDepth && m_dummyDepth && m_clearGpuHeap && m_clearCpuHeap) {
    ID3D12DescriptorHeap* heaps[] = { m_clearGpuHeap.Get() };
    cmd()->SetDescriptorHeaps(1, heaps);
    const FLOAT value[4] = { 0.f, 0.f, 0.f, 0.f };
    cmd()->ClearUnorderedAccessViewFloat(
      m_clearGpuHeap->GetGPUDescriptorHandleForHeapStart(),
      m_clearCpuHeap->GetCPUDescriptorHandleForHeapStart(),
      m_dummyDepth.Get(), value, 0, nullptr);
  }
  return true;
}

bool Graphics::submitForInterop()
{
  if (m_interopContinuation || !m_cmd || !m_cmdContinuation || !m_queue)
    return false;
  if (FAILED(cmd()->Close()))
    return false;
  ID3D12CommandList* lists[] = { cmd() };
  m_queue->ExecuteCommandLists(1, lists);

  // The second allocator/list pair records the post-NR FSR work. The queue
  // bridge inserts its own GPU fence wait; there is no CPU wait in this path.
  if (FAILED(m_allocContinuation[m_frameSlot]->Reset()))
    return false;
  if (FAILED(m_cmdContinuation->Reset(m_allocContinuation[m_frameSlot].Get(), nullptr)))
    return false;
  m_interopContinuation = true;
  return true;
}

ID3D12Resource* Graphics::snapshotDlssNrInput(ID3D12Resource* colour, D3D12_RESOURCE_STATES colourState)
{
  if (!colour || !m_dev || !cmd()) return nullptr;
  const auto desc = colour->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width == 0 || desc.Height == 0)
    return nullptr;
  if (!m_dlssNrInput || m_dlssNrInput->GetDesc().Width != desc.Width ||
      m_dlssNrInput->GetDesc().Height != desc.Height ||
      m_dlssNrInput->GetDesc().Format != desc.Format) {
    if (m_dlssNrInput) m_retiredDlssNrInputs.push_back(std::move(m_dlssNrInput));
    auto snapshotDesc = desc;
    snapshotDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    snapshotDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(m_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &snapshotDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_dlssNrInput))))
      return nullptr;
    m_dlssNrInputState = D3D12_RESOURCE_STATE_COMMON;
  }
  D3D12_RESOURCE_BARRIER before[2]{};
  before[0].Type = before[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  before[0].Transition = { colour, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, colourState, D3D12_RESOURCE_STATE_COPY_SOURCE };
  before[1].Transition = { m_dlssNrInput.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, m_dlssNrInputState, D3D12_RESOURCE_STATE_COPY_DEST };
  cmd()->ResourceBarrier(2, before);
  cmd()->CopyResource(m_dlssNrInput.Get(), colour);
  D3D12_RESOURCE_BARRIER after[2]{};
  after[0].Type = after[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  after[0].Transition = { colour, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, colourState };
  after[1].Transition = { m_dlssNrInput.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
  cmd()->ResourceBarrier(2, after);
  m_dlssNrInputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  return m_dlssNrInput.Get();
}

ID3D12Resource* Graphics::ensureDlssNrOutput(ID3D12Resource* colour)
{
  if (!colour || !m_dev) return nullptr;
  const auto desc = colour->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      desc.Width == 0 || desc.Height == 0 || desc.SampleDesc.Count != 1)
    return nullptr;
  if (m_dlssNrOutput) {
    const auto current = m_dlssNrOutput->GetDesc();
    if (current.Width == desc.Width && current.Height == desc.Height &&
        current.Format == desc.Format)
      return m_dlssNrOutput.Get();
    m_retiredDlssNrOutputs.push_back(std::move(m_dlssNrOutput));
  }
  auto outputDesc = desc;
  outputDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
  outputDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  hp.CreationNodeMask = 1;
  hp.VisibleNodeMask = 1;
  if (FAILED(m_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE,
      &outputDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
      IID_PPV_ARGS(&m_dlssNrOutput))))
    return nullptr;
  return m_dlssNrOutput.Get();
}

void Graphics::end() { cmd()->Close(); }

bool Graphics::captureBackbufferScreenshot(const std::wstring& folder, uint64_t frameIndex)
{
  if (!m_cmd || !m_back[m_index] || m_screenshotPending)
    return false;

  ID3D12Resource* back = m_back[m_index].Get();
  const D3D12_RESOURCE_DESC desc = back->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      desc.Width == 0 || desc.Height == 0)
    return false;

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT numRows = 0;
  UINT64 rowSize = 0;
  UINT64 totalBytes = 0;
  m_dev->GetCopyableFootprints(&desc, 0, 1, 0,
                               &footprint, &numRows, &rowSize, &totalBytes);
  if (totalBytes == 0 || numRows == 0)
    return false;

  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = totalBytes;
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  ComPtr<ID3D12Resource> readback;
  if (FAILED(m_dev->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&readback))))
    return false;

  D3D12_RESOURCE_BARRIER toCopy{};
  toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  toCopy.Transition.pResource = back;
  toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  cmd()->ResourceBarrier(1, &toCopy);

  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = back;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;

  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = footprint;

  cmd()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

  D3D12_RESOURCE_BARRIER toRender{};
  toRender.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  toRender.Transition.pResource = back;
  toRender.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  toRender.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  toRender.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  cmd()->ResourceBarrier(1, &toRender);

  m_screenshotReadback = readback;
  m_screenshotFootprint = footprint;
  m_screenshotWidth = (UINT)desc.Width;
  m_screenshotHeight = (UINT)desc.Height;
  m_screenshotFolder = folder;
  m_screenshotFrame = frameIndex;
  m_screenshotPending = true;
  return true;
}

static bool writeScreenshotPng(const std::wstring& folder,
                               uint64_t frameIndex,
                               UINT width,
                               UINT height,
                               UINT rowPitch,
                               const BYTE* pixels)
{
  if (!pixels || width == 0 || height == 0)
    return false;

  std::wstring dir = folder;
  if (dir.empty()) {
    wchar_t p[MAX_PATH * 4]{};
    SHGetFolderPathW(nullptr, CSIDL_MYPICTURES, nullptr, SHGFP_TYPE_CURRENT, p);
    dir = p;
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    dir += L"NRLive";
  }

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec)
    return false;

  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t name[128]{};
  swprintf_s(name, L"NRLive_%04u%02u%02u_%02u%02u%02u_%llu.png",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond,
             (unsigned long long)frameIndex);
  const std::wstring file = dir + L"\\" + name;

  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool uninit = SUCCEEDED(hr);

  ComPtr<IWICImagingFactory> fac;
  ComPtr<IWICBitmap> bmp;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> enc;
  ComPtr<IWICBitmapFrameEncode> frame;

  // The presentation backbuffer can contain undefined/zero alpha after FSR,
  // even though the composed image is visibly opaque. Normalize to tightly
  // packed RGBA and force alpha to 255 so dark pixels never become transparent
  // in PNG viewers/editors. Copy row-by-row because D3D12 readback rows are
  // padded to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT.
  const UINT packedRowPitch = width * 4;
  if (rowPitch < packedRowPitch) {
    if (uninit) CoUninitialize();
    return false;
  }
  std::vector<BYTE> opaquePixels((size_t)packedRowPitch * height);
  for (UINT y = 0; y < height; ++y) {
    BYTE* dst = opaquePixels.data() + (size_t)y * packedRowPitch;
    const BYTE* src = pixels + (size_t)y * rowPitch;
    std::memcpy(dst, src, packedRowPitch);
    for (UINT x = 0; x < width; ++x)
      dst[(size_t)x * 4 + 3] = 255;
  }

  bool ok = false;
  do {
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&fac)))) break;
    if (FAILED(fac->CreateBitmapFromMemory(
          width, height, GUID_WICPixelFormat32bppRGBA,
          packedRowPitch, (UINT)opaquePixels.size(),
          opaquePixels.data(), &bmp))) break;
    if (FAILED(fac->CreateStream(&stream))) break;
    if (FAILED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE))) break;
    if (FAILED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) break;
    if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) break;
    if (FAILED(enc->CreateNewFrame(&frame, nullptr))) break;
    if (FAILED(frame->Initialize(nullptr))) break;
    if (FAILED(frame->WriteSource(bmp.Get(), nullptr))) break;
    if (FAILED(frame->Commit())) break;
    if (FAILED(enc->Commit())) break;
    ok = true;
  } while (false);

  if (uninit)
    CoUninitialize();
  return ok;
}

bool Graphics::present()
{
  // submitForInterop() may have already submitted m_cmd and continued the
  // frame on m_cmdContinuation. Present must execute the currently active
  // list, not replay the already-submitted original list.
  ID3D12CommandList* lists[] = { cmd() };
  m_queue->ExecuteCommandLists(1, lists);

  // Fence the allocator slot BEFORE Present. FSR FG submits generation work
  // inside Present on the same queue; that work does not use m_alloc[slot].
  // Signaling after Present made begin() wait on FG completion and halved
  // base-frame rate under frame generation.
  const uint64_t v = ++m_fenceValue;
  m_queue->Signal(m_fence.Get(), v);
  m_frameSlotFence[m_frameSlot] = v;

  // Sync interval 0 = no vsync wait on CPU.
  m_swap->Present(0, 0);

  // A screenshot is synchronized only on the frame that requested it.
  // This guarantees the PNG contains the post-FSR backbuffer while normal
  // frames retain the existing buffered, non-blocking path.
  if (m_screenshotPending) {
    if (m_fence->GetCompletedValue() < v) {
      m_fence->SetEventOnCompletion(v, m_fenceEvent);
      WaitForSingleObject(m_fenceEvent, 1000);
    }

    if (m_fence->GetCompletedValue() >= v && m_screenshotReadback) {
      void* mapped = nullptr;
      D3D12_RANGE range{0, (SIZE_T)(m_screenshotFootprint.Footprint.RowPitch * m_screenshotHeight)};
      if (SUCCEEDED(m_screenshotReadback->Map(0, &range, &mapped)) && mapped) {
        const BYTE* base = static_cast<const BYTE*>(mapped) + m_screenshotFootprint.Offset;
        writeScreenshotPng(m_screenshotFolder, m_screenshotFrame,
                           m_screenshotWidth, m_screenshotHeight,
                           m_screenshotFootprint.Footprint.RowPitch, base);
        D3D12_RANGE written{0, 0};
        m_screenshotReadback->Unmap(0, &written);
      }
    }

    m_screenshotReadback.Reset();
    m_screenshotPending = false;
    m_screenshotFolder.clear();
    m_screenshotFrame = 0;
  }

  m_index = m_swap->GetCurrentBackBufferIndex();
  // The D3D12 queue serializes work that targets the same backbuffer. DXGI
  // chooses the next available buffer; do not CPU-wait here for the prior
  // buffer fence, which throttles base-frame submission under FSR FG.
  m_frameSlot = (m_frameSlot + 1) % 3;
  m_interopContinuation = false;
  return true;
}

bool Graphics::waitForGpu()
{
  // Close any in-flight command list, execute it, then CPU-wait on a fresh
  // fence value so every previously submitted command has completed on the
  // GPU before the caller starts releasing D3D12 resources.
  if (!m_queue || !m_fence) return false;

  // If the command list is open, close + execute it so the GPU sees it.
  if (cmd()) {
    HRESULT hrClose = cmd()->Close();
    if (SUCCEEDED(hrClose)) {
      ID3D12CommandList* lists[] = { cmd() };
      m_queue->ExecuteCommandLists(1, lists);
    }
  }

  const uint64_t v = ++m_fenceValue;
  m_queue->Signal(m_fence.Get(), v);
  if (m_fence->GetCompletedValue() < v) {
    if (m_fenceEvent) {
      m_fence->SetEventOnCompletion(v, m_fenceEvent);
      WaitForSingleObject(m_fenceEvent, 5000);  // 5s safety timeout
    }
  }
  return true;
}
