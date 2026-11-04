#pragma once
#include "LmxxfNrApi.h"
#include "MochizukiNrControls.h"
#include "opennr.h"
#include <algorithm>

// [OpenNR] settings into the runtime's controls. c holds the runtime's defaults
// (MochizukiNrGetControlDefaults) for every field not set here.
inline void fillOpenNrControls(const OpenNrSettings& s, MochizukiNrControls& c)
{
  c.history_strength = std::clamp(s.historyStrength, 0.f, 1.f);
  c.anti_halo_margin = .25f * (1.f - std::clamp(s.antiHaloStrength, 0.f, 1.f));
  c.style_grade = s.styleGrade ? 1u : 0u;
  c.noise_every_frame = s.noiseEveryFrame ? 1u : 0u;
  c.preprocess = s.preprocess ? 1u : 0u;
  c.preprocess_exposure = std::min(s.preprocessExposure, 2u);
  c.preprocess_bias_ev = std::clamp(s.preprocessBias, -8.f, 8.f);
  c.preprocess_curve = std::min(s.preprocessCurve, 6u);
  c.preprocess_contrast = std::clamp(s.preprocessContrast, .5f, 2.f);
  c.preprocess_saturation = std::clamp(s.preprocessSaturation, .05f, 2.f);
  c.renodx = 1u;
  c.reno_nr_enabled = 1u;
#define MZR_F(name, def, lo, hi, group, label, member, help) c.member = std::clamp(s.name, lo, hi);
#define MZR_U(name, def, lo, hi, group, label, member, help) c.member = std::clamp(s.name, uint32_t(lo), uint32_t(hi));
#define MZR_B(name, def, lo, hi, group, label, member, help) c.member = s.name ? 1u : 0u;
#include "MochizukiRenoKeys.inc"
#undef MZR_F
#undef MZR_U
#undef MZR_B
}

// int4 mixed runs as ACO's machine code only and is made from the 310.8 model:
// with the Small model it stays stored and does not run.
inline bool openNrInt4Runs(const OpenNrSettings& s, bool acoSession) { return s.int4 && s.originalModel && acoSession; }

// LmxxfNrFrameInfo::flags for the model and the pass extras.
inline uint32_t openNrFrameFlags(const OpenNrSettings& s, bool acoSession)
{
  return LMXXF_NR_FRAME_FLAG_STRENGTH | (openNrInt4Runs(s, acoSession) ? LMXXF_NR_FRAME_FLAG_INT4 : 0u) |
         (s.originalModel ? LMXXF_NR_FRAME_FLAG_ORIGINAL_MODEL : 0u) |
         (s.cascadeDetail ? LMXXF_NR_FRAME_FLAG_CASCADE_DETAIL : 0u) | (s.antiHalo ? LMXXF_NR_FRAME_FLAG_ANTI_HALO : 0u) |
         (s.passEstimate ? LMXXF_NR_FRAME_FLAG_PASS3_ESTIMATE : 0u);
}
