@echo off
rem ============================================================================
rem  PenHu Ledger - portable build (configure + build), ASCII-only on purpose.
rem  This exists because this machine has MSVC but no MSBuild, so the
rem  "Visual Studio 17 2022" generator is unusable -- we must use Ninja,
rem  and Ninja needs the vcvars environment for cl.exe / INCLUDE / LIB.
rem ============================================================================
setlocal

set "BT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
rem 路径不用手填：%~dp0 就是本脚本所在目录（项目根）
set "PROJ=%~dp0"
if "%PROJ:~-1%"=="\" set "PROJ=%PROJ:~0,-1%"
set "NINJA=%PROJ%\..\tools\ninja.exe"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"

echo [1/3] vcvars64
call "%BT%\VC\Auxiliary\Build\vcvars64.bat"
echo VCVARS_EXIT=%errorlevel%
where cl.exe

echo.
echo [2/3] configure
"%CMAKE%" -S "%PROJ%" -B "%PROJ%\build-portable" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=Release -DPENHU_WITH_NATIVE_DEPS=OFF -DPENHU_BUILD_TESTS=ON
echo CONFIGURE_EXIT=%errorlevel%

echo.
echo [3/3] build
"%CMAKE%" --build "%PROJ%\build-portable" --target penhu-tests-portable
echo BUILD_EXIT=%errorlevel%

endlocal
