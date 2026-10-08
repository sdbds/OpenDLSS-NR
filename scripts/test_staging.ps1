param(
  [ValidateSet(16, 256)][int]$DefaultMiB = 16,
  [switch]$Benchmark,
  [string]$LogDirectory = 'tmp/staging/transfers'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
$previous = [Environment]::GetEnvironmentVariable('DLSS5VK_STAGING_MIB', 'Process')
try {
  if (Test-Path -LiteralPath $LogDirectory) { throw 'staging log directory exists; choose a new directory' }
  New-Item -ItemType Directory -Path $LogDirectory | Out-Null
  $exe = Join-Path $root 'build/tests/staging_regression.exe'
  $hash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
  function Run-Case([string]$Name, [string[]]$Arguments) {
    & $exe @Arguments *> (Join-Path $LogDirectory "$Name.log")
    if ($LASTEXITCODE -ne 0) {
      Get-Content -LiteralPath (Join-Path $LogDirectory "$Name.log") -Tail 8
      throw "staging case failed: $Name"
    }
    Write-Output "PASS $Name"
  }
  Remove-Item -LiteralPath Env:DLSS5VK_STAGING_MIB -ErrorAction SilentlyContinue
  Run-Case 'default' @('--expect-mib', "$DefaultMiB")
  foreach ($mib in 1, 16, 64, 256) {
    $env:DLSS5VK_STAGING_MIB = "$mib"
    Run-Case "capacity-$mib" @('--expect-mib', "$mib")
  }
  $index = 0
  foreach ($value in @('0', '-1', '257', '999999999999999999999', '16MiB', '16.5', ' 16', '16 ', '+16', '')) {
    [Environment]::SetEnvironmentVariable('DLSS5VK_STAGING_MIB', $value, 'Process')
    Run-Case "invalid-$index" @('--expect-config-error')
    ++$index
  }
  if ($Benchmark) {
    $runs = @()
    foreach ($round in 1, 2, 3) {
      $order = if ($round % 2) { @(256, 64, 16) } else { @(16, 64, 256) }
      foreach ($mib in $order) {
        $env:DLSS5VK_STAGING_MIB = "$mib"
        $name = "timing-$mib-r$round"
        Run-Case $name @('--expect-mib', "$mib", '--benchmark')
        foreach ($line in (Get-Content -LiteralPath (Join-Path $LogDirectory "$name.log"))) {
          if (!$line.StartsWith('TRANSFER ')) { continue }
          $entry = $line.Substring(9) | ConvertFrom-Json
          $entry | Add-Member -NotePropertyName round -NotePropertyValue $round
          $runs += $entry
        }
      }
    }
    if ($runs.Count -ne 27) { throw 'incomplete staging transfer timing samples' }
    [ordered]@{ executable_sha256 = $hash; warmup = 2; samples_per_payload = 5; rounds = 3; runs = $runs } |
      ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $LogDirectory 'timings.json') -Encoding utf8
  }
  if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $hash) { throw 'staging executable changed during tests' }
  Write-Output 'PASS staging policies, early validation, chunked transfer bytes and ownership'
} finally {
  if ($null -eq $previous) { Remove-Item -LiteralPath Env:DLSS5VK_STAGING_MIB -ErrorAction SilentlyContinue }
  else { [Environment]::SetEnvironmentVariable('DLSS5VK_STAGING_MIB', $previous, 'Process') }
  Pop-Location
}
