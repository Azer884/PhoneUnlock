@echo off
rem Builds bin\PhoneUnlockCP.dll (lock-screen credential provider) and bin\PhoneUnlockSetup.exe (app, with the DLL embedded).
rem Needs Visual Studio 2022 (or Build Tools) with the "Desktop development with C++" workload.
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere.exe not found - install Visual Studio Build Tools & exit /b 1)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo No C++ toolset found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist bin mkdir bin
if not exist obj mkdir obj
if not exist src\app.ico powershell -NoProfile -ExecutionPolicy Bypass -File tools\make_icon.ps1 || exit /b 1

set CFLAGS=/nologo /O2 /MT /EHsc /W4 /permissive- /std:c++17 /DUNICODE /D_UNICODE /Foobj\
set LIBS=ole32.lib advapi32.lib shlwapi.lib secur32.lib ws2_32.lib bcrypt.lib gdi32.lib user32.lib uuid.lib credui.lib propsys.lib

cl %CFLAGS% /LD src\dll.cpp src\provider.cpp src\credential.cpp src\shared.cpp src\guids.cpp /Febin\PhoneUnlockCP.dll /link /DEF:src\PhoneUnlockCP.def %LIBS% || exit /b 1

rc /nologo /fo obj\app.res src\app.rc || exit /b 1
cl %CFLAGS% src\app.cpp src\shared.cpp src\guids.cpp obj\app.res /Febin\PhoneUnlockSetup.exe /link /SUBSYSTEM:WINDOWS /MANIFESTUAC:"level='requireAdministrator'" %LIBS% comctl32.lib shell32.lib || exit /b 1

echo BUILD OK
