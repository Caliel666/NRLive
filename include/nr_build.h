#pragma once
#include <algorithm>
#include <cstdint>

// What OpenNR's runtime says about the network it is building (MochizukiNrInfo),
// for the HUD and the status line.
struct NrBuild {
  bool active = false;
  uint32_t stage = 0;   // build_stage: 1 planning .. 5 finishing, 6 the pipeline check
  uint32_t done = 0, total = 0; // build_done of build_total, stage 4 only
  uint32_t network = 0; // 0 FP8 (RX 9000), 1-2 RDNA3 (RX 7000), 3 RDNA2 (RX 6000)
};

// done/total while the runtime counts the pipelines; below 0 in every other
// stage, which it reports no progress for.
inline float nrBuildFraction(const NrBuild& b)
{
  if (b.stage != 4 || !b.total) return -1.0f;
  return std::min(float(b.done) / float(b.total), 1.0f);
}

inline bool nrBuildChecksPipelines(const NrBuild& b) { return b.stage == 6; }

inline const wchar_t* nrBuildCard(const NrBuild& b) { return b.network == 3 ? L"RX 6000" : L"RX 7000"; }

inline const wchar_t* nrBuildStage(const NrBuild& b)
{
  switch (b.stage) {
  case 1: return L"planning";
  case 2: return L"reading the weights";
  case 3: return L"uploading the weights";
  case 4: return L"making pipelines";
  case 5: return L"finishing";
  case 6: return b.network == 3 ? L"RX 6000 pipeline check" : L"RX 7000 pipeline check";
  default: return L"starting";
  }
}
