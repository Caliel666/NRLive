#include "upscaler.h"
#include <cctype>
#include <cstring>

namespace {
const UpscalerInfo kUpscalers[] = {
  { UpscalerKind::Fsr4,  L"fsr4",  L"FSR 4",   4, 0, 99, nullptr, true },
  { UpscalerKind::Fsr31, L"fsr31", L"FSR 3.1", 3, 1, 3,  nullptr, true },
  { UpscalerKind::None,  L"none",  L"Native scaling", 0, 0, 0, nullptr, false },
};

// First "major.minor" in a provider name; false when there is none.
bool parseVersion(const char* s, int& major, int& minor)
{
  for (; s && *s; ++s) {
    if (!std::isdigit(static_cast<unsigned char>(*s))) continue;
    major = 0;
    while (std::isdigit(static_cast<unsigned char>(*s))) major = major * 10 + (*s++ - '0');
    minor = 0;
    if (*s == '.' && std::isdigit(static_cast<unsigned char>(s[1]))) {
      ++s;
      while (std::isdigit(static_cast<unsigned char>(*s))) minor = minor * 10 + (*s++ - '0');
    }
    return true;
  }
  return false;
}
}

const UpscalerInfo& upscalerInfo(UpscalerKind kind)
{
  for (const auto& u : kUpscalers)
    if (u.kind == kind) return u;
  return kUpscalers[0];
}

const UpscalerInfo* findUpscaler(const std::wstring& id)
{
  for (const auto& u : kUpscalers)
    if (_wcsicmp(u.id, id.c_str()) == 0) return &u;
  return nullptr;
}

std::wstring upscalerIds()
{
  std::wstring ids;
  for (const auto& u : kUpscalers) {
    if (!ids.empty()) ids += L", ";
    ids += u.id;
  }
  return ids;
}

bool upscalerMatchesProvider(const UpscalerInfo& info, const char* providerName)
{
  if (!info.ffx || !providerName) return false;
  if (info.nameContains) return std::strstr(providerName, info.nameContains) != nullptr;
  int major = 0, minor = 0;
  if (!parseVersion(providerName, major, minor)) return false;
  if (major > info.maxMajor) return false;
  return major > info.minMajor || (major == info.minMajor && minor >= info.minMinor);
}
