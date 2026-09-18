@echo off
rem Build the DIAGNOSTIC variant: two log sinks (Documents AND beside the DLL,
rem which is Overwrite under MO2), the face-module census, the player cell line,
rem the appearance watch forced on, and "1.1.9-diag1" stamped into the version
rem string so any log it produces identifies itself on line 3.
rem
rem For handing to ONE reporter over Discord. Never for release: make_fomod.sh
rem refuses to give a diagnostic DLL the release name.
rem
rem TWO THINGS THIS DELIBERATELY DOES NOT SHARE WITH build.bat:
rem   1. Its own build tree (build\diag), so build\release keeps the bytes that
rem      were tested and packaged.
rem   2. Its own OUTPUT_FOLDER, so the POST_BUILD deploy CANNOT write a
rem      diagnostic DLL anywhere the game can load it. redeploy.ps1 is the only
rem      thing that reaches the game and it reads build\deploy, never this.
rem
rem It builds the plugin target ONLY. The test suites are build.bat's job and
rem running them twice proves nothing about a logging-only variant.
call :main > "%~dp0build_diag.log" 2>&1
exit /b %errorlevel%

:main
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if %errorlevel% neq 0 goto :fail
set "VCPKG_ROOT=C:\Users\Maarten\vcpkg"
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "CM=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cd /d "%~dp0.."
echo === CONFIGURE START ===
rem !! QUOTE THE DEPLOY PATH and use forward slashes. Unquoted, %CD% splits on
rem the space in "C:\Studios\Mod Studio\Fitting Room" and CMake receives
rem "C:\Studios\Mod", which it will happily deploy a whole payload into: a stray
rem C:\Studios\Mod tree appeared the first time Menu Studio's twin ran.
rem
rem FR_BODY_STUDIO is passed explicitly for build.bat's reason: it is a cached
rem option, so a tree configured once with it OFF keeps OFF through every later
rem configure that stays silent.
"%CM%" --preset release -B build/diag -DFR_DIAG=ON -DFR_BODY_STUDIO=ON "-DOUTPUT_FOLDER=%CD%/build/diag-deploy"
if %errorlevel% neq 0 goto :fail
echo === BUILD START ===
"%CM%" --build build/diag --target FittingRoom
if %errorlevel% neq 0 goto :fail
echo === DIAG_DONE ===
exit /b 0

:fail
echo ***DIAG_BUILD_FAILED*** errorlevel %errorlevel%
exit /b 1
