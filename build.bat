@echo off
rem Builds cr-srt.dll and cr-probe.dll with MinGW-w64 (nuwen) and Ninja, then copies one of them to the name given:
rem   build.bat [name.dll]        cr-srt.dll   -> name (default cr-srt-dev.dll)
rem   build.bat probe [name.dll]  cr-probe.dll -> name (default cr-probe-dev.dll)
rem From WSL: cmd.exe /c build.bat [...], with the repo as a Windows-side working directory.
rem A DLL the game has loaded is locked: unload it first (touch <name>.unload) or the copy fails.
call C:\MinGW\set_distro_paths.bat >nul
set "DLL=cr-srt"
set "OUT=%~1"
if /i "%~1"=="probe" (
  set "DLL=cr-probe"
  set "OUT=%~2"
)
if "%OUT%"=="" set "OUT=%DLL%-dev.dll"
rem The toolchain lives in CMakePresets.json, which VS Code's CMake Tools also reads. gcc-ar and gcc-ranlib there,
rem not ar: bare ar scans lib\bfd-plugins, loads libdep.a and pops a Bad Image dialog.
if not exist build\build.ninja cmake --preset mingw || goto :fail
cmake --build build || goto :fail
copy /y build\%DLL%.dll "%OUT%" >nul || goto :fail
echo built %OUT%
exit /b 0
:fail
echo FAILED
exit /b 1
