@echo off
cd /d %~dp0\..\third_party\XbSymbolDatabase
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build --target XbSymbolDatabaseCLI
