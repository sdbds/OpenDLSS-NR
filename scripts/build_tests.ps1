# Build the headless GPU regression runners with the same toolchain as build.ps1.
param([switch]$GraphOnly, [switch]$WorkspaceOnly, [switch]$PlannerOnly, [switch]$StagingOnly)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$vcvars = & (Join-Path $PSScriptRoot "find_vcvars.ps1")
$out = Join-Path $root "build\tests"
New-Item -ItemType Directory -Force (Join-Path $out "obj") | Out-Null
$sourceNames = @("vk_context.cpp", "nr_model.cpp", "nr_graph.cpp", "kernels.cpp", "reference.cpp")
if (Test-Path -LiteralPath (Join-Path $root "src\compute_trace.cpp")) { $sourceNames += "compute_trace.cpp" }
foreach ($name in "nr_workspace.cpp", "nr_graph_workspace.cpp") {
  if (Test-Path -LiteralPath (Join-Path $root "src\$name")) { $sourceNames += $name }
}
if ($StagingOnly) { $sourceNames = @('vk_context.cpp', 'compute_trace.cpp') }
$sources = $sourceNames |
  ForEach-Object { '"' + (Join-Path $root "src\$_") + '"' }
$include = '/I"' + (Join-Path $root "tools\Vulkan-Headers\include") + '" /I"' + (Join-Path $root "tools\volk") +
           '" /I"' + (Join-Path $root "src") + '" /I"' + (Join-Path $root "demo") + '"'
$volk = '"' + (Join-Path $root "tools\volk\volk.c") + '"'
$runner = if ($StagingOnly) { 'staging_regression' } elseif ($PlannerOnly) { "workspace_plan_tests" } elseif ($WorkspaceOnly) { "workspace_regression" } else { "graph_regression" }
$test = '"' + (Join-Path $root "tests\$runner.cpp") + '"'
$cmd = "`"$vcvars`" >nul && cl /nologo /std:c++20 /EHsc /W3 /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DVK_ENABLE_BETA_EXTENSIONS $include /Fo`"$out\obj\\`" $($sources -join ' ') $test $volk /Fe:`"$out\$runner.exe`" /link /SUBSYSTEM:CONSOLE"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "$runner build failed" }
if ($GraphOnly -or $WorkspaceOnly -or $PlannerOnly -or $StagingOnly) { return }  # older references and focused test builds
$glslang = Join-Path $root "tools\glslang\bin\glslang.exe"
New-Item -ItemType Directory -Force (Join-Path $out "demo-shaders") | Out-Null
Get-ChildItem (Join-Path $root "demo\shaders\*.comp") | ForEach-Object {
  & $glslang -V --target-env vulkan1.3 -I"$root\demo\shaders" $_.FullName -o (Join-Path $out "demo-shaders\$($_.Name).spv")
  if ($LASTEXITCODE -ne 0) { throw "demo shader compilation failed" }
}
& $glslang -V --target-env vulkan1.3 (Join-Path $root "tests\history_copy.comp") -o (Join-Path $out "history_copy.spv")
if ($LASTEXITCODE -ne 0) { throw "history readback shader compilation failed" }
$test = '"' + (Join-Path $root "tests\pass_regression.cpp") + '"'
$pass = '"' + (Join-Path $root "demo\nr_pass.cpp") + '"'
$objects = ($sourceNames + "volk.c") |
  ForEach-Object { '"' + (Join-Path $out ("obj\" + [IO.Path]::GetFileNameWithoutExtension($_) + ".obj")) + '"' }
$cmd = "`"$vcvars`" >nul && cl /nologo /std:c++20 /EHsc /W3 /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DVK_ENABLE_BETA_EXTENSIONS $include /Fo`"$out\obj\\`" $test $pass $($objects -join ' ') /Fe:`"$out\pass_regression.exe`" /link /SUBSYSTEM:CONSOLE"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "pass regression build failed" }
$test = '"' + (Join-Path $root "tests\aux_regression.cpp") + '"'
$cmd = "`"$vcvars`" >nul && cl /nologo /std:c++20 /EHsc /W3 /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DVK_ENABLE_BETA_EXTENSIONS $include /Fo`"$out\obj\\`" $test $($objects -join ' ') /Fe:`"$out\aux_regression.exe`" /link /SUBSYSTEM:CONSOLE"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "aux regression build failed" }
$test = '"' + (Join-Path $root "tests\workspace_regression.cpp") + '"'
$cmd = "`"$vcvars`" >nul && cl /nologo /std:c++20 /EHsc /W3 /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DVK_ENABLE_BETA_EXTENSIONS $include /Fo`"$out\obj\\`" $test $($objects -join ' ') /Fe:`"$out\workspace_regression.exe`" /link /SUBSYSTEM:CONSOLE"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "workspace regression build failed" }
$test = '"' + (Join-Path $root "tests\workspace_plan_tests.cpp") + '"'
$cmd = "`"$vcvars`" >nul && cl /nologo /std:c++20 /EHsc /W3 /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DVK_ENABLE_BETA_EXTENSIONS $include /Fo`"$out\obj\\`" $test $($objects -join ' ') /Fe:`"$out\workspace_plan_tests.exe`" /link /SUBSYSTEM:CONSOLE"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "workspace planner build failed" }
$test = '"' + (Join-Path $root "tests\staging_regression.cpp") + '"'
$cmd = "`"$vcvars`" >nul && cl /nologo /std:c++20 /EHsc /W3 /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DVK_ENABLE_BETA_EXTENSIONS $include /Fo`"$out\obj\\`" $test $($objects -join ' ') /Fe:`"$out\staging_regression.exe`" /link /SUBSYSTEM:CONSOLE"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw 'staging regression build failed' }
