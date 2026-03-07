@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
set VCPKG_ROOT=C:\Users\JHG\vcpkg
cd /d D:\.workspace\apex_core
if not exist "build\debug" mkdir "build\debug"
if not exist "build\debug\compile_commands.json" type nul > "build\debug\compile_commands.json"
cmake --preset debug && cmake --build build/debug && ctest --test-dir build/debug --output-on-failure
