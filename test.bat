@echo off
setlocal

set "ROOT_DIR=%~dp0"
pushd "%ROOT_DIR%" >nul

set "CONFIGURATION=%~1"
if "%CONFIGURATION%"=="" set "CONFIGURATION=Release"

set "CTEST_EXE=ctest"
where /q "%CTEST_EXE%" >nul 2>nul
if errorlevel 1 set "CTEST_EXE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"

if not exist "%CTEST_EXE%" (
    echo Failed to locate ctest.exe
    popd >nul
    exit /b 1
)

"%CTEST_EXE%" --test-dir build --build-config "%CONFIGURATION%" --output-on-failure
set "EXIT_CODE=%ERRORLEVEL%"

popd >nul
exit /b %EXIT_CODE%
