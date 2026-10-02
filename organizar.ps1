# Recolhe .exe/.dll/.log soltos na raiz para dll\ e logs\.
# Idempotente: pode rodar em todo compile, rodar.ps1 e dashboard.

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$dllDir = Join-Path $root "dll"
$logDir = Join-Path $root "logs"

New-Item -ItemType Directory -Force -Path $dllDir, $logDir | Out-Null

function Mover-Para($src, $destDir) {
    if (-not (Test-Path $src)) { return }
    $nome = Split-Path $src -Leaf
    $dest = Join-Path $destDir $nome
    if ([string]::Equals((Resolve-Path $src).Path, (Join-Path (Resolve-Path $destDir).Path $nome), [StringComparison]::OrdinalIgnoreCase)) {
        return
    }
    if (Test-Path $dest) { Remove-Item $dest -Force }
    Move-Item -LiteralPath $src -Destination $dest -Force
}

Get-ChildItem -LiteralPath $root -File -ErrorAction SilentlyContinue | ForEach-Object {
    $n = $_.Name
    $ext = $_.Extension.ToLowerInvariant()
    if ($ext -eq ".dll" -or $n -ieq "burnin.exe") {
        Mover-Para $_.FullName $dllDir
    } elseif ($ext -eq ".log" -or $n -ieq "burnin_log.txt") {
        Mover-Para $_.FullName $logDir
    }
}
