# Downloads everything the conversion needs into deps\ (no installers, no
# administrator rights, nothing outside this folder):
#   llvm-mingw (Clang C/C++ compiler for Windows), CMake, Ninja, an embedded
#   Python with capstone, SDL3 for MinGW, and the XbSymbolDatabase sources
#   (built with the downloaded compiler).
# With -Ghidra it also fetches Ghidra and a JDK for tools\decompile.bat.
#
#   powershell -ExecutionPolicy Bypass -File tools\setup.ps1 [-Ghidra]
#
# Re-running skips what is already in place. Versions are pinned; each
# download is checked against its SHA-256 when one is listed.
param([switch]$Ghidra)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"  # Invoke-WebRequest is much faster without the progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Root = Split-Path -Parent $PSScriptRoot
$Deps = Join-Path $Root "deps"
$Cache = Join-Path $Deps "downloads"
New-Item -ItemType Directory -Force $Deps, $Cache | Out-Null

$XbsdbCommit = "20eced544726f5558c5a408458f38a086cc4e543"
$Packages = @(
    @{ Name = "llvm-mingw"; Check = "bin\clang.exe"; Strip = $true
       Url = "https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/llvm-mingw-20260922-ucrt-x86_64.zip" },
    @{ Name = "cmake"; Check = "bin\cmake.exe"; Strip = $true
       Url = "https://github.com/Kitware/CMake/releases/download/v4.4.3/cmake-4.4.3-windows-x86_64.zip" },
    @{ Name = "ninja"; Check = "ninja.exe"; Strip = $false
       Url = "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip" },
    @{ Name = "python"; Check = "python.exe"; Strip = $false
       Url = "https://www.python.org/ftp/python/3.13.7/python-3.13.7-embed-amd64.zip" },
    @{ Name = "SDL3"; Check = "x86_64-w64-mingw32\lib\cmake\SDL3\SDL3Config.cmake"; Strip = $true
       Url = "https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-devel-3.4.16-mingw.zip" },
    @{ Name = "XbSymbolDatabase"; Check = "CMakeLists.txt"; Strip = $true
       Url = "https://github.com/Cxbx-Reloaded/XbSymbolDatabase/archive/$XbsdbCommit.zip" }
)
if ($Ghidra) {
    $Packages += @(
        @{ Name = "jdk"; Check = "bin\java.exe"; Strip = $true
           Url = "https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.12.1%2B1/OpenJDK21U-jdk_x64_windows_hotspot_21.0.12.1_1.zip" },
        @{ Name = "ghidra"; Check = "support\analyzeHeadless.bat"; Strip = $true
           Url = "https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.4_build/ghidra_12.1.4_PUBLIC_20260921.zip" }
    )
}
# SHA-256 of the downloads (filled in for the pinned versions).
# Windows' own bsdtar unpacks zip files (a GNU tar earlier on PATH would not).
$Tar = Join-Path $env:SystemRoot "System32\tar.exe"
$Sha256 = @{}
$HashFile = Join-Path $PSScriptRoot "setup.sha256"
if (Test-Path $HashFile) {
    foreach ($line in Get-Content $HashFile) {
        $parts = $line -split "\s+", 2
        if ($parts.Count -eq 2) { $Sha256[$parts[1].Trim()] = $parts[0].Trim().ToLower() }
    }
}

# SHA-256 through .NET (Get-FileHash is not available everywhere).
function Get-Sha256($path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($path)
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream)) -replace "-", "").ToLower() }
    finally { $stream.Dispose(); $sha.Dispose() }
}

