# NRLive

<img width="1000" height="720" alt="image" src="https://github.com/user-attachments/assets/e3babe3e-afcf-400e-911b-a0f5358b5372" />


NRLive is a Windows utility that captures a game or app window and presents it in a scaled output window with AMD FidelityFX Super Resolution (FSR) and motion-based enhancements. It is built around a DXGI/D3D12 capture and presentation pipeline for Windows.

It is designed for situations where you want a source window to remain visible in a game or app while presenting a larger, more comfortable scaled view in a separate output surface.

## What it does

NRLive can:

- capture a target window
- display it in a configurable output overlay/window
- scale the captured content to a larger presentation window
- enable AMD FSR upscaling and frame generation
- support hotkeys for toggling the overlay, scaling, and stopping the app
- show a HUD for status, FPS, and capture state

The general idea is simple: a small source window can be transformed into a larger output window while preserving the game/app input flow and overlay behavior.
<img width="3550" height="1147" alt="Untitled-1" src="https://github.com/user-attachments/assets/ce6ee296-6c8b-4c27-9207-c91b2f31e447" />

## So why "NR"

This app was made in mind to be able to use it with DLSSNR, specifically AMD's versions
you can also use it with optiscaler and reshade to be able to apply those to any game, specially ones with heavy anti cheat solutions

## Features

- Windows-only DXGI/D3D12 capture pipeline
- AMD FSR 4 and FSR 3.1 upscaling, or native scaling (`--upscaler fsr4|fsr31|none`)
- DLSSNR through OpenNR's `MochizukiNrRuntime.dll` (RenoDX look controls, int4, ACO, RX 7000 network), included in release packages; see [docs/OPENNR-RUNTIME.md](docs/OPENNR-RUNTIME.md)
- Motion-vector and frame-generation support
- Toggleable overlay HUD
- Keyboard hotkeys for scale/unscale and stop actions
- Portable launcher packaging for Windows

## Repository layout

- `src/` — main capture, graphics, overlay, target, and FSR logic
- `include/` — shared headers
- `ui/` — Windows launcher UI project
- `scripts/` — dependency fetch and build helpers
- `tests/` — frame timing / limiter validation tests
- `.github/workflows/windows-build.yml` — CI build workflow

## Windows build instructions

These instructions follow the repository’s GitHub Actions workflow.

### 1) Install prerequisites

Install:

- Visual Studio 2022 with Desktop development with C++
- Windows 10/11 SDK
- .NET 10 SDK for the launcher project
- Git and PowerShell

### 2) Clone and fetch dependencies

```powershell
git clone https://github.com/Caliel666/NRLive.git
cd NRLive
powershell -ExecutionPolicy Bypass -File .\scripts\fetch_deps.ps1
```
### 3) Build

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 `
  -FfxSdk .\third_party\FidelityFX-SDK `
  -FfxBin .\third_party\ffx-bin
```

For DLSSNR through OpenNR's runtime (not vendored; downloaded from Hugging Face with a pinned SHA-256 and staged as `opennr\`):

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\fetch_opennr_runtime.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 `
  -FfxSdk .\third_party\FidelityFX-SDK -FfxBin .\third_party\ffx-bin `
  -OpenNrRuntime .\third_party\opennr-runtime
```

With Visual Studio 2026, add `-Generator "Visual Studio 18 2026"`.
  <circle cx="1185" cy="250" r="14" fill="#0a0d12"/>
</svg>
