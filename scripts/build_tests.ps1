# Build the headless GPU regression runners with the same toolchain as build.ps1.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$vcvars = & (Join-Path $PSScriptRoot "find_vcvars.ps1")
$out = Join-Path $root "build\tests"
New-Item -ItemType Directory -Force (Join-Path $out "obj") | Out-Null
$sources = @("vk_context.cpp", "nr_model.cpp", "nr_graph.cpp", "kernels.cpp", "reference.cpp") |
  ForEach-Object { '"' + (Join-Path $root "src\$_") + '"' }
$include = '/I"' + (Join-Path $root "tools\Vulkan-Headers\include") + '" /I"' + (Join-Path $root "tools\volk") +
           '" /I"' + (Join-Path $root "src") + '" /I"' + (Join-Path $root "demo") + '"'
$volk = '"' + (Join-Path $root "tools\volk\volk.c") + '"'
$test = '"' + (Join-Path $root "tests\graph_regression.cpp") + '"'
$cmd = "`"$vcvars`" >nul && cl /nologo /std:c++20 /EHsc /W3 /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DVK_ENABLE_BETA_EXTENSIONS $include /Fo`"$out\obj\\`" $($sources -join ' ') $test $volk /Fe:`"$out\graph_regression.exe`" /link /SUBSYSTEM:CONSOLE"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "graph regression build failed" }
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
$objects = @("vk_context", "nr_model", "nr_graph", "kernels", "reference", "volk") |
  ForEach-Object { '"' + (Join-Path $out "obj\$_.obj") + '"' }
$cmd = "`"$vcvars`" >nul && cl /nologo /std:c++20 /EHsc /W3 /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DVK_ENABLE_BETA_EXTENSIONS $include /Fo`"$out\obj\\`" $test $pass $($objects -join ' ') /Fe:`"$out\pass_regression.exe`" /link /SUBSYSTEM:CONSOLE"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "pass regression build failed" }
