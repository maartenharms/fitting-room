@echo off
rem Twin of the release build with /Zi + /DEBUG:FULL for crash symbolication.
rem /OPT:REF /OPT:ICF are forced because /DEBUG flips their defaults - the
rem layout must match the shipped Release DLL byte-for-byte at the crash RVAs
rem (verified by tools-side byte compare before trusting any symbol).
rem OUTPUT_FOLDER is redirected so the POST_BUILD copy can NEVER deploy this.
call :main > "%~dp0build_symbols.log" 2>&1
exit /b %errorlevel%

:main
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 goto :fail
set "VCPKG_ROOT=C:\Users\Maarten\vcpkg"
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "CM=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cd /d "C:\Studios\Mod Studio\Outfit Slots"
echo === CONFIGURE START ===
"%CM%" -S . -B build/symbols -G Ninja ^
  -DCMAKE_MAKE_PROGRAM="C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe" ^
  -DCMAKE_CXX_COMPILER=cl.exe ^
  -DCMAKE_CXX_FLAGS="/permissive- /Zc:preprocessor /EHsc /MP /W4 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE /Zi" ^
  -DCMAKE_SHARED_LINKER_FLAGS="/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO" ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md ^
  -DVCPKG_OVERLAY_TRIPLETS=cmake ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DOUTPUT_FOLDER="%TEMP%\outfitslots_symbols_deploy"
if errorlevel 1 goto :fail
echo === BUILD START ===
"%CM%" --build build/symbols
if errorlevel 1 goto :fail
echo === ALL_DONE ===
exit /b 0

:fail
echo ***BUILD_FAILED*** errorlevel %errorlevel%
exit /b 1
