# Instala inicio automatico da dashboard no login + atalho no Desktop.
# Duplo clique em instalar_inicio_automatico.bat
param(
    [switch]$Remover
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$dash = Join-Path $root "dashboard"
$loginPs1 = Join-Path $dash "iniciar_no_login.ps1"
$abrirBat = Join-Path $root "abrir_dashboard.bat"
$startupDir = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\Startup"
$desktopDir = [Environment]::GetFolderPath("Desktop")
$startupLnk = Join-Path $startupDir "Burn-in Dashboard.lnk"
$desktopLnk = Join-Path $desktopDir "Burn-in Dashboard.lnk"
$vbsPath = Join-Path $dash "iniciar_no_login.vbs"

function Novo-Atalho($caminho, $alvo, $argsLinha, $workdir, $desc) {
    $w = New-Object -ComObject WScript.Shell
    $s = $w.CreateShortcut($caminho)
    $s.TargetPath = $alvo
    $s.Arguments = $argsLinha
    $s.WorkingDirectory = $workdir
    $s.Description = $desc
    $s.WindowStyle = 7
    $s.Save()
}

if (-not (Test-Path $loginPs1)) { throw "Nao achei $loginPs1" }
if (-not (Test-Path $abrirBat)) { throw "Nao achei $abrirBat" }

if ($Remover) {
    foreach ($p in @($startupLnk, $desktopLnk, $vbsPath)) {
        if (Test-Path $p) {
            Remove-Item $p -Force
            Write-Host "Removido: $p"
        }
    }
    Write-Host "Inicio automatico removido."
    exit 0
}

# VBS sem janela preta no login
$vbs = @"
Set sh = CreateObject("WScript.Shell")
sh.CurrentDirectory = "$dash"
cmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""$loginPs1"""
sh.Run cmd, 0, False
"@
[System.IO.File]::WriteAllText($vbsPath, $vbs, [System.Text.Encoding]::ASCII)

Novo-Atalho $startupLnk "wscript.exe" "`"$vbsPath`"" $dash "Sobe a dashboard Burn-in no login"
Novo-Atalho $desktopLnk $abrirBat "" $root "Abre a dashboard Burn-in (http://localhost:8000)"

Write-Host ""
Write-Host "OK: dashboard vai subir sozinha no login do Windows." -ForegroundColor Green
Write-Host "  Startup: $startupLnk"
Write-Host "  Desktop: $desktopLnk"
Write-Host ""
Write-Host "Agora: use o atalho 'Burn-in Dashboard' no Desktop (abre o Chrome)."
Write-Host "Para remover: .\instalar_inicio_automatico.ps1 -Remover"
Write-Host ""

# Sobe agora para nao precisar reiniciar o PC
& (Join-Path $dash "iniciar_dashboard.ps1")
exit $LASTEXITCODE
