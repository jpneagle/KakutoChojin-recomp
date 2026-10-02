# Downloads the SDL3 development package for MSVC (zlib license) into
# third_party/, where CMakeLists.txt finds it.
#   powershell -ExecutionPolicy Bypass -File tools\fetch_sdl3.ps1 [-Version 3.4.16]
param([string]$Version = "3.4.16")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root "third_party"
if (Test-Path (Join-Path $dest "SDL3-$Version\cmake\SDL3Config.cmake")) {
    Write-Host "SDL3 $Version is already in third_party"
    exit 0
}
New-Item -ItemType Directory -Force $dest | Out-Null
$zip = Join-Path $dest "SDL3-devel-$Version-VC.zip"
$url = "https://github.com/libsdl-org/SDL/releases/download/release-$Version/SDL3-devel-$Version-VC.zip"
Write-Host "Downloading $url"
Invoke-WebRequest -Uri $url -OutFile $zip
Expand-Archive -Path $zip -DestinationPath $dest -Force
Remove-Item $zip
Write-Host "SDL3 $Version unpacked to $dest\SDL3-$Version"
