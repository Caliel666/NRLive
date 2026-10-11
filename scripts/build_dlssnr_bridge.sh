#!/usr/bin/env bash
set -euo pipefail
src=${DLSSNR_SOURCE:?Set DLSSNR_SOURCE to the extracted DLSSNR-AMD source root}
out=${1:-build/dlssnr-runtime}
mkdir -p "$out/obj" "$out/generated"
out=$(realpath "$out")
src=$(realpath "$src")
repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$src"
# Extend the pinned upstream queue-frame contract so the host can provide a
# fresh immutable input and a separate destination. The bridge already supports
# BridgeInputs::source; expose it on D3D12QueueFrame instead of mutating the
# capture pool's texture in place.
python3 - <<'PY'
from pathlib import Path
h = Path("windows/src/pe/nr_pe_session.hpp")
s = h.read_text()
old = """        ID3D12Resource* target{};
        D3D12_RESOURCE_STATES target_state{D3D12_RESOURCE_STATE_RENDER_TARGET};
        ID3D12Resource* motion{};"""
new = """        ID3D12Resource* target{};
        D3D12_RESOURCE_STATES target_state{D3D12_RESOURCE_STATE_RENDER_TARGET};
        ID3D12Resource* source{};
        D3D12_RESOURCE_STATES source_state{D3D12_RESOURCE_STATE_COMMON};
        ID3D12Resource* motion{};"""
if old in s:
    h.write_text(s.replace(old, new, 1))
elif new not in s:
    raise SystemExit("Could not patch D3D12QueueFrame source fields")

p = Path("windows/src/pe/nr_pe_session.cpp")
s = p.read_text()
# NRLive is an external post-process, not the game itself. The upstream 1 GiB
# VRAM / 3 GiB commit reserves reject a 793 MiB model on machines with 1.8 GiB
# VRAM and 2.9 GiB commit free, even though the allocation would fit. Keep
# headroom for the game while allowing the bridge to use a realistic reserve.
reserve_old = "constexpr uint64_t kVramReserve = 1024ull << 20, kCommitReserve = 3072ull << 20;"
reserve_new = "constexpr uint64_t kVramReserve = 512ull << 20, kCommitReserve = 1024ull << 20;"
if reserve_old in s:
    s = s.replace(reserve_old, reserve_new, 1)
elif reserve_new not in s:
    raise SystemExit("Could not patch upstream DLSSNR memory guard reserves")

# Keep the global system-commit low-water mark, but disable the running
# network's VRAM-free low-water mark. On Windows/WDDM the Vulkan memory-budget
# extension can report a small per-process budget (here 1677 MB) that becomes
# fully consumed by the model's own persistent arenas. Treating that expected
# steady-state usage as global VRAM exhaustion makes the runtime evict its own
# live network every 500 ms, then rebuild it in a 10/20/40-second loop. The
# admission check still uses the VRAM budget plus reserve before building; the
# running guard continues to yield under real system-commit pressure.
low_old = "constexpr uint64_t kVramLow = 384ull << 20, kCommitLow = 1536ull << 20;"
low_new = "constexpr uint64_t kVramLow = 0ull, kCommitLow = 1024ull << 20;"
if low_old in s:
    s = s.replace(low_old, low_new, 1)
elif low_new not in s:
    raise SystemExit("Could not patch DLSSNR running-network commit threshold")
old = """    in.target = f.target; in.target_state = f.target_state;
    in.motion = f.motion; in.motion_state = f.motion_state;"""
new = """    in.target = f.target; in.target_state = f.target_state;
    in.source = f.source; in.source_state = f.source_state;
    in.motion = f.motion; in.motion_state = f.motion_state;"""
if old in s:
    p.write_text(s.replace(old, new, 1))
elif new not in s:
    raise SystemExit("Could not patch D3D12 queue frame source forwarding")
PY

# ACO-enabled Windows builds stall during weight upload on RDNA4. Batch bounded
# 16 MiB staging allocations into one command-buffer submission to avoid both a
# huge contiguous mapped allocation and repeated queue/fence waits. Keep the
# original serial path as an allocation-failure fallback.
python3 - <<'PY'
from pathlib import Path
p = Path("windows/src/core/nrvk.hpp")
s = p.read_text()
include = '#include "nr_log.hpp"'
if include not in s:
    needle = '#include "nr_pipeline_binary.hpp"'
    if needle not in s:
        raise SystemExit("Could not locate Vulkan pipeline-binary include")
    s = s.replace(needle, needle + "\n" + include, 1)
