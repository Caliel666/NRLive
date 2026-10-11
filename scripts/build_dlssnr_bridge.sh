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
                        fill(done, n, static_cast<uint8_t*>(stage.mapped));
                        done += n;
                    }
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
                fill(done, n, static_cast<uint8_t*>(stage.mapped));
                one_shot([&](VkCommandBuffer cmd) {
                    VkBufferCopy region{};
                    region.srcOffset = 0; region.dstOffset = offset + done; region.size = n;
                    vkCmdCopyBuffer(cmd, stage.handle, gpu.handle, 1, &region);
                });
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


# Keep detailed ACO pipeline tracing opt-in via NR_ACO_DUMP; avoid per-pipeline log spam in normal runs.

python3 - <<'PY'
from pathlib import Path
# ACO record parsing, shell loading, capture, splice or binary import can throw. Keep
# those failures local to this shader and retry with the original SPIR-V through the driver.
# This preserves the rest of network initialization and logs the exact failed kernel.
p = Path("windows/src/core/nr_pipeline_binary.hpp")
s = p.read_text()
fn = 'inline VkResult create_or_default(VkDevice device,VkPipelineCache cache,const VkComputePipelineCreateInfo& ci,'
pos = s.find(fn)
if pos < 0:
    raise SystemExit("Could not locate create_or_default for native-compiler fallback")
brace = s.find("{", pos)
depth = 0
end_fn = -1
for i in range(brace, len(s)):
    if s[i] == "{": depth += 1
    elif s[i] == "}":
        depth -= 1
        if depth == 0:
            end_fn = i
            break
if end_fn < 0:
    raise SystemExit("Could not find end of create_or_default")
body = s[brace + 1:end_fn]
if "using the AMD driver's compiler for this kernel" not in body:
    body = '''
    try {''' + body + '''
    } catch (const std::exception& e) {
        const std::string kernel = path.substr(path.find_last_of("/\\\\") + 1);
        nr::logf("[nr] ACO pipeline %s failed (%s); retrying with the AMD driver's compiler",
                 kernel.c_str(), e.what());
        return vkCreateComputePipelines(device, cache, 1, &ci, nullptr, out);
    }
'''
    s = s[:brace + 1] + body + s[end_fn:]
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
