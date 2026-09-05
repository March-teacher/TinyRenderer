@echo off
REM ============================================================================
REM  TinyRenderer - command line build (MSVC)
REM
REM  NOTE: this file is deliberately pure ASCII. cmd.exe parses .bat files using
REM  the console's legacy code page (GBK on a Simplified Chinese Windows), so a
REM  UTF-8 encoded batch file with Chinese comments gets mis-decoded and the
REM  script breaks in confusing ways. Chinese documentation lives in README.md.
REM
REM  Usage:  build.bat          -> Release build (/O2)
REM          build.bat debug    -> Debug build (/Od /Zi)
REM
REM  Output: renderer.exe in the repository root, objects in .buildtmp\
REM ============================================================================
setlocal

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=release"

REM --- Locate the MSVC toolchain ---------------------------------------------
REM If we are already inside a Developer Command Prompt, skip the setup.
if defined VSCMD_ARG_TGT_ARCH goto have_msvc

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found. Is Visual Studio installed?
    echo         Alternatively, open "x64 Native Tools Command Prompt" first.
    exit /b 1
)

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"

if not defined VSPATH (
    echo [ERROR] No Visual Studio installation with the C++ toolset was found.
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [ERROR] Failed to initialise the MSVC environment.
    exit /b 1
)

:have_msvc

REM --- Compiler flags ---------------------------------------------------------
REM  /std:c++20  match the language level configured in Project1.vcxproj
REM  /utf-8      the sources are UTF-8 and contain Chinese comments; without
REM              this MSVC guesses the local code page and warns / mis-parses
REM  /EHsc       standard C++ exception model (the OBJ parser uses std::stoi)
set "COMMON=/nologo /std:c++20 /utf-8 /EHsc /W3"
if /i "%CONFIG%"=="debug" (
    set "FLAGS=%COMMON% /Od /Zi /MDd /D_DEBUG"
) else (
    set "FLAGS=%COMMON% /O2 /MD /DNDEBUG"
)

if not exist ".buildtmp" mkdir ".buildtmp"

echo [BUILD] configuration = %CONFIG%
cl %FLAGS% /Fe:renderer.exe /Fo:.buildtmp\ /Fd:.buildtmp\ Project1\main.cpp Project1\model.cpp Project1\scene.cpp Project1\our_gl.cpp Project1\shaders.cpp Project1\texture.cpp Project1\image_io.cpp Project1\tgaimage.cpp

if errorlevel 1 (
    echo [BUILD] failed
    exit /b 1
)

echo [BUILD] done -^> renderer.exe
echo.
echo Try:
echo   renderer.exe obj\african_head\african_head.obj --floor -o head.png
endlocal
