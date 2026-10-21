@echo off
rem Builds bin\d3d9.dll (x86) and bin\nfsmw_vr_host.exe (x64, the OpenXR bridge for
rem 64-bit-only runtimes such as SteamVR) with the VS2017 Build Tools.
setlocal
set "VCDIR=C:\Program Files (x86)\Microsoft Visual Studio\2017\BuildTools\VC\Auxiliary\Build"
if not exist "%VCDIR%\vcvars32.bat" (
  echo vcvars32.bat not found - edit build.cmd to point at your Visual Studio install
  exit /b 1
)
cd /d "%~dp0"
if not exist bin mkdir bin
if not exist obj mkdir obj
if not exist obj\x64 mkdir obj\x64
set CFLAGS=/nologo /c /O2 /MT /EHsc /std:c++17 /W3 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /Ithird_party\openxr\include
call :x86 || exit /b 1
call :x64 || exit /b 1
echo Built bin\d3d9.dll, bin\nfsmw_vr_host.exe and bin\nfsmw_crashdump.exe
exit /b 0

:x86
setlocal
call "%VCDIR%\vcvars32.bat" >nul || exit /b 1
cl %CFLAGS% /Z7 /Foobj\ src\common.cpp src\xr.cpp src\game.cpp src\d3d9proxy.cpp src\spectator.cpp src\cockpit.cpp src\crash.cpp src\crash_helper.cpp src\smaa.cpp src\frametrace.cpp || exit /b 1
link /nologo /DLL /DEBUG /OPT:REF /OPT:ICF /PDB:bin\nfsmw_vr.pdb /OUT:bin\d3d9.dll /DEF:src\d3d9.def /MACHINE:X86 /SAFESEH:NO ^
   obj\common.obj obj\xr.obj obj\game.obj obj\d3d9proxy.obj obj\spectator.obj obj\cockpit.obj obj\crash.obj obj\smaa.obj obj\frametrace.obj ^
   d3d11.lib dxgi.lib user32.lib advapi32.lib || exit /b 1
link /nologo /DEBUG /OPT:REF /OPT:ICF /PDB:bin\nfsmw_crashdump.pdb /OUT:bin\nfsmw_crashdump.exe /SUBSYSTEM:WINDOWS /MACHINE:X86 ^
   obj\common.obj obj\crash_helper.obj dbghelp.lib psapi.lib user32.lib || exit /b 1
exit /b 0

:x64
setlocal
call "%VCDIR%\vcvars64.bat" >nul || exit /b 1
cl %CFLAGS% /DNFSMW_XR_HOST /Foobj\x64\ src\common.cpp src\xr.cpp src\xr_host.cpp || exit /b 1
link /nologo /OUT:bin\nfsmw_vr_host.exe /SUBSYSTEM:WINDOWS /MACHINE:X64 ^
   obj\x64\common.obj obj\x64\xr.obj obj\x64\xr_host.obj ^
   d3d11.lib dxgi.lib user32.lib advapi32.lib || exit /b 1
exit /b 0
