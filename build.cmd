@echo off
rem Builds build\cppsparring.exe with MSVC + Ninja. The MSVC linker is named explicitly so msys2's ld.exe on PATH is not picked up.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64 >nul 2>&1
for /f "delims=" %%L in ('where link.exe ^| findstr /i "VC\\Tools\\MSVC"') do if not defined MSVC_LINK set "MSVC_LINK=%%L"
cd /d "%~dp0"
if not exist build mkdir build
cmake -S . -B build -G Ninja -DCMAKE_LINKER="%MSVC_LINK%" || exit /b 1
ninja -C build
