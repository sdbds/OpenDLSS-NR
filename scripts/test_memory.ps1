# Compare the current full NrPass routes without overwriting the pinned probes/results.
param(
  [ValidateRange(1, 20)][int]$Rounds = 3,
  [ValidateRange(6, 10000)][int]$Frames = 20,
  [ValidateSet('every', 'first')][string[]]$ResetModes = @('every', 'first'),
  [string]$PinnedReport = 'build/vram-compare/results.json',
  [string]$InputDirectory = 'build/vram-compare/inputs',
  [string]$ResultsDirectory = '',
  [switch]$BuildOnly,
  [switch]$SkipBuild,
  [switch]$CompareStaging
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'build/nr-memory'
$exe = Join-Path $out 'nr_memory_probe.exe'
$manifestPath = Join-Path $out 'source-manifest.json'
function File-Identity([string[]]$Paths) {
  foreach ($path in ($Paths | Sort-Object -Unique)) {
    $file = Get-Item -LiteralPath $path
    [pscustomobject]@{ path = [IO.Path]::GetRelativePath($root, $file.FullName); bytes = $file.Length
      sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
  }
}
function Source-Identity {
  $paths = @()
  foreach ($directory in 'src', 'shaders', 'scripts/ptx', 'demo/shaders') {
    $paths += Get-ChildItem -LiteralPath (Join-Path $root $directory) -File |
      Where-Object Extension -in '.cpp', '.h', '.comp', '.glsl', '.py' | Select-Object -ExpandProperty FullName
  }
  $paths += @('demo/nr_pass.cpp', 'demo/nr_pass.h', 'demo/gpu_bridge.h', 'tests/pass_regression.cpp',
              'tests/nr_memory_probe.cpp', 'tests/gpu_memory_meter.h', 'tests/history_copy.comp',
              'scripts/test_memory.ps1', 'scripts/build_shaders.ps1') | ForEach-Object { Join-Path $root $_ }
  @(File-Identity $paths)
}
function Shader-Identity {
  $paths = foreach ($directory in 'build/shaders', 'build/ptx', 'build/nr-memory/demo-shaders') {
    Get-ChildItem -LiteralPath (Join-Path $root $directory) -File | Select-Object -ExpandProperty FullName
  }
  @(File-Identity $paths)
}
function Identity-Json($Value) { ConvertTo-Json -InputObject @($Value) -Depth 5 -Compress }
function Median($Values) {
  $sorted = @($Values | Sort-Object)
  $sorted[[int][math]::Floor($sorted.Count / 2)]
}
Push-Location $root
$previousEnvironment = @{}
$environmentChanged = $false
try {
  if (!$SkipBuild) {
    New-Item -ItemType Directory -Force -Path $out, (Join-Path $out 'obj'), (Join-Path $out 'demo-shaders') | Out-Null
    $sourcesBefore = Source-Identity
    $sourcesBefore | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'source-before-build.json') -Encoding utf8
    & ./scripts/build_shaders.ps1 *> (Join-Path $out 'shader-build.log')
    $vcvars = & ./scripts/find_vcvars.ps1
    $include = @('src', 'demo', 'tests', 'tools/volk', 'tools/Vulkan-Headers/include') |
      ForEach-Object { '/I"' + (Join-Path $root $_) + '"' }
    $sources = @('src/vk_context.cpp', 'src/nr_model.cpp', 'src/nr_graph.cpp', 'src/nr_workspace.cpp',
                 'src/nr_graph_workspace.cpp', 'src/kernels.cpp', 'src/reference.cpp', 'src/compute_trace.cpp',
                 'demo/nr_pass.cpp', 'tests/nr_memory_probe.cpp', 'tools/volk/volk.c') |
      ForEach-Object { '"' + (Join-Path $root $_) + '"' }
    $cmd = "`"$vcvars`" >nul && cl /nologo /std:c++20 /EHsc /W3 /O2 /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DVK_ENABLE_BETA_EXTENSIONS $($include -join ' ') /Fo`"$out\obj\\`" $($sources -join ' ') /Fe:`"$exe`" /link dxgi.lib"
    cmd /d /c $cmd *> (Join-Path $out 'build.log')
    if ($LASTEXITCODE -ne 0) { throw "memory probe build failed: $out/build.log" }
    $glslang = Join-Path $root 'tools/glslang/bin/glslang.exe'
    foreach ($shader in (Get-ChildItem -LiteralPath 'demo/shaders' -Filter '*.comp')) {
      & $glslang -V --target-env vulkan1.3 -I"$root/demo/shaders" $shader.FullName -o (Join-Path $out "demo-shaders/$($shader.Name).spv")
      if ($LASTEXITCODE -ne 0) { throw 'memory probe demo shader build failed' }
    }
    $sourcesAfter = Source-Identity
    $sourcesAfter | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'source-after-build.json') -Encoding utf8
    if ((Identity-Json $sourcesBefore) -ne (Identity-Json $sourcesAfter)) {
      $oldHashes = @{}
      foreach ($entry in $sourcesBefore) { $oldHashes[$entry.path] = $entry.sha256 }
      $changed = @($sourcesAfter | Where-Object { $oldHashes[$_.path] -ne $_.sha256 } | Select-Object -ExpandProperty path)
      throw "source changed during build; rebuild the candidate: $($changed -join ', ')"
    }
    [ordered]@{ revision = (git rev-parse HEAD).Trim(); dirty = [bool](git status --porcelain)
      built_utc = [DateTime]::UtcNow.ToString('o'); sources = $sourcesAfter; shaders = (Shader-Identity)
      executable = @(File-Identity @($exe))[0] } |
      ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8
  }
  if ($BuildOnly) { Write-Output "BUILT $exe"; return }
  $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
  if ((Identity-Json (Shader-Identity)) -ne (Identity-Json $manifest.shaders) -or
      @(File-Identity @($exe))[0].sha256 -ne $manifest.executable.sha256) { throw 'candidate assets changed since build' }
  $pinned = Get-Content -LiteralPath $PinnedReport -Raw | ConvertFrom-Json
  $pinnedHash = (Get-FileHash -LiteralPath $PinnedReport -Algorithm SHA256).Hash
  $modelManifest = Get-Content -LiteralPath 'models/nr/manifest.json' -Raw | ConvertFrom-Json
  foreach ($stage in $modelManifest.stages) {
    $path = Join-Path 'models/nr/model' $stage.file
    if ((Get-Item -LiteralPath $path).Length -ne $stage.packedByteLength -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $stage.sha256) { throw "model integrity failed: $($stage.id)" }
  }
  if (!$ResultsDirectory) { $ResultsDirectory = 'tmp/memory-lifetimes/measured-' + (Get-Date -Format 'yyyyMMdd-HHmmss') }
  $ResultsDirectory = if ([IO.Path]::IsPathRooted($ResultsDirectory)) { [IO.Path]::GetFullPath($ResultsDirectory) }
                      else { [IO.Path]::GetFullPath((Join-Path $root $ResultsDirectory)) }
  if (Test-Path -LiteralPath $ResultsDirectory) { throw 'results directory already exists; choose a new one' }
  New-Item -ItemType Directory -Path $ResultsDirectory | Out-Null
  Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $ResultsDirectory 'source-manifest.json')
  Copy-Item -LiteralPath $PinnedReport -Destination (Join-Path $ResultsDirectory 'pinned-results.json')
  $runExe = Join-Path $ResultsDirectory 'nr_memory_probe.exe'
  Copy-Item -LiteralPath $exe -Destination $runExe
  $sourceId = $manifest.revision.Substring(0, 7) + $(if ($manifest.dirty) { '-dirty-' } else { '-clean-' }) +
    (Get-FileHash $manifestPath -Algorithm SHA256).Hash.Substring(0, 12)
  $environmentChanged = $true
  foreach ($entry in (Get-ChildItem Env: | Where-Object Name -like 'DLSS5VK_*')) {
    $previousEnvironment[$entry.Name] = $entry.Value
    Remove-Item -LiteralPath ("Env:" + $entry.Name)
  }
  $env:DLSS5VK_PTX_DIR = Join-Path $root 'build/ptx'
  $sizes = @(@(1920,1080), @(2560,1440), @(3840,2160))
  $modes = if ($CompareStaging) { @('staging256', 'staging16') } else { @('dedicated', 'reuse') }
  $entries = @()
  foreach ($reset in $ResetModes) { foreach ($size in $sizes) {
    $width = $size[0]; $height = $size[1]
    $input = [IO.Path]::GetFullPath((Join-Path $InputDirectory "${width}x${height}.f32"))
    for ($round = 1; $round -le $Rounds; ++$round) {
      $order = if ($round % 2) { $modes } else { @($modes[1], $modes[0]) }
      foreach ($mode in $order) {
        $workspace = if ($CompareStaging) { 'reuse' } else { $mode }
        $stagingMiB = if ($mode -eq 'staging256') { 256 } elseif ($mode -eq 'staging16') { 16 } else { 0 }
        $log = Join-Path $ResultsDirectory "${width}x${height}-${reset}-${mode}-r${round}.log"
        Write-Output "Measuring $mode ${width}x${height} reset=$reset round=$round"
        $arguments = @('--width', "$width", '--height', "$height", '--frames', "$Frames", '--input', $input,
          '--model', (Join-Path $root 'models/nr'), '--shaders', (Join-Path $root 'build/shaders'),
          '--demo-shaders', (Join-Path $out 'demo-shaders'), '--workspace', $workspace, '--reset', $reset, '--source-id', $sourceId)
        if ($stagingMiB) { $arguments += @('--staging-mib', "$stagingMiB") }
        $quoted = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
        $process = Start-Process -FilePath $runExe -ArgumentList $quoted -WorkingDirectory $root -WindowStyle Hidden `
          -Wait -PassThru -RedirectStandardOutput $log -RedirectStandardError ($log + '.stderr')
        $process.WaitForExit(); $code = $process.ExitCode; $process.Dispose()
        if ($code -ne 0) { throw "memory probe failed: $log" }
        $lines = Get-Content -LiteralPath $log
        if ('MEMORY PROBE PASS' -notin $lines -or !($lines -match '^CANARY PASS:')) { throw "probe confirmation missing: $log" }
        $samples = @($lines | Where-Object { $_.StartsWith('MEM ') } | ForEach-Object { $_.Substring(4) | ConvertFrom-Json })
        $owned = @($lines | Where-Object { $_.StartsWith('OWNED ') } | ForEach-Object { $_.Substring(6) | ConvertFrom-Json })
        $gpu = @($lines | Where-Object { $_.StartsWith('GPU ') } | ForEach-Object { $_.Substring(4) | ConvertFrom-Json })
        $init = @($lines | Where-Object { $_.StartsWith('INIT ') } | ForEach-Object { $_.Substring(5) | ConvertFrom-Json })
        $output = @($lines | Where-Object { $_.StartsWith('OUTPUT {') } | ForEach-Object { $_.Substring(7) | ConvertFrom-Json })
        $baseline = @($samples | Where-Object phase -eq 'baseline')
        $steady = @($samples | Where-Object phase -like 'steady_*')
        $ownedSteady = @($owned | Where-Object phase -eq 'steady')
        if ($baseline.Count -ne 1 -or $steady.Count -ne 3 -or $ownedSteady.Count -ne 1 -or $gpu.Count -ne 1 -or $output.Count -ne 1 -or $init.Count -ne 1) {
          throw "incomplete probe samples: $log"
        }
        if ($stagingMiB -and $ownedSteady[0].staging_bytes -ne [long]$stagingMiB * 1MB) { throw 'probe ignored the staging selection' }
        $local = [long](($steady.local_bytes | Measure-Object -Average).Average)
        $shared = [long](($steady.shared_bytes | Measure-Object -Average).Average)
        $peak = [long](($samples | Where-Object { $_.phase -notlike 'canary*' -and $_.phase -ne 'after_release' } |
          Measure-Object -Property local_bytes -Maximum).Maximum)
        $entries += [pscustomobject][ordered]@{ width = $width; height = $height; reset = $reset; workspace = $workspace; mode = $mode; round = $round
          staging_bytes = $ownedSteady[0].staging_bytes; init_ms = $init[0].nr_pass_ms
          baseline_local_bytes = [long]$baseline[0].local_bytes; process_steady_local_bytes = $local
          runtime_increment_local_bytes = $local - [long]$baseline[0].local_bytes
          runtime_increment_nonlocal_bytes = $shared - [long]$baseline[0].shared_bytes
          sampled_process_peak_local_bytes = $peak; sampled_runtime_peak_local_bytes = $peak - [long]$baseline[0].local_bytes
          steady_min_local_bytes = [long](($steady.local_bytes | Measure-Object -Minimum).Minimum)
          steady_max_local_bytes = [long](($steady.local_bytes | Measure-Object -Maximum).Maximum)
          owned = $ownedSteady[0]; gpu = $gpu[0]; output_sha256 = $output[0].sha256
          adapter = @($lines -match '^ADAPTER ')[0]; log_file = Split-Path -Leaf $log }
        Write-Output ("  NR LOCAL={0:N2} MiB NON_LOCAL={1:N2} MiB" -f (($local - $baseline[0].local_bytes) / 1MB), (($shared - $baseline[0].shared_bytes) / 1MB))
        $entries | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $ResultsDirectory 'runs.json') -Encoding utf8
      }
    }
  } }
  $rows = @()
  foreach ($reset in $ResetModes) { foreach ($size in $sizes) {
    $runs = @($entries | Where-Object { $_.reset -eq $reset -and $_.width -eq $size[0] -and $_.height -eq $size[1] })
    if (@($runs.output_sha256 | Sort-Object -Unique).Count -ne 1) { throw "full-size output A/B mismatch: $size reset=$reset" }
    $old = @($pinned.rows | Where-Object { $_.width -eq $size[0] -and $_.height -eq $size[1] })[0]
    $row = [ordered]@{ width = $size[0]; height = $size[1]; reset = $reset
      original_historical_reset_every_bytes = $old.native_runtime_median_bytes
      pinned_historical_reset_every_bytes = $old.optimized_runtime_median_bytes }
    foreach ($mode in $modes) {
      $selected = @($runs | Where-Object mode -eq $mode)
      $row["${mode}_local_median_bytes"] = Median $selected.runtime_increment_local_bytes
      $row["${mode}_local_min_bytes"] = ($selected.runtime_increment_local_bytes | Measure-Object -Minimum).Minimum
      $row["${mode}_local_max_bytes"] = ($selected.runtime_increment_local_bytes | Measure-Object -Maximum).Maximum
      $row["${mode}_nonlocal_median_bytes"] = Median $selected.runtime_increment_nonlocal_bytes
      $row["${mode}_gpu_median_ms"] = Median $selected.gpu.median_ms
      $row["${mode}_init_median_ms"] = Median $selected.init_ms
    }
    if ($CompareStaging) {
      if (@($runs.owned.graph_bytes | Sort-Object -Unique).Count -ne 1) { throw 'staging comparison changed Graph allocations' }
      $row.saved_nonlocal_bytes = $row.staging256_nonlocal_median_bytes - $row.staging16_nonlocal_median_bytes
      $row.saved_local_bytes = $row.staging256_local_median_bytes - $row.staging16_local_median_bytes
      if ($row.saved_local_bytes + $row.saved_nonlocal_bytes -le 0) { throw 'smaller staging did not lower measured driver usage' }
    } else {
      $row.reuse_saved_local_bytes = $row.dedicated_local_median_bytes - $row.reuse_local_median_bytes
      if ($row.reuse_saved_local_bytes -le 0) { throw 'owned capacity savings did not reduce steady LOCAL usage' }
    }
    $rows += [pscustomobject]$row
  } }
  if ((Get-FileHash -LiteralPath $PinnedReport -Algorithm SHA256).Hash -ne $pinnedHash) { throw 'pinned report changed during measurement' }
  if ((Identity-Json (Shader-Identity)) -ne (Identity-Json $manifest.shaders)) { throw 'shader assets changed during measurement' }
  $inputs = foreach ($size in $sizes) { Join-Path $InputDirectory "$($size[0])x$($size[1]).f32" }
  [ordered]@{ metric = 'DXGI current-process LOCAL CurrentUsage on the Vulkan adapter LUID; host baseline subtracted'
    generated_utc = [DateTime]::UtcNow.ToString('o'); source_id = $sourceId; source_manifest = 'source-manifest.json'
    source_changed_after_build = (Identity-Json (Source-Identity)) -ne (Identity-Json $manifest.sources)
    gpu = (& nvidia-smi --query-gpu=name,driver_version --format=csv,noheader); rounds = $Rounds; frames_per_run = $Frames
    comparison = $(if ($CompareStaging) { 'staging256-vs-staging16; workspace=reuse' } else { 'dedicated-vs-reuse' })
    controls = @{ autoMask = $true; style = 0; intensity = 1; localTone = 1; localStructure = 1; skinStructure = -1 }
    pinned_report_sha256 = $pinnedHash; inputs = @(File-Identity $inputs); rows = $rows; runs = $entries
    caveats = @('Original DLL and 9370065 values are preserved historical reset-every controls, not freshly rerun or first-reset measurements.',
      'Candidate includes motion, preprocess, F16 features, graph, temporal surfaces and direct RGBA8 composite. No extra F32 feature buffer.',
      'Allocated bytes, logical storage extents and DXGI residency are different metrics. Reused views do not add physical allocation bytes.',
      'Peaks are sampled after completed frames, not exhaustive transient peaks. GPU times are a short-run diagnostic, not a speedup guarantee.',
      'Staging capacity is recorded per run. NON_LOCAL is reported separately; it is not added to LOCAL VRAM savings.') } |
    ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $ResultsDirectory 'results.json') -Encoding utf8
  if ($CompareStaging) {
    $rows | Format-Table width,height,reset,staging256_nonlocal_median_bytes,staging16_nonlocal_median_bytes,saved_nonlocal_bytes,saved_local_bytes -AutoSize
  } else {
    $rows | Format-Table width,height,reset,dedicated_local_median_bytes,reuse_local_median_bytes,reuse_saved_local_bytes -AutoSize
  }
  Write-Output "PASS memory comparison: $ResultsDirectory/results.json"
} finally {
  if ($environmentChanged) {
    Remove-Item -LiteralPath Env:DLSS5VK_PTX_DIR -ErrorAction SilentlyContinue
    foreach ($key in $previousEnvironment.Keys) { [Environment]::SetEnvironmentVariable($key, $previousEnvironment[$key], 'Process') }
  }
  Pop-Location
}
