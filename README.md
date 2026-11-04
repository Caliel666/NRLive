# NRLive
NRLive is a Windows utility that captures a game or app window and presents it in a scaled output window with AMD FidelityFX Super Resolution (FSR), motion-based enhancements and AMD DLSSNR. It is built around a DXGI/D3D12 capture and presentation pipeline for Windows.

## Where do i get it?

NRLive is included and managed on OpenNR: https://github.com/zmodelerlover/OpenNR-Installer

## What it does

NRLive can:

- capture a target window
- display it in a configurable output overlay/window
- scale the captured content to a larger presentation window
- enable AMD FSR upscaling and frame generation (`--upscaler fsr4|fsr31|none`)
- support hotkeys for toggling the overlay and stopping the app
- configurable overlay
- native AMD DLSSNR by [mochizuki](https://gitlab.com/mochizuki0323/DLSSNR-AMD)

The general idea is simple: a small source window can be transformed into a larger output window while adding the effects.

<img width="1920" height="1995" alt="nrliveguide" src="https://github.com/user-attachments/assets/939c0a29-fd6e-45c1-86a4-f484327b5d96" />

## Windows build instructions

These instructions follow the repository's GitHub Actions workflow.

### 1) Install prerequisites

Install:

- Visual Studio 2022 with Desktop development with C++
- Windows 10/11 SDK
- Git and PowerShell
- CMake 3.24 or newer

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
