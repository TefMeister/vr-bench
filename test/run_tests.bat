@echo off
rem Builds and runs the host tests (no runtime, no headset). Used by build.bat and on its own.
rem Needs Visual Studio 2022 Build Tools (C++); found through vswhere, like Hand Freeze's build.bat.
setlocal
set HERE=%~dp0
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSROOT="
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%i"
if "%VSROOT%"=="" (echo Visual Studio Build Tools not found & exit /b 1)
rem (2>&1 too: Microsoft's own vcvars64.bat complains about a vswhere.exe it cannot find, harmlessly)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if not exist "%HERE%..\build\test" mkdir "%HERE%..\build\test"
cl /nologo /EHsc /std:c++17 /W4 /I"%HERE%..\layer" "%HERE%vb_input_test.cpp" /Fo"%HERE%..\build\test\\" /Fe"%HERE%..\build\test\vb_input_test.exe"
if errorlevel 1 (echo TEST BUILD FAILED & exit /b 1)
"%HERE%..\build\test\vb_input_test.exe"