start = s.find("    void upload_with(Buffer& gpu, VkDeviceSize offset, VkDeviceSize bytes,")
end = s.find("    void upload(Buffer& b, VkDeviceSize off,", start)
if start < 0 or end < 0:
    raise SystemExit("Could not locate Context::upload_with in upstream nrvk.hpp")
new = r"""    void upload_with(Buffer& gpu, VkDeviceSize offset, VkDeviceSize bytes,
                     const std::function<void(VkDeviceSize, VkDeviceSize, uint8_t*)>& fill) {
        if (!bytes) return;
#if defined(_WIN64)
        // Avoid both a single 172 MB contiguous host-visible allocation and
        // eleven serial queue submissions. Allocate bounded 16 MiB staging
        // buffers, fill them, then record every copy into one command buffer
        // and wait once. This preserves bounded individual allocations while
        // removing repeated queue/fence churn from the ACO startup path.
        if (bytes <= (VkDeviceSize(256) << 20)) {
            const VkDeviceSize chunk = kStageBytes;
            std::vector<Buffer> stages;
            bool allocated = true;
            try {
                for (VkDeviceSize done = 0; done < bytes; done += chunk) {
                    const VkDeviceSize n = std::min(chunk, bytes - done);
                    nr::logf("[nr] weight upload: allocating staging chunk %.1f MB at %.1f / %.1f MB",
                             double(n) / 1e6, double(done) / 1e6, double(bytes) / 1e6);
                    stages.push_back(buffer(n, true));
                }
            } catch (const std::exception& e) {
                allocated = false;
                for (auto& stage : stages) destroy(stage);
                stages.clear();
                nr::logf("[nr] batched staging allocation unavailable (%s); using serial 16 MiB uploads", e.what());
            }
            if (allocated) {
                try {
                    VkDeviceSize done = 0;
                    for (auto& stage : stages) {
                        const VkDeviceSize n = stage.bytes;
                        nr::logf("[nr] weight upload: filling %.1f MB at %.1f MB",
                                 double(n) / 1e6, double(done) / 1e6);
                        fill(done, n, static_cast<uint8_t*>(stage.mapped));
                        done += n;
                    }
                    nr::logf("[nr] weight upload: submitting %zu staging chunks (%.1f MB) in one queue operation",
                             stages.size(), double(bytes) / 1e6);
                    one_shot([&](VkCommandBuffer cmd) {
                        VkDeviceSize dst = offset;
                        for (const auto& stage : stages) {
                            VkBufferCopy region{};
                            region.srcOffset = 0;
                            region.dstOffset = dst;
                            region.size = stage.bytes;
                            vkCmdCopyBuffer(cmd, stage.handle, gpu.handle, 1, &region);
                            dst += stage.bytes;
                        }
                    });
                    for (auto& stage : stages) destroy(stage);
                    nr::logf("[nr] weight upload: batched transfer completed");
                    return;
                } catch (...) {
                    for (auto& stage : stages) destroy(stage);
                    throw;
                }
            }
        }
#endif
        const VkDeviceSize chunk = std::min(bytes, kStageBytes);
        Buffer stage = buffer(chunk, true);
        try {
            for (VkDeviceSize done = 0; done < bytes; done += chunk) {
                const VkDeviceSize n = std::min(chunk, bytes - done);
                nr::logf("[nr] weight upload: serial chunk %.1f / %.1f MB begin",
                         double(done) / 1e6, double(bytes) / 1e6);
                fill(done, n, static_cast<uint8_t*>(stage.mapped));
                one_shot([&](VkCommandBuffer cmd) {
                    VkBufferCopy region{};
                    region.srcOffset = 0; region.dstOffset = offset + done; region.size = n;
                    vkCmdCopyBuffer(cmd, stage.handle, gpu.handle, 1, &region);
                });
                nr::logf("[nr] weight upload: serial chunk %.1f MB complete",
                         double(done + n) / 1e6);
            }
        } catch (...) {
            destroy(stage);
            throw;
        }
        destroy(stage);
    }
"""
s = s[:start] + new + s[end:]
p.write_text(s)
PY


