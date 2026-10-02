@echo off
chcp 65001 >nul
echo ============================================
echo Compilando Burn-in HSM950 (API REST)
echo ============================================

set "MINGW=C:\msys64\mingw64\bin"
if exist "%MINGW%\gcc.exe" (
    echo [OK] MinGW encontrado
    set "PATH=%MINGW%;%PATH%"
    if not exist "dll" mkdir dll
    "%MINGW%\gcc.exe" -Wall -O2 burnin.c -o dll\burnin.exe -lcurl -lws2_32
    if not errorlevel 1 (
        "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0organizar.ps1"
        if exist burnin.exe del /q burnin.exe
        echo.
        echo Compilacao bem-sucedida! Execute:
        echo   rodar.ps1
        echo   ou dll\burnin.exe --help
    ) else (
        echo [ERRO] Falha na compilacao
    )
) else (
    echo [ERRO] MinGW nao encontrado em %MINGW%
    echo Instale MinGW: https://winlibs.com
)

echo.
pause
