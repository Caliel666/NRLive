#include "opennr_controls.h"
#include <cstring>
#include <iostream>

static int fail(int code, const char* what) { std::cerr << what << "\n"; return code; }

int main() {
  // At its defaults the look and encoding block sends what the header says the runtime
  // starts from: preprocess off, so the network sees the frame as it was.
  MochizukiNrControls c{};
  c.struct_size = sizeof(c);
  OpenNrSettings s;
  fillOpenNrControls(s, c);
  if (c.preprocess != 0 || c.preprocess_exposure != 1 || c.preprocess_bias_ev != 0.f || c.preprocess_curve != 3 ||
      c.preprocess_contrast != 1.f || c.preprocess_saturation != 1.f || c.noise_every_frame != 0)
    return fail(1, "preprocess defaults");
  if (c.reno_look_mode != 0 || c.reno_source_encoding != 0 || c.reno_codec_mode != 2 || c.reno_look_brighten != 1.f ||
      c.reno_paper_white_scale != 2.5375f)
    return fail(2, "RenoDX defaults");

  // The new keys reach the struct, limited to their ranges.
  s.preprocess = true; s.preprocessCurve = 6; s.preprocessBias = -20.f; s.preprocessContrast = 9.f; s.noiseEveryFrame = true;
  fillOpenNrControls(s, c);
  if (c.preprocess != 1 || c.preprocess_curve != 6 || c.preprocess_bias_ev != -8.f || c.preprocess_contrast != 2.f ||
      c.noise_every_frame != 1)
    return fail(3, "preprocess values");
  s.preprocessCurve = 99;
  fillOpenNrControls(s, c);
  if (c.preprocess_curve != 6) return fail(4, "curve range");

  // int4 mixed runs on the 310.8 model and under ACO only; the Small model never gets it and stays Small.
  OpenNrSettings m;
  m.int4 = true;
  m.originalModel = false;
  auto flags = openNrFrameFlags(m, true);
  if ((flags & LMXXF_NR_FRAME_FLAG_INT4) || (flags & LMXXF_NR_FRAME_FLAG_ORIGINAL_MODEL)) return fail(5, "int4 on the Small model");
  m.originalModel = true;
  flags = openNrFrameFlags(m, true);
  if (!(flags & LMXXF_NR_FRAME_FLAG_INT4) || !(flags & LMXXF_NR_FRAME_FLAG_ORIGINAL_MODEL)) return fail(6, "int4 on the Original model");
  flags = openNrFrameFlags(m, false);
  if ((flags & LMXXF_NR_FRAME_FLAG_INT4) || !(flags & LMXXF_NR_FRAME_FLAG_ORIGINAL_MODEL)) return fail(7, "int4 without ACO");

  // A percentage only where the runtime counts; a stage without numbers has none.
  for (uint32_t stage : { 0u, 1u, 2u, 3u, 5u, 6u })
    if (nrBuildFraction({ true, stage, 5, 10, 0 }) >= 0) return fail(8, "a stage without progress has a fraction");
  if (nrBuildFraction({ true, 4, 0, 0, 0 }) >= 0) return fail(9, "no total yet");
  if (nrBuildFraction({ true, 4, 6, 24, 0 }) != 0.25f) return fail(10, "pipelines");
  if (nrBuildFraction({ true, 4, 30, 24, 0 }) != 1.f) return fail(11, "more done than total");
  if (std::wcscmp(nrBuildStage({ true, 6, 0, 0, 3 }), L"RX 6000 pipeline check") != 0 ||
      std::wcscmp(nrBuildStage({ true, 6, 0, 0, 1 }), L"RX 7000 pipeline check") != 0)
    return fail(12, "the check names the card");
  return 0;
}
