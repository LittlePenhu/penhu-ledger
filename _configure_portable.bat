@echo off
rem PenHu Ledger - portable build configure (ASCII-only paths on purpose)
setlocal

set "BT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
rem 路径不用手填：%~dp0 就是本脚本所在目录（项目根）
set "PROJ=%~dp0"
if "%PROJ:~-1%"=="\" set "PROJ=%PROJ:~0,-1%"
set "NINJA=%PROJ%\..\tools\ninja.exe"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"

echo === vcvars64 ===
call "%BT%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo ERROR: vcvars64.bat failed
  endlocal
  exit /b 1
)
echo vcvars OK
where cl.exe

echo === configure ===
"%CMAKE%" -S "%PROJ%" -B "%PROJ%\build-portable" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=Release -DPENHU_WITH_NATIVE_DEPS=OFF -DPENHU_BUILD_TESTS=ON
echo CMAKE_CONFIGURE_EXIT=%errorlevel%

echo === build ===
"%CMAKE%" --build "%PROJ%\build-portable" --target penhu-tests-portable
echo CMAKE_BUILD_EXIT=%errorlevel%

endlocal
