# OpenNR DLSSNR runtime

NRLive can run DLSSNR through OpenNR's `MochizukiNrRuntime.dll`: mochizuki's DLSSNR-AMD
Vulkan runtime (v0.0.8.2, OpenNR 0.6.0) with OpenNR's RenoDX composition stage, ACO machine code, int4
mixed, the Small (310.9.16) and Original (310.8) models and an RX 7000 and an RX 6000 network
picked at run time. It runs on NRLive's own D3D12 queue on the captured frame,
after the motion pass and before the upscaler, the same place OptiScaler runs it before
Super Resolution.

## Layout

```
NRLive.exe
dlssnr-amd\dlssnr-small.bin     your extracted models (also used by the 0.0.6 bridge)
dlssnr-amd\dlssnr.bin
opennr\MochizukiNrRuntime.dll
opennr\dlssnr-amd\shaders\ ...  the runtime's own data (aco, int4, prewarm, shaders-rdna3, shaders-rdna2)
```

The runtime reads `dlssnr-amd\` beside its own DLL. It lives in `opennr\` because the 0.0.6
bridge's shaders use the same folder name. On first use NRLive hard-links the models from
`dlssnr-amd\` into `opennr\dlssnr-amd\` (no copy, no extra space); if your drive cannot hard
link, copy them there yourself. `MochizukiNrRuntime.dll` directly beside `NRLive.exe` also
works, with its `dlssnr-amd\` there.

Model weights are never downloaded or packaged. Extract `dlssnr-small.bin` and `dlssnr.bin`
from your own `nvngx_dlssnr.dll` 310.9.16 (see DLSSNR-AMD-SETUP.txt). int4 mixed also needs
`dlssnr-int4.bin`, made from `dlssnr.bin`: it comes with OpenNR's mochizuki package, but the fetch
script leaves it out with the other weights, so copy it from a game you installed OpenNR into
(`dlssnr-amd\dlssnr-int4.bin`) to `dlssnr-amd\` beside NRLive.exe.

## Getting the runtime

- Release packages of NRLive include it in `opennr\`.
- `scriptsetch_opennr_runtime.ps1` downloads the runtime package OpenNR's installer uses
  from Hugging Face (`https://huggingface.co/datasets/zmodelerlover/amd-nr/resolve/main/mochizuki/<version>/`:
  `mochizuki-<version>.zip` plus the RX 7000 and RX 6000 overlays `mochizuki-rdna3-<version>.zip` and
  `mochizuki-rdna2-<version>.zip`), checks them against the SHA-256 pinned in the script, and unpacks them into
  `third_party\opennr-runtime`, dropping any model file. CI runs the same script.
  `-From <folder>` takes a local OpenNR build instead.
- `scriptsuild.ps1 ... -OpenNrRuntime third_party\opennr-runtime` stages it as
  `build\Release\opennr`.
- To move to a newer runtime: add its version and the three SHA-256 (from OpenNR's
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
control); a model, pass or style change rebuilds the network, a look change does not. The
overlay's Settings > NR tab edits the same keys and applies them on the next frame.

