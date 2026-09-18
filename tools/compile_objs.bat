@echo off
rem Compiles named object files of a plugin target and NOTHING else: no link,
rem no POST_BUILD copy into the live mod folder. The game may be up.
rem   objs.bat <repo dir> <ninja object target> [<more targets>...]
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "VCPKG_ROOT=%USERPROFILE%\vcpkg"
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "CM=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cd /d "%~1"
shift
:loop
if "%~1"=="" goto :done
echo === OBJ %~1 ===
"%CM%" --build build/release --target "%~1"
if %errorlevel% neq 0 (
    echo ***OBJ_FAILED*** %~1
    exit /b 1
)
shift
goto :loop
:done
echo === OBJS_DONE ===
