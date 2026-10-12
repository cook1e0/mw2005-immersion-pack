@echo off
rem Builds bin\d3d9.dll (x86) with the VS2017 Build Tools.
setlocal
set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\2017\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
if not exist %VCVARS% (
  echo vcvars32.bat not found - edit build.cmd to point at your Visual Studio install
  exit /b 1
)
call %VCVARS% >nul || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
if not exist obj mkdir obj
cl /nologo /c /O2 /MT /EHsc /std:c++17 /W3 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
   /Ithird_party\openxr\include /Foobj\ src\common.cpp src\xr.cpp src\game.cpp src\d3d9proxy.cpp src\spectator.cpp || exit /b 1
link /nologo /DLL /OUT:bin\d3d9.dll /DEF:src\d3d9.def /MACHINE:X86 /SAFESEH:NO ^
   obj\common.obj obj\xr.obj obj\game.obj obj\d3d9proxy.obj obj\spectator.obj ^
   d3d11.lib dxgi.lib user32.lib advapi32.lib || exit /b 1
echo Built bin\d3d9.dll
