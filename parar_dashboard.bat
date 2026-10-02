@echo off
title Parar Dashboard Burn-in
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0dashboard\parar_dashboard.ps1"
timeout /t 2 /nobreak >nul
