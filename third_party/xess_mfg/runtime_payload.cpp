#include "runtime_payload.h"
#include "sha256_util.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace xess_payload {
namespace {
constexpr int kFirstProgramResource = 7000;
constexpr int kLastProgramResource = 7054;
constexpr int kFirstShaderResource = 7100;
constexpr const char* kManifestHash = "d74bd783d29280ed372653701ce9b0395362e381faace1213206e0bdd9bfc9e5";

std::vector<unsigned char> ReadResource(HMODULE module, int id) {
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) throw std::runtime_error("xess_payload_resource_missing_" + std::to_string(id));
    const DWORD size = SizeofResource(module, resource);
    const HGLOBAL loaded = LoadResource(module, resource);
    const auto* data = loaded ? static_cast<const unsigned char*>(LockResource(loaded)) : nullptr;
    if (!data || !size) throw std::runtime_error("xess_payload_resource_empty_" + std::to_string(id));
    return {data, data + size};
}

void ValidateDxil(const std::vector<unsigned char>& bytes, const char* stage) {
    uint32_t declared = 0;
    if (bytes.size() < 32 || bytes.size() > 1000000 ||
        std::memcmp(bytes.data(), "DXBC", 4) != 0) throw std::runtime_error(stage);
    std::memcpy(&declared, bytes.data() + 24, sizeof(declared));
    if (declared != bytes.size()) throw std::runtime_error(stage);
}
}

std::string Hash(std::span<const unsigned char> bytes) {
    sha256_util::Digest digest{};
    if (!sha256_util::Compute(bytes.data(), bytes.size(), digest))
        throw std::runtime_error("xess_payload_hash_failed");
    return sha256_util::LowerHex(digest);
}

Programs LoadPrograms(HMODULE resourceModule) {
    if (!resourceModule) throw std::runtime_error("xess_payload_module_missing");
    std::unordered_map<std::string, std::vector<unsigned char>> byHash;
    std::vector<unsigned char> manifest;
    for (int id = kFirstProgramResource; id <= kLastProgramResource; ++id) {
        HRSRC resource = FindResourceW(resourceModule, MAKEINTRESOURCEW(id), RT_RCDATA);
        if (!resource) continue;
        auto bytes = ReadResource(resourceModule, id);
        const auto hash = Hash(bytes);
        if (hash == kManifestHash) manifest = std::move(bytes);
        else byHash.emplace(hash, std::move(bytes));
    }
    if (manifest.empty()) throw std::runtime_error("xess_payload_manifest_missing");
    std::istringstream input(std::string(reinterpret_cast<const char*>(manifest.data()), manifest.size()));
    Programs result;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream row(line);
        std::string key, hash, extra;
        if (!(row >> key >> hash) || (row >> extra) ||
            key.size() < 4 || key.size() > 24 || hash.size() != 64 ||
            !std::all_of(key.begin(), key.end(), [](char c) { return c >= '0' && c <= '9'; }))
            throw std::runtime_error("xess_payload_manifest_row");
        const auto found = byHash.find(hash);
        if (found == byHash.end()) throw std::runtime_error("xess_payload_program_missing_" + key);
        ValidateDxil(found->second, "xess_payload_program_shape");
        if (!result.emplace(key, found->second).second)
            throw std::runtime_error("xess_payload_duplicate_program");
    }
    if (result.size() != 54) throw std::runtime_error("xess_payload_program_count");
    return result;
}

std::vector<Replacement> LoadReplacements(HMODULE resourceModule) {
    struct Native { const char* name; const char* hash; size_t bytes; const char* replacementHash; };
    static constexpr Native native[] = {
        {"shaders/splat_motion_aware.dxil", "3f3f91d2ecd4345042f0519f7455776314be9e7730453a3fcb42c57f0acb49b5", 4008, "43ccfe670409fd807b604b870674012958fbb4067305a449304ee91ab6154f3f"},
        {"shaders/splat_motion_aware_inverted.dxil", "c3649191906bdd300462dbafe6add8f82b09e8e10c41e92784a209d99513224b", 4012, "6c9bf4b59956c7fec3d764ffe6520e7aec3e6137553b9fca0ba65ecdd7642ce8"},
        {"shaders/unet_3ed03daf.dxil", "01c8beac27d3f3f82ecba8f3dc2d9d8b60d4c175a41df3c24a24e8643f5578f5", 35852, "9900c30febf95bfa81b4da1178e5773863116beba4d3f032897da4254b6d8926"},
        {"shaders/unet_748b0baf.dxil", "8291bbc36b1100b07d0788f35a3a9fadfbfcc2267ff0ccb7afb114d76f5d69fe", 57376, "e78063f97ad2c0913412f07589cd17d0f2a6ebeeb6edd70647a69cca6b6a02bf"},
        {"shaders/unet_cb391582.dxil", "a60f7488872a675d66afd2bec02b0b89e3cc38c1c4ed1d25d745a7b2fe677412", 57372, "7a29fbf311cd8dee5b8c1cfdadf31ee983092a512cf9c10845a079b457eb62a9"},
        {"shaders/unet_aa016078.dxil", "8c2efb9e955f39379a91887ed3d90412c4b7ae2c142a874e3da5f699de197f7c", 47728, "ce847b58086ce7e9d25f7d8b90e989247cd4582226506d9552112f1ef4130643"},
        {"shaders/unet_efbe3788.dxil", "40e159414d5b839ba7e0e07cbf8ff4ef425ee026c216b2928ef3bb6dd0e20d40", 54224, "efa8da5ef5f90c62b789b0fed8e77e2ce043e558d164cc552ca7fb9127cedb00"}
    };
    std::vector<Replacement> result;
    for (size_t i = 0; i < std::size(native); ++i) {
        auto bytes = ReadResource(resourceModule, kFirstShaderResource + static_cast<int>(i));
        if (Hash(bytes) != native[i].replacementHash)
            throw std::runtime_error(std::string("xess_payload_shader_hash_") + native[i].name);
        ValidateDxil(bytes, "xess_payload_shader_shape");
        result.push_back({native[i].name, native[i].hash, native[i].bytes, std::move(bytes)});
    }
    return result;
}
}
