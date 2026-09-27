@echo off
rem Builds out\ets2_trainer.dll (SCS telemetry plugin) and out\selftest.exe.
rem Usage: build.cmd [test]    "test" additionally runs the self-test.
rem Works from any directory; sets up MSVC x64 itself when cl.exe is not on PATH.
setlocal EnableExtensions EnableDelayedExpansion

set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"
set "OUT=%SRC%\out"
set "OBJ=%OUT%\obj"

where cl.exe >nul 2>&1
if errorlevel 1 (
    call :setup_msvc
    if errorlevel 1 exit /b 1
)

for %%d in ("%OBJ%\common" "%OBJ%\plugin" "%OBJ%\tests") do (
    if not exist "%%~d" mkdir "%%~d"
)

set CXXFLAGS=/nologo /std:c++17 /O2 /EHsc /W4 /MT /DNDEBUG /DUNICODE /D_UNICODE ^
 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /utf-8 /permissive- ^
 /Zc:__cplusplus /Zi /FS /MP
set LIBS=kernel32.lib user32.lib shell32.lib ole32.lib uuid.lib
set LDFLAGS=/nologo /DEBUG /OPT:REF /OPT:ICF /INCREMENTAL:NO

set COMMON=log mem_access mem_scan vector_io calibration calibration_scalar calibration_velocity ^
 calibration_velocity_verify calibration_position calibration_orientation async_scan cloud_storage input_device ^
 trainer trainer_features trainer_motion trainer_status plugin_telemetry
set PLUGIN=plugin bridge platform_win
set TESTS=selftest fake_game unit_tests scenario_tests velocity_tests

call :compile common "%SRC%" "%COMMON%" || goto :fail
call :compile plugin "%SRC%" "%PLUGIN%" || goto :fail
call :compile tests "%SRC%\tests" "%TESTS%" || goto :fail

set "COMMON_OBJS="
for %%f in (%COMMON%) do set COMMON_OBJS=!COMMON_OBJS! "%OBJ%\common\%%f.obj"
set "PLUGIN_OBJS="
for %%f in (%PLUGIN%) do set PLUGIN_OBJS=!PLUGIN_OBJS! "%OBJ%\plugin\%%f.obj"
set "TEST_OBJS="
for %%f in (%TESTS%) do set TEST_OBJS=!TEST_OBJS! "%OBJ%\tests\%%f.obj"

echo Linking ets2_trainer.dll
link %LDFLAGS% /DLL /DEF:"%SRC%\ets2_trainer.def" /OUT:"%OUT%\ets2_trainer.dll" ^
 /PDB:"%OUT%\ets2_trainer.pdb" /IMPLIB:"%OBJ%\ets2_trainer.lib" %COMMON_OBJS% %PLUGIN_OBJS% %LIBS% || goto :fail

echo Linking selftest.exe
link %LDFLAGS% /SUBSYSTEM:CONSOLE /OUT:"%OUT%\selftest.exe" /PDB:"%OUT%\selftest.pdb" ^
 %COMMON_OBJS% %TEST_OBJS% %LIBS% || goto :fail

echo Build OK: "%OUT%\ets2_trainer.dll", "%OUT%\selftest.exe"
if /i "%~1"=="test" (
    "%OUT%\selftest.exe"
    exit /b !errorlevel!
)
exit /b 0

rem ---- :compile <objdir> <sourcedir> "<names>" ------------------------------------------
:compile
set "SOURCES="
for %%f in (%~3) do set SOURCES=!SOURCES! "%~2\%%f.cpp"
cl %CXXFLAGS% /c %SOURCES% /Fo"%OBJ%\%~1\\" /Fd"%OBJ%\%~1\vc.pdb"
exit /b %errorlevel%

rem ---- :setup_msvc -----------------------------------------------------------------------
:setup_msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSROOT="
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
)
if not defined VSROOT if exist "%ProgramFiles%\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat" set "VSROOT=%ProgramFiles%\Microsoft Visual Studio\18\Professional"
if not defined VSROOT (
    echo error: Visual Studio with the C++ x64 tools was not found.
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
where cl.exe >nul 2>&1
if errorlevel 1 (
    echo error: vcvars64.bat did not provide cl.exe
    exit /b 1
)
exit /b 0

:fail
echo Build FAILED
exit /b 1
