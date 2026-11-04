# Gets OpenNR's DLSSNR runtime (MochizukiNrRuntime.dll + dlssnr-amd\) for NRLive.
# It is a runtime dependency, not vendored: this script downloads the package OpenNR's
# installer uses from Hugging Face, checks its SHA-256 and unpacks it into its own folder.
# NRLive loads it from opennr\ beside NRLive.exe.
#
#   scripts\fetch_opennr_runtime.ps1                       # pinned OpenNR runtime (see $known)
#   scripts\fetch_opennr_runtime.ps1 -From <folder>        # a local OpenNR build instead
#
# Then build with -OpenNrRuntime <Destination> (or copy <Destination> to opennr\ beside
# NRLive.exe). NVIDIA model weights are never fetched: NRLive links the dlssnr.bin /
# dlssnr-small.bin you extracted into dlssnr-amd\ beside NRLive.exe.
param(
  [string]$Version = "0.6.0-opennr",
  [string]$Sha256 = "",
  [string]$Rdna3Sha256 = "",
  [string]$Rdna2Sha256 = "",
  [switch]$NoRdna3,
  [switch]$NoRdna2,
  [string]$From = "",
  [string]$Destination = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Destination)) {
  $Destination = Join-Path $root "third_party\opennr-runtime"
}

# SHA-256 of the published packages. To move to a newer runtime, add its version here
# with the hashes of mochizuki-<version>.zip, mochizuki-rdna3-<version>.zip and
# mochizuki-rdna2-<version>.zip and change the -Version default. The RX 7000 and RX 6000
# packages are overlays with shaders-rdna3 and that network's data.
$known = @{
  "0.6.0-opennr" = @{
    main  = "07f4278f805a3e98ac02ebf4f342a97d14d145b6aee75b5902a73ff13159d8ac"
    rdna3 = "a60f9219623a7a045068a8b26d0507b6635dda0cfa036e08def41691193c394d"
    rdna2 = "277426f48f3b431e3698d45d72367de9259a52d03222efa113e619b4b0dddde2"
  }
  "0.5.9-opennr" = @{
    main  = "57ba906bc61f8d931840173504c5f39e3c2d81d2e66573e67d387c7736d95aa2"
    rdna3 = "47a0d9853f9d5bd030dcbd6f835050f853a0ed0b5fa63de6a7425b03237d4fd0"
  }
}
if ($known.ContainsKey($Version)) {
  if ([string]::IsNullOrWhiteSpace($Sha256)) { $Sha256 = $known[$Version].main }
  if ([string]::IsNullOrWhiteSpace($Rdna3Sha256)) { $Rdna3Sha256 = $known[$Version].rdna3 }
  if ([string]::IsNullOrWhiteSpace($Rdna2Sha256)) { $Rdna2Sha256 = $known[$Version].rdna2 }
}

# Model weights (dlssnr.bin, dlssnr-small.bin and the int4 dlssnr-int4.bin made from
# dlssnr.bin) and per-machine caches never go into the runtime folder. The int4
# tables under int4\swin and int4\vit are mochizuki's own (MIT) and stay.
function Test-Skipped([string]$relative) {
  $name = Split-Path -Leaf $relative
  return $name -like "dlssnr*.bin" -or $relative -match '(^|\\)(aco-cache|prewarm-cache)(\\|$)' -or
         $name -like "pipeline*.cache" -or $name -like "*.log" -or $name -in @("build.pending", "build.failed")
}

function Get-RuntimeZip([string]$name, [string]$sha, [string]$into) {
  if ([string]::IsNullOrWhiteSpace($sha)) { throw "No SHA-256 for $name; pass it with -Sha256 / -Rdna3Sha256." }
  $url = "https://huggingface.co/datasets/zmodelerlover/amd-nr/resolve/main/mochizuki/$Version/$name"
  $download = Join-Path ([System.IO.Path]::GetTempPath()) ("NRLive-opennr-" + [guid]::NewGuid().ToString("N"))
  New-Item -ItemType Directory -Force -Path $download | Out-Null
  try {
    $zip = Join-Path $download $name
    Write-Host "Fetching $url"
    curl.exe -L --fail --retry 5 --retry-delay 3 --silent --show-error --output $zip $url
    if ($LASTEXITCODE -ne 0) { throw "Failed to download $url" }
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant()
    if ($actual -ne $sha.ToLowerInvariant()) { throw "SHA-256 mismatch for $name ($actual)" }
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
        $target = [System.IO.Path]::GetFullPath((Join-Path $into $relative))
        if (-not $target.StartsWith($into + [System.IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
          throw "Archive entry escapes the destination: $($entry.FullName)"
        }
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
      }
    } finally {
      $archive.Dispose()
    }
  } finally {
    Remove-Item -LiteralPath $download -Recurse -Force -ErrorAction SilentlyContinue
  }
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
  Get-RuntimeZip "mochizuki-$Version.zip" $Sha256 $destinationFull
  if (-not $NoRdna3) {
    if ([string]::IsNullOrWhiteSpace($Rdna3Sha256)) {
      Write-Warning "No RX 7000 package hash for $Version; skipping shaders-rdna3."
    } else {
      Get-RuntimeZip "mochizuki-rdna3-$Version.zip" $Rdna3Sha256 $destinationFull
    }
  }
  if (-not $NoRdna2) {
    if ([string]::IsNullOrWhiteSpace($Rdna2Sha256)) {
      Write-Warning "No RX 6000 package hash for $Version; skipping its network."
    } else {
      Get-RuntimeZip "mochizuki-rdna2-$Version.zip" $Rdna2Sha256 $destinationFull
    }
  }
}

foreach ($required in @("MochizukiNrRuntime.dll", "dlssnr-amd\shaders")) {
  if (-not (Test-Path -LiteralPath (Join-Path $destinationFull $required))) { throw "OpenNR runtime is missing $required" }
}
if (Get-ChildItem -LiteralPath $destinationFull -Recurse -File -Filter "dlssnr*.bin") { throw "Model weights ended up in $destinationFull" }
Write-Host "OpenNR runtime $Version : $destinationFull"
Write-Host "Build with: scripts\build.ps1 ... -OpenNrRuntime `"$destinationFull`" (staged as opennr\ beside NRLive.exe)"
