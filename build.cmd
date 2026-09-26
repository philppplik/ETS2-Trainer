@echo off
rem ETS2 Trainer - builds the in-game plugin (C++), runs all tests and publishes the app.
rem Result: dist\ETS2Trainer\ETS2Trainer.exe (+ plugin\ets2_trainer.dll)
setlocal EnableExtensions
set "ROOT=%~dp0"
set "DIST=%ROOT%dist\ETS2Trainer"

echo.
echo [1/4] Plugin bauen + Selbsttest (C++) ...
call "%ROOT%src\plugin\build.cmd" test || goto :fail

echo.
echo [2/4] Tests (C#) ...
dotnet run --project "%ROOT%src\Ets2Trainer.Tests" -c Release || goto :fail

echo.
echo [3/4] App veroeffentlichen ...
dotnet publish "%ROOT%src\Ets2Trainer.App" -c Release -o "%DIST%" -nologo -v q || goto :fail

echo.
echo [4/4] Plugin beilegen ...
if not exist "%DIST%\plugin" mkdir "%DIST%\plugin"
copy /y "%ROOT%src\plugin\out\ets2_trainer.dll" "%DIST%\plugin\" >nul || goto :fail

echo.
echo Fertig: %DIST%\ETS2Trainer.exe
echo Naechster Schritt: Trainer starten ^> SETUP ^> "Installieren / Aktualisieren".
exit /b 0

:fail
echo.
echo BUILD FEHLGESCHLAGEN - siehe Ausgabe oben.
exit /b 1
