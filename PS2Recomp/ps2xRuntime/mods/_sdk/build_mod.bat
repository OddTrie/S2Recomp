@echo off
REM Builds a code mod: every .c and .cpp file in <mod folder>\src becomes <mod folder>\mod.dll.
REM   build_mod.bat ..\HelloMod        (or drag a mod folder onto this file)
REM   build_mod.bat all                builds every mod folder next to _sdk that has a src folder
REM Needs Visual Studio (or the Build Tools) with "Desktop development with C++". Close the game first:
REM Windows keeps mod.dll locked while the game runs.
setlocal EnableDelayedExpansion
set "SDK=%~dp0"
if "%SDK:~-1%"=="\" set "SDK=%SDK:~0,-1%"

if "%~1"=="" (
    echo Usage: build_mod.bat ^<mod folder^>    or    build_mod.bat all
    pause
    exit /b 1
)

REM ---- Visual Studio's compiler (cl.exe) ----
where cl >nul 2>nul
if errorlevel 1 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" goto novs
    set "VSPATH="
    for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
    if not defined VSPATH goto novs
    call "!VSPATH!\Common7\Tools\VsDevCmd.bat" -arch=amd64 -no_logo >nul 2>nul
)

set FAILED=0
if /i "%~1"=="all" (
    for /d %%m in ("%SDK%\..\*") do (
        if exist "%%~fm\src" call :build "%%~fm"
    )
) else (
    call :build "%~f1"
)
if "%FAILED%"=="0" (echo All done.) else (echo Some mods did not build, see above.)
exit /b %FAILED%

:build
set "MOD=%~1"
set "SOURCES="
for %%f in ("%MOD%\src\*.c" "%MOD%\src\*.cpp") do set SOURCES=!SOURCES! "%%~f"
if not defined SOURCES (
    echo [!] No .c or .cpp files in "%MOD%\src"
    set FAILED=1
    exit /b 1
)
echo Building %~nx1 ...
set "OBJ=%TEMP%\s2r_mod_build_%~nx1"
if not exist "%OBJ%" mkdir "%OBJ%"
REM /MT: the C runtime is built into the DLL, so the mod needs nothing else installed.
cl /nologo /O2 /MT /LD /W3 /EHsc /std:c++17 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I"%SDK%" /Fo"%OBJ%\\" %SOURCES% /Fe"%MOD%\mod.dll" /link /NOIMPLIB /NOEXP >"%OBJ%\build.log"
if errorlevel 1 (
    type "%OBJ%\build.log"
    echo [!] %~nx1 did not build. If it says mod.dll cannot be opened, close the game and try again.
    set FAILED=1
    exit /b 1
)
echo   -^> %MOD%\mod.dll
exit /b 0

:novs
echo [!] Visual Studio's C++ compiler was not found. Install Visual Studio or the Build Tools for
echo     Visual Studio with "Desktop development with C++", then run this again.
pause
exit /b 1
