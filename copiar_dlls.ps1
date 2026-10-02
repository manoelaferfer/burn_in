# Repoe em dll\ as bibliotecas que o burnin.exe carrega na abertura.
# Elas nao ficam no Git: vem do MSYS2. Sem elas o Windows mata o processo
# antes do main (erro "libcurl-4.dll nao foi encontrado").

param([string]$Mingw = "C:\msys64\mingw64\bin")

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$destino = Join-Path $root "dll"

$necessarias = @(
    "libcurl-4.dll",
    "libcrypto-3-x64.dll",
    "libssl-3-x64.dll",
    "libbrotlicommon.dll",
    "libbrotlidec.dll",
    "libiconv-2.dll",
    "libidn2-0.dll",
    "libintl-8.dll",
    "libnghttp2-14.dll",
    "libnghttp3-9.dll",
    "libngtcp2-16.dll",
    "libngtcp2_crypto_ossl-0.dll",
    "libpsl-5.dll",
    "libssh2-1.dll",
    "libunistring-5.dll",
    "libzstd.dll",
    "zlib1.dll"
)

New-Item -ItemType Directory -Force -Path $destino | Out-Null

if (-not (Test-Path $Mingw)) {
    Write-Host "AVISO: MSYS2 nao encontrado em $Mingw" -ForegroundColor Yellow
    Write-Host "Instale o MSYS2/MinGW ou rode: copiar_dlls.ps1 -Mingw <caminho\bin>"
    exit 1
}

$copiadas = 0
$ausentes = @()
$travadas = @()

foreach ($nome in $necessarias) {
    $src = Join-Path $Mingw $nome
    $dst = Join-Path $destino $nome
    if (-not (Test-Path $src)) { $ausentes += $nome; continue }
    if (Test-Path $dst) {
        # Só repoe se o tamanho divergir (MSYS2 atualizado).
        if ((Get-Item $src).Length -eq (Get-Item $dst).Length) { continue }
    }
    try {
        Copy-Item $src -Destination $dst -Force -ErrorAction Stop
        $copiadas++
    } catch {
        $travadas += $nome
    }
}

if ($copiadas -gt 0) { Write-Host "DLLs repostas em dll\: $copiadas" -ForegroundColor Green }
if ($travadas)  { Write-Host "Em uso (feche o burnin.exe): $($travadas -join ', ')" -ForegroundColor Yellow }
if ($ausentes)  { Write-Host "Nao existem no MSYS2: $($ausentes -join ', ')" -ForegroundColor Red }

$total = @(Get-ChildItem $destino -Filter *.dll -ErrorAction SilentlyContinue).Count
if ($total -lt $necessarias.Count) {
    Write-Host "dll\ tem $total de $($necessarias.Count) bibliotecas." -ForegroundColor Yellow
    exit 1
}
exit 0
