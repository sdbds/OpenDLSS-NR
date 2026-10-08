param([string]$OutputDirectory = '', [string]$SourceDirectory = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (!$SourceDirectory) { $SourceDirectory = $root }
if (!$OutputDirectory) { $OutputDirectory = Join-Path $root "tmp/ngx/evidence-tests-$([guid]::NewGuid().ToString('N'))" }
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Use a new evidence test output directory' }
$fixture = Join-Path $output 'fixture'
foreach ($directory in @('scripts', 'models/nr/model', 'build/ngx', 'build/ptx', 'build/shaders', 'docs', 'tools/volk', 'tools/nvapi')) {
  New-Item -ItemType Directory -Path (Join-Path $fixture $directory) -Force | Out-Null
}
foreach ($name in @('package_ngx.ps1', 'ngx_evidence.ps1', 'test_ngx.ps1', 'benchmark_ngx.ps1')) {
  $path = Join-Path $SourceDirectory "scripts/$name"
  if (Test-Path -LiteralPath $path) { Copy-Item -LiteralPath $path -Destination (Join-Path $fixture 'scripts') }
}
# Packaging only hashes/copies these files; producer tests deliberately hit an invalid probe.
$kernels = @('nr_frame', 'nr_ops', 'mlp_e4m3_K64_H256_N64_split_native_hidden', 'gemm2_e4m3_K64_f8_n32',
  'global_normalize_e4m3_native', 'global_attention_e4m3_p64_native', 'global_attention_e4m3_p128_native',
  'global_attention_e4m3_p192_native', 'global_attention_e4m3_p256_native')
$assetPaths = @('models/nr/model/stage.bin', 'build/ptx/graph.ptx', 'build/shaders/graph.spv')
$assetPaths += $kernels | ForEach-Object { "build/ngx/$_.ptx" }
foreach ($path in ($assetPaths + @('build/ngx/nvngx_dlssnr.dll', 'build/ngx/nvngx.dll-d3d12-probe.exe',
    'docs/ngx-compatibility.md', 'LICENSE', 'tools/volk/LICENSE.md', 'tools/nvapi/nvapi_interface.h'))) {
  [IO.File]::WriteAllText((Join-Path $fixture $path), "fixture:$path")
}
$modelFile = Join-Path $fixture 'models/nr/model/stage.bin'
@{ stages = @(@{ file = 'stage.bin'; packedByteLength = (Get-Item -LiteralPath $modelFile).Length;
    sha256 = (Get-FileHash -LiteralPath $modelFile -Algorithm SHA256).Hash }) } |
  ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $fixture 'models/nr/manifest.json') -Encoding utf8
$assetPaths += 'models/nr/manifest.json'
$assets = @($assetPaths | ForEach-Object {
  @{ file = $_; sha256 = (Get-FileHash -LiteralPath (Join-Path $fixture $_) -Algorithm SHA256).Hash }
})
$dllHash = (Get-FileHash -LiteralPath (Join-Path $fixture 'build/ngx/nvngx_dlssnr.dll') -Algorithm SHA256).Hash
$validation = @{ schemaVersion = 1; kind = 'validation'; completed = $true; requestedCases = @('default', 'odd');
  replacementSHA256 = $dllHash; assets = $assets; cases = @(
    @{ case = 'default'; referenceExit = 0; replacementExit = 0; bitExact = $true },
    @{ case = 'odd'; referenceExit = 0; replacementExit = 0; bitExact = $true }) }
$benchmark = @{ schemaVersion = 1; kind = 'benchmark'; completed = $true; requestedCases = @('512');
  replacementSHA256 = $dllHash; assets = $assets; rounds = 2; frames = 4; warmup = 1; results = @(
    @{ case = '512'; round = 1; samples = 3; endpointBitExact = $true },
    @{ case = '512'; round = 2; samples = 3; endpointBitExact = $true }) }
