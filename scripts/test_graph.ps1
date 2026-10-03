# Capture with the original implementation, then compare after each optimization.
param(
  [switch]$RecordBaseline,
  [switch]$Half,
  [switch]$FullSizes,
  [int]$Frames = 3,
  [string]$BaselineDirectory = "tmp/optimization/baseline-a",
  [string]$LogDirectory = "tmp/optimization/results"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ($RecordBaseline -and $Half) { throw "record the original F32 path, not FP16" }
$cases = @(
  @{ Name = "default"; Args = @(); Saved = 32571392; SavedF16 = 42008576 },
  @{ Name = "odd-boundaries"; Args = @("--width", "513", "--height", "377", "--boundaries"); Saved = 27000832; SavedF16 = 34865152 },
  @{ Name = "unfused"; Args = @("--width", "320", "--height", "320", "--unfused", "--boundaries"); Saved = 0; SavedF16 = 6553600 },
  @{ Name = "intermediates"; Args = @("--width", "320", "--height", "320", "--intermediates"); Saved = 0; SavedF16 = 6553600 },
  @{ Name = "no-pre"; Args = @(); Env = "DLSS5VK_NO_FUSE_PRE"; Value = "1"; Saved = 13697024; SavedF16 = 32571392 },
  @{ Name = "no-pool"; Args = @(); Env = "DLSS5VK_NO_FUSE_POOL"; Value = "1"; Saved = 8978432; SavedF16 = 18415616 },
  @{ Name = "no-upres"; Args = @(); Env = "DLSS5VK_NO_FUSE_UPRES"; Value = "1"; Saved = 27852800; SavedF16 = 37289984 },
  @{ Name = "glsl-block32"; Args = @(); Env = "DLSS5VK_PTX_BLOCK32"; Value = "0"; Saved = 32571392; SavedF16 = 42008576 }
)
if ($FullSizes) {
  $cases += @(
    @{ Name = "1080p"; Args = @("--width", "1920", "--height", "1080"); Saved = 241303552; SavedF16 = 312082432 },
    @{ Name = "1440p"; Args = @("--width", "2560", "--height", "1440"); Saved = 412876800; SavedF16 = 533463040 },
    @{ Name = "4k"; Args = @("--width", "3840", "--height", "2160"); Saved = 911212544; SavedF16 = 1178599424 }
  )
}
$variables = @("DLSS5VK_NO_FUSE_PRE", "DLSS5VK_NO_FUSE_POOL", "DLSS5VK_NO_FUSE_UPRES", "DLSS5VK_PTX_BLOCK32")
$previous = @{}
foreach ($name in $variables) { $previous[$name] = [Environment]::GetEnvironmentVariable($name) }
Push-Location $root
try {
  New-Item -ItemType Directory -Force $LogDirectory | Out-Null
  # Model's current hash checker compares uppercase digests to lowercase manifests.
  # Verify independently here; do not change model data or disable integrity checking.
  $manifest = Get-Content -LiteralPath "models/nr/manifest.json" -Raw | ConvertFrom-Json
  foreach ($stage in $manifest.stages) {
    $path = Join-Path "models/nr/model" $stage.file
    if ((Get-Item -LiteralPath $path).Length -ne $stage.packedByteLength -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $stage.sha256) {
      throw "model integrity check failed: $($stage.id)"
    }
  }
  foreach ($case in $cases) {
    foreach ($name in $variables) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
    if ($case.Env) { Set-Item -LiteralPath "Env:$($case.Env)" -Value $case.Value }
    $reference = Join-Path $BaselineDirectory $case.Name
    if ($RecordBaseline -and (Test-Path (Join-Path $reference "head.bin"))) {
      throw "reference already exists: $reference; use another baseline directory"
    }
    $arguments = @("--reference", $reference, "--frames", $Frames, "--no-verify") + $case.Args
    if ($RecordBaseline) { $arguments += "--record" }
    else { $arguments += @("--minimum-saved", $(if ($Half) { $case.SavedF16 } else { $case.Saved })) }
    if ($Half) { $arguments += "--fp16" }
    Write-Host "case: $($case.Name)"
    $output = & (Join-Path $root "build/tests/graph_regression.exe") @arguments
    $code = $LASTEXITCODE
    $output | Tee-Object -FilePath (Join-Path $LogDirectory "$($case.Name).txt")
    if ($code -ne 0) { throw "graph regression failed: $($case.Name)" }
  }
} finally {
  Pop-Location
  foreach ($name in $variables) {
    if ($null -eq $previous[$name]) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
    else { Set-Item -LiteralPath "Env:$name" -Value $previous[$name] }
  }
}
