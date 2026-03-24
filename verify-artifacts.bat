@echo off
setlocal

set "ROOT_DIR=%~dp0"
pushd "%ROOT_DIR%" >nul

set "CONFIGURATION=%~1"
if "%CONFIGURATION%"=="" set "CONFIGURATION=Release"

if not exist "build\%CONFIGURATION%\codex-cpp-static.lib" (
    echo Missing build\%CONFIGURATION%\codex-cpp-static.lib
    popd >nul
    exit /b 1
)

if not exist "build\%CONFIGURATION%\codex-cpp.dll" (
    echo Missing build\%CONFIGURATION%\codex-cpp.dll
    popd >nul
    exit /b 1
)

if not exist "build\%CONFIGURATION%\codex-cpp.lib" (
    echo Missing build\%CONFIGURATION%\codex-cpp.lib
    popd >nul
    exit /b 1
)

echo Artifacts verified for %CONFIGURATION%.
popd >nul
exit /b 0
