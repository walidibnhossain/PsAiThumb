@echo off
setlocal
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSW%" (
  echo Visual Studio Build Tools paoa jayni. Install korun: https://aka.ms/vs/17/release/vs_BuildTools.exe
  echo "Desktop development with C++" workload select korben.
  pause & exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH ( echo C++ tools paoa jayni. & pause & exit /b 1 )
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
cd /d "%~dp0"
cl /nologo /EHsc /O2 /MT /LD /std:c++17 /DUNICODE /D_UNICODE PsAiThumb.cpp /link /OUT:PsAiThumb.dll windowscodecs.lib shlwapi.lib ole32.lib advapi32.lib uuid.lib shell32.lib
if errorlevel 1 ( echo BUILD FAILED & pause & exit /b 1 )
echo.
echo OK: PsAiThumb.dll toiri hoyeche. Ekhon install.bat "Run as administrator" diye chalan.
pause
