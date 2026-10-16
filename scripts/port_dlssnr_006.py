#!/usr/bin/env python3
"""Port the 0.0.6 dual-model changes onto the upstream Windows preview source tree."""
from pathlib import Path
import shutil
import sys

root = Path(sys.argv[1]).resolve()
linux = root / "linux" / "src" / "core"
windows = root / "windows" / "src" / "core"
tools = root / "windows" / "package" / "model-tools"

def replace_once(path: Path, old: str, new: str, label: str) -> None:
    s = path.read_text(encoding="utf-8")
    n = s.count(old)
    if n != 1:
        raise SystemExit(f"{path.relative_to(root)}: expected exactly one {label}, found {n}")
    path.write_text(s.replace(old, new, 1), encoding="utf-8", newline="")

# The plan and its layer table are portable host code. The small model uses the
# same kernels, but has a different descriptor and ViT block range.
shutil.copyfile(linux / "nr_native_plan.cpp", windows / "nr_native_plan.cpp")
shutil.copyfile(linux / "nr_native_plan.hpp", windows / "nr_native_plan.hpp")
shutil.copyfile(linux / "nr_native_plan_data_small.hpp", windows / "nr_native_plan_data_small.hpp")
shutil.copyfile(linux / "nr_ini_network.hpp", windows / "nr_ini_network.hpp")

runtime_h = windows / "nr_runtime.hpp"
replace_once(runtime_h,
    '#include <vulkan/vulkan.h>\n',
    '#include <vulkan/vulkan.h>\n#include "nr_native_plan.hpp"\n',
    "runtime network enum include")
replace_once(runtime_h,
    '    float model_scale = 1.0f;\n',
    '    float model_scale = 1.0f;\n'
    '    // -1 follows DLSSNR_MODEL / [Network] Model; 0 = original 310.8 model, 1 = new 310.9 model.\n'
    '    int network = -1;\n',
    "runtime network config")
replace_once(runtime_h,
    '    uint32_t model_height() const;\n',
    '    uint32_t model_height() const;\n'
    '    NetworkModel network() const;\n',
    "runtime network getter")

runtime_cpp = windows / "nr_runtime.cpp"
replace_once(runtime_cpp,
    '#if NR_INT4\n#include "nr_ini_int4.hpp"\n#ifdef _WIN32\nextern "C" __declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableA(const char*, char*, unsigned long);\n#endif\n#endif',
    '#include "nr_ini_network.hpp"\n'
    '#if NR_INT4\n#include "nr_ini_int4.hpp"\n#endif\n'
    '#ifdef _WIN32\n'
    'extern "C" __declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableA(const char*, char*, unsigned long);\n'
    '#endif',
    "network environment parsing include")
replace_once(runtime_cpp,
    'constexpr float kPostBlendScale = 0.73974609375f;\n',
    'constexpr float kPostBlendScale = 0.73974609375f;\n'
    'constexpr float kPostBlendScaleSmall = 0.76708984375f;\n',
    "new model temporal blend constant")
replace_once(runtime_cpp,
    'struct Runtime::Impl {\n',
    'struct Runtime::Impl {\n'
    '    NetworkModel network = NetworkModel::Original;\n'
    '    float post_blend_scale = kPostBlendScale;\n',
    "runtime model state")
