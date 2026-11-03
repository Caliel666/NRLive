# Gets OpenNR's DLSSNR runtime (MochizukiNrRuntime.dll + dlssnr-amd\) for NRLive.
# It is an optional runtime dependency, not vendored: this script puts it in its own
# folder, and NRLive loads it from opennr\ beside NRLive.exe.
#
#   scripts\fetch_opennr_runtime.ps1                       # OpenNR release package
#   scripts\fetch_opennr_runtime.ps1 -From <folder>        # an OpenNR build (e.g. mochizuki-rdna3)
#
# Then build with -OpenNrRuntime <Destination> (or copy <Destination> to opennr\ beside
# NRLive.exe). NVIDIA model weights are never fetched: NRLive links the dlssnr.bin /
# dlssnr-small.bin you extracted into dlssnr-amd\ beside NRLive.exe.
param(
  [string]$Version = "0.5.7-opennr",
  [string]$Sha256 = "",
  [string]$From = "",
  [string]$Destination = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Destination)) {
  $Destination = Join-Path $root "third_party\opennr-runtime"
}
$knownSha = @{ "0.5.7-opennr" = "d28c0090671f408bd1104e15ea66bcdce9c52e340d8537b346137dc0d81827e7" }
if ([string]::IsNullOrWhiteSpace($Sha256) -and $knownSha.ContainsKey($Version)) { $Sha256 = $knownSha[$Version] }

# Model weights (dlssnr.bin, dlssnr-small.bin and the int4 dlssnr-int4.bin made from
# dlssnr.bin) and per-machine caches never go into the runtime folder. The int4
# tables under int4\swin and int4\vit are mochizuki's own (MIT) and stay.
function Test-Skipped([string]$relative) {
  $name = Split-Path -Leaf $relative
  return $name -like "dlssnr*.bin" -or $relative -match '(^|\\)(aco-cache|prewarm-cache)(\\|$)' -or
         $name -like "pipeline*.cache" -or $name -like "*.log" -or $name -in @("build.pending", "build.failed")
}

if (Test-Path -LiteralPath $Destination) { Remove-Item -LiteralPath $Destination -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $Destination "dlssnr-amd") | Out-Null
$destinationFull = (Resolve-Path -LiteralPath $Destination).Path

if (-not [string]::IsNullOrWhiteSpace($From)) {
  $From = (Resolve-Path -LiteralPath $From).Path
  if (-not (Test-Path -LiteralPath (Join-Path $From "MochizukiNrRuntime.dll"))) { throw "No MochizukiNrRuntime.dll in $From" }
  Copy-Item -LiteralPath (Join-Path $From "MochizukiNrRuntime.dll") -Destination $destinationFull
  $data = Join-Path $From "dlssnr-amd"
  Get-ChildItem -LiteralPath $data -Recurse -File | ForEach-Object {
    $relative = $_.FullName.Substring($data.Length + 1)
    if (Test-Skipped $relative) { return }
    $target = Join-Path $destinationFull (Join-Path "dlssnr-amd" $relative)
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath $_.FullName -Destination $target
  }
} else {
  $url = "https://huggingface.co/datasets/zmodelerlover/amd-nr/resolve/main/mochizuki/$Version/mochizuki-$Version.zip"
  $download = Join-Path $env:TEMP "NRLive-opennr-$Version"
  if (Test-Path -LiteralPath $download) { Remove-Item -LiteralPath $download -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $download | Out-Null
  $zip = Join-Path $download "mochizuki-$Version.zip"
  Write-Host "Fetching OpenNR runtime $Version..."
  curl.exe -L --fail --retry 5 --retry-delay 3 --output $zip $url
  if ($LASTEXITCODE -ne 0) { throw "Failed to download $url" }
  if (-not [string]::IsNullOrWhiteSpace($Sha256)) {
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256.ToLowerInvariant()) { throw "SHA-256 mismatch for $zip ($actual)" }
  } else {
    Write-Warning "No known SHA-256 for $Version; the archive was not verified."
  }
  # The package stores folders flattened with dots: "dlssnr-amd.aco.records/x.nrp" is dlssnr-amd\aco\records\x.nrp.
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  $archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
  try {
    foreach ($entry in $archive.Entries) {
      if ([string]::IsNullOrEmpty($entry.Name)) { continue }
      $parts = $entry.FullName -split '[\\/]'
      $folder = if ($parts.Count -gt 1) { ($parts[0..($parts.Count - 2)] -join '.') -replace '\.', '\' } else { "" }
      $relative = if ($folder) { Join-Path $folder $entry.Name } else { $entry.Name }
      if (Test-Skipped $relative) { continue }
      $target = [System.IO.Path]::GetFullPath((Join-Path $destinationFull $relative))
      if (-not $target.StartsWith($destinationFull + [System.IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Archive entry escapes the destination: $($entry.FullName)"
      }
      New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
      [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
    }
  } finally {
    $archive.Dispose()
  }
  Remove-Item -LiteralPath $download -Recurse -Force -ErrorAction SilentlyContinue
}

foreach ($required in @("MochizukiNrRuntime.dll", "dlssnr-amd\shaders")) {
  if (-not (Test-Path -LiteralPath (Join-Path $destinationFull $required))) { throw "OpenNR runtime is missing $required" }
}
Write-Host "OpenNR runtime: $destinationFull"
Write-Host "Build with: scripts\build.ps1 ... -OpenNrRuntime `"$destinationFull`" (staged as opennr\ beside NRLive.exe)"
