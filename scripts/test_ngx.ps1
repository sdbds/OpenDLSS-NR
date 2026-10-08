param(
  [string]$ReferenceDll = "DLSS5VK_MODEL/nvngx_dlssnr.dll",
  [string]$ReplacementDll = "build/ngx/nvngx_dlssnr.dll",
  [string]$OutputDirectory = "tmp/ngx/comparison",
  [string[]]$Cases = @("default", "odd", "reset", "disabled", "intensity", "controls", "motion", "fractional-motion")
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ngx_evidence.ps1')
function Absolute([string]$path) {
  if ([IO.Path]::IsPathRooted($path)) { return [IO.Path]::GetFullPath($path) }
  return [IO.Path]::GetFullPath((Join-Path $root $path))
}
$reference = Absolute $ReferenceDll
$replacement = Absolute $ReplacementDll
$output = Absolute $OutputDirectory
if (Test-Path -LiteralPath $output) { throw "Use a new output directory to preserve earlier captures: $output" }
$referenceHash = (Get-FileHash -LiteralPath $reference -Algorithm SHA256).Hash
$replacementHash = (Get-FileHash -LiteralPath $replacement -Algorithm SHA256).Hash
$assetRoot = Resolve-NgxAssetRoot $replacement
$assetHashes = @(Get-NgxAssetHashes $assetRoot)
$matrix = [ordered]@{
  "default" = @("--frames", "3")
  "odd" = @("--width", "513", "--height", "377", "--frames", "3")
  "reset" = @("--frames", "5", "--reset-every", "2")
  "disabled" = @("--frames", "2", "--disabled")
  "intensity" = @("--frames", "3", "--intensity", "0.45")
  "controls" = @("--frames", "3", "--no-auto-mask", "--tone", "0.75", "--structure", "0.35", "--skin", "0.5")
  "motion" = @("--frames", "3", "--motion-x", "2", "--motion-y", "-1")
  "fractional-motion" = @("--frames", "3", "--motion-x", "0.375", "--motion-y", "-0.875")
  "toggle" = @("--frames", "4", "--disable-frame", "1")
  "alpha" = @("--frames", "3", "--input-alpha", "0.25")
  "disabled-alpha" = @("--frames", "2", "--disabled", "--input-alpha", "0.25")
  "rotation" = @("--frames", "20", "--rotate-resources")
  "motion-field" = @("--frames", "5", "--motion-field")
  "zero-intensity" = @("--frames", "4", "--zero-intensity-frame", "1")
  "skin" = @("--frames", "3", "--tone", "0.75", "--structure", "0.35", "--skin", "0.5")
  "1080p" = @("--width", "1920", "--height", "1080", "--frames", "3")
  "1440p" = @("--width", "2560", "--height", "1440", "--frames", "3")
  "4k" = @("--width", "3840", "--height", "2160", "--frames", "3")
}
Assert-NgxRequestedCases $Cases
foreach ($case in $Cases) { if (!$matrix.Contains($case)) { throw "Unknown test case: $case" } }
New-Item -ItemType Directory -Path $output | Out-Null
$results = @()
$completed = $false
Push-Location $root
try {
  foreach ($case in $Cases) {
    $arguments = $matrix[$case]
    $expected = Join-Path $output "$case/reference"
    $actual = Join-Path $output "$case/replacement"
    New-Item -ItemType Directory -Force (Join-Path $output $case) | Out-Null
    & ./build/ngx/nvngx.dll-d3d12-probe.exe --dll $reference --out $expected @arguments *> (Join-Path $output "$case/reference.log")
    $nativeExit = $LASTEXITCODE
    if ($nativeExit -ne 0) { throw "Reference failed for $case (exit $nativeExit); see $output/$case/reference.log" }
    & ./build/ngx/nvngx.dll-d3d12-probe.exe --dll $replacement --out $actual --reference $expected @arguments *> (Join-Path $output "$case/replacement.log")
    $exitCode = $LASTEXITCODE
    $results += [pscustomobject]@{ case = $case; referenceExit = $nativeExit; replacementExit = $exitCode; bitExact = ($exitCode -eq 0) }
    Write-Host "$case bit-exact=$($exitCode -eq 0)"
  }
  if ($results | Where-Object { !$_.bitExact }) { throw "One or more NGX comparisons failed: $output" }
  if ((Get-FileHash -LiteralPath $reference -Algorithm SHA256).Hash -ne $referenceHash) { throw "Reference DLL changed during testing" }
  if ((Get-FileHash -LiteralPath $replacement -Algorithm SHA256).Hash -ne $replacementHash) { throw "Replacement DLL changed during testing" }
  Assert-NgxAssetHashes $assetHashes @(Get-NgxAssetHashes $assetRoot)
  $completed = $true
} finally {
  Pop-Location
  [pscustomobject]@{ schemaVersion = 1; kind = 'validation'; completed = $completed; requestedCases = @($Cases);
    reference = $reference; referenceSHA256 = $referenceHash; replacement = $replacement;
    replacementSHA256 = $replacementHash; assetRoot = $assetRoot; assets = $assetHashes; cases = $results } | ConvertTo-Json -Depth 5 |
    Set-Content -LiteralPath (Join-Path $output "results.json") -Encoding utf8
}
Write-Host "PASS: all requested cases bit-exact; reference DLL unchanged"
