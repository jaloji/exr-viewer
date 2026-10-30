@echo off
rem Removes the EXR WIC codec.
rem Run as administrator.
setlocal

net session >nul 2>&1 || (
  echo Please run this file as administrator.
  exit /b 1
)

set "APPDIR=%ProgramFiles%\ExrViewer"

if exist "%APPDIR%\ExrWic.dll" (
  regsvr32 /s /u "%APPDIR%\ExrWic.dll"
  rem Explorer may still hold the DLL; it can then be deleted after a restart.
  del /f "%APPDIR%\ExrWic.dll" 2>nul
  if exist "%APPDIR%\ExrWic.dll" echo ExrWic.dll is in use, delete "%APPDIR%" after a restart.
  rmdir "%APPDIR%" 2>nul
  echo EXR codec removed.
)
