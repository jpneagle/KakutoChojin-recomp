@echo off
rem Runs a command with the x86 MSVC toolchain, CMake and Ninja on PATH.
rem Finds any Visual Studio 2019+ installation (Build Tools, Community, ...)
rem that has the C++ tools.
rem usage: vcenv.bat <command> [args...]
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (echo Visual Studio Installer not found. Install "Visual Studio Build Tools" with "Desktop development with C++". & exit /b 1)
set VSDIR=
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if "%VSDIR%"=="" (echo No Visual Studio with C++ tools found. Install "Desktop development with C++". & exit /b 1)
rem 64-bit hosted compiler targeting x86: large generated sources exhaust the 32-bit one.
set VCARCH=x86
if /i "%PROCESSOR_ARCHITECTURE%"=="AMD64" set VCARCH=amd64_x86
rem KT_ARCH=x64: 64-bit build (KakutoChojin.exe, lifted code only).
if /i "%KT_ARCH%"=="x64" set VCARCH=amd64
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" %VCARCH% >nul || exit /b 1
set PATH=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
%*
