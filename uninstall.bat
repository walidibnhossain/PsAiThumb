@echo off
net session >nul 2>&1
if errorlevel 1 ( echo Run as administrator. & pause & exit /b 1 )
taskkill /f /im explorer.exe >nul 2>&1
regsvr32 /u /s "%ProgramFiles%\PsAiThumb\PsAiThumb.dll"
rmdir /s /q "%ProgramFiles%\PsAiThumb"
start explorer.exe
echo Uninstall hoyeche.
pause
