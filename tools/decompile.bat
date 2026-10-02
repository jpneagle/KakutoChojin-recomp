@echo off
rem Imports the symbolised ELF into a headless Ghidra project, applies the
rem KakutoChojin-recomp annotations and exports decompiled C.
rem
rem usage: decompile.bat [ghidra dir] [jdk dir]
rem   defaults: deps\ghidra and deps\jdk (tools\setup.ps1 -Ghidra), else %USERPROFILE%\Tools\ghidra_*  and  %USERPROFILE%\Tools\jdk-21*
rem   KT_XBE / KT_ANALYSIS select another title (default xbe\default.xbe, analysis);
rem   KT_ANALYSIS must already hold symbols.txt and rtti_classes.json.
setlocal
cd /d %~dp0\..
if "%KT_XBE%"=="" set KT_XBE=xbe\default.xbe
if "%KT_ANALYSIS%"=="" set KT_ANALYSIS=analysis
set A=%KT_ANALYSIS%

set GHIDRA=%~1
set JDK=%~2
if "%GHIDRA%"=="" if exist "%CD%\deps\ghidra\support\analyzeHeadless.bat" set "GHIDRA=%CD%\deps\ghidra"
if "%JDK%"=="" if exist "%CD%\deps\jdk\bin\java.exe" set "JDK=%CD%\deps\jdk"
if "%GHIDRA%"=="" for /d %%d in ("%USERPROFILE%\Tools\ghidra_*") do set GHIDRA=%%d
if "%JDK%"=="" for /d %%d in ("%USERPROFILE%\Tools\jdk-21*") do set JDK=%%d
if not exist "%GHIDRA%\support\analyzeHeadless.bat" (echo Ghidra not found: "%GHIDRA%" & exit /b 1)
if not exist "%JDK%\bin\java.exe" (echo JDK 21 not found: "%JDK%" & exit /b 1)
set JAVA_HOME=%JDK%
set PATH=%JDK%\bin;%PATH%

python tools\xbe2elf.py "%KT_XBE%" "%A%\default.elf" --symbols "%A%\symbols.txt" --rtti "%A%\rtti_classes.json" || exit /b 1
python tools\gen_ghidra_annotations.py "%KT_XBE%" "%A%\symbols.txt" "%A%\rtti_classes.json" "%A%\ghidra_annotations.tsv" || exit /b 1

if not exist "%A%\ghidra" mkdir "%A%\ghidra"
call "%GHIDRA%\support\analyzeHeadless.bat" "%A%\ghidra" kt -import "%A%\default.elf" -overwrite ^
  -processor x86:LE:32:default -cspec windows -scriptPath tools\ghidra ^
  -postScript KtApply.java "%A%\ghidra_annotations.tsv" ^
  -postScript KtNames.java "%A%\ghidra_annotations.tsv" ^
  -postScript KtMembers.java ^
  -postScript KtPropagate.java ^
  -postScript KtTypes.java "%A%\ghidra_annotations.tsv" 30 ^
  -postScript KtExportC.java "%A%\decomp" 60