$results = [Collections.Generic.List[object]]::new()
function Run-Case([string]$Name, [string]$Kind, [scriptblock]$Change, [bool]$Accept = $false) {
  $template = if ($Kind -eq 'validation') { $validation } else { $benchmark }
  $report = $template | ConvertTo-Json -Depth 8 | ConvertFrom-Json -AsHashtable
  & $Change $report
  $reportPath = Join-Path $output "$Name.json"
  $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding utf8
  $destination = Join-Path $output "package-$Name"
  $arguments = @{ Destination = $destination; NoZip = $true }
  $arguments[$(if ($Kind -eq 'validation') { 'ValidationResults' } else { 'BenchmarkResults' })] = $reportPath
  $errorText = ''
  try {
    & (Join-Path $fixture 'scripts/package_ngx.ps1') @arguments *> (Join-Path $output "$Name.log")
    $accepted = $true
  } catch {
    $accepted = $false
    $errorText = $_.Exception.Message
  }
  $manifestExists = Test-Path -LiteralPath (Join-Path $destination 'package-manifest.json')
  $passed = ($accepted -eq $Accept) -and ($manifestExists -eq $Accept)
  if (!$Accept -and (Test-Path -LiteralPath $destination)) { $passed = $false }
  $results.Add([pscustomobject]@{ name = $Name; passed = $passed; accepted = $accepted; error = $errorText })
  Write-Host "$(if ($passed) { 'PASS' } else { 'FAIL' }) $Name accepted=$accepted"
}
Run-Case 'valid-validation' 'validation' {} $true
Run-Case 'valid-benchmark-only' 'benchmark' {} $true
Run-Case 'legacy-validation' 'validation' { param($r) $r.Remove('schemaVersion'); $r.Remove('completed') }
Run-Case 'aborted-empty' 'validation' { param($r) $r.completed = $false; $r.cases = @() }
Run-Case 'empty-completed' 'validation' { param($r) $r.cases = @() }
Run-Case 'partial-validation' 'validation' { param($r) $r.cases = @($r.cases[0]) }
Run-Case 'duplicate-validation-row' 'validation' { param($r) $r.cases[1] = $r.cases[0] }
Run-Case 'unexpected-case' 'validation' { param($r) $r.cases[1].case = 'other' }
Run-Case 'missing-requested-cases' 'validation' { param($r) $r.Remove('requestedCases') }
Run-Case 'duplicate-requested-cases' 'validation' { param($r) $r.requestedCases = @('default', 'default') }
Run-Case 'failed-comparison' 'validation' { param($r) $r.cases[1].bitExact = $false }
Run-Case 'failed-probe' 'validation' { param($r) $r.cases[1].referenceExit = 1 }
Run-Case 'string-completed' 'validation' { param($r) $r.completed = 'true' }
Run-Case 'wrong-report-kind' 'validation' { param($r) $r.kind = 'benchmark' }
Run-Case 'benchmark-without-assets' 'benchmark' { param($r) $r.Remove('assets') }
Run-Case 'partial-benchmark' 'benchmark' { param($r) $r.results = @($r.results[0]) }
Run-Case 'duplicate-benchmark-round' 'benchmark' { param($r) $r.results[1].round = 1 }
Run-Case 'unexpected-benchmark-round' 'benchmark' { param($r) $r.results[1].round = 3 }
Run-Case 'incomplete-benchmark-samples' 'benchmark' { param($r) $r.results[1].samples = 2 }
Run-Case 'failed-benchmark-endpoint' 'benchmark' { param($r) $r.results[0].endpointBitExact = $false }
Run-Case 'empty-assets' 'validation' { param($r) $r.assets = @() }
Run-Case 'missing-asset' 'validation' { param($r) $r.assets = @($r.assets | Where-Object file -ne 'build/ngx/nr_frame.ptx') }
Run-Case 'duplicate-asset' 'validation' { param($r) $r.assets[1] = $r.assets[0] }
Run-Case 'asset-path-outside-root' 'validation' { param($r) $r.assets[1].file = '../outside.ptx' }
Run-Case 'wrong-dll' 'validation' { param($r) $r.replacementSHA256 = '0' * 64 }
$changedAsset = Join-Path $fixture 'build/ngx/nr_frame.ptx'
$before = [IO.File]::ReadAllBytes($changedAsset)
try {
  [IO.File]::WriteAllText($changedAsset, 'changed after benchmarking')
  Run-Case 'changed-benchmark-asset' 'benchmark' {}
} finally { [IO.File]::WriteAllBytes($changedAsset, $before) }
[IO.File]::WriteAllText((Join-Path $fixture 'build/ptx/added.ptx'), 'added after validation')
Run-Case 'added-asset' 'validation' {}
$previousAssetRoot = $env:OPEN_DLSS_NR_ROOT
$env:OPEN_DLSS_NR_ROOT = $fixture
try {
  foreach ($kind in @('validation', 'benchmark')) {
    $name = "aborted-$kind-report"
    $directory = Join-Path $output $name
    $script = if ($kind -eq 'validation') { 'test_ngx.ps1' } else { 'benchmark_ngx.ps1' }
    $case = if ($kind -eq 'validation') { 'default' } else { '512' }
    $arguments = @{ ReferenceDll = (Join-Path $fixture 'LICENSE');
      ReplacementDll = (Join-Path $fixture 'build/ngx/nvngx_dlssnr.dll'); OutputDirectory = $directory; Cases = @($case) }
    if ($kind -eq 'benchmark') { $arguments.Rounds = 1; $arguments.Frames = 2; $arguments.Warmup = 1 }
    $producerFailed = $false
    try { & (Join-Path $fixture "scripts/$script") @arguments *> (Join-Path $output "$name.log") }
    catch { $producerFailed = $true }
    $reportPath = Join-Path $directory 'results.json'
    $report = if (Test-Path -LiteralPath $reportPath) { Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json } else { $null }
    $passed = $producerFailed -and $report.completed -is [bool] -and !$report.completed -and
      $report.kind -eq $kind -and @($report.requestedCases).Count -eq 1 -and $report.requestedCases[0] -eq $case -and
      $report.assets.Count -gt 0
    $results.Add([pscustomobject]@{ name = $name; passed = $passed; producerFailed = $producerFailed })
    Write-Host "$(if ($passed) { 'PASS' } else { 'FAIL' }) $name explicitIncomplete=$passed"
  }
} finally { $env:OPEN_DLSS_NR_ROOT = $previousAssetRoot }
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'results.json') -Encoding utf8
$failed = @($results | Where-Object { !$_.passed })
if ($failed.Count) { throw "$($failed.Count) of $($results.Count) evidence regression cases failed; see $output" }
Write-Host "PASS $($results.Count) NGX evidence and packaging cases"
