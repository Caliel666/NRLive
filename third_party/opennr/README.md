C ABI headers of OpenNR's `MochizukiNrRuntime.dll` (https://github.com/MatheusFerreiraS/OpenNR-OptiScaler, GPL-3.0),
copied unchanged so NRLive can load the runtime at run time. No code from the runtime is linked.

- `LmxxfNrApi.h` — session/frame ABI (`LmxxfNrGetApi`), OpenNR `OptiScaler/dlssnr/backend/lmxxf_runtime/`
- `MochizukiNrControls.h` — mochizuki's own exports (controls, info); from the `mochizuki-rdna3` branch, which adds
  `MochizukiNrInfo::network` (older runtimes leave it 0)
- `MochizukiRenoKeys.inc` — the RenoDX stage settings (key, default, range, member); NRLive reads the same keys from
  `scaleconfig.ini` `[OpenNR]` that OptiScaler reads from `[DlssNr]`

Sources: OpenNR commits 8098b8e71 (main) and fb58bd68e (mochizuki-rdna3).
