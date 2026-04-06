@echo off
setlocal

set CMAKE="C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set CONFIG=Release
set GENERATOR=Visual Studio 18 2026

if /i "%1"=="debug" set CONFIG=Debug
if /i "%1"=="clean" (
    echo Cleaning build directory...
    rmdir /s /q build 2>nul
    echo Done.
    exit /b 0
)

if not exist build (
    echo Configuring CMake...
    %CMAKE% -B build -G "%GENERATOR%" -A x64
    if errorlevel 1 (
        echo CMake configure failed.
        exit /b 1
    )
)

echo Building %CONFIG%...
%CMAKE% --build build --config %CONFIG% -- /m
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo.
echo Output: build\bin\%CONFIG%\relimiter.addon64