# The weight upload can complete while the background network build stalls in
# Vulkan pipeline creation. Emit a breadcrumb for each ACO pipeline stage even
# without NR_ACO_DUMP, and identify the exact shader being created. This also
# distinguishes a cache miss/driver compile from binary import.
python3 - <<'PY'
from pathlib import Path
p = Path("windows/src/core/nr_pipeline_binary.hpp")
s = p.read_text()
old = '    const bool trace=std::getenv("NR_ACO_DUMP")!=nullptr;\n    auto step=[&](const char* s){if(trace){std::fprintf(stderr,"ACO %s: %s\\n",keyname(record.spv_hash).c_str(),s);std::fflush(stderr);}};'
new = '    const bool trace=true;\n    auto step=[&](const char* s){if(trace){std::fprintf(stderr,"[nr] ACO pipeline %s: %s\\n",keyname(record.spv_hash).c_str(),s);std::fflush(stderr);}};'
if old in s:
    s = s.replace(old, new, 1)
elif new not in s:
    raise SystemExit("Could not enable unconditional ACO pipeline stage logs")
# Define the logger before the cache lookup, since cache hit/miss paths use it.
s = s.replace(new + "\n", "", 1)
cache_marker = "    if(!cache_file.empty()) {"
if cache_marker not in s:
    raise SystemExit("Could not locate ACO cache lookup for diagnostic logger")
s = s.replace(cache_marker, new + "\n" + cache_marker, 1)
# Cache hit/miss diagnostics. Don't print full paths to avoid enormous logs.
old = '        if(load_cached(cache_file,key,blob)) {\n            try{return import_pipeline(f,device,ci,key,blob);}catch(const std::exception&){}   // made again below\n        }'
new = '        if(load_cached(cache_file,key,blob)) {\n            step("cached binary found; importing");\n            try { auto p=import_pipeline(f,device,ci,key,blob); step("cached binary imported"); return p; }\n            catch(const std::exception& e) { std::fprintf(stderr,"[nr] ACO cached import failed: %s; rebuilding binary\\n",e.what()); std::fflush(stderr); }\n        } else { step("cache miss; compiling template and capturing binary"); }'
if old in s:
    s = s.replace(old, new, 1)
elif new not in s:
    raise SystemExit("Could not patch ACO cache diagnostics")
p.write_text(s)

p = Path("windows/src/core/nrvk.hpp")
s = p.read_text()
old = '''        const std::vector<uint32_t> code = read_spirv(spirv_path);
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};'''
new = '''        const auto pipeline_t0 = std::chrono::steady_clock::now();
        nr::logf("[nr] pipeline create begin: %s", spirv_path.substr(spirv_path.find_last_of("/\\\\") + 1).c_str());
        const std::vector<uint32_t> code = read_spirv(spirv_path);
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};'''
if old in s:
    s = s.replace(old, new, 1)
elif new not in s:
    raise SystemExit("Could not add pipeline creation begin log")
old = '''        NRVK_CHECK(nr::binary::create_or_default(device, ctx.pipeline_cache, cpi, spirv_path,
                                                code.data(), code.size() * 4, &pipeline, push_bytes));'''
new = '''        const VkResult pipeline_result = nr::binary::create_or_default(device, ctx.pipeline_cache, cpi, spirv_path,
                                                code.data(), code.size() * 4, &pipeline, push_bytes);
        nr::logf("[nr] pipeline create %s: %s (%.1f ms)",
                 spirv_path.substr(spirv_path.find_last_of("/\\\\") + 1).c_str(),
                 pipeline_result == VK_SUCCESS ? "complete" : "FAILED",
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pipeline_t0).count());
        NRVK_CHECK(pipeline_result);'''
if old in s:
    s = s.replace(old, new, 1)
elif new not in s:
    raise SystemExit("Could not add pipeline creation completion log")
# Ensure chrono is directly included for standalone use.
if '#include <chrono>' not in s:
    s = s.replace('#include <cstdint>', '#include <chrono>\n#include <cstdint>', 1)
p.write_text(s)

# The upstream create_or_default path forgot to pass its configured cache
# directory into create(), so every launch recompiled every ACO pipeline. Persist
# imported ACO binaries per GPU/driver identity and SPIR-V hash. A cache miss still
# follows the original capture/splice/import path; cache errors only disable caching.
p = Path("windows/src/core/nr_pipeline_binary.hpp")
s = p.read_text()
if "#include <filesystem>" not in s:
    s = s.replace("#include <cstdio>", "#include <cstdio>\\n#include <filesystem>", 1)
