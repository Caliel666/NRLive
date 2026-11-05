# XeSS Multi Frame Generation integration research

Status: native D3D12 backend implementation is wired on `feature/xess-mfg-amd`; AMD hardware runtime validation is still pending. See `docs/XESS-MFG-SETUP.md` for user-supplied runtime setup.

## Goal

Add a Frame Generation backend selector to NRLive:

- **FSR FG**: keep the existing AMD FidelityFX implementation and swapchain path unchanged.
- **XeSS MFG (XeMFG)**: a separate D3D12 backend that can run on AMD GPUs, with an independently controlled multiplier (2x–6x / dynamic where supported).

The selected backend is exclusive at startup. NRLive initializes and presents through only one frame-generation swapchain owner per session.

## What the references establish

### MFG Unlocker (primary implementation reference)

`dashdogy/MFG-Unlocker` documents an experimental standalone XeMFG path for DirectX 12 and Vulkan, on NVIDIA, AMD and Intel GPUs. Its D3D12 host:

- loads the user's `libxess_fg.dll` and `libxell.dll` runtime pair rather than redistributing those binaries;
- takes ownership of the game's DXGI swapchain;
- receives depth and motion-vector inputs from Streamline tags or from the game's DLSS / XeSS / FSR upscaler evaluations;
- supplies frame constants and present IDs to XeFG and uses XeLL for pacing;
- embeds program data needed to run the multi-frame model on non-Intel GPUs.

Relevant upstream source: https://github.com/dashdogy/MFG-Unlocker/tree/main/source/native/xess

Important licensing boundary: MFG Unlocker's project code is MIT, but its notice explicitly says some embedded DXIL/SPIR-V program data is derived from Intel XeSS SDK programs and remains subject to Intel's XeSS SDK license. NRLive uses the release DLL only as a resource-only module to read hash-checked payloads at runtime; the DLL and payload are not included in NRLive builds or release packages. The required notices are packaged with NRLive.

### dlssg-to-xefg-xmx

https://github.com/realdody/dlssg-to-xefg-xmx

This is a GPL-3.0 fork focused on the DLSS-G-to-XeFG route and XMX/CUDA-related paths. It is useful for understanding the translation approach, but its CUDA/XMX implementation is not a drop-in AMD backend and its GPL-3.0 terms need to be considered before reusing code.

### OptiScaler XEMFG work

https://github.com/TheAutomatic/dlss-5-amd-project

This is a large OptiScaler fork (GPL-3.0) and a useful reference for how XeFG is integrated as an output provider in an existing upscaler/frame-generation pipeline. It is not a small standalone library that NRLive can simply link.

## NRLive-specific architecture

NRLive is not currently a game-side proxy. It captures a game's image, generates motion vectors/depth in its own D3D12 device, and presents through an NRLive-owned swapchain. The existing FSR FG implementation wraps that output swapchain and calls the FFX generation callback from the presentation path.

The upstream standalone host demonstrates the XeFG/XeLL call sequence. NRLive uses a separate native host around its own output swapchain and its own motion-vector/depth resources; no game-side hook or instrumentation is used.

The native NRLive XeMFG host is adapted around NRLive's resources:

1. Create an XeFG context for NRLive's D3D12 device and output swapchain.
2. Load and validate a compatible XeSS FG / XeLL runtime pair from a user-provided runtime directory. Do not silently mix incompatible DLL versions.
3. Feed NRLive's per-frame color, depth, motion-vector resources, dimensions, jitter, camera constants, frame time and reset state into XeFG.
4. Use XeFG's generated output and XeLL pacing in the output presentation path, with explicit ownership and teardown ordering.
5. Add a backend selector persisted in the existing INI config. Switching backend must drain the GPU, tear down the old swapchain wrapper, rebuild the ordinary swapchain if necessary, then initialize exactly one backend.
6. Expose multiplier / dynamic MFG controls only when the runtime reports that the selected mode is supported. Keep FSR's current controls and defaults unchanged.
7. Add CPU-side policy tests for backend selection, runtime validation, frame IDs, input validity and failure fallback.

## Initial implementation constraints

- Keep this branch isolated from `main`.
- Keep FSR FG as the default and preserve its existing code path.
- XeMFG initialization failure must fall back to ordinary presentation without disabling capture, upscaling, OpenNR, or the FSR backend.
- Do not bundle Intel's XeSS runtime DLLs or unlicensed Intel-derived shader/model payloads.
- Runtime discovery must be explicit and logged (resolved DLL paths, versions, exports, GPU adapter, and failure reason).
- Do not advertise XeMFG as active until an actual generated frame has been successfully presented.
- The first native implementation target is D3D12 / Windows. Vulkan can be a later phase.

## Implemented first milestone

The branch now includes a D3D12 XeFG/XeLL host, AMD runtime-layout validation, the MFG model/shader compatibility port, input resource tagging, frame constants, present IDs, a 2x–6x selector, backend-exclusive swapchain ownership, and failure fallback to ordinary NRLive presentation. The build uses XeSS SDK 3.0.2 public headers; the end-user runtime DLLs and MFG Unlocker resource pack are supplied separately. The XMX fork remains reference-only for AMD. Actual generated-frame output still needs validation on an AMD GPU.
