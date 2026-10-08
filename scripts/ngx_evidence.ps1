function Resolve-NgxAssetRoot([string]$ReplacementDll) {
  $directory = Split-Path -Parent $ReplacementDll
  $candidates = @((Join-Path $directory 'opendlss-nr'), $directory,
    (Split-Path -Parent (Split-Path -Parent $directory)))
  if ($env:OPEN_DLSS_NR_ROOT) {
    if (![IO.Path]::IsPathFullyQualified($env:OPEN_DLSS_NR_ROOT)) { throw 'OPEN_DLSS_NR_ROOT must be absolute' }
    $candidates = @($env:OPEN_DLSS_NR_ROOT)
  }
  foreach ($candidate in $candidates) {
    if (Test-Path -LiteralPath (Join-Path $candidate 'models/nr/manifest.json') -PathType Leaf) {
      return [IO.Path]::GetFullPath($candidate)
    }
  }
  throw "Cannot find the replacement's model/assets"
}

function Get-NgxAssetHashes([string]$AssetRoot) {
  $manifestPath = Join-Path $AssetRoot 'models/nr/manifest.json'
  $files = @((Get-Item -LiteralPath $manifestPath))
  $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
  foreach ($stage in $manifest.stages) {
    if (!$stage.file -or [IO.Path]::GetFileName($stage.file) -ne $stage.file) { throw 'Unsafe model stage filename' }
    $files += Get-Item -LiteralPath (Join-Path $AssetRoot "models/nr/model/$($stage.file)")
  }
  foreach ($path in @('build/ptx', 'build/shaders', 'build/ngx')) {
    $shaders = @(Get-ChildItem -LiteralPath (Join-Path $AssetRoot $path) -File |
      Where-Object { $_.Extension -in '.ptx', '.spv' })
    if (!$shaders.Count) { throw "Missing shader assets: $path" }
    $files += $shaders
  }
  foreach ($file in ($files | Sort-Object FullName -Unique)) {
    [pscustomobject]@{ file = [IO.Path]::GetRelativePath($AssetRoot, $file.FullName).Replace('\', '/');
      sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash }
  }
}

function Assert-NgxAssetHashes([object[]]$Expected, [object[]]$Actual) {
  if (!$Expected.Count -or $Expected.Count -ne $Actual.Count) { throw 'Missing or changed asset inventory' }
  $remaining = @{}
  foreach ($asset in $Actual) { $remaining[$asset.file] = $asset.sha256 }
  foreach ($asset in $Expected) {
    if (!$asset.file -or !$remaining.ContainsKey($asset.file) -or
        $asset.sha256 -notmatch '^[0-9a-fA-F]{64}$' -or $remaining[$asset.file] -ne $asset.sha256) {
      throw "Missing, duplicate or changed validated asset: $($asset.file)"
    }
    $remaining.Remove($asset.file)
  }
}

function Assert-NgxRequestedCases([object[]]$Cases) {
  if (!$Cases.Count) { throw 'Requested cases must not be empty' }
  $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
  foreach ($case in $Cases) {
    if ($case -isnot [string] -or [string]::IsNullOrWhiteSpace($case) -or !$seen.Add($case)) {
      throw 'Requested cases must be nonempty, unique names'
    }
  }
}