old = "*out=create(device,ci,record,push_bytes,workgroup);return VK_SUCCESS;"
new = r\'''    std::string cache_file;
    const std::string cache_root = cache_dir();
    if (!cache_root.empty()) {
        cache_file = cache_root + "/" + std::to_string(identity()) + "/" + keyname(h);
        try {
            std::filesystem::create_directories(std::filesystem::path(cache_file).parent_path());
        } catch (const std::exception& e) {
            nr::logf("[nr] ACO pipeline cache unavailable (%s); compiling without persistent cache", e.what());
            cache_file.clear();
        }
    }
    *out=create(device,ci,record,push_bytes,workgroup,cache_file);
    return VK_SUCCESS;\'''
if old in s:
    s = s.replace(old, new, 1)
elif new not in s:
    raise SystemExit("Could not wire the configured ACO pipeline cache into create_or_default")
p.write_text(s)
PY

source windows/build/arch/rdna4.sh
python3 windows/build/generate_device_chain.py toolchain/Vulkan-Headers "$out/generated/nr_device_chain_sizes.hpp"
cxx=x86_64-w64-mingw32-g++
common=(-std=c++17 -O2 -DNDEBUG -DNR_BUILD_STAMP='"NRLive-DLSSNR-0.0.4"' -DNR_LIVE_DLSSNR_BRIDGE_EXPORTS
  -Itoolchain/Vulkan-Headers/include -Iwindows/src -Iwindows/src/core -Iwindows/src/pe
  -I"$out/generated" -I"$repo/include")
compile() {
  local name=$1 path=$2
  shift 2
  "$cxx" "${common[@]}" "$@" -c "$path" -o "$out/obj/$name.o"
}
compile runtime windows/src/core/nr_runtime.cpp "${NR_PRODUCT_DEFINES[@]}"
compile native_plan windows/src/core/nr_native_plan.cpp
compile log windows/src/pe/nr_pe_log.cpp
compile crash windows/src/pe/nr_pe_crash.cpp
compile interop windows/src/pe/nr_pe_interop.cpp
compile session windows/src/pe/nr_pe_session.cpp
compile bridge windows/src/pe/nr_pe_bridge.cpp
compile d3d12split windows/src/pe/nr_pe_d3d12split.cpp
compile config windows/src/pe/nr_pe_config.cpp
compile host_api "$repo/src/dlssnr_bridge_api.cpp"
compile vkdevice_stub "$repo/src/dlssnr_vkdevice_stub.cpp"
objs=("$out"/obj/*.o)
ldflags=(-ld3d12 -ldxgi -ldwmapi -lgdi32 -lole32 -ldbghelp -ld3dcompiler -pthread -static -static-libgcc -static-libstdc++)
undefined=$({ "$cxx" -shared -o /dev/null "${objs[@]}" "${ldflags[@]}" 2>&1 || true; } |
  grep -oE "undefined reference to .vk[A-Za-z0-9_]+" | sed -E "s/.*(vk[A-Za-z0-9_]+).*/\\1/" | sort -u || true)
{
  echo '// Generated by scripts/build_dlssnr_bridge.sh.'
  echo '    .text'
  for sym in $undefined; do printf '    .globl %s\n%s:\n    jmp *nr_vk_p_%s(%%rip)\n' "$sym" "$sym" "$sym"; done
  echo '    .data'
  for sym in $undefined; do printf '    .globl nr_vk_p_%s\n    .align 8\nnr_vk_p_%s:\n    .quad nr_vk_unresolved\n' "$sym" "$sym"; done
} > "$out/vulkan_lazy.s"
{
  echo '#include <windows.h>'
  echo 'long nr_vk_unresolved(void) { return -3; }'
  for sym in $undefined; do printf 'extern void* nr_vk_p_%s;\n' "$sym"; done
  echo 'void nr_vk_load(void) {'
  echo '  HMODULE m = GetModuleHandleW(L"vulkan-1.dll");'
  echo '  if (!m) m = LoadLibraryW(L"vulkan-1.dll");'
  echo '  if (!m) return;'
  for sym in $undefined; do printf '  { void* p=(void*)GetProcAddress(m,"%s"); if(p) nr_vk_p_%s=p; }\n' "$sym" "$sym"; done
  echo '}'
} > "$out/vulkan_lazy.c"
"$cxx" -c "$out/vulkan_lazy.s" -o "$out/obj/vulkan_lazy_s.o"
x86_64-w64-mingw32-gcc -O2 -c "$out/vulkan_lazy.c" -o "$out/obj/vulkan_lazy_c.o"
"$cxx" -shared -o "$out/NRLiveDlssNrBridge.dll" "${objs[@]}" "$out/obj/vulkan_lazy_s.o" "$out/obj/vulkan_lazy_c.o" "${ldflags[@]}"
test -s "$out/NRLiveDlssNrBridge.dll"
rm -rf "$out/obj" "$out/generated" "$out/vulkan_lazy.s" "$out/vulkan_lazy.c"
