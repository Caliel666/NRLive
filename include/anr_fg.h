#pragma once

#include "graphics.h"

#include <string>
#include <memory>

struct AnrNgxParameterMap;

class AnrFrameGeneration {
public:
  AnrFrameGeneration() = default;
  ~AnrFrameGeneration();

  AnrFrameGeneration(const AnrFrameGeneration&) = delete;
  AnrFrameGeneration& operator=(const AnrFrameGeneration&) = delete;

  // Loads the ANR NGX provider from NRLive's own "anr" folder and initializes
  // it for the current D3D12 device. No files are ever resolved from the target game.
  bool init(ID3D12Device* device, Size render, Size display);
  bool resize(Size render, Size display);
  bool backbufferFormatMatches(ID3D12Resource* backbuffer) const;
  bool setBackbufferFormat(ID3D12Resource* backbuffer); // call after the GPU is idle

  // Feature creation is submitted and fenced separately before the first Evaluate.
  bool needsFeatureCreate() const { return m_needsCreate; }
  bool createFeature(ID3D12GraphicsCommandList* commandList);

  bool evaluate(ID3D12GraphicsCommandList* commandList,
                ID3D12Resource* backbuffer,
                ID3D12Resource* depth,
                ID3D12Resource* motion,
                bool reset);

  void shutdown();

  ID3D12Resource* generatedFrame() const { return m_generated.Get(); }
  ID3D12Resource* realFrame() const { return m_real.Get(); }
  Size renderSize() const { return m_render; }
  Size displaySize() const { return m_display; }
  bool ready() const { return m_initialized && m_handle != nullptr && !m_failed; }
  bool failed() const { return m_failed; }
  const std::wstring& lastError() const { return m_lastError; }

private:
  bool createOutputs();
  void updateCameraMatrices();
  void releaseFeature();
  bool fail(const std::wstring& message);

  using InitFn = int (__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
  using PopulateFn = int (__cdecl*)(void*);
  using CreateFn = int (__cdecl*)(ID3D12GraphicsCommandList*, int, void*, void**);
  using EvaluateFn = int (__cdecl*)(ID3D12GraphicsCommandList*, const void*, void*, void*);
  using ReleaseFn = int (__cdecl*)(void*);
  using ShutdownFn = int (__cdecl*)();
  using Shutdown1Fn = int (__cdecl*)(ID3D12Device*);

  HMODULE m_module = nullptr;
  ComPtr<ID3D12Device> m_device;
  InitFn m_init = nullptr;
  PopulateFn m_populate = nullptr;
  CreateFn m_create = nullptr;
  EvaluateFn m_evaluate = nullptr;
  ReleaseFn m_release = nullptr;
  ShutdownFn m_shutdown = nullptr;
  Shutdown1Fn m_shutdown1 = nullptr;
  std::unique_ptr<AnrNgxParameterMap> m_parameters;
  void* m_handle = nullptr;

  ComPtr<ID3D12Resource> m_generated;
  ComPtr<ID3D12Resource> m_real;
  D3D12_RESOURCE_STATES m_outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  Size m_render{};
  Size m_display{};
  DXGI_FORMAT m_format = DXGI_FORMAT_R8G8B8A8_UNORM;
  unsigned long long m_frameId = 0;
  // NGX matrix parameters may be consumed by queued command lists. Keep them
  // in object storage and update only at init/resize while the GPU is idle.
  float m_viewToClip[16]{};
  float m_clipToView[16]{};
  bool m_initialized = false;
  bool m_needsCreate = false;
  bool m_failed = false;
  std::wstring m_lastError;
};
