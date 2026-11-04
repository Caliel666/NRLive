C ABI headers of OpenNR's `MochizukiNrRuntime.dll` (https://github.com/MatheusFerreiraS/OpenNR-OptiScaler, GPL-3.0),
copied unchanged so NRLive can load the runtime at run time. No code from the runtime is linked.

- `LmxxfNrApi.h` — session/frame ABI (`LmxxfNrGetApi`), OpenNR `OptiScaler/dlssnr/backend/lmxxf_runtime/`
- `MochizukiNrControls.h` — mochizuki's own exports (controls, info), OpenNR `OptiScaler/dlssnr/backend/mochizuki_runtime/`
- `MochizukiRenoKeys.inc` — the RenoDX stage settings (key, default, range, member); NRLive reads the same keys from
  `scaleconfig.ini` `[OpenNR]` that OptiScaler reads from `[DlssNr]`

Source: OpenNR's mega-update branch at 1099e0620, the runtime published as 0.6.0-opennr.
