@echo off
rem Builds VR Bench into dist\: the OpenXR layer, 64-bit and 32-bit (C++, needs Visual Studio 2022 Build Tools),
rem then builds and runs the host tests (test\run_tests.bat) and stops if one fails. Run install.ps1 afterwards.
setlocal
set HERE=%~dp0
set VCVARS=
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (echo Visual Studio Build Tools not found & exit /b 1)
rem (2>&1 too: Microsoft's own vcvars64.bat complains about a vswhere.exe it cannot find, harmlessly)
call "%VCVARS%" >nul 2>&1
if not exist "%HERE%dist" mkdir "%HERE%dist"
if not exist "%HERE%build" mkdir "%HERE%build"
if not exist "%HERE%build\x86" mkdir "%HERE%build\x86"

rem The layer source lands with the first layer release; until then only the tests build.
if not exist "%HERE%layer\vrbench_layer.cpp" (echo no layer source yet, building the tests only & goto tests)

cl /nologo /O2 /EHsc /std:c++17 /LD /MT /W4 /I"%HERE%third_party" "%HERE%layer\vrbench_layer.cpp" ^
   /Fo"%HERE%build\\" /Fe"%HERE%dist\vrbench_layer.dll" /link /NOLOGO /IMPLIB:"%HERE%build\vrbench_layer.lib"
if errorlevel 1 (echo LAYER BUILD FAILED & exit /b 1)

rem a 32-bit copy of the layer for 32-bit OpenXR games (built in its own shell, so the 64-bit tools above stay)
set "VCVARS32=%VCVARS:vcvars64=vcvars32%"
cmd /c "call "%VCVARS32%" >nul 2>&1 && cl /nologo /O2 /EHsc /std:c++17 /LD /MT /W4 /I"%HERE%third_party" "%HERE%layer\vrbench_layer.cpp" /Fo"%HERE%build\x86\\" /Fe"%HERE%dist\vrbench_layer32.dll" /link /NOLOGO /IMPLIB:"%HERE%build\x86\vrbench_layer32.lib""
if errorlevel 1 (echo 32-BIT LAYER BUILD FAILED & exit /b 1)

:tests
call "%HERE%test\run_tests.bat"
if errorlevel 1 (echo TESTS FAILED & exit /b 1)
echo BUILD OK: %HERE%dist
