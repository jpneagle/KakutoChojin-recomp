@echo off
rem KakutoChojin-recomp: converts Kakuto Chojin (Xbox) into a Windows program.
rem
rem Drag and drop an .iso (redump or xiso) or an .xbe onto this file. The
rem result is written next to the input as "<name>_win\" - start it with
rem play.bat. See tools\convert.py for the steps and the output layout.
rem
rem The first run downloads the tools it needs into deps\ (compiler, CMake,
rem Python, SDL3, XbSymbolDatabase; about 300 MB, no installation and no
rem administrator rights). Later runs reuse them.
rem
rem usage: convert.bat <game.iso | default.xbe> [output dir] [--decompile] [--jobs N]
rem   --decompile  also writes readable C with Ghidra (downloads Ghidra + JDK, about 700 MB)
setlocal EnableExtensions
set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
if "%~1"=="" goto usage

set "GHIDRA_ARG="
echo %* | findstr /c:"--decompile" >nul && set "GHIDRA_ARG=-Ghidra"
echo Preparing tools (first run downloads them)...
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\tools\setup.ps1" %GHIDRA_ARG%
if errorlevel 1 goto fail

"%ROOT%\deps\python\python.exe" "%ROOT%\tools\convert.py" %*
if errorlevel 1 goto fail
pause
exit /b 0

:usage
echo Converts an original Xbox game into a Windows program.
echo Drag and drop an .iso or .xbe onto this file.
echo   usage: convert.bat ^<game.iso ^| default.xbe^> [output dir] [--decompile] [--jobs N]
pause
exit /b 2

:fail
echo.
echo Conversion failed.
pause
exit /b 1
