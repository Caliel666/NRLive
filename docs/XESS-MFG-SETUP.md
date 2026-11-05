# XeSS MFG runtime setup for NRLive

XeSS MFG is an experimental **native NRLive D3D12 output backend**. It is not a game proxy, swapchain hook, OptiScaler install, or game-side mod. NRLive captures the game, creates its own motion vectors/depth, owns its presentation swapchain, and sends those inputs directly to XeFG.

## Build-time SDK

The Windows CI workflow downloads Intel XeSS SDK 3.0.2 and verifies the archive SHA-256 before compiling. It also downloads the x64 runtime pair and MFG Unlocker v1.7.0, verifies the Unlocker archive, and stages all three files under `build/Release/xess/` to check the runtime layout. Those separately licensed binaries are removed before the public NRLive ZIP is assembled. For a local build, extract the same SDK and pass its root directory to CMake:

\`\`\`powershell
cmake -S . -B build -A x64
cmake --build build --config Release
\`\`\`

For a local build, add \`-DXESS_SDK_ROOT="C:/sdk/XeSS_SDK_3.0.2"\` together with the existing FFX SDK arguments. The root passed to \`XESS_SDK_ROOT\` must contain \`inc/xess_fg\` and \`inc/xell\`. Public SDK headers are a build dependency only. NRLive's public portable ZIP does not bundle Intel's runtime DLLs or the MFG Unlocker payload.

## Runtime files

Create an \`xess\` directory beside \`NRLive.exe\`:

\`\`\`text
NRLive.exe
xess/
  libxess_fg.dll
  libxell.dll
  MFGUnlocker.dll
\`\`\`

1. Copy \`libxess_fg.dll\` and \`libxell.dll\` from the **same XeSS SDK 3.0.2 runtime package**. Do not mix versions or use similarly named DLLs from unrelated packages.
2. Download the \`MFGUnlocker-v1.7.0.zip\` release from <https://github.com/dashdogy/MFG-Unlocker/releases/tag/v1.7.0> and place its \`MFGUnlocker.dll\` in the \`xess\` directory.
3. NRLive maps \`MFGUnlocker.dll\` as a **resource-only module** and reads the embedded, hash-checked RCDATA payloads. It does not execute that DLL's code, call its hooks, or install a proxy/ASI. Its embedded DXIL resources are used by NRLive's own native compatibility port.

The first implementation is deliberately AMD-only (PCI vendor ID \`0x1002\`) and D3D12-only. It validates the loaded XeSS pair against the supported private runtime layouts before applying any in-memory patches. Unsupported runtimes fail closed and NRLive rebuilds an ordinary output swapchain; the FSR backend and capture pipeline remain available.

## Intel license limitation

The official XeSS SDK `LICENSE.txt` permits redistribution of its binaries only without modification, and says that reverse engineering or modifying the software or its operation during execution is not permitted. This AMD compatibility route uses private runtime-layout discovery and patches XeSS runtime code in memory. Therefore, **this experimental route is not authorized by the standard public XeSS SDK license alone**. Use it only if you have separate permission or another valid authorization from Intel covering this specific use. Having your own copy of the DLLs does not by itself grant that permission.

The MFG Unlocker release notice also states that some embedded DXIL programs are derived from Intel XeSS programs and modified, and remain subject to Intel's license. NRLive does not redistribute those resources: the MFG Unlocker DLL is supplied separately by the user and is read as a resource-only module. This does not remove the license limitation above.

This is a technical prototype, not an Intel-supported integration or a statement that the compatibility patches are permitted under the standard SDK terms.

## Selecting a backend

Open Settings → General → Frame Generation:

- **Backend**: FSR Frame Generation or XeSS MFG (AMD).
- **XeSS output multiplier**: 2x through 6x.
- Command-line override: `NRLive.exe --fg fsr` or `NRLive.exe --fg xemfg`. This takes precedence over the saved UI preference for that launch.
- The backend choice is applied on the next NRLive launch, because only one provider may own the output swapchain at a time.
- The FG button in NRLive's top bar enables/disables whichever provider is selected.

The XeSS multiplier is the output-frame multiplier, so the SDK receives one through five interpolated frames for 2x through 6x respectively.

## Licensing and distribution

- NRLive does not package \`libxess_fg.dll\` or \`libxell.dll\`; obtain them under Intel's applicable XeSS SDK license.
- The MFG Unlocker source-derived compatibility code is accompanied by \`MFG-UNLOCKER-LICENSE.txt\`.
- Some resource payloads embedded in the upstream MFG Unlocker release are derived from Intel XeSS SDK program data. \`XEFG-PROGRAMS-NOTICE.txt\` documents that boundary. Those resources are not copied into NRLive's binaries or release archives by this project; users provide the release DLL themselves and are responsible for compliance with Intel's applicable terms.
- Do not redistribute the runtime files or the resource pack as part of an NRLive mirror/package unless the applicable Intel terms expressly allow it.

## Current validation scope

The Windows build validates compilation and the existing NRLive test suite. Runtime generation still requires testing on an AMD D3D12 system with the three user-provided files above. NRLive logs DLL hashes, layout validation, patch activation, XeFG resource-tag failures, and XeLL pacing failures to the debugger output.
