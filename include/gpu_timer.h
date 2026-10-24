#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <cstdint>

// D3D12 timestamp queries around NRLive's own passes, read back a few frames
// later through a fence so the CPU never waits. Off unless the HUD asks.
class GpuTimer {
public:
  enum Mark { Begin, Motion, Nr, Fsr, End, MarkCount };
  struct Times { float motion = -1, nr = -1, fsr = -1, total = -1; }; // ms, <0 = not run

  bool init(ID3D12Device* device, ID3D12CommandQueue* queue);
  void setEnabled(bool enabled) { m_enabled = enabled && m_heap; }
  void mark(ID3D12GraphicsCommandList* cmd, Mark m);
  void resolve(ID3D12GraphicsCommandList* cmd); // after the End mark, on the last list of the frame
  void frameDone();                             // after the frame's last ExecuteCommandLists
  const Times& latest() const { return m_latest; }

private:
  static constexpr uint32_t kSlots = 4;
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_heap;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_readback;
  Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
  ID3D12CommandQueue* m_queue = nullptr;
  uint64_t* m_mapped = nullptr;
  uint64_t m_freq = 0;
  uint64_t m_fenceValue = 0;
  uint64_t m_slotFence[kSlots]{};
  uint32_t m_slotMarks[kSlots]{};
  bool m_slotPending[kSlots]{};
  uint32_t m_slot = 0;
  bool m_enabled = false;
  bool m_recording = false;
  Times m_latest{};
};
