#pragma once
#include <string>

// The upscalers NRLive can run after capture (and after DLSSNR). Every FFX one
// goes through the same FFX upscale API and differs only in which provider the
// loader is asked for; a new provider (another FFX upscaler DLL) is one more
// row in the table in upscaler.cpp. None stretches the frame to the output
// with the presentation blit.
enum class UpscalerKind { Fsr4, Fsr31, None };

struct UpscalerInfo {
  UpscalerKind kind;
  const wchar_t* id;     // --upscaler value and profile key
  const wchar_t* label;  // HUD / log
  // FFX provider wanted, matched against ffxQueryDescGetVersions names:
  // the name's first "major.minor" must be >= minMajor.minMinor and its major
  // <= maxMajor; or, when nameContains is set, the name must contain it.
  int minMajor, minMinor, maxMajor;
  const char* nameContains;
  bool ffx;              // false: no upscaler context at all
};

const UpscalerInfo& upscalerInfo(UpscalerKind kind);
// Null for an unknown id.
const UpscalerInfo* findUpscaler(const std::wstring& id);
std::wstring upscalerIds(); // "fsr4, fsr31, none" for usage/errors
// Whether an FFX provider name (for example "FSR 3.1.5") is the one the entry wants.
bool upscalerMatchesProvider(const UpscalerInfo& info, const char* providerName);
