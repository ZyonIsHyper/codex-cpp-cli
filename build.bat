@echo off
setlocal

set "ROOT_DIR=%~dp0"
pushd "%ROOT_DIR%" >nul

set "CONFIGURATION=%~1"
if "%CONFIGURATION%"=="" set "CONFIGURATION=Release"

set "HAS_CACHE=0"
if exist "build\CMakeCache.txt" set "HAS_CACHE=1"

set "GENERATOR=%CODEX_CPP_CMAKE_GENERATOR%"
if "%GENERATOR%"=="" set "GENERATOR=Visual Studio 18 2026"

set "ARCHITECTURE=%CODEX_CPP_CMAKE_ARCH%"
if "%ARCHITECTURE%"=="" set "ARCHITECTURE=x64"

set "BUILD_STATIC=%CODEX_CPP_BUILD_STATIC_LIB%"
if "%BUILD_STATIC%"=="" set "BUILD_STATIC=ON"

set "BUILD_SHARED=%CODEX_CPP_BUILD_SHARED_LIB%"
if "%BUILD_SHARED%"=="" set "BUILD_SHARED=ON"

set "BUILD_TESTS=%CODEX_CPP_BUILD_TESTS%"
if "%BUILD_TESTS%"=="" set "BUILD_TESTS=ON"

set "CMAKE_EXE=cmake"
where /q "%CMAKE_EXE%" >nul 2>nul
if errorlevel 1 set "CMAKE_EXE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

if not exist "%CMAKE_EXE%" (
    echo Failed to locate cmake.exe
    popd >nul
    exit /b 1
)

if "%HAS_CACHE%"=="1" (
    "%CMAKE_EXE%" -S . -B build -DCODEX_CPP_BUILD_STATIC_LIB=%BUILD_STATIC% -DCODEX_CPP_BUILD_SHARED_LIB=%BUILD_SHARED% -DCODEX_CPP_BUILD_TESTS=%BUILD_TESTS%
) else (
    "%CMAKE_EXE%" -S . -B build -G "%GENERATOR%" -A "%ARCHITECTURE%" -DCODEX_CPP_BUILD_STATIC_LIB=%BUILD_STATIC% -DCODEX_CPP_BUILD_SHARED_LIB=%BUILD_SHARED% -DCODEX_CPP_BUILD_TESTS=%BUILD_TESTS%
)
if errorlevel 1 (
    popd >nul
    exit /b 1
)

"%CMAKE_EXE%" --build build --config "%CONFIGURATION%" --parallel
set "EXIT_CODE=%ERRORLEVEL%"

popd >nul
exit /b %EXIT_CODE%
