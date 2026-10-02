@echo off
title Instalar inicio automatico - Dashboard Burn-in
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0instalar_inicio_automatico.ps1"
if errorlevel 1 pause