function Get-Package($p) {
    $dest = Join-Path $Deps $p.Name
    if (Test-Path (Join-Path $dest $p.Check)) { Write-Host "  $($p.Name): ready"; return }
    $file = Join-Path $Cache ([IO.Path]::GetFileName(([Uri]$p.Url).AbsolutePath))
    if (-not (Test-Path $file)) {
        Write-Host "  $($p.Name): downloading $($p.Url)"
        Invoke-WebRequest -Uri $p.Url -OutFile "$file.part" -UseBasicParsing
        Move-Item -Force "$file.part" $file
    }
    $name = [IO.Path]::GetFileName($file)
    if ($Sha256.ContainsKey($name)) {
        $hash = (Get-Sha256 $file)
        if ($hash -ne $Sha256[$name]) {
            Remove-Item $file
            throw "$name has an unexpected SHA-256 ($hash); the download was removed, try again"
        }
    }
    Write-Host "  $($p.Name): unpacking"
    $tmp = Join-Path $Deps ("_unpack_" + $p.Name)
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory $tmp | Out-Null
    & $Tar -xf $file -C $tmp
    if ($LASTEXITCODE -ne 0) { throw "cannot unpack $file" }
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    $inner = Get-ChildItem $tmp
    if ($p.Strip -and $inner.Count -eq 1 -and $inner[0].PSIsContainer) {
        Move-Item $inner[0].FullName $dest
        Remove-Item -Recurse -Force $tmp
    } else {
        Move-Item $tmp $dest
    }
    if (-not (Test-Path (Join-Path $dest $p.Check))) { throw "$($p.Name): $($p.Check) missing after unpacking" }
}

Write-Host "Setting up tools in $Deps"
foreach ($p in $Packages) { Get-Package $p }

# Embedded Python: enable site-packages and add capstone (a pure wheel).
$py = Join-Path $Deps "python"
$site = Join-Path $py "Lib\site-packages"
if (-not (Test-Path (Join-Path $site "capstone"))) {
    Write-Host "  capstone: downloading"
    $whl = Join-Path $Cache "capstone-5.0.9-py3-none-win_amd64.whl"
    if (-not (Test-Path $whl)) {
        Invoke-WebRequest -UseBasicParsing -OutFile $whl `
            -Uri "https://files.pythonhosted.org/packages/50/e6/6f06fdb6a9ed32b2f7cd9c036b92d5324112c3ef7080f2c71efc367d40dd/capstone-5.0.9-py3-none-win_amd64.whl"
    }
    $name = [IO.Path]::GetFileName($whl)
    if ($Sha256.ContainsKey($name) -and (Get-Sha256 $whl) -ne $Sha256[$name]) {
        Remove-Item $whl
        throw "$name has an unexpected SHA-256; the download was removed, try again"
    }
    New-Item -ItemType Directory -Force $site | Out-Null
    & $Tar -xf $whl -C $site
    $pth = Get-ChildItem $py -Filter "python3*._pth" | Select-Object -First 1
    $lines = Get-Content $pth.FullName
    if ($lines -notcontains "Lib\site-packages") {
        Set-Content -Encoding ascii $pth.FullName (@($lines) + "Lib\site-packages" + "import site")
    }
}

# XbSymbolDatabase CLI, built with the downloaded toolchain.
$xbsdb = Join-Path $Deps "XbSymbolDatabase"
$cli = Join-Path $xbsdb "build\projects\cli\XbSymbolDatabaseCLI.exe"
if (-not (Test-Path $cli)) {
    Write-Host "  XbSymbolDatabase: building"
    # Through cmd so that compiler output on stderr is not a PowerShell error.
    # The unit tests would fetch more sources with git; they are not needed.
    $env_bat = Join-Path $PSScriptRoot "env.bat"
    $log = Join-Path $Deps "xbsdb_build.log"
    $build = Join-Path $xbsdb "build"
    cmd /c "`"$env_bat`" cmake -S `"$xbsdb`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DXBSDB_BUILD_UNITTEST=OFF -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF > `"$log`" 2>&1"
    cmd /c "`"$env_bat`" cmake --build `"$build`" --target XbSymbolDatabaseCLI -j 2 >> `"$log`" 2>&1"
    if (-not (Test-Path $cli)) { throw "XbSymbolDatabase build failed; see deps\xbsdb_build.log" }
}
Write-Host "Tools are ready."
