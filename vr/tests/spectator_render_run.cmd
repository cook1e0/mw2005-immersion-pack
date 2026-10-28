@echo off
setlocal
cd /d "%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2017\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
if not exist obj\tests mkdir obj\tests
if not exist ..\dist\validation mkdir ..\dist\validation
cl /nologo /EHsc /std:c++17 /MT /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /Foobj\tests\ /Feobj\tests\spectator_render_tests.exe tests\spectator_render_tests.cpp /link d3d9.lib user32.lib || exit /b 1
obj\tests\spectator_render_tests.exe
