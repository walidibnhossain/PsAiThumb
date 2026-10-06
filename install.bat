@echo off
net session >nul 2>&1
if errorlevel 1 ( echo Right-click kore "Run as administrator" diye chalan. & pause & exit /b 1 )
set "DEST=%ProgramFiles%\PsAiThumb"
taskkill /f /im explorer.exe >nul 2>&1
mkdir "%DEST%" 2>nul
copy /y "%~dp0PsAiThumb.dll" "%DEST%\" >nul
regsvr32 /s "%DEST%\PsAiThumb.dll"
if errorlevel 1 ( echo Register hoyni. & start explorer.exe & pause & exit /b 1 )
del /f /q /a "%LocalAppData%\Microsoft\Windows\Explorer\thumbcache_*.db" >nul 2>&1
del /f /q /a "%LocalAppData%\Microsoft\Windows\Explorer\iconcache_*.db" >nul 2>&1
start explorer.exe
echo Install hoyeche. File Explorer e "Large icons" view e thumbnail dekhun.
pause
