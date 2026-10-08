param(
  [string]$ReferenceDll = "DLSS5VK_MODEL/nvngx_dlssnr.dll",
  [string]$ReplacementDll = "build/ngx/nvngx_dlssnr.dll",
  [string]$OutputDirectory = "tmp/ngx/benchmark",
  [int]$Rounds = 3,
  [int]$Frames = 120,
  [int]$Warmup = 20,
  [string[]]$Cases = @("512", "1080p", "1440p", "4k")
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ngx_evidence.ps1')
function Absolute([string]$path) {
  if ([IO.Path]::IsPathRooted($path)) { return [IO.Path]::GetFullPath($path) }
  return [IO.Path]::GetFullPath((Join-Path $root $path))
}
function Statistics([string]$path) {
  $log = Get-Content -LiteralPath $path -Raw
  $gpu = [regex]::Match($log, '(?m)^gpu_ms samples=(\d+) median=([0-9.]+) mean=([0-9.]+) min=([0-9.]+) max=([0-9.]+)')
  $cpu = [regex]::Match($log, '(?m)^cpu_record_ms samples=(\d+) median=([0-9.]+)')
  $memory = [regex]::Match($log, '(?m)^process_local_bytes_after_evaluation=(\d+)')
  if (!$gpu.Success -or !$cpu.Success -or !$memory.Success) { throw "Missing measurement in $path" }
  return [pscustomobject]@{
    samples = [int]$gpu.Groups[1].Value
    gpuMedianMs = [double]::Parse($gpu.Groups[2].Value, [Globalization.CultureInfo]::InvariantCulture)
    cpuRecordMedianMs = [double]::Parse($cpu.Groups[2].Value, [Globalization.CultureInfo]::InvariantCulture)
    processLocalBytes = [long]$memory.Groups[1].Value
  }
}
if ($Rounds -lt 1 -or $Frames -le $Warmup -or $Warmup -lt 0) { throw "Invalid round/frame/warmup counts" }
$reference = Absolute $ReferenceDll
$replacement = Absolute $ReplacementDll
$output = Absolute $OutputDirectory
if (Test-Path -LiteralPath $output) { throw "Use a new output directory: $output" }
$matrix = @{ "512" = @(512, 512); "1080p" = @(1920, 1080); "1440p" = @(2560, 1440); "4k" = @(3840, 2160) }
Assert-NgxRequestedCases $Cases
foreach ($case in $Cases) { if (!$matrix.ContainsKey($case)) { throw "Unknown benchmark case: $case" } }
$referenceHash = (Get-FileHash -LiteralPath $reference -Algorithm SHA256).Hash
$replacementHash = (Get-FileHash -LiteralPath $replacement -Algorithm SHA256).Hash
$assetRoot = Resolve-NgxAssetRoot $replacement
$assetHashes = @(Get-NgxAssetHashes $assetRoot)
New-Item -ItemType Directory -Path $output | Out-Null
$results = @()
$completed = $false
Push-Location $root
try {
  $gpu = (& nvidia-smi --query-gpu=name,driver_version --format=csv,noheader) -join "`n"
  for ($round = 1; $round -le $Rounds; ++$round) {
    foreach ($case in $Cases) {
      $dimensions = $matrix[$case]
      $directory = Join-Path $output "$case/round-$round"
      New-Item -ItemType Directory -Path $directory -Force | Out-Null
      $order = if ($round % 2) { @("reference", "replacement") } else { @("replacement", "reference") }
      $stats = @{}
      $hashes = @{}
      foreach ($kind in $order) {
        $dll = if ($kind -eq "reference") { $reference } else { $replacement }
        $capture = Join-Path $directory $kind
        $log = Join-Path $directory "$kind.log"
        & ./build/ngx/nvngx.dll-d3d12-probe.exe --dll $dll --width $dimensions[0] --height $dimensions[1] `
          --benchmark --frames $Frames --warmup $Warmup --out $capture *> $log
        if ($LASTEXITCODE -ne 0) { throw "$kind benchmark failed: $log" }
        $stats[$kind] = Statistics $log
        if ($stats[$kind].samples -ne ($Frames - $Warmup)) { throw "Incomplete benchmark samples: $log" }
        $hashes[$kind] = (Get-FileHash -LiteralPath (Join-Path $capture "frame-$($Frames - 1).rgba16f") -Algorithm SHA256).Hash
      }
      $native = $stats.reference
      $port = $stats.replacement
      $results += [pscustomobject]@{
        case = $case; round = $round; order = $order -join ","; samples = $native.samples
        nativeGpuMs = $native.gpuMedianMs; replacementGpuMs = $port.gpuMedianMs
        speedup = $native.gpuMedianMs / $port.gpuMedianMs
        nativeCpuRecordMs = $native.cpuRecordMedianMs; replacementCpuRecordMs = $port.cpuRecordMedianMs
        nativeProcessLocalBytes = $native.processLocalBytes; replacementProcessLocalBytes = $port.processLocalBytes
        endpointBitExact = $hashes.reference -eq $hashes.replacement
        nativeEndpointSHA256 = $hashes.reference; replacementEndpointSHA256 = $hashes.replacement
      }
      Write-Host "$case round=$round native=$($native.gpuMedianMs)ms replacement=$($port.gpuMedianMs)ms endpoint-exact=$($hashes.reference -eq $hashes.replacement)"
    }
  }
  if ($results | Where-Object { !$_.endpointBitExact }) { throw "One or more long-run endpoints differ; see $output" }
  if ((Get-FileHash -LiteralPath $reference -Algorithm SHA256).Hash -ne $referenceHash) { throw "Reference DLL changed during benchmarking" }
  if ((Get-FileHash -LiteralPath $replacement -Algorithm SHA256).Hash -ne $replacementHash) { throw "Replacement DLL changed during benchmarking" }
  Assert-NgxAssetHashes $assetHashes @(Get-NgxAssetHashes $assetRoot)
  $completed = $true
} finally {
  Pop-Location
  $results | Export-Csv -LiteralPath (Join-Path $output "results.csv") -NoTypeInformation -Encoding utf8
  [pscustomobject]@{
    schemaVersion = 1; kind = 'benchmark'; completed = $completed; requestedCases = @($Cases)
    gpu = $gpu; frames = $Frames; warmup = $Warmup; rounds = $Rounds
    reference = $reference; referenceSHA256 = $referenceHash
    replacement = $replacement; replacementSHA256 = $replacementHash; assetRoot = $assetRoot; assets = $assetHashes; results = $results
  } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output "results.json") -Encoding utf8
}
Write-Host "PASS: warm GPU timings and bit-exact long-run endpoints; these are not game FPS measurements"
