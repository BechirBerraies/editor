@echo off
rem Usage: build          debug build  (build\editor.exe)
rem        build release  optimized build
setlocal

where cl >nul 2>nul
if not errorlevel 1 goto :compile

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
    echo error: Visual Studio with the C++ tools was not found.
    exit /b 1
)
rem vcvarsall itself runs vswhere from PATH.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1

:compile
set "ROOT=%~dp0"
if not exist "%ROOT%build" mkdir "%ROOT%build"

set "FLAGS=/nologo /std:c11 /W4 /WX /Zi /FC /Fe:editor.exe"
if /i "%~1"=="release" (
    set "FLAGS=%FLAGS% /O2 /MT /DEDITOR_DEBUG=0"
) else (
    set "FLAGS=%FLAGS% /Od /MTd /DEDITOR_DEBUG=1"
)
set "LIBS=user32.lib gdi32.lib comdlg32.lib"

pushd "%ROOT%build"
cl %FLAGS% "%ROOT%src\win32_editor.c" /link /INCREMENTAL:NO /SUBSYSTEM:WINDOWS %LIBS%
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
