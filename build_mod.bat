@echo off
setlocal

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"

cmake -G "Visual Studio 18 2026" -A x64 --preset default -B ./build || exit /b 1
cmake --build ./build --config %CONFIG% --target INSTALL || exit /b 1
