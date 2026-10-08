# Public ABI declarations only. No NVIDIA runtime or model is downloaded.
$ErrorActionPreference = "Stop"
$revision = "374959484e79a640feaba44c93ac8cfb0a03f5b5"
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root "tools/ngx"
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($name in @("nvsdk_ngx.h", "nvsdk_ngx_defs.h", "nvsdk_ngx_params.h", "nvsdk_ngx_vk.h", "nvsdk_ngx_defs_vk.h")) {
  $path = Join-Path $out $name
  Invoke-WebRequest -Uri "https://raw.githubusercontent.com/NVIDIA/DLSS/$revision/include/$name" -OutFile $path
}
Invoke-WebRequest -Uri "https://raw.githubusercontent.com/NVIDIA/DLSS/$revision/LICENSE.txt" -OutFile (Join-Path $out "LICENSE.txt")
Write-Host "NGX ABI headers: $revision -> $out"
$nvapiRevision = "70d337db9186e968eab622f7e786de7e437faf3d"
$nvapi = Join-Path $root "tools/nvapi"
New-Item -ItemType Directory -Force $nvapi | Out-Null
foreach ($name in @("nvapi.h", "nvapi_interface.h", "nvapi_lite_salstart.h", "nvapi_lite_common.h", "nvapi_lite_sli.h", "nvapi_lite_surround.h", "nvapi_lite_stereo.h", "nvapi_lite_d3dext.h", "nvapi_lite_salend.h")) {
  Invoke-WebRequest -Uri "https://raw.githubusercontent.com/NVIDIA/nvapi/$nvapiRevision/$name" -OutFile (Join-Path $nvapi $name)
}
Write-Host "NVAPI headers: $nvapiRevision -> $nvapi"
