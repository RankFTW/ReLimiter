@echo off
setlocal

set CMAKE="C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set CONFIG=Release
set GENERATOR=Visual Studio 18 2026

if /i "%1"=="debug" set CONFIG=Debug
if /i "%1"=="clean" (
    echo Cleaning build directories...
    rmdir /s /q build 2>nul
    rmdir /s /q build32 2>nul
    echo Done.
    exit /b 0
)

if /i "%1"=="32" goto build32
if /i "%1"=="all" goto buildall
goto build64

:build64
if not exist build (
    echo Configuring CMake 64-bit...
    %CMAKE% -B build -G "%GENERATOR%" -A x64
    if errorlevel 1 (
        echo CMake configure failed.
        exit /b 1
    )
)

echo Building 64-bit %CONFIG%...
%CMAKE% --build build --config %CONFIG% -- /m
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo.
echo Output: build\bin\%CONFIG%\relimiter.addon64
if /i "%1"=="all" goto build32
exit /b 0

:build32
if not exist build32 (
    echo Configuring CMake 32-bit...
    %CMAKE% -B build32 -G "%GENERATOR%" -A Win32
    if errorlevel 1 (
        echo CMake configure failed (32-bit).
        exit /b 1
    )
)

echo Building 32-bit %CONFIG%...
%CMAKE% --build build32 --config %CONFIG% -- /m
if errorlevel 1 (
    echo Build failed (32-bit).
    exit /b 1
)

echo.
echo Output: build32\bin\%CONFIG%\relimiter.addon32
exit /b 0

:buildall
call :build64
if errorlevel 1 exit /b 1
goto build32
