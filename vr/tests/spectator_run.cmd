@echo off
setlocal
cd /d "%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2017\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
if not exist obj\tests mkdir obj\tests
cl /nologo /EHsc /std:c++17 /MT /W3 /Foobj\tests\ /Feobj\tests\spectator_tests.exe tests\spectator_tests.cpp || exit /b 1
obj\tests\spectator_tests.exe
