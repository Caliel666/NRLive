#pragma once
#include "graphics.h"
#include <cstdint>
#include <filesystem>
#include <string>

// DLSSNR through OpenNR's MochizukiNrRuntime.dll (mochizuki's Vulkan runtime with
// the RenoDX composition stage, ACO, int4 and the RX 7000 network), loaded at run
// time through its C ABI (third_party/opennr). It runs on NRLive's own D3D12
// queue between the motion pass and the upscaler, as OptiScaler runs it before
// Super Resolution. Missing files or a failed frame leave the captured colour
// unchanged.

// Which DLSSNR runtime NRLive uses ([DLSSNR] runtime in scaleconfig.ini).
enum class NrRuntimeChoice { Auto, OpenNr, Bridge };

// [OpenNR] in scaleconfig.ini: OptiScaler's [DlssNr] Mochizuki* key names.
struct OpenNrSettings {
  bool originalModel = false; // MochizukiOriginalModel: 310.8 instead of 310.9.16 Small
  bool int4 = false;          // MochizukiInt4: needs ACO and dlssnr-amd\dlssnr-int4.bin
  bool aco = true;            // MochizukiAco: decided once per process
  bool temporal = true;       // MochizukiTemporal: history along NRLive's motion vectors
  float historyStrength = 1.0f;
  bool styleGrade = true;
  bool cascadeDetail = true;
  bool antiHalo = true;
  float antiHaloStrength = 0.8f;
  bool passEstimate = true;
  // The RenoDX stage's keys (MochizukiRenoKeys.inc), by key name.
#define MZR_F(name, def, lo, hi, group, label, member, help) float name = def;
#define MZR_U(name, def, lo, hi, group, label, member, help) uint32_t name = def;
#define MZR_B(name, def, lo, hi, group, label, member, help) bool name = def;
#include "MochizukiRenoKeys.inc"
#undef MZR_F
#undef MZR_U
#undef MZR_B
  uint32_t passes() const;
};

struct NrRuntimeConfig {
  NrRuntimeChoice runtime = NrRuntimeChoice::Auto;
  OpenNrSettings openNr;
};

// scaleconfig.ini beside NRLive.exe, read again whenever the file changes (the
// launcher and the overlay both write it).
class NrConfigWatcher {
public:
  // True when the file changed and the config was read again.
  bool refresh();
  const NrRuntimeConfig& get() const { return m_cfg; }
  // The overlay edits the live settings in place and saves the [OpenNR] keys it
  // shows; the next frame's process() call takes them.
  OpenNrSettings& edit() { return m_cfg.openNr; }
  void save();

private:
  NrRuntimeConfig m_cfg;
  uint64_t m_stamp = ~0ull;
  uint64_t m_lastCheck = 0;
};

class OpenNrRuntime {
public:
  ~OpenNrRuntime();
  // MochizukiNrRuntime.dll in <exe>\opennr\ (preferred: the 0.0.6 bridge owns
  // <exe>\dlssnr-amd\) or beside NRLive.exe; empty when there is none.
  static std::filesystem::path locate();

  // Records the network for `colour` (in colourState) on graphics' list, submits
  // that list, queues the network and continues on the continuation list, where
  // the result is copied out. Returns the result in PIXEL|NON_PIXEL_SHADER_RESOURCE
  // (hand it back with restState() after use), or null: the frame keeps its colour.
  ID3D12Resource* process(Graphics& graphics, ID3D12Resource* colour, D3D12_RESOURCE_STATES colourState,
                          Size size, ID3D12Resource* motion, D3D12_RESOURCE_STATES motionState, bool reset,
                          const OpenNrSettings& settings);
  static constexpr D3D12_RESOURCE_STATES restState() { return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; }
  // After the frame's last list was submitted (after Present).
  void retire();
  // Lets NR start again after a failure (the NR toggle); call between frames.
  void clearFailure();
  // Call with the GPU idle.
  void shutdown();
  // One line for the HUD/log: building, running (size, GPU time, network) or why not.
  std::wstring status();
  // From the last status() call: the network is still being built, and its median GPU ms (<0 none).
  bool building() const { return m_building; }
  float gpuMs() const { return m_gpuMs; }

private:
  bool ensureSession(ID3D12Device* device, ID3D12CommandQueue* queue, const OpenNrSettings& s);
  void sendControls(const OpenNrSettings& s);
  void destroySession();
  bool load();

  struct Api;
  Api* m_api = nullptr;
  HMODULE m_module = nullptr;
  void* m_session = nullptr;
  void* m_job = nullptr;
  uint64_t m_frame = 0;
  bool m_failed = false;
  bool m_sessionAco = false;
  bool m_running = false;
  bool m_building = false;
  bool m_sessionFailed = false;
  std::wstring m_error;
  std::wstring m_status;
  uint64_t m_infoTick = 0;
  float m_gpuMs = -1;
};
