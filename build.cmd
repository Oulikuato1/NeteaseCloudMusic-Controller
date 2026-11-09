@echo off
rem Builds ncmctl.exe: the NetEase Cloud Music data layer (src\ncm.cpp) plus its
rem console front end (src\cli.cpp).
rem   build.cmd          -> build\ncmctl.exe
rem   build.cmd clean    -> remove build\
rem Toolchain: MSVC (VS Build Tools 2022) + Windows SDK. No external downloads.
setlocal
cd /d "%~dp0"

if /i "%~1"=="clean" goto clean

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" goto no_vcvars
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

rem KitsRoot10 in the registry points at a dead alias (E:\...), so the SDK
rem include/lib directories are appended explicitly instead of trusting it.
set "SDKROOT=D:\Windows Kits\10"
set "SDKVER=10.0.26100.0"
set "INCLUDE=%INCLUDE%;%SDKROOT%\Include\%SDKVER%\ucrt;%SDKROOT%\Include\%SDKVER%\shared;%SDKROOT%\Include\%SDKVER%\um;%SDKROOT%\Include\%SDKVER%\winrt"
set "LIB=%LIB%;%SDKROOT%\Lib\%SDKVER%\ucrt\x64;%SDKROOT%\Lib\%SDKVER%\um\x64"

set "INCS=/Isrc /Ithird_party\sqlite"
set "DEFS=/DNDEBUG /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS"
set "FLAGS=/nologo /std:c++20 /O2 /Oi /MT /EHsc /utf-8 %DEFS%"
set "SRC=src\cli.cpp src\ncm.cpp"
set "SQLITE=third_party\sqlite\sqlite3.c"
set "LIBS=user32.lib psapi.lib winhttp.lib winmm.lib ole32.lib oleaut32.lib"

if not exist build mkdir build

echo [1/1] build\ncmctl.exe
cl %FLAGS% %INCS% /Fobuild\ %SRC% %SQLITE% /link /SUBSYSTEM:CONSOLE /OUT:build\ncmctl.exe %LIBS% || exit /b 1

echo.
echo built: %CD%\build\ncmctl.exe
endlocal
exit /b 0

:clean
if exist build rmdir /s /q build
echo cleaned: %CD%\build
endlocal
exit /b 0

:no_vcvars
echo [!] vcvars64.bat not found: %VCVARS%
exit /b 1
