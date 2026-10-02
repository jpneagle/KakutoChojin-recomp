@echo off
rem Configure and build KakutoChojin-recomp with the x86 MSVC toolchain.
cd /d %~dp0\..
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build
