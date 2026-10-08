# Capture with the original implementation, then compare after each optimization.
param(
  [switch]$RecordBaseline,
  [switch]$Half,
  [switch]$HalfHead,
  [switch]$ReuseWorkspace,
  [switch]$FullSizes,
  [switch]$ExpertFallbacks,
  [switch]$PostFallback,
  [switch]$AuxFallbacks,
  [int]$Frames = 3,
  [string]$BaselineDirectory = "tmp/optimization/baseline-a",
  [string]$LogDirectory = "tmp/optimization/results"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ($RecordBaseline -and ($Half -or $HalfHead)) { throw "record the original F32 input and head, not FP16" }
if ($RecordBaseline -and $ReuseWorkspace) { throw "workspace candidates must not replace independent references" }
$cases = @(
  @{ Name = "default"; Args = @(); Saved = 36700160; SavedF16 = 46137344; HeadSaved = 2359296; ResidentSaved = 5111808 },
  @{ Name = "odd-boundaries"; Args = @("--width", "513", "--height", "377", "--boundaries"); Saved = 30441472; SavedF16 = 38305792; HeadSaved = 1966080; ResidentSaved = 5111808 },
  @{ Name = "unfused"; Args = @("--width", "320", "--height", "320", "--unfused", "--boundaries"); Saved = 0; SavedF16 = 6553600; HeadSaved = 819200; ResidentSaved = 0 },
  @{ Name = "intermediates"; Args = @("--width", "320", "--height", "320", "--intermediates"); Saved = 0; SavedF16 = 6553600; HeadSaved = 819200; ResidentSaved = 0 },
  @{ Name = "no-pre"; Args = @(); Env = @{ DLSS5VK_NO_FUSE_PRE = "1" }; Saved = 17825792; SavedF16 = 36700160; HeadSaved = 2359296; ResidentSaved = 5111808 },
  @{ Name = "no-pool"; Args = @(); Env = @{ DLSS5VK_NO_FUSE_POOL = "1" }; Saved = 13107200; SavedF16 = 22544384; HeadSaved = 2359296; ResidentSaved = 5111808 },
  @{ Name = "no-upres"; Args = @(); Env = @{ DLSS5VK_NO_FUSE_UPRES = "1" }; Saved = 31981568; SavedF16 = 41418752; HeadSaved = 2359296; ResidentSaved = 5111808 },
  @{ Name = "glsl-block32"; Args = @(); Env = @{ DLSS5VK_PTX_BLOCK32 = "0" }; Saved = 36700160; SavedF16 = 46137344; HeadSaved = 2359296; ResidentSaved = 5111808 }
)
if ($ExpertFallbacks) {
  $cases += @(
    @{ Name = "ptx-mlp"; Args = @(); Env = @{ DLSS5VK_PTX_FFN = "0" }; Saved = 32571392; SavedF16 = 42008576; HeadSaved = 2359296; ResidentSaved = 5111808 },
    @{ Name = "glsl-mlp"; Args = @(); Env = @{ DLSS5VK_PTX_FFN = "0"; DLSS5VK_NO_PTX_MLP = "1" }; Saved = 32571392; SavedF16 = 42008576; HeadSaved = 2359296; ResidentSaved = 0 }
  )
}
if ($PostFallback) {
  $cases += @{ Name = "no-post"; Args = @(); Env = @{ DLSS5VK_NO_FUSE_POST = "1" }; Saved = 36700160; SavedF16 = 46137344; HeadSaved = 2359296; ResidentSaved = 5111808 }
}
if ($AuxFallbacks) {
  $cases += @(
    @{ Name = "gemmt-fallback"; Args = @(); Env = @{ DLSS5VK_PTX_GEMMV = "0" }; Saved = 36700160; SavedF16 = 46137344; HeadSaved = 2359296; ResidentSaved = 5111808 },
    @{ Name = "aux-glsl"; Args = @(); Env = @{ DLSS5VK_PTX_GEMM = "0"; DLSS5VK_PTX_FFN = "0"; DLSS5VK_PTX_QKV = "0"; DLSS5VK_PTX_ATTN = "0"; DLSS5VK_PTX_BLOCK32 = "0"; DLSS5VK_NO_PTX_MLP = "1" }; Saved = 32571392; SavedF16 = 42008576; HeadSaved = 2359296; ResidentSaved = 0 }
  )
}
if ($FullSizes) {
  $cases += @(
    @{ Name = "1080p"; Args = @("--width", "1920", "--height", "1080"); Saved = 272269312; SavedF16 = 343048192; HeadSaved = 17694720; ResidentSaved = 5111808 },
    @{ Name = "1440p"; Args = @("--width", "2560", "--height", "1440"); Saved = 465633280; SavedF16 = 586219520; HeadSaved = 30146560; ResidentSaved = 5111808 },
    @{ Name = "4k"; Args = @("--width", "3840", "--height", "2160"); Saved = 1028194304; SavedF16 = 1295581184; HeadSaved = 66846720; ResidentSaved = 5111808 }
  )
}
$variables = @("DLSS5VK_NO_FUSE_PRE", "DLSS5VK_NO_FUSE_POOL", "DLSS5VK_NO_FUSE_UPRES", "DLSS5VK_PTX_BLOCK32",
               "DLSS5VK_PTX_FFN", "DLSS5VK_NO_PTX_MLP", "DLSS5VK_NO_FUSE_POST", "DLSS5VK_PTX_GEMM",
               "DLSS5VK_PTX_GEMMV", "DLSS5VK_PTX_GEMMT", "DLSS5VK_PTX_QKV", "DLSS5VK_PTX_ATTN")
$previous = @{}
foreach ($name in $variables) { $previous[$name] = [Environment]::GetEnvironmentVariable($name) }
Push-Location $root
try {
  New-Item -ItemType Directory -Force $LogDirectory | Out-Null
  # The original baseline's hash checker is case-sensitive. Verify independently
  # so both historical and current runners can use the same integrity-checked data.
  $manifest = Get-Content -LiteralPath "models/nr/manifest.json" -Raw | ConvertFrom-Json
  foreach ($stage in $manifest.stages) {
    $path = Join-Path "models/nr/model" $stage.file
    if ((Get-Item -LiteralPath $path).Length -ne $stage.packedByteLength -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $stage.sha256) {
      throw "model integrity check failed: $($stage.id)"
    }
  }
  if ($AuxFallbacks -and !$RecordBaseline) {
    Write-Host "case: compact-aux-storage"
    $output = & (Join-Path $root "build/tests/aux_regression.exe") --no-verify
    $code = $LASTEXITCODE
    $output | Tee-Object -FilePath (Join-Path $LogDirectory "compact-aux-storage.txt")
    if ($code -ne 0) { throw "compact aux regression failed" }
  }
  foreach ($case in $cases) {
    foreach ($name in $variables) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
    if ($case.Env) {
      foreach ($name in $case.Env.Keys) { Set-Item -LiteralPath "Env:$name" -Value $case.Env[$name] }
    }
    $reference = Join-Path $BaselineDirectory $case.Name
    if ($RecordBaseline -and (Test-Path (Join-Path $reference "head.bin"))) {
      throw "reference already exists: $reference; use another baseline directory"
    }
    $arguments = @("--reference", $reference, "--frames", $Frames, "--no-verify") + $case.Args
    if ($RecordBaseline) { $arguments += "--record" }
    else {
      $minimumSaved = if ($Half) { $case.SavedF16 } else { $case.Saved }
      if ($HalfHead) { $minimumSaved += $case.HeadSaved }
      $arguments += @("--minimum-saved", $minimumSaved)
      if (Test-Path (Join-Path $reference "resident-allocation-bytes.txt")) {
        $arguments += @("--minimum-resident-saved", $case.ResidentSaved)
      }
    }
    if ($Half) { $arguments += "--fp16" }
    if ($HalfHead) { $arguments += "--fp16-head" }
    if ($ReuseWorkspace) { $arguments += "--reuse-workspace" }
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
