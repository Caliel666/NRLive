#include "gpu_timer.h"

bool GpuTimer::init(ID3D12Device* device, ID3D12CommandQueue* queue) {
  if (!device || !queue || FAILED(queue->GetTimestampFrequency(&m_freq)) || !m_freq) return false;
  D3D12_QUERY_HEAP_DESC qd{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kSlots * MarkCount, 0 };
  if (FAILED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&m_heap)))) return false;
  D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = sizeof(uint64_t) * kSlots * MarkCount;
  rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_readback))) ||
      FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence))) ||
      FAILED(m_readback->Map(0, nullptr, (void**)&m_mapped))) {
    m_heap.Reset();
    return false;
  }
  m_queue = queue;
  return true;
}

void GpuTimer::mark(ID3D12GraphicsCommandList* cmd, Mark m) {
  if (!cmd) return;
  if (m == Begin) {
    // Skip the frame when the slot's previous results are still in flight.
    m_recording = m_enabled && !m_slotPending[m_slot];
    if (m_recording) m_slotMarks[m_slot] = 0;
    else if (!m_enabled) m_latest = {};
  }
  if (!m_recording) return;
  cmd->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, m_slot * MarkCount + m);
  m_slotMarks[m_slot] |= 1u << m;
}

void GpuTimer::resolve(ID3D12GraphicsCommandList* cmd) {
  if (!m_recording || !cmd) return;
  cmd->ResolveQueryData(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, m_slot * MarkCount, MarkCount,
                        m_readback.Get(), sizeof(uint64_t) * m_slot * MarkCount);
}

void GpuTimer::frameDone() {
  if (!m_queue) return;
  if (m_recording) {
    m_queue->Signal(m_fence.Get(), ++m_fenceValue);
    m_slotFence[m_slot] = m_fenceValue;
    m_slotPending[m_slot] = true;
    m_slot = (m_slot + 1) % kSlots;
    m_recording = false;
  }
  const uint64_t done = m_fence->GetCompletedValue();
  for (uint32_t s = 0; s < kSlots; ++s) {
    if (!m_slotPending[s] || m_slotFence[s] > done) continue;
    m_slotPending[s] = false;
    const uint64_t* t = m_mapped + s * MarkCount;
    const uint32_t marks = m_slotMarks[s];
    auto has = [&](Mark m) { return (marks >> m) & 1u; };
    auto ms = [&](Mark a, Mark b) { return t[b] >= t[a] ? (float)((t[b] - t[a]) * 1000.0 / (double)m_freq) : -1.0f; };
    if (!has(Begin) || !has(End)) continue;
    Times r;
    Mark prev = Begin;
    if (has(Motion)) { r.motion = ms(Begin, Motion); prev = Motion; }
    if (has(Nr)) { r.nr = ms(prev, Nr); prev = Nr; }
    if (has(Fsr)) r.fsr = ms(prev, Fsr);
    r.total = ms(Begin, End);
    m_latest = r;
  }
}
