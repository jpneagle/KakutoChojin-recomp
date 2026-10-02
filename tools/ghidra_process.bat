@echo off
rem Re-runs post scripts on the existing Ghidra project (no import/analysis).
rem usage: ghidra_process.bat <script> [args...] [-postScript <script> args...]
setlocal
cd /d %~dp0\..
for /d %%d in ("%USERPROFILE%\Tools\ghidra_*") do set GHIDRA=%%d
for /d %%d in ("%USERPROFILE%\Tools\jdk-21*") do set JDK=%%d
set JAVA_HOME=%JDK%
set PATH=%JDK%\bin;%PATH%
if "%KT_ANALYSIS%"=="" set KT_ANALYSIS=analysis
call "%GHIDRA%\support\analyzeHeadless.bat" "%KT_ANALYSIS%\ghidra" kt -process default.elf -noanalysis -scriptPath tools\ghidra -postScript %*