| Key | Default | |
|---|---|---|
| MochizukiOriginalModel | false | 310.8 model instead of Small |
| MochizukiInt4 | false | int4 mixed (Original model, ACO, dlssnr-int4.bin). Made from the 310.8 model only: with the Small model selected it stays stored and does not run, and the overlay greys it out |
| MochizukiAco | true | ACO machine code; read once per NRLive run |
| MochizukiTemporal | true | history along NRLive's motion vectors |
| MochizukiHistoryStrength | 1.0 | |
| MochizukiStyleGrade | true | grade Natural/Cinematic after the network |
| MochizukiNoiseEveryFrame | false | frames without motion vectors: the network's noise seed moves every frame |
| MochizukiHistoryPrecision | 16 | the temporal history in RGBA16F; 32 in RGBA32F. A change rebuilds the network |
| MochizukiWhitePoint | 0 | NRLive's own key: what counts as white in a frame taken as Linear, handed over as 1 / exposure; 0 measures the frame |
| MochizukiCascadeDetail, MochizukiAntiHalo, MochizukiAntiHaloStrength, MochizukiPassEstimate | true, true, 0.8, true | multi-pass extras |
| MochizukiPreprocess | false | changes what the network is shown and takes it back out of its answer; the first time it is on the network is rebuilt once |
| MochizukiPreprocessExposure | 1 | 0 off, 1 auto, 2 fixed (the bias alone) |
| MochizukiPreprocessBias | 0 | EV, -8..8 (the overlay's slider stops at -3..3) |
| MochizukiPreprocessCurve | 3 | 0 none, 1 neutral, 2 Reinhard, 3 filmic, 4 GT, 5 ACES, 6 AgX |
| MochizukiPreprocessContrast, MochizukiPreprocessSaturation | 1, 1 | 0.5..2, 0.05..2 |
| MochizukiPreprocessInverse | true | the network's change comes back through the curve's inverse; false, as its ratio |
| MochizukiReno* | as OptiScaler | every RenoDX stage key in `third_party/opennr/MochizukiRenoKeys.inc`: Intensity, Style, LocalTone, LocalStructure, SkinStructure, AutoMask, Passes, ColorStrength, Look*, ... |

## Status

The first line of the overlay's Settings > NR tab and NRLive.log show what the runtime is doing: the network build
(planning, weights, pipelines n/m, and on an RX 7000 the one-time pipeline check), then the
network size and GPU time, with "RX 7000 (experimental)" when the RDNA3 network runs. While a
network builds the HUD shows a bar under its items, whichever are on: the runtime's real count
while it makes the pipelines, an animation with no number in the stages it reports no progress
for. The pipeline check is one of those: it compiles every pipeline once in a child process,
1 to 2 minutes on an RX 7000, half an hour or more on an RX 6000, once per driver, the frames
going on without NR until it ends. The
runtime's own log is `opennr\mochizuki_nr.log`. If anything is missing or fails, the frame
goes on unchanged; the NR button retries.

## What the overlay leaves out

Every `MochizukiReno*` key is read whether the overlay shows it or not. The NR tab groups the
ones that change NRLive's picture as OptiScaler's menu does (Composition with its Look details,
Encoding and exposure with Encoding and Preprocess) and has no control for: the pass 2, 3 and 4
keys, unlocking passes past 4 and chained history (no look of their own, and each pass costs the
whole network again), follow render resolution and the motion vector scales (NRLive's own
vectors are at the capture's size and in the right direction), and the keys that measured no
change on NRLive's picture in any encoding, on a still frame or right after a change of brightness:
highlight guard, HDR dark pedestal, neural floor guard, detail stability, look stabilize with its
time and detail, divisor governor, cut aware, attack, release and slew.

## Encoding on a capture

The stage picks its codec from the frame's format and the host's linear flag. A capture is BGRA8,
so by default it runs the SDR codec, which reads no encoding key. NRLive keeps that for Source
encoding "From the format", the default, so the default picture stays as it was. Choosing SDR,
Linear or PQ sets `linear_input`, which rebuilds the network on the HDR codec with the frame read
in that encoding: there the rest of the Encoding group, Transfer strength, Chroma clamp, Transfer
mode and Codec act. White point stands in for the exposure a game hands OptiScaler: with Linear
and Linear unit 0, `MochizukiNrSetExposure(1 / white)` feeds the stage's divisor instead of its
content meter. The curves that also work on the SDR path, AgX among them, are under Preprocess.

## Licenses

NRLive is GPL-3.0. `MochizukiNrRuntime.dll` is built from mochizuki0323/DLSSNR-AMD (MIT) and
OpenNR's runtime code (GPL-3.0); the package carries `dlssnr-amd\LICENSE-DLSSNR-AMD.txt`.
The ABI headers in `third_party/opennr` are OpenNR's (GPL-3.0). NVIDIA's weights are not
part of any of it.
