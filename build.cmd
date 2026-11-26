@echo off
rem Builds build\cppsparring.exe with MSVC + Ninja, from any Visual Studio 2022 or newer that has the C++ workload.
rem The MSVC linker is named explicitly so a MinGW/MSYS2 ld.exe on PATH is not picked up.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio was not found. Install Visual Studio 2022 or newer with "Desktop development with C++".
    exit /b 1
)
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%I"
if not defined VSDIR (
    echo Visual Studio has no C++ tools. Add the "Desktop development with C++" workload.
    exit /b 1
)
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 >nul 2>&1
for /f "delims=" %%L in ('where link.exe ^| findstr /i "VC\\Tools\\MSVC"') do if not defined MSVC_LINK set "MSVC_LINK=%%L"
cd /d "%~dp0"
if not exist build mkdir build
cmake -S . -B build -G Ninja -DCMAKE_LINKER="%MSVC_LINK%" || exit /b 1
ninja -C build
