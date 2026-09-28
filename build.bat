@echo off
rem Builds cr-srt.dll with MinGW-w64 (nuwen) and Ninja, then copies it to the name given (default cr-srt-dev.dll).
rem From WSL: cmd.exe /c build.bat [name.dll], with the repo as a Windows-side working directory.
rem A DLL the game has loaded is locked: unload it first (touch <name>.unload) or the copy fails.
call C:\MinGW\set_distro_paths.bat >nul
set "OUT=%~1"
if "%OUT%"=="" set "OUT=cr-srt-dev.dll"
rem gcc-ar and gcc-ranlib, not ar: bare ar scans lib\bfd-plugins, loads libdep.a and pops a Bad Image dialog.
if not exist build\build.ninja cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_AR=C:/MinGW/bin/gcc-ar.exe -DCMAKE_RANLIB=C:/MinGW/bin/gcc-ranlib.exe || goto :fail
cmake --build build || goto :fail
copy /y build\cr-srt.dll "%OUT%" >nul || goto :fail
echo built %OUT%
exit /b 0
:fail
echo FAILED
exit /b 1
