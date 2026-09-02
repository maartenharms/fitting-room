@echo off
setlocal EnableExtensions EnableDelayedExpansion

if "%~1"=="" (
    echo usage: build_body_studio_dev.bat BUILD_ID
    exit /b 2
)

set "FR_BUILD_ID=%~1"
powershell -NoProfile -Command "if ($env:FR_BUILD_ID -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') { exit 2 }"
if errorlevel 1 (
    echo invalid build id: %FR_BUILD_ID%
    exit /b 2
)
if /I "%FR_BUILD_ID%"=="release" (
    echo release is not a valid Body Studio development build id
    exit /b 2
)

call :main > "%~dp0build_body_studio_dev.log" 2>&1
set "FR_RESULT=%errorlevel%"
if not "%FR_RESULT%"=="0" type "%~dp0build_body_studio_dev.log"
exit /b %FR_RESULT%

:main
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 goto :fail
set "VCPKG_ROOT=C:\Users\Maarten\vcpkg"
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "FR_CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cd /d "%~dp0.."

echo === BODY STUDIO DEV CLEAN DEPLOY ===
"%FR_CMAKE%" -E remove_directory "%~dp0..\build\body-studio-dev\deploy"
if errorlevel 1 goto :fail

echo === BODY STUDIO DEV CONFIGURE %FR_BUILD_ID% ===
"%FR_CMAKE%" --preset body-studio-dev -DFR_BUILD_ID="%FR_BUILD_ID%" -DOUTPUT_FOLDER="%~dp0..\build\body-studio-dev\deploy"
if errorlevel 1 goto :fail

echo === BODY STUDIO DEV BUILD ===
"%FR_CMAKE%" --build build/body-studio-dev
if errorlevel 1 goto :fail

echo === BODY STUDIO DEV TESTS ===
set /a FR_TEST_COUNT=0
for %%T in (build\body-studio-dev\*Tests.exe) do (
    set /a FR_TEST_COUNT+=1
    echo --- %%~nxT
    "%%T"
    if !errorlevel! neq 0 goto :fail
)
rem The floor rises with the integration: the dye-cost work brought DyeQualityTests
rem in alongside the 33 this branch was cut with, so 34 is the honest number now.
rem A glob that quietly finds fewer than every registered suite is the thing this
rem check exists to catch, so raise it whenever a target is added.
if !FR_TEST_COUNT! LSS 35 (
    echo expected at least 35 test executables, found !FR_TEST_COUNT!
    goto :fail
)

echo === BODY STUDIO DEV ALL DONE: !FR_TEST_COUNT! suites ===
exit /b 0

:fail
echo *** BODY STUDIO DEV BUILD FAILED ***
exit /b 1
