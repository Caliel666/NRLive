# XeSS MFG runtime setup for NRLive

XeSS MFG is an experimental native NRLive D3D12 output backend. It is not a game proxy, swapchain hook, OptiScaler install, or game-side mod. NRLive captures the game, creates its own motion vectors/depth, owns its presentation swapchain, and sends those inputs directly to XeFG.

## Build-time SDK

The Windows CI workflow downloads Intel XeSS SDK 3.0.2 and MFG Unlocker v1.7.0, verifies both upstream archive SHA-256 values, validates that all three runtime DLLs are x64, and includes the unmodified files in the NRLive package under xess/ beside NRLive.exe. The DLLs are copied byte-for-byte; the package does not patch or rewrite them.

For a local build, extract the SDK and pass its root directory to CMake:

```powershell
cmake -S . -B build -A x64 -DXESS_SDK_ROOT="C:/sdk/XeSS_SDK_3.0.2"
cmake --build build --config Release
```

The SDK root must contain inc/xess_fg and inc/xell. Include the existing FFX SDK arguments used by the project.

## Runtime files

The release package includes this layout:

```text
NRLive.exe
xess/
  libxess_fg.dll
  libxell.dll
  MFGUnlocker.dll
```

The two Intel DLLs come from the same XeSS SDK 3.0.2 archive. MFGUnlocker.dll comes from the pinned MFG Unlocker v1.7.0 release. CI validates the source archives and x64 architecture before copying the three files unchanged into the package. No manual runtime download or file placement is required.

NRLive maps MFGUnlocker.dll as a resource-only module and reads its embedded, hash-checked RCDATA payloads. It does not execute that DLL's code, call its hooks, or install a proxy/ASI. Its embedded DXIL resources are used by NRLive's native compatibility port.

The implementation is AMD-only (PCI vendor ID 0x1002) and D3D12-only. It validates the loaded XeSS pair against the supported private runtime layouts before applying in-memory compatibility patches. Unsupported runtime layouts fail closed and NRLive rebuilds an ordinary output swapchain; the FSR backend and capture pipeline remain available.

## Selecting a backend

Open Settings → General → Frame Generation:

- Backend: FSR Frame Generation or XeSS MFG (AMD).
- XeSS output multiplier: 2x through 6x.
- Command-line override: NRLive.exe --fg fsr or NRLive.exe --fg xemfg. This takes precedence over the saved UI preference for that launch.
- The backend choice is applied on the next NRLive launch, because only one provider may own the output swapchain at a time.
- The FG button in NRLive's top bar enables/disables whichever provider is selected.

The XeSS multiplier is the output-frame multiplier, so the SDK receives one through five interpolated frames for 2x through 6x respectively.

## Licensing and notices

- The three runtime DLLs in the package are copied unchanged from their pinned upstream archives.
- The package includes MFG-UNLOCKER-LICENSE.txt and XEFG-PROGRAMS-NOTICE.txt alongside the runtime files.
- NRLive's experimental compatibility layer applies its runtime patches in process memory after loading; it does not alter the distributed DLL files on disk.
- This is a technical prototype, not an Intel-supported integration.

## Current validation scope

The Windows build validates compilation, runtime file staging, package contents, and the existing NRLive test suite. Actual frame generation still requires testing on an AMD D3D12 system. NRLive logs DLL hashes, layout validation, patch activation, XeFG resource-tag failures, and XeLL pacing failures to the debugger output.
