@echo off
REM Numerical check of the look-at / attach maths in src/replay/freecam.h.
REM
REM Validates the port against the GAME's own extraction formulas
REM (camFrame::ComputeHeading/Pitch/RollFromMatrix), which is what makes the
REM sign conventions falsifiable without launching GTA. Needs no game, no
REM Ghidra, and about two seconds.
REM
REM Re-run after ANY edit to freecam.h. Outside src/ so the ASI's CMake glob
REM does not pick it up.

setlocal
cd /d "%~dp0"

for /f "usebackq tokens=*" %%i in (`
  "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
`) do set "VSPATH=%%i"

if not defined VSPATH (
  echo Could not locate Visual Studio via vswhere.
  exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

cl /nologo /EHsc /std:c++20 /W3 /I "%~dp0..\src" lookat_test.cpp /Fe:lookat_test.exe >nul
if errorlevel 1 (
  echo Compile failed.
  exit /b 1
)

REM Fully qualified: the current directory is not on PATH under cmd /c.
"%~dp0lookat_test.exe"
set RC=%errorlevel%

del /q lookat_test.obj lookat_test.exe >nul 2>&1
exit /b %RC%
