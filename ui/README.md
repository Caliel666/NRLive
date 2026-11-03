# NRLive Windows launcher

The launcher is a native **WPF / .NET 10** desktop app styled for Windows 11. Its layout and workflow take inspiration from Lossless Scaling and Magpie without copying their assets.

## Build

On Windows with the .NET 10 SDK:

```powershell
dotnet publish .\NRLiveUI.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -o .\build
```

The result is `ui\build\NRLiveUI.exe`. GitHub Actions builds and publishes the launcher alongside NRLive.

Place `NRLive.exe` beside the UI, or use **Browse for NRLive.exe…**.

## Scaling workflow

- Press **Scale** or the global toggle hotkey (default `Ctrl+Alt+S`).
- The button shows the countdown while NRLive silently tracks the foreground target. Switch to the game during this period.
- The button changes to **Unscale**. Press it or the same toggle hotkey to terminate NRLive.
- NRLive's own picker countdown window is intentionally hidden; the launcher owns the visible countdown.

The global toggle must differ from NRLive's stop key (`Ctrl+Shift+A` by default) and overlay key (`Ctrl+Home` by default).

## Settings

Profiles are JSON files under `profiles\`. FSR sharpening is configured from the in-game NRLive overlay menu and persisted to scaleconfig.ini. FastMV is the default motion-vector implementation; AMDOF remains selectable. The upscaler (FSR 4, FSR 3.1 or native scaling) is part of the profile.

The DLSSNR card writes `scaleconfig.ini` beside the selected NRLive.exe (`[DLSSNR]` and `[OpenNR]`), which NRLive re-reads while it runs.
