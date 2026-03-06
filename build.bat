@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
set VCPKG_ROOT=C:\Users\JHG\vcpkg
cd /d D:\.workspace\BoostAsioCore
rmdir /s /q build\debug 2>nul
cmake --preset debug && cmake --build build/debug && ctest --test-dir build/debug --output-on-failure
