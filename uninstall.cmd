@echo off
setlocal
cd /d "%~dp0"

fltmc >nul 2>&1 || (echo Run as administrator.& goto fail)
if not exist ExplorerBetterFileSizes.dll (echo ExplorerBetterFileSizes.dll not found, see README.md.& goto fail)
regsvr32 /s /u ExplorerBetterFileSizes.dll || (echo regsvr32 failed.& goto fail)
rd /s /q "%LOCALAPPDATA%\ExplorerBetterFileSizes" 2>nul

call :restart_explorer
echo Uninstalled. Restart Everything if it is running, then delete this folder.
pause
exit /b 0

:fail
pause
exit /b 1

:restart_explorer
rem Let Windows restart the shell: started from here it would be elevated.
taskkill /f /im explorer.exe >nul 2>&1
timeout /t 3 /nobreak >nul
tasklist /fi "imagename eq explorer.exe" | find /i "explorer.exe" >nul || start explorer.exe
exit /b
