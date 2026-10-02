# Testa se o JWT aceita POST /api/hardware/benchmark (HTTP 200).
param(
    [string]$SessaoPath = (Join-Path $PSScriptRoot "sessao.json"),
    [string]$BaseUrl = "",
    [string]$ServerId = "",
    [string]$Token = ""
)

function Sair($msg, $code = 1) {
    Write-Host $msg
    exit $code
}

function Normalizar-BaseUrl([string]$raw) {
    $u = ($raw -as [string]).Trim().TrimEnd('/')
    if (-not $u) { return "" }
    if ($u -notmatch '^https?://') { $u = "http://$u" }
    return $u
}

function Url-Swagger([string]$base) {
    return (Normalizar-BaseUrl $base) + "/swagger-ui/index.html"
}

if (-not (Test-Path $SessaoPath)) { Sair "ERRO: sessao.json nao encontrado." }

$sessao = Get-Content -Raw -Encoding UTF8 $SessaoPath | ConvertFrom-Json
$base = Normalizar-BaseUrl $BaseUrl
$servidor = $null

if ($ServerId -and $sessao.servidores) {
    $servidor = $sessao.servidores | Where-Object { $_.id -eq $ServerId } | Select-Object -First 1
    if (-not $servidor) { Sair "ERRO: servidor '$ServerId' nao encontrado em sessao.json." }
    if (-not $base) { $base = Normalizar-BaseUrl $servidor.base_url }
} elseif (-not $base) {
    $servidor = $sessao.servidores | Where-Object { $_.papel -eq "lab" } | Select-Object -First 1
    if ($servidor) {
        $base = Normalizar-BaseUrl $servidor.base_url
    } else {
        $base = Normalizar-BaseUrl $sessao.endpoints.base_url
    }
}

if (-not $Token) {
    if ($servidor -and $servidor.token) {
        $Token = $servidor.token.Trim()
    } else {
        $Token = $sessao.token.Trim()
    }
} else {
    $Token = $Token.Trim()
}

$apdu = if ($sessao.endpoints.apdu) { $sessao.endpoints.apdu.Trim() } else { "00C002A008" }
$reps = if ($sessao.endpoints.repetitions) { [int]$sessao.endpoints.repetitions } else { 10 }

if (-not $base)  { Sair "ERRO: base_url ausente. Use -BaseUrl ou configure sessao.json." }
if (-not $Token) { Sair "ERRO: token ausente. Use -Token ou preencha sessao.json." }
if (($Token.ToCharArray() | Where-Object { $_ -eq '.' }).Count -ne 2) {
    Sair "ERRO: token nao parece um JWT (esperadas 3 partes separadas por ponto)."
}

$uri = "$base/api/hardware/benchmark"
$body = @{ apdu = $apdu; repetitions = $reps } | ConvertTo-Json -Compress
$headers = @{
    Accept         = "application/json"
    "Content-Type" = "application/json"
    Authorization  = "Bearer $Token"
}

Write-Host "Alvo: $uri"
Write-Host "APDU: $apdu | repetitions: $reps | token: $($Token.Length) chars"

try {
    $resp = Invoke-RestMethod -Uri $uri -Method POST -Headers $headers -Body $body -TimeoutSec 30
    if ($resp.success -eq $true) {
        Write-Host "OK: HTTP 200, success=true, totalErrors=$($resp.totalErrors)"
        exit 0
    }
    Write-Host "FALHA: HTTP 200 mas success=$($resp.success)"
    exit 2
} catch {
    $code = 0
    if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode }
    Write-Host "FALHA: HTTP $code"
    if ($code -eq 401) {
        $swagger = Url-Swagger $base
        Write-Host "Token recusado neste HSM ($base)."
        Write-Host "Swagger: $swagger"
        Write-Host "Rode: .\renovar_token.ps1 -BaseUrl `"$base`" -TokenPai `"SEU_JWT_AQUI`""
    }
    exit 1
}
