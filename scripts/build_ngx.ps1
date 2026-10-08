param([switch]$ParametersOnly, [switch]$NvapiOnly, [switch]$CoreOnly, [switch]$GraphOnly, [switch]$WorkspaceOnly, [switch]$DllOnly, [switch]$KernelsOnly, [switch]$D3d12ProbeOnly)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root "build/ngx"
$obj = Join-Path $out "obj"
New-Item -ItemType Directory -Force $obj | Out-Null
if (!(Test-Path (Join-Path $root "tools/ngx/nvsdk_ngx.h"))) {
  throw "run scripts/fetch_ngx.ps1 first"
}
$vcvars = & (Join-Path $PSScriptRoot "find_vcvars.ps1")
$include = '/I"' + (Join-Path $root "tools/ngx") + '" /I"' + (Join-Path $root "tests") + '" /I"' + (Join-Path $root "src") + '"'
$flags = "/nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DNGX_SNIPPET_BUILD"
if ($GraphOnly -or $WorkspaceOnly -or $DllOnly -or $KernelsOnly) {
  $include += ' /I"' + (Join-Path $root "ngx") + '" /I"' + (Join-Path $root "tools/nvapi") + '" /I"' + (Join-Path $root "tools/Vulkan-Headers/include") + '" /I"' + (Join-Path $root "tools/volk") + '"'
  $flags += " /DVK_ENABLE_BETA_EXTENSIONS"
  $cuda = $env:CUDA_PATH
  if (!$cuda) { $cuda = Split-Path -Parent (Split-Path -Parent (Get-Command nvcc -ErrorAction Stop).Source) }
  $nvcc = Join-Path $cuda "bin/nvcc.exe"
  foreach ($kernel in @("nr_ops", "nr_frame")) {
    $source = Join-Path $root "ngx/$kernel.cu"
    $cmd = "`"$vcvars`" >nul && `"$nvcc`" --ptx -arch=sm_89 --std=c++17 --fmad=false -o `"$out\$kernel.ptx`" `"$source`""
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "CUDA kernel build failed: $kernel" }
  }
  & python (Join-Path $root "scripts/ptx/ngx_kernels.py") $out
  if ($LASTEXITCODE -ne 0) { throw "NGX PTX generation failed" }
  if ($KernelsOnly) { return }
  $paths = @("src/vk_context.cpp", "src/compute_trace.cpp", "src/nr_model.cpp", "src/nr_graph.cpp", "src/nr_graph_workspace.cpp", "src/nr_workspace.cpp", "src/kernels.cpp", "src/reference.cpp", "tools/volk/volk.c", "ngx/d3d12_nvapi.cpp", "ngx/d3d12_graph.cpp")
  if ($DllOnly) { $paths += @("ngx/d3d12_feature.cpp", "ngx/exports.cpp") }
  else {
    $runner = if ($WorkspaceOnly) { 'd3d12_workspace_test' } else { 'd3d12_graph_test' }
    $paths += "tests/$runner.cpp"
  }
  $sources = $paths |
    ForEach-Object { '"' + (Join-Path $root $_) + '"' }
  if ($DllOnly) {
    $definition = Join-Path $root "ngx/nvngx_dlssnr.def"
    $resource = Join-Path $root "ngx/version.rc"
    $cmd = "`"$vcvars`" >nul && rc /nologo /fo`"$obj\version.res`" `"$resource`" && cl $flags /MT /LD $include /Fo`"$obj\\`" $($sources -join ' ') `"$obj\version.res`" /Fe:`"$out\nvngx_dlssnr.dll`" /link /DEF:`"$definition`" d3d12.lib dxgi.lib"
  } else {
    $cmd = "`"$vcvars`" >nul && cl $flags $include /Fo`"$obj\\`" $($sources -join ' ') /Fe:`"$out\$runner.exe`" /link /SUBSYSTEM:CONSOLE d3d12.lib dxgi.lib"
  }
  cmd /c $cmd
  if ($LASTEXITCODE -ne 0) { throw "NGX backend build failed" }
  return
}
if ($CoreOnly) {
  $include += ' /I"' + (Join-Path $root "tools/Vulkan-Headers/include") + '" /I"' + (Join-Path $root "tools/volk") + '"'
  $flags += " /DVK_ENABLE_BETA_EXTENSIONS"
  $test = Join-Path $root "tests/ngx_core_tests.cpp"
  $implementation = Join-Path $root "src/compute_trace.cpp"
  $cmd = "`"$vcvars`" >nul && cl $flags $include /Fo`"$obj\\`" `"$test`" `"$implementation`" /Fe:`"$out\ngx_core_tests.exe`" /link /SUBSYSTEM:CONSOLE"
  cmd /c $cmd
  if ($LASTEXITCODE -ne 0) { throw "NGX core test build failed" }
  return
}
if ($NvapiOnly) {
  $include += ' /I"' + (Join-Path $root "ngx") + '" /I"' + (Join-Path $root "tools/nvapi") + '"'
  $implementation = Join-Path $root "ngx/d3d12_nvapi.cpp"
  foreach ($name in @("nvapi_d3d12_test", "ngx_normalization_test")) {
    $test = Join-Path $root "tests/$name.cpp"
    $cmd = "`"$vcvars`" >nul && cl $flags $include /Fo`"$obj\\`" `"$test`" `"$implementation`" /Fe:`"$out\$name.exe`" /link /SUBSYSTEM:CONSOLE d3d12.lib dxgi.lib"
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "NVAPI D3D12 test build failed: $name" }
  }
  return
}
if (!$D3d12ProbeOnly) {
$source = Join-Path $root "tests/ngx_parameter_tests.cpp"
$cmd = "`"$vcvars`" >nul && cl $flags $include /Fo`"$obj\\`" `"$source`" /Fe:`"$out\ngx_parameter_tests.exe`" /link /SUBSYSTEM:CONSOLE"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "NGX parameter test build failed" }
if ($ParametersOnly) { return }
$cuda = $env:CUDA_PATH
if (!$cuda) {
  $nvcc = Get-Command nvcc -ErrorAction Stop
  $cuda = Split-Path -Parent (Split-Path -Parent $nvcc.Source)
}
if (!(Test-Path (Join-Path $cuda "include/cuda.h"))) { throw "CUDA toolkit headers not found: $cuda" }
$include += ' /I"' + (Join-Path $cuda "include") + '"'
$source = Join-Path $root "tests/ngx_probe.cpp"
$library = Join-Path $cuda "lib/x64/cuda.lib"
$cmd = "`"$vcvars`" >nul && cl $flags $include /Fo`"$obj\\`" `"$source`" /Fe:`"$out\nvngx.dll-probe.exe`" /link /SUBSYSTEM:CONSOLE `"$library`""
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "NGX probe build failed" }
Write-Host "built $out\nvngx.dll-probe.exe"
}
$source = Join-Path $root "tests/ngx_d3d12_probe.cpp"
$include += ' /I"' + (Join-Path $root "tools/nvapi") + '"'
$cmd = "`"$vcvars`" >nul && cl $flags $include /Fo`"$obj\\`" `"$source`" /Fe:`"$out\nvngx.dll-d3d12-probe.exe`" /link /SUBSYSTEM:CONSOLE d3d12.lib dxgi.lib"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "NGX D3D12 probe build failed" }
Write-Host "built $out\nvngx.dll-d3d12-probe.exe"
if ($D3d12ProbeOnly) { return }
$source = Join-Path $root "tests/ngx_d3d11_probe.cpp"
$cmd = "`"$vcvars`" >nul && cl $flags $include /Fo`"$obj\\`" `"$source`" /Fe:`"$out\nvngx.dll-d3d11-probe.exe`" /link /SUBSYSTEM:CONSOLE d3d11.lib dxgi.lib"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "NGX D3D11 probe build failed" }
Write-Host "built $out\nvngx.dll-d3d11-probe.exe"
