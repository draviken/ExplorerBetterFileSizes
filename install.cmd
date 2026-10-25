@echo off
setlocal
cd /d "%~dp0"

fltmc >nul 2>&1 || (echo Run as administrator.& goto fail)
if not exist ExplorerBetterFileSizes.dll (echo ExplorerBetterFileSizes.dll not found.& goto fail)
regsvr32 /s ExplorerBetterFileSizes.dll || (echo regsvr32 failed.& goto fail)

echo Fetching offsets...
start "" /wait rundll32.exe "%~dp0ExplorerBetterFileSizes.dll",UpdateSymbols /v
if errorlevel 1 (echo Symbol update failed. Rerun install.cmd to retry.& goto fail)

call :restart_explorer
echo Installed.
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
