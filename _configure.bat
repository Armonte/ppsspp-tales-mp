@echo off
REM One-time CMake configure for the Windows SDL3 build (Ninja + clang-cl, like ymir-gekko).
REM Uses C:\dev\vcpkg unless VCPKG_ROOT is set. Afterwards use _build.bat.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set "PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin;C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
if "%VCPKG_ROOT%"=="" set "VCPKG_ROOT=C:\dev\vcpkg"
cd /d "%~dp0"
cmake -S . -B out\build\win-sdl -G Ninja ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_C_COMPILER=clang-cl ^
  -DCMAKE_CXX_COMPILER=clang-cl ^
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md ^
  -DUNITTEST=OFF -DHEADLESS=ON -DPPSSPP_SDL_ON_WINDOWS=ON
exit /b %errorlevel%
