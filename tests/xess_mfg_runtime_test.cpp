#include "runtime_layout.h"
#include "runtime_payload.h"
#include "host_unlock.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <iterator>
#include <string>

namespace
{
bool requireExports(HMODULE module, const char* library,
                    const char* const* names, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        if (!GetProcAddress(module, names[i]))
        {
            std::cerr << library << " is missing required export: " << names[i] << "\n";
            return false;
        }
    }
    return true;
}

HMODULE loadLibrary(const wchar_t* path, DWORD flags)
{
    HMODULE module = LoadLibraryExW(path, nullptr, flags);
    if (!module)
        std::wcerr << L"Could not load " << path << L" (Win32 error "
                   << GetLastError() << L")\n";
    return module;
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 4)
    {
        std::wcerr << L"Usage: xess_mfg_runtime_test <libxess_fg.dll> <libxell.dll> <MFGUnlocker.dll>\n";
        return 2;
    }

    // Match NRLive's load order and altered-search-path behavior. The FG DLL
    // may import XeLL and other DLLs from the same staged runtime directory.
    HMODULE ll = loadLibrary(argv[2], LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!ll) return 3;
    HMODULE fg = loadLibrary(argv[1], LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!fg) return 4;
    HMODULE payload = loadLibrary(argv[3],
        LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!payload) return 5;

    static constexpr const char* fgExports[] = {
        "xefgSwapChainD3D12CreateContext",
        "xefgSwapChainD3D12InitFromSwapChainDesc",
        "xefgSwapChainD3D12GetSwapChainPtr",
        "xefgSwapChainD3D12TagFrameResource",
        "xefgSwapChainTagFrameConstants",
        "xefgSwapChainSetEnabled",
        "xefgSwapChainSetNumInterpolatedFrames",
        "xefgSwapChainSetPresentId",
        "xefgSwapChainGetLastPresentStatus",
        "xefgSwapChainSetLatencyReduction",
        "xefgSwapChainSetLoggingCallback",
        "xefgSwapChainDestroy",
        "xefgSwapChainGetVersion"
    };
    static constexpr const char* llExports[] = {
        "xellD3D12CreateContext",
        "xellSetSleepMode",
        "xellSleep",
        "xellAddMarkerData",
        "xellSetLoggingCallback",
        "xellDestroyContext",
        "xellGetVersion"
    };

    if (!requireExports(fg, "libxess_fg.dll", fgExports, std::size(fgExports)) ||
        !requireExports(ll, "libxell.dll", llExports, std::size(llExports)))
        return 6;

    try
    {
        // This is the same private-instruction-family check NRLive runs before
        // applying any executable patches. It validates the actual staged DLL
        // images, not a copied version string or a mock layout.
        const auto& fgLayout = xess_layout::Get(fg, true);
        const auto& llLayout = xess_layout::Get(ll, false);

        // Verify the AMD compatibility payload embedded in MFGUnlocker.dll:
        // all 54 model programs and all seven replacement DXIL shaders must
        // pass the production SHA-256 and DXBC container checks.
        const auto programs = xess_payload::LoadPrograms(payload);
        const auto replacements = xess_payload::LoadReplacements(payload);
        if (programs.size() != 54 || replacements.size() != 7)
        {
            std::cerr << "Unexpected MFG payload dimensions: programs="
                      << programs.size() << " replacements=" << replacements.size() << "\n";
            return 7;
        }

        // Execute the same checked in-memory patch transactions NRLive uses.
        // Both routes must be safely applicable and fully restorable on the
        // exact staged provider pair before this runtime is accepted.
        {
            xess_host_unlock::Unlock native2x(fg, ll, 5, true);
            const auto& sites = native2x.Applied();
            const auto keep2x = std::find_if(sites.begin(), sites.end(),
                [](const auto& site) { return std::string(site.name) == "fg_keep_2x_model"; });
            if (!native2x.Keeps2xModel() || keep2x == sites.end())
            {
                std::cerr << "Native 2x route did not retain XeFG's native 2x model site.\n";
                return 9;
            }
            native2x.Restore();
        }
        {
            xess_host_unlock::Unlock portedMfg(fg, ll, 5, false);
            const auto& sites = portedMfg.Applied();
            const auto keep2x = std::find_if(sites.begin(), sites.end(),
                [](const auto& site) { return std::string(site.name) == "fg_keep_2x_model"; });
            if (portedMfg.Keeps2xModel() || keep2x != sites.end())
            {
                std::cerr << "Ported MFG route unexpectedly retained the native 2x model site.\n";
                return 10;
            }
            portedMfg.Restore();
        }
        {
            xess_host_unlock::OutputRing ring(fg, 2);
            if (!ring.Applied() || ring.Depth() != 2)
            {
                std::cerr << "XeFG two-slot output ring did not apply.\n";
                return 11;
            }
            ring.Restore();
        }

        std::cout << "XeSS MFG runtime validation: PASS\n"
                  << "  XeFG layout: " << fgLayout.family << "\n"
                  << "  XeLL layout: " << llLayout.family << "\n"
                  << "  required exports: " << std::size(fgExports) + std::size(llExports) << "\n"
                  << "  model programs: " << programs.size() << "\n"
                  << "  replacement shaders: " << replacements.size() << "\n"
                  << "  native 2x patch transaction: PASS\n"
                  << "  ported MFG patch transaction: PASS\n"
                  << "  output ring patch transaction: PASS\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "XeSS MFG runtime validation failed: " << e.what() << "\n";
        return 8;
    }
}
