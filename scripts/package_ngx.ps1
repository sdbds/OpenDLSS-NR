param(
  [string]$Destination = "build/packages/opendlss-nr-d3d12-0.1",
  [string]$ValidationResults = "",
  [string]$BenchmarkResults = "",
  [switch]$NoZip
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ngx_evidence.ps1')
if (![IO.Path]::IsPathRooted($Destination)) { $Destination = Join-Path $root $Destination }
$destination = [IO.Path]::GetFullPath($Destination)
$zip = "$destination.zip"
if ((Test-Path -LiteralPath $destination) -or (Test-Path -LiteralPath $zip)) {
  throw "Package destination already exists; use a fresh directory: $destination"
}
$model = Join-Path $root "models/nr"
$manifest = Get-Content -LiteralPath (Join-Path $model "manifest.json") -Raw | ConvertFrom-Json
foreach ($stage in $manifest.stages) {
  if ([IO.Path]::GetFileName($stage.file) -ne $stage.file) { throw "Unsafe model stage filename" }
  $file = Join-Path $model "model/$($stage.file)"
  if ((Get-Item -LiteralPath $file).Length -ne $stage.packedByteLength) { throw "Model size mismatch: $file" }
  if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $stage.sha256) { throw "Model hash mismatch: $file" }
}
$required = @("build/ngx/nvngx_dlssnr.dll", "build/ngx/nvngx.dll-d3d12-probe.exe", "docs/ngx-compatibility.md", "LICENSE")
$kernels = @("nr_frame", "nr_ops", "mlp_e4m3_K64_H256_N64_split_native_hidden", "gemm2_e4m3_K64_f8_n32",
  "global_normalize_e4m3_native", "global_attention_e4m3_p64_native", "global_attention_e4m3_p128_native",
  "global_attention_e4m3_p192_native", "global_attention_e4m3_p256_native")
