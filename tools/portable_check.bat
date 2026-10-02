@echo off
rem Syntax-checks the portable runtime sources as a non-Windows 64-bit build
rem would see them: KT_PORTABLE_CHECK makes src/host/xtypes.h define the
rem Windows types itself instead of including <windows.h>, so any use of the
rem Win32 API outside the platform files fails to compile.
rem
rem   set KT_ARCH=x64
rem   tools\vcenv.bat tools\portable_check.bat
rem
rem Platform files (os.cpp, crash.cpp, gpu_d3d11.cpp) are not checked: their
rem POSIX halves need a POSIX system to compile. Neither is gpu_gl.cpp: SDL's
rem OpenGL header includes <windows.h> on Windows.
setlocal enabledelayedexpansion
set "ROOT=%~dp0.."
set "SDL_INC="
for /d %%d in ("%ROOT%\third_party\SDL3-*") do set "SDL_INC=%%d\include"
if not defined SDL_INC (echo SDL3 not found in third_party - run tools\fetch_sdl3.ps1& exit /b 1)

set FILES=manifest.cpp kpcr.cpp kernel.cpp kernel_io.cpp kernel_timer.cpp ob.cpp hostfs.cpp hle.cpp log.cpp ^
 guest_mem.cpp runtime64.cpp xapi\input.cpp platform\platform_sdl.cpp audio\mixer.cpp gpu\gpu.cpp ^
 dsound\dsound.cpp dsound\adpcm.cpp d3d\device.cpp d3d\resource.cpp d3d\state.cpp d3d\shader.cpp d3d\ffgen.cpp ^
 d3d\overlay.cpp d3d\vsh.cpp d3d\psh.cpp

set FAILED=0
for %%f in (%FILES%) do (
  cl /nologo /Zs /std:c++17 /EHsc /utf-8 /W3 /DKT_PORTABLE_CHECK /DKT_GPU_GL /I "!SDL_INC!" "%ROOT%\src\host\%%f" > "%TEMP%\kt_portable_check.txt" 2>&1
  if errorlevel 1 (
    set FAILED=1
    echo --- %%f
    type "%TEMP%\kt_portable_check.txt"
  ) else (
    echo ok  %%f
  )
)
if %FAILED%==1 (echo portable check FAILED& exit /b 1)
echo portable check passed