anchor = (
    '    const uint32_t mw = model_extent(config.width), mh = model_extent(config.height);\n'
    '    impl_->mw = mw; impl_->mh = mh; impl_->scaled = mw != config.width || mh != config.height;\n'
)
model_select = anchor + (
    '    NetworkModel network = NetworkModel::Small;\n'
    '    {\n'
    '        std::string env;\n'
    '#ifdef _WIN32\n'
    '        { char b[16] = {}; const unsigned long n = GetEnvironmentVariableA("DLSSNR_MODEL", b, sizeof b); if (n && n < sizeof b) env = b; }\n'
    '#endif\n'
    '        if (env.empty()) if (const char* e = std::getenv("DLSSNR_MODEL")) env = e;\n'
    '        const int ev = env.empty() ? -1 : nr::network_value(env);\n'
    '        if (!env.empty() && ev < 0) nr::logf("[nr] DLSSNR_MODEL=%s is not small or original: ignored", env.c_str());\n'
    '        const int iv = config.network >= 0 || ev >= 0 ? -1 : nr::ini_network((data.parent_path() / "dlssnr-amd.ini").string());\n'
    '        const int v = config.network >= 0 ? config.network : ev >= 0 ? ev : iv;\n'
    '        if (v == 0) network = NetworkModel::Original;\n'
    '        std::error_code ec;\n'
    '        const char* missing = network != NetworkModel::Small ? nullptr\n'
    '            : !installed ? "not an installed tree"\n'
    '            : !std::filesystem::is_regular_file(data / "dlssnr-small.bin", ec) ? "dlssnr-amd/dlssnr-small.bin is missing (extract from nvngx_dlssnr.dll 310.9.16)"\n'
    '            : nullptr;\n'
    '        if (missing) {\n'
    '            nr::logf("[nr] small model requested but %s: using original model", missing);\n'
    '            network = NetworkModel::Original;\n'
    '        } else {\n'
    '            nr::logf("[nr] %s model selected", network == NetworkModel::Small ? "new 310.9.16" : "original 310.8.0");\n'
    '        }\n'
    '    }\n'
    '    impl_->network = network;\n'
    '    impl_->post_blend_scale = network == NetworkModel::Small ? kPostBlendScaleSmall : kPostBlendScale;\n'
)
replace_once(runtime_cpp, anchor, model_select, "runtime model resolution")
replace_once(runtime_cpp,
    'const auto native = make_native_plan(mw, mh);',
    'const auto native = make_native_plan(mw, mh, network);',
    "selected native plan")
replace_once(runtime_cpp,
    'const char* missing = !installed ? "not an installed tree"',
    'const char* missing = network == NetworkModel::Small ? "the new model has no int4 mixed network"\n'
    '            : !installed ? "not an installed tree"',
    "int4 compatibility guard")
replace_once(runtime_cpp,
    'const auto pack = data / "dlssnr.bin";',
    'const auto pack = data / (network == NetworkModel::Small ? "dlssnr-small.bin" : "dlssnr.bin");',
    "selected model weights")
replace_once(runtime_cpp,
    'uint32_t Runtime::model_height() const { return impl_->mh; }\n',
    'uint32_t Runtime::model_height() const { return impl_->mh; }\n'
    'NetworkModel Runtime::network() const { return impl_->network; }\n',
    "runtime network getter implementation")
replace_once(runtime_cpp,
    't.history_strength * kPostBlendScale,',
    't.history_strength * impl_->post_blend_scale,',
    "model-specific temporal blend")

