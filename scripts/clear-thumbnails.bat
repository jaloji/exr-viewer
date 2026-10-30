@echo off
rem Clears the Explorer thumbnail cache so .exr files get new thumbnails.
rem Explorer (taskbar, open folders) restarts during this.
taskkill /f /im explorer.exe >nul
timeout /t 1 /nobreak >nul
del /f /q /a "%LocalAppData%\Microsoft\Windows\Explorer\thumbcache_*.db" 2>nul
start explorer.exe
echo Thumbnail cache cleared.
