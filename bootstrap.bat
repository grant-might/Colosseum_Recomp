@echo off
rem Fetch the pinned upstreams and copy this repo's overlay over them.
rem This does not build anything and does not push anything.
rem
rem Usage:
rem   bootstrap.bat                 fetch into .\work
rem   set WORK=D:\some\dir ^& bootstrap.bat
rem   bootstrap.bat --with-dolphin  also fetch the Dolphin submodule (about 880 MB)

setlocal enabledelayedexpansion

set "HERE=%~dp0"
if "%WORK%"=="" set "WORK=%HERE%work"

set "TEMPLATE_URL=https://github.com/ExpansionPak/ModernGekko-Template.git"
set "MODERNGEKKO_URL=https://github.com/ExpansionPak/ModernGekko.git"
set "DOLRECOMP_URL=https://github.com/ExpansionPak/DolRecomp.git"

set "TEMPLATE_COMMIT=c2553a99584b2045dbe46a70e1570994dae8beef"
set "MODERNGEKKO_COMMIT=edd460adad0e82b56909af1d793642c83b6d31c8"
set "DOLRECOMP_COMMIT=1bec3554ecc4817cf78319ca3d8a0669477f29fa"

set "WITH_DOLPHIN=0"
if /i "%~1"=="--with-dolphin" set "WITH_DOLPHIN=1"

where git >nul 2>nul
if errorlevel 1 (
  echo git is required
  exit /b 1
)

echo work dir: %WORK%
if not exist "%WORK%" mkdir "%WORK%"

rem 1. Outer tree. Submodules are fetched separately so the commits are the
rem    ones in versions.txt, not whatever the outer tree records.
call :fetch "%TEMPLATE_URL%" "%TEMPLATE_COMMIT%" "%WORK%\ModernGekko-Template"
if errorlevel 1 exit /b 1

rem 2. ModernGekko at the pinned commit, into the submodule path.
call :fetch "%MODERNGEKKO_URL%" "%MODERNGEKKO_COMMIT%" "%WORK%\ModernGekko-Template\lib\ModernGekko"
if errorlevel 1 exit /b 1

rem 3. DolRecomp at the pinned commit, into the other submodule path.
call :fetch "%DOLRECOMP_URL%" "%DOLRECOMP_COMMIT%" "%WORK%\ModernGekko-Template\lib\DolRecomp"
if errorlevel 1 exit /b 1

if "%WITH_DOLPHIN%"=="1" (
  echo fetching vendor\dolphin, this is the large one
  git -C "%WORK%\ModernGekko-Template\lib\ModernGekko" submodule update --init --depth 1 vendor/dolphin
  if errorlevel 1 exit /b 1
) else (
  echo skipped vendor\dolphin. The build needs it, see versions.txt.
)

rem 4. Overlay. Paths in overlay\ are rooted at the template tree.
echo applying overlay
xcopy /E /I /Y /Q "%HERE%overlay\*" "%WORK%\" >nul
if errorlevel 1 (
  echo overlay copy failed
  exit /b 1
)

echo.
echo done
echo   template    %WORK%\ModernGekko-Template  %TEMPLATE_COMMIT%
echo   moderngekko %WORK%\ModernGekko-Template\lib\ModernGekko  %MODERNGEKKO_COMMIT%
echo   dolrecomp   %WORK%\ModernGekko-Template\lib\DolRecomp  %DOLRECOMP_COMMIT%
echo.
echo sixteen files replaced by the overlay:
echo   Makefile
echo   lib\ModernGekko\CMakeLists.txt
echo   lib\ModernGekko\tools\frontend_config.cpp
echo   lib\ModernGekko\tools\moderngekko_launcher.cpp
echo   lib\ModernGekko\tools\moderngekko_run.cpp
echo   lib\ModernGekko\vendor\dolphin\Data\Sys\GameSettings\GC6E01.ini
echo   lib\ModernGekko\vendor\dolphin\GXRuntime\include\core\cpu.h
echo   lib\ModernGekko\vendor\dolphin\GXRuntime\src\core\cpu.c
echo   lib\ModernGekko\vendor\dolphin\Source\Core\Core\PowerPC\StaticRecomp\StaticRecompCore.cpp
echo   lib\ModernGekko\vendor\dolphin\Source\Core\Core\PowerPC\StaticRecomp\StaticRecompCore.h
echo   lib\ModernGekko\vendor\dolphin\Source\Core\Core\PowerPC\StaticRecomp\StaticRecompCore_Hooks.cpp
echo   lib\ModernGekko\vendor\dolphin\Source\Core\Core\PowerPC\StaticRecomp\StaticRecompCore_Run.cpp
echo   lib\ModernGekko\vendor\dolphin\Source\Core\Core\PowerPC\StaticRecomp\StaticRecompCore_SMC.cpp
echo   lib\ModernGekko\vendor\dolphin\module-template\CMakeLists.txt
echo   lib\ModernGekko\vendor\dolphin\module-template\gen_module_tables.py
echo   lib\ModernGekko\vendor\dolphin\module-template\module_export.c
echo.
echo lib\ModernGekko and lib\ModernGekko\vendor\dolphin are now dirty
echo submodules. That is expected.
echo Nothing was pushed and nothing was committed.

endlocal
exit /b 0

:fetch
set "F_URL=%~1"
set "F_COMMIT=%~2"
set "F_DEST=%~3"
if exist "%F_DEST%\.git" (
  echo already present: %F_DEST%
) else (
  if not exist "%~dp3" mkdir "%~dp3"
  echo cloning %F_URL%
  git clone --quiet "%F_URL%" "%F_DEST%"
  if errorlevel 1 exit /b 1
)
echo checking out %F_COMMIT% in %F_DEST%
git -C "%F_DEST%" fetch --quiet origin "%F_COMMIT%" 2>nul
git -C "%F_DEST%" -c advice.detachedHead=false checkout --quiet "%F_COMMIT%"
exit /b %errorlevel%