# The graph had hard-coded block numbers for the old model's ViT segment. Derive
# those boundaries from the plan so both models share the same graph builder.
graph = windows / "nr_graph.cpp"
graph_replacements = [
('    std::map<int, int> last_layer;   // block -> its final layer\n',
 '    std::map<int, int> last_layer;   // block -> its final layer\n'
 '    int vit_first = -1, vit_last = -1, vit_last_layer = -1;\n'),
('    for (const Step& s : plan)\n        last_layer[s.block] = std::max(last_layer[s.block], s.layer);\n',
 '    for (const Step& s : plan)\n        last_layer[s.block] = std::max(last_layer[s.block], s.layer);\n'
 '    for (const Step& s : plan)\n'
 '        if (s.type.rfind("CCVit1D", 0) == 0) {\n'
 '            if (vit_first < 0) vit_first = s.block;\n'
 '            vit_last = s.block; vit_last_layer = s.layer;\n'
 '        }\n'
 '    if (vit_first < 0) throw std::runtime_error("plan has no ViT blocks");\n'),
('if (host_boundary && s.block == 31 && s.layer == 0)\n            vsize[lift_key(31)]',
 'if (host_boundary && s.block == vit_first && s.layer == 0)\n            vsize[lift_key(vit_first)]'),
('if (host_boundary && s.block == 38 && s.layer == 4)\n            vsize[ups_key(38)]',
 'if (host_boundary && s.block == vit_last && s.layer == vit_last_layer)\n            vsize[ups_key(vit_last)]'),
('if (host_boundary && s.block == 31 && s.layer == 0) return lift_key(31);',
 'if (host_boundary && s.block == vit_first && s.layer == 0) return lift_key(vit_first);'),
('if (host_boundary && s.block == 39) return ups_key(38);',
 'if (host_boundary && s.type == "CCDecInputUpsample") return ups_key(vit_last);'),
('host_boundary && s.block == 31 && s.layer == 0 &&',
 'host_boundary && s.block == vit_first && s.layer == 0 &&'),
('it->s->block == 30 && it->s->layer == 4 && it->kern == "gemmnores"',
 'it->s->block == vit_first - 1 && it->s->type == "CCSplitSwin16HFinalHead" && it->kern == "gemmnores"'),
('voff[lift_key(31)]', 'voff[lift_key(vit_first)]'),
('host_boundary && s.block == 31 && s.layer == 0 && !in_folded',
 'host_boundary && s.block == vit_first && s.layer == 0 && !in_folded'),
('voff[blk_out(30)], uint32_t(voff[lift_key(31)])',
 'voff[blk_out(vit_first - 1)], uint32_t(voff[lift_key(vit_first)])'),
('host_boundary && s.block == 38 && s.layer == 4 && d.kern == "gemmproj" &&',
 'host_boundary && s.block == vit_last && s.layer == vit_last_layer && d.kern == "gemmproj" &&'),
('!(s.block == 38 && s.layer == 4)', '!(s.block == vit_last && s.layer == vit_last_layer)'),
('voff[ups_key(38)]', 'voff[ups_key(vit_last)]'),
('host_boundary && s.block == 38 && s.layer == 4 && !out_folded',
 'host_boundary && s.block == vit_last && s.layer == vit_last_layer && !out_folded'),
('voff[key_of(38,4)], uint32_t(voff[ups_key(38)])',
 'voff[key_of(vit_last,vit_last_layer)], uint32_t(voff[ups_key(vit_last)])'),
('if (j == i) { out.push_back(std::move(disp[i])); i = j + 1; continue; }',
 '// A one-layer run can still fold a downsample or upsample into that layer.\n'
 '                const bool short_run = j == i;'),
('                    fp(out.back()).o_off == fp(disp[i]).x_off;\n',
 '                    fp(out.back()).o_off == fp(disp[i]).x_off;\n'
 '                if (short_run && !ds_fold && !up_fold) { out.push_back(std::move(disp[i])); i = i + 1; continue; }\n'),
('host_boundary && s2.block==38 && s2.layer==4',
 'host_boundary && s2.block==vit_last && s2.layer==vit_last_layer'),
('std::string(seed_dir)+"/b038l4_2d.bin"',
 'std::string(seed_dir)+"/b"+(vit_last<100?"0":"")+(vit_last<10?"0":"")+std::to_string(vit_last)+"l"+std::to_string(vit_last_layer)+"_2d.bin"'),
('put_slot(ups_key(38),canon,M,C);', 'put_slot(ups_key(vit_last),canon,M,C);'),
('(s2.block == 31 && s2.layer == 0)', '(s2.block == vit_first && s2.layer == 0)'),
('pool_key(s2.block) : lift_key(31)', 'pool_key(s2.block) : lift_key(vit_first)'),
]
for old, new in graph_replacements:
    replace_once(graph, old, new, "dynamic ViT graph boundary: " + old[:55])

