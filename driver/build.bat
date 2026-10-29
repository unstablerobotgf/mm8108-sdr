@echo off
rem Build mmsdr.dll (libmmsdr) and its clients: mm8108_source.dll (SDR++ module), mmtest.exe, mm_rtltcp.exe.
rem Needs: SDR++ source at %SDRPP_SRC% (same core as the install), import libs from mkimplib.py in build\.
rem Deploy: mmsdr.dll + mm_libusb-1.0.dll next to the client exe (sdrpp.exe for the module).
setlocal
if "%SDRPP_SRC%"=="" set SDRPP_SRC=%~dp0..\..\sdrpp_src
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d %~dp0
set GIT=dev
for /f %%h in ('git -C "%~dp0.." rev-parse --short HEAD 2^>nul') do set GIT=%%h
set CF=/nologo /O2 /Ob2 /std:c++17 /EHsc /MD /DNOMINMAX /D_USE_MATH_DEFINES /Iinclude /Isrc
cl %CF% /LD /DMMSDR_GIT=\"%GIT%\" src\mmsdr.cpp src\mm8108.cpp /Fobuild\ /Febuild\mmsdr.dll /link /LIBPATH:build mm_libusb-1.0.lib || exit /b 1
cl %CF% src\mmtest.cpp /Fobuild\ /Febuild\mmtest.exe /link /LIBPATH:build mmsdr.lib || exit /b 1
cl %CF% /LD /I"%SDRPP_SRC%\core\src" /I"%SDRPP_SRC%\core\src\imgui" src\main.cpp /Fobuild\ /Febuild\mm8108_source.dll ^
   /link /LIBPATH:build sdrpp_core.lib volk.lib mmsdr.lib || exit /b 1
cl %CF% src\rtltcp.cpp /Fobuild\ /Febuild\mm_rtltcp.exe /link /LIBPATH:build mmsdr.lib || exit /b 1
for %%t in (mmsdr_rx mmsdr_info) do cl %CF% src\%%t.cpp /Fobuild\ /Febuild\%%t.exe /link /LIBPATH:build mmsdr.lib || exit /b 1
for %%t in (rx_reentry uid_paths) do cl %CF% test\%%t.cpp /Fobuild\ /Febuild\%%t.exe /link /LIBPATH:build mmsdr.lib || exit /b 1
echo BUILD OK
