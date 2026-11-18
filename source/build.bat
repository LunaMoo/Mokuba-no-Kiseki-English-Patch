@echo off
rem Builds mokuba_patcher.exe with MSVC (run from a "Developer Command Prompt") or MinGW gcc.
where cl >nul 2>nul
if %errorlevel%==0 (
  cl /nologo /O2 /MT mokuba_patcher.c /Fe:mokuba_patcher.exe
  goto done
)
where gcc >nul 2>nul
if %errorlevel%==0 (
  gcc -O2 -static -municode -o mokuba_patcher.exe mokuba_patcher.c
  goto done
)
echo No compiler found. Open a Visual Studio Developer Command Prompt or install MinGW.
:done
pause
