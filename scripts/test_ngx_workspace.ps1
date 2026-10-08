param(
  [string]$Dll = 'build/ngx/nvngx_dlssnr.dll',
  [string]$Probe = 'build/ngx/nvngx.dll-d3d12-probe.exe',
  [string]$OutputDirectory = 'tmp/ngx/workspace',
  [ValidateSet('dedicated', 'reuse')][string]$ExpectedDefault = 'reuse'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
function Absolute([string]$path) {
  if ([IO.Path]::IsPathRooted($path)) { return [IO.Path]::GetFullPath($path) }
  [IO.Path]::GetFullPath((Join-Path $root $path))
}
$dllPath = Absolute $Dll
$probePath = Absolute $Probe
$out = Absolute $OutputDirectory
if (Test-Path -LiteralPath $out) { throw 'Use a new workspace test output directory' }
New-Item -ItemType Directory -Path $out | Out-Null
$dllHash = (Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash
$previous = [Environment]::GetEnvironmentVariable('OPEN_DLSS_NR_WORKSPACE', 'Process')
function Set-Workspace($value) {
  if ($null -eq $value) { Remove-Item -LiteralPath Env:OPEN_DLSS_NR_WORKSPACE -ErrorAction SilentlyContinue }
  else { [Environment]::SetEnvironmentVariable('OPEN_DLSS_NR_WORKSPACE', $value, 'Process') }
}
function Run-Policy([string]$name, $value, [switch]$Invalid) {
  Set-Workspace $value
  $log = Join-Path $out "$name.log"
  & $probePath --dll $dllPath --width 512 --height 512 --capabilities --create-only *> $log
  $exitCode = $LASTEXITCODE
  $text = Get-Content -LiteralPath $log -Raw
  if ($Invalid) {
    if ($exitCode -eq 0 -or $text -notmatch 'D3D12 CreateFeature: 0xbad00005') {
      throw "Invalid workspace configuration was not rejected as InvalidParameter: $name"
    }
    Write-Host "PASS $name rejected before feature creation"
    return
  }
  $stats = [regex]::Match($text, '(?m)^NGX stats bytes=(\d+)')
  if ($exitCode -ne 0 -or !$stats.Success -or $text -notmatch 'PASS lifecycle only') {
    throw "Workspace policy failed: $name; see $log"
  }
  [long]$stats.Groups[1].Value
}
Push-Location $root
try {
  $dedicated = Run-Policy 'dedicated' '0'
  $reuse = Run-Policy 'reuse' '1'
  if ($reuse -le 0 -or $reuse -ge $dedicated) { throw 'Workspace selection did not lower actual D3D12 owned allocations' }
  Write-Host "PASS owned allocations dedicated=$dedicated reuse=$reuse"
  $default = Run-Policy 'default' $null
  $expected = if ($ExpectedDefault -eq 'reuse') { $reuse } else { $dedicated }
  if ($default -ne $expected) { throw "Default workspace allocation policy is not $ExpectedDefault" }
  $index = 0
  foreach ($value in @('', '2', '-1', 'yes', '01', ' 1', '1 ')) {
    Run-Policy "invalid-$index" $value -Invalid
    ++$index
  }
  [ordered]@{ dll=$dllPath; dll_sha256=$dllHash; dedicated_owned_bytes=$dedicated;
    reuse_owned_bytes=$reuse; default_owned_bytes=$default; expected_default=$ExpectedDefault } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $out 'results.json') -Encoding utf8
  Write-Host 'PASS D3D12 workspace selection, rollback, default and invalid configuration'
} finally {
  Set-Workspace $previous
  Pop-Location
  if ((Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash -ne $dllHash) { throw 'DLL changed during workspace tests' }
}
