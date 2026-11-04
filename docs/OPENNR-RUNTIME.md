# OpenNR DLSSNR runtime

NRLive can run DLSSNR through OpenNR's `MochizukiNrRuntime.dll`: mochizuki's DLSSNR-AMD
Vulkan runtime (v0.0.7, OpenNR 0.5.9) with OpenNR's RenoDX composition stage, ACO machine code, int4
mixed, the Small (310.9.16) and Original (310.8) models and, in newer builds, an RX 7000
network picked at run time. It runs on NRLive's own D3D12 queue on the captured frame,
after the motion pass and before the upscaler, the same place OptiScaler runs it before
Super Resolution.

## Layout

```
NRLive.exe
dlssnr-amd\dlssnr-small.bin     your extracted models (also used by the 0.0.6 bridge)
dlssnr-amd\dlssnr.bin
opennr\MochizukiNrRuntime.dll
opennr\dlssnr-amd\shaders\ ...  the runtime's own data (aco, int4, prewarm, shaders-rdna3)
```

The runtime reads `dlssnr-amd\` beside its own DLL. It lives in `opennr\` because the 0.0.6
bridge's shaders use the same folder name. On first use NRLive hard-links the models from
`dlssnr-amd\` into `opennr\dlssnr-amd\` (no copy, no extra space); if your drive cannot hard
link, copy them there yourself. `MochizukiNrRuntime.dll` directly beside `NRLive.exe` also
works, with its `dlssnr-amd\` there.

Model weights are never downloaded or packaged. Extract `dlssnr-small.bin` and `dlssnr.bin`
from your own `nvngx_dlssnr.dll` 310.9.16 (see DLSSNR-AMD-SETUP.txt). int4 mixed also needs
`dlssnr-int4.bin`, which OpenNR's installer makes from `dlssnr.bin`; copy it from a game you
installed OpenNR into (`dlssnr-amd\dlssnr-int4.bin`) to `dlssnr-amd\` beside NRLive.exe.

## Getting the runtime

- Release packages of NRLive include it in `opennr\`.
- `scriptsetch_opennr_runtime.ps1` downloads the runtime package OpenNR's installer uses
  from Hugging Face (`https://huggingface.co/datasets/zmodelerlover/amd-nr/resolve/main/mochizuki/<version>/`:
  `mochizuki-<version>.zip` plus the RX 7000 overlay `mochizuki-rdna3-<version>.zip`), checks
  both against the SHA-256 pinned in the script, and unpacks them into
  `third_party\opennr-runtime`, dropping any model file. CI runs the same script.
  `-From <folder>` takes a local OpenNR build instead.
- `scriptsuild.ps1 ... -OpenNrRuntime third_party\opennr-runtime` stages it as
  `build\Release\opennr`.
- To move to a newer runtime: add its version and the two SHA-256 (from OpenNR's
  installer `payload.json`, or `Get-FileHash` of the downloaded zips) to `$known` in
  `scriptsetch_opennr_runtime.ps1` and change the `-Version` default. Check that
  `third_party/opennr` still matches that runtime's ABI headers.
- OpenNR releases: https://github.com/MatheusFerreiraS/OpenNR-OptiScaler/releases (the
  OptiScaler package carries the same DLL and `dlssnr-amd\`).

## Settings (scaleconfig.ini)

`[DLSSNR] runtime` = `auto` (OpenNR when its DLL is found, else the 0.0.6 bridge), `opennr`
or `bridge`. `[DLSSNR] enabled` is the overlay's NR button.

`[OpenNR]` uses OptiScaler's `[DlssNr]` key names, so a working OptiScaler.ini section can be
copied over. NRLive re-reads the file while it runs (the launcher writes it as you move a
control); a model, pass or style change rebuilds the network, a look change does not.

| Key | Default | |
|---|---|---|
| MochizukiOriginalModel | false | 310.8 model instead of Small |
| MochizukiInt4 | false | int4 mixed (Original model, ACO, dlssnr-int4.bin) |
| MochizukiAco | true | ACO machine code; read once per NRLive run |
| MochizukiTemporal | true | history along NRLive's motion vectors |
| MochizukiHistoryStrength | 1.0 | |
| MochizukiStyleGrade | true | grade Natural/Cinematic after the network |
| MochizukiCascadeDetail, MochizukiAntiHalo, MochizukiAntiHaloStrength, MochizukiPassEstimate | true, true, 0.8, true | multi-pass extras |
| MochizukiReno* | as OptiScaler | every RenoDX stage key in `third_party/opennr/MochizukiRenoKeys.inc`: Intensity, Style, LocalTone, LocalStructure, SkinStructure, AutoMask, Passes, ColorStrength, Look*, ... |

## Status

The last line of the overlay's Settings > DLSSNR tab and NRLive.log show what the runtime is doing: the network build
(planning, weights, pipelines n/m, and on an RX 7000 the one-time pipeline check), then the
network size and GPU time, with "RX 7000 (experimental)" when the RDNA3 network runs. The
runtime's own log is `opennr\mochizuki_nr.log`. If anything is missing or fails, the frame
goes on unchanged; the NR button retries.

## Licenses

NRLive is GPL-3.0. `MochizukiNrRuntime.dll` is built from mochizuki0323/DLSSNR-AMD (MIT) and
OpenNR's runtime code (GPL-3.0); the package carries `dlssnr-amd\LICENSE-DLSSNR-AMD.txt`.
The ABI headers in `third_party/opennr` are OpenNR's (GPL-3.0). NVIDIA's weights are not
part of any of it.
