@echo off
title Burn-in HSM/SAM
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0rodar.ps1"
if errorlevel 1 pause
