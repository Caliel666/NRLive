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
source windows/build/arch/rdna4.sh
python3 windows/build/generate_device_chain.py toolchain/Vulkan-Headers "$out/generated/nr_device_chain_sizes.hpp"
cxx=x86_64-w64-mingw32-g++
common=(-std=c++17 -O2 -DNDEBUG -DNR_BUILD_STAMP='"NRLive-DLSSNR-0.0.5"' -DNR_LIVE_DLSSNR_BRIDGE_EXPORTS
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
# DLSSNR-AMD 0.0.5 adds RenoDX-style processing and screenshot support to Session.
compile renodx windows/src/core/nr_renodx.cpp
compile screenshot windows/src/pe/nr_screenshot.cpp
compile host_api "$repo/src/dlssnr_bridge_api.cpp"
compile vkdevice_stub "$repo/src/dlssnr_vkdevice_stub.cpp"
objs=("$out"/obj/*.o)
ldflags=(-ld3d12 -ldxgi -ldwmapi -lgdi32 -lole32 -loleaut32 -lwindowscodecs -ldbghelp -ld3dcompiler -pthread -static -static-libgcc -static-libstdc++)
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
