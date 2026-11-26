@echo off
rem Makes dist\CppSparring-<version>-win64.zip: an optimized build with the C++ runtime built in (no extra DLLs to install),
rem the lessons, the README and the license.     release.cmd v0.1.0
setlocal
set "VERSION=%~1"
if "%VERSION%"=="" (
    echo Usage: release.cmd v0.1.0
    exit /b 1
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%I"
if not defined VSDIR (
    echo Visual Studio with C++ tools was not found.
    exit /b 1
)
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 >nul 2>&1
for /f "delims=" %%L in ('where link.exe ^| findstr /i "VC\\Tools\\MSVC"') do if not defined MSVC_LINK set "MSVC_LINK=%%L"
cd /d "%~dp0"
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_LINKER="%MSVC_LINK%" || exit /b 1
ninja -C build-release || exit /b 1

set "STAGE=dist\CppSparring-%VERSION%-win64"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"
copy /y build-release\cppsparring.exe "%STAGE%\" >nul
xcopy /e /i /q lessons "%STAGE%\lessons" >nul
copy /y README.md "%STAGE%\" >nul
copy /y LICENSE "%STAGE%\" >nul
if exist "%STAGE%.zip" del "%STAGE%.zip"
powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%' -DestinationPath '%STAGE%.zip'" || exit /b 1
echo Made %STAGE%.zip
