param([string]$OutputDirectory = 'build/tests/ngx-prepare')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (![IO.Path]::IsPathRooted($OutputDirectory)) { $OutputDirectory = Join-Path $root $OutputDirectory }
$out = [IO.Path]::GetFullPath($OutputDirectory)
$obj = Join-Path $out 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null
$vcvars = & (Join-Path $PSScriptRoot 'find_vcvars.ps1')
$cuda = $env:CUDA_PATH
if (!$cuda) { $cuda = Split-Path -Parent (Split-Path -Parent (Get-Command nvcc -ErrorAction Stop).Source) }
$nvcc = Join-Path $cuda 'bin/nvcc.exe'
$source = Join-Path $root 'tests/ngx_prepare_store_test.cu'
$ptx = Join-Path $out 'ngx_prepare_store_test.ptx'
$cmd = "`"$vcvars`" >nul && `"$nvcc`" --ptx -arch=sm_89 --std=c++17 --fmad=false -o `"$ptx`" `"$source`""
cmd /d /c $cmd
if ($LASTEXITCODE -ne 0) { throw 'Prepare store test PTX compilation failed' }
$include = @('tests','ngx','tools/nvapi') | ForEach-Object { '/I"' + (Join-Path $root $_) + '"' }
$source = Join-Path $root 'tests/ngx_prepare_store_test.cpp'
$nvapi = Join-Path $root 'ngx/d3d12_nvapi.cpp'
$exe = Join-Path $out 'ngx_prepare_store_test.exe'
$flags = '/nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS'
$cmd = "`"$vcvars`" >nul && cl $flags $($include -join ' ') `"$source`" `"$nvapi`" /Fo`"$obj\\`" /Fe:`"$exe`" /link /SUBSYSTEM:CONSOLE d3d12.lib dxgi.lib"
cmd /d /c $cmd
if ($LASTEXITCODE -ne 0) { throw 'Prepare store test build failed' }
& $exe $ptx
if ($LASTEXITCODE -ne 0) { throw 'Prepare store bit-pattern test failed' }
