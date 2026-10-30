@echo off
rem Configures and builds a Release with CMake, then gathers a ready-to-install package in dist\.
rem Requires CMake and Visual Studio 2022 (or its Build Tools) with the C++ workload.
setlocal
cd /d "%~dp0"
cmake -S . -B build -A x64 || exit /b 1
cmake --build build --config Release || exit /b 1
cmake --install build --config Release --prefix dist || exit /b 1
echo.
echo Package ready in %~dp0dist