$required += $kernels | ForEach-Object { "build/ngx/$_.ptx" }
foreach ($file in $required) {
  if (!(Test-Path -LiteralPath (Join-Path $root $file) -PathType Leaf)) { throw "Missing package input: $file" }
}
$dllHash = (Get-FileHash -LiteralPath (Join-Path $root "build/ngx/nvngx_dlssnr.dll") -Algorithm SHA256).Hash
foreach ($inputReport in @(@{ path = $ValidationResults; kind = 'validation' }, @{ path = $BenchmarkResults; kind = 'benchmark' })) {
  $reportPath = $inputReport.path
  if (!$reportPath) { continue }
  $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
  if ($report.schemaVersion -ne 1 -or $report.kind -cne $inputReport.kind -or
      $report.completed -isnot [bool] -or !$report.completed) {
    throw "Report is not a completed $($inputReport.kind) run; regenerate it: $reportPath"
  }
  Assert-NgxRequestedCases @($report.requestedCases)
  if ($report.replacementSHA256 -ne $dllHash) { throw "Validation used a different DLL: $reportPath" }
  if ($inputReport.kind -eq 'validation') {
    if (@($report.cases).Count -ne @($report.requestedCases).Count) { throw "Incomplete validation: $reportPath" }
    foreach ($case in $report.requestedCases) {
      $rows = @($report.cases | Where-Object { $_.case -ceq $case })
      if ($rows.Count -ne 1 -or $rows[0].bitExact -isnot [bool] -or !$rows[0].bitExact -or
          $rows[0].referenceExit -ne 0 -or $rows[0].replacementExit -ne 0) {
        throw "Missing, duplicate or failed validation case: $case"
      }
    }
  } else {
    foreach ($field in @('rounds', 'frames', 'warmup')) {
      if ($report.$field -isnot [int] -and $report.$field -isnot [long]) { throw "Invalid benchmark $field" }
    }
    if ($report.rounds -lt 1 -or $report.warmup -lt 0 -or $report.frames -le $report.warmup -or
        @($report.results).Count -ne (@($report.requestedCases).Count * $report.rounds)) {
      throw "Incomplete benchmark: $reportPath"
    }
    foreach ($case in $report.requestedCases) {
      for ($round = 1; $round -le $report.rounds; ++$round) {
        $rows = @($report.results | Where-Object { $_.case -ceq $case -and $_.round -eq $round })
        if ($rows.Count -ne 1 -or $rows[0].endpointBitExact -isnot [bool] -or !$rows[0].endpointBitExact -or
            $rows[0].samples -ne ($report.frames - $report.warmup)) {
          throw "Missing, duplicate or failed benchmark case: $case round=$round"
        }
      }
    }
  }
  Assert-NgxAssetHashes @($report.assets) @(Get-NgxAssetHashes $root)
}
$data = Join-Path $destination "opendlss-nr"
foreach ($path in @($destination, "$data/models/nr/model", "$data/build/ptx", "$data/build/shaders", "$data/build/ngx", "$destination/licenses")) {
  New-Item -ItemType Directory -Path $path -Force | Out-Null
}
Copy-Item -LiteralPath (Join-Path $root "build/ngx/nvngx_dlssnr.dll") -Destination $destination
Copy-Item -LiteralPath (Join-Path $root "build/ngx/nvngx.dll-d3d12-probe.exe") -Destination $destination
Copy-Item -LiteralPath (Join-Path $root "docs/ngx-compatibility.md") -Destination (Join-Path $destination "README.md")
Copy-Item -LiteralPath (Join-Path $root "LICENSE") -Destination (Join-Path $destination "licenses/OpenDLSS-NR.txt")
Copy-Item -LiteralPath (Join-Path $root "tools/volk/LICENSE.md") -Destination (Join-Path $destination "licenses/volk.txt")
Copy-Item -LiteralPath (Join-Path $root "tools/nvapi/nvapi_interface.h") -Destination (Join-Path $destination "licenses/NVAPI-interface.h")
if ($ValidationResults) { Copy-Item -LiteralPath $ValidationResults -Destination (Join-Path $destination "validation.json") }
if ($BenchmarkResults) { Copy-Item -LiteralPath $BenchmarkResults -Destination (Join-Path $destination "benchmark.json") }
Copy-Item -LiteralPath (Join-Path $model "manifest.json") -Destination (Join-Path $data "models/nr")
foreach ($stage in $manifest.stages) {
  Copy-Item -LiteralPath (Join-Path $model "model/$($stage.file)") -Destination (Join-Path $data "models/nr/model")
}
foreach ($kind in @(@("ptx", "*.ptx"), @("shaders", "*.spv"))) {
  $files = @(Get-ChildItem -LiteralPath (Join-Path $root "build/$($kind[0])") -Filter $kind[1] -File)
  if (!$files.Count) { throw "No compiled $($kind[0]) assets" }
  foreach ($file in $files) { Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $data "build/$($kind[0])") }
}
foreach ($name in $kernels) {
  Copy-Item -LiteralPath (Join-Path $root "build/ngx/$name.ptx") -Destination (Join-Path $data "build/ngx")
}
$files = @(Get-ChildItem -LiteralPath $destination -File -Recurse | Sort-Object FullName)
$checksums = foreach ($file in $files) {
  [pscustomobject]@{ file = [IO.Path]::GetRelativePath($destination, $file.FullName).Replace('\', '/');
    bytes = $file.Length; sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
}
[pscustomobject]@{ product = "OpenDLSS-NR"; version = "0.1.0"; backend = "D3D12"; files = $checksums } |
  ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $destination "package-manifest.json") -Encoding utf8
if (!$NoZip) {
  $entries = @(Get-ChildItem -LiteralPath $destination | ForEach-Object { $_.FullName })
  Compress-Archive -LiteralPath $entries -DestinationPath $zip
}
Write-Host "Package: $destination"
if (!$NoZip) { Write-Host "Archive: $zip" }
