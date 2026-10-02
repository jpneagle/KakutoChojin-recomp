@echo off
rem Runs a command with the tools from deps\ (tools\setup.ps1) first on PATH.
rem   tools\env.bat <command> [args...]
set "KT_DEPS=%~dp0..\deps"
set "PATH=%KT_DEPS%\llvm-mingw\bin;%KT_DEPS%\cmake\bin;%KT_DEPS%\ninja;%KT_DEPS%\python;%PATH%"
%*
