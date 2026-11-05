#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <map>
#include <vector>

namespace xess_payload {
using Programs = std::map<std::string, std::vector<unsigned char>>;
struct Replacement {
    const char* name{};
    const char* nativeHash{};
    size_t nativeBytes{};
    std::vector<unsigned char> bytes;
};
std::string Hash(std::span<const unsigned char> bytes);
Programs LoadPrograms(HMODULE resourceModule);
std::vector<Replacement> LoadReplacements(HMODULE resourceModule);
}
