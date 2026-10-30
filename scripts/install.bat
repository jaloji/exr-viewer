@echo off
rem Installs the EXR WIC codec (Windows Explorer thumbnails for EXR images).
rem Run as administrator from the release folder (ExrWic.dll next to this file).
setlocal
cd /d "%~dp0"

net session >nul 2>&1 || (
  echo Please run this file as administrator.
  exit /b 1
)
if not exist ExrWic.dll (
  echo ExrWic.dll not found next to install.bat. Build the project first, then run dist\install.bat.
  exit /b 1
)

set "APPDIR=%ProgramFiles%\ExrViewer"

if not exist "%APPDIR%" mkdir "%APPDIR%"
if exist "%APPDIR%\ExrWic.dll" regsvr32 /s /u "%APPDIR%\ExrWic.dll"
copy /y ExrWic.dll "%APPDIR%\ExrWic.dll" >nul || (
  echo Could not replace "%APPDIR%\ExrWic.dll": it is probably in use by Explorer.
  echo Run uninstall.bat, sign out or restart, then run install.bat again.
  exit /b 1
)
regsvr32 /s "%APPDIR%\ExrWic.dll" || (
  echo Codec registration failed. If Microsoft Defender attack surface reduction rules are
  echo enforced on this PC, allow the DLL first ^(admin PowerShell^):
  echo   Add-MpPreference -AttackSurfaceReductionOnlyExclusions "%APPDIR%\ExrWic.dll"
  exit /b 1
)
echo EXR codec installed: %APPDIR%\ExrWic.dll
echo.
echo Done. If .exr files still show blank icons, run clear-thumbnails.bat.
