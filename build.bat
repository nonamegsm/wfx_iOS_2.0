@echo off
rem Builds wfx_iOS.wfx64 and wfx_iOS.wfx with MSVC + vcpkg (static, self-contained).
rem Run from a normal cmd prompt; Visual Studio 2019/2022 with C++ and git must be installed.
setlocal
cd /d "%~dp0"

if not defined VCPKG_ROOT (
    if not exist vcpkg\vcpkg.exe (
        git clone https://github.com/microsoft/vcpkg.git vcpkg || goto :err
        call vcpkg\bootstrap-vcpkg.bat -disableMetrics || goto :err
    )
    set "VCPKG_ROOT=%~dp0vcpkg"
)
set "TC=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"

cmake -S . -B build\x64 -A x64 -DCMAKE_TOOLCHAIN_FILE="%TC%" -DVCPKG_TARGET_TRIPLET=x64-windows-static || goto :err
cmake --build build\x64 --config Release || goto :err

cmake -S . -B build\x86 -A Win32 -DCMAKE_TOOLCHAIN_FILE="%TC%" -DVCPKG_TARGET_TRIPLET=x86-windows-static || goto :err
cmake --build build\x86 --config Release || goto :err

if not exist dist mkdir dist
copy /y build\x64\Release\wfx_iOS.wfx64 dist\ >nul
copy /y build\x86\Release\wfx_iOS.wfx dist\ >nul
copy /y pluginst.inf dist\ >nul
copy /y readme.txt dist\ >nul
echo.
echo Done. Plugin files are in "%~dp0dist".
exit /b 0
:err
echo.
echo BUILD FAILED
exit /b 1
