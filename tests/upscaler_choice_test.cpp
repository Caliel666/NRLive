#include "upscaler.h"
#include <iostream>

int main() {
  const auto& fsr4 = upscalerInfo(UpscalerKind::Fsr4);
  const auto& fsr31 = upscalerInfo(UpscalerKind::Fsr31);
  const auto& none = upscalerInfo(UpscalerKind::None);
  if (!upscalerMatchesProvider(fsr4, "FSR 4.0.2") || upscalerMatchesProvider(fsr4, "FSR 3.1.5") ||
      !upscalerMatchesProvider(fsr4, "4.1.0")) {
    std::cerr << "FSR 4 provider match failed\n"; return 1;
  }
  if (!upscalerMatchesProvider(fsr31, "FSR 3.1.5") || !upscalerMatchesProvider(fsr31, "3.1.4 (upscaler)") ||
      upscalerMatchesProvider(fsr31, "FSR 3.0.3") || upscalerMatchesProvider(fsr31, "FSR 4.0.2")) {
    std::cerr << "FSR 3.1 provider match failed\n"; return 2;
  }
  if (upscalerMatchesProvider(none, "FSR 3.1.5") || upscalerMatchesProvider(fsr31, nullptr) ||
      upscalerMatchesProvider(fsr31, "no version")) {
    std::cerr << "Non-FFX or unnamed provider matched\n"; return 3;
  }
  if (findUpscaler(L"FSR31") != &fsr31 || findUpscaler(L"none") != &none || findUpscaler(L"dlss") != nullptr) {
    std::cerr << "Upscaler id lookup failed\n"; return 4;
  }
  return 0;
}
