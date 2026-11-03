param(
  [Parameter(Mandatory=$true)]
  [string]$FfxSdk,
  [string]$FfxBin = "",
  # Optional: a folder from scripts\fetch_opennr_runtime.ps1, staged as opennr\ beside NRLive.exe.
  [string]$OpenNrRuntime = "",
  [string]$Generator = "Visual Studio 17 2022"
)

$ErrorActionPreference = "Stop"
$FfxSdk = $FfxSdk.Trim().TrimEnd('\', '/')
if (-not (Test-Path -LiteralPath $FfxSdk)) {
  throw "FSR SDK source path does not exist: $FfxSdk"
}
if ([string]::IsNullOrWhiteSpace($FfxBin)) {
  $FfxBin = Join-Path (Split-Path -Parent $PSScriptRoot) "third_party\ffx-bin"
}
$FfxBin = $FfxBin.Trim().TrimEnd('\', '/')
if (-not (Test-Path -LiteralPath (Join-Path $FfxBin "amd_fidelityfx_loader_dx12.dll"))) {
  throw "FSR runtime folder does not contain amd_fidelityfx_loader_dx12.dll: $FfxBin. Run scripts\fetch_deps.ps1 or pass -FfxBin."
}
if (-not [string]::IsNullOrWhiteSpace($OpenNrRuntime)) {
  $OpenNrRuntime = (Resolve-Path -LiteralPath $OpenNrRuntime.Trim()).Path
}

$root = Split-Path -Parent $PSScriptRoot
Write-Host "NRLive root : $root"
Write-Host "FSR SDK     : $FfxSdk"
Write-Host "FSR runtime : $FfxBin"
Write-Host "OpenNR      : $(if ($OpenNrRuntime) { $OpenNrRuntime } else { '(not staged)' })"
Write-Host ""

cmake -S $root -B "$root\build" -G $Generator -A x64 "-DFFX_SDK=$FfxSdk" "-DFFX_BIN=$FfxBin" "-DOPENNR_RUNTIME=$OpenNrRuntime" -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build "$root\build" --config Release --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

ctest --test-dir "$root\build" -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ""
Write-Host "Built: $root\build\Release\NRLive.exe"
Write-Host "FSR SDK 2.3.0 loader + frame-generation + upscaler providers staged beside the EXE."
if ($OpenNrRuntime) { Write-Host "OpenNR DLSSNR runtime staged in build\Release\opennr." }
Write-Host "If using OptiScaler, copy its dxgi.dll and OptiScaler.ini beside the EXE."