# Make the Windows extractor able to build either model from a 310.9.16 DLL.
extractor = tools / "dlssnr_extract_model.cpp"
replace_once(extractor,
    'static const char kDllSha256[] = "e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e";',
    'static const char kDllSha256_3108[] = "e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e";\n'
    'static const char kDllSha256_3109[] = "2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f";',
    "supported model DLL hashes")
replace_once(extractor,
    'std::vector<Span> weights_ht() const {',
    'std::vector<Span> weights_ht(const char* resource = "WEIGHTS_HT") const {',
    "parameterized model resource")
replace_once(extractor,
    'walk(tree, 0, 0, false, active, out);',
    'walk(tree, 0, 0, false, resource, active, out);',
    "resource tree selector")
replace_once(extractor,
    'void walk(Span tree, u64 rel, int depth, bool weights, std::set<u64>& active,',
    'void walk(Span tree, u64 rel, int depth, bool weights, const char* resource, std::set<u64>& active,',
    "resource tree selector recursion")
replace_once(extractor,
    '                static const char want[] = "WEIGHTS_HT";\n'
    '                is_weights = len == 10;\n'
    '                for (u32 j = 0; is_weights && j < 10; ++j)\n'
    '                    is_weights = label.p[2 * j] == u8(want[j]) && label.p[2 * j + 1] == 0;',
    '                const size_t want_len = std::strlen(resource);\n'
    '                is_weights = len == want_len;\n'
    '                for (u32 j = 0; is_weights && j < want_len; ++j)\n'
    '                    is_weights = label.p[2 * j] == u8(resource[j]) && label.p[2 * j + 1] == 0;',
    "dynamic model resource name")
replace_once(extractor,
    'walk(tree, child & 0x7fffffffu, depth + 1, weights || is_weights, active, out);',
    'walk(tree, child & 0x7fffffffu, depth + 1, weights || is_weights, resource, active, out);',
    "resource tree selector recursion call")
replace_once(extractor,
    '    plan_fixed(records, "block70.layer0.layer", "unpacked-postblock",',
    '#ifdef NR_MODEL_SMALL\n'
    '    plan_fixed(records, "block30.layer0.layer", "unpacked-postblock",\n'
    '#else\n'
    '    plan_fixed(records, "block70.layer0.layer", "unpacked-postblock",\n'
    '#endif',
    "model-specific post block")
replace_once(extractor,
    '        std::vector<Span> matches = pe.weights_ht();',
    '#ifdef NR_MODEL_SMALL\n'
    '        std::vector<Span> matches = pe.weights_ht("WEIGHTS_CC_SMALL_FP8");\n'
    '#else\n'
    '        std::vector<Span> matches = pe.weights_ht("WEIGHTS_HT");\n'
    '#endif',
    "model-specific weight resource")
replace_once(extractor,
    '    if (got != kDllSha256)\n'
    '        throw Failure("This is not nvngx_dlssnr 310.8.0 (SHA256 " + got + "). The 310.8.0 DLL is required.");',
    '#ifdef NR_MODEL_SMALL\n'
    '    if (got != kDllSha256_3109)\n'
    '        throw Failure("The new model extractor requires nvngx_dlssnr.dll 310.9.16 (SHA256 " + got + ").");\n'
    '#else\n'
    '    if (got != kDllSha256_3108 && got != kDllSha256_3109)\n'
    '        throw Failure("The original model extractor requires nvngx_dlssnr.dll 310.8.0 or 310.9.16 (SHA256 " + got + ").");\n'
    '#endif',
    "model DLL hash validation")
replace_once(extractor,
    '    const char* want_resource = "WEIGHTS_HT";',
    '    const char* want_resource = "WEIGHTS_HT";',
    "noop guard")

print("Ported 0.0.6 dual-model plan/runtime and model extractor support into the Windows source tree.")
