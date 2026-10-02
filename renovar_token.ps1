# Gera JWT novo via POST /api/auth/generateToken e grava em sessao.json.
# Requer um token "pai" valido (MASTER ou outro com escopo /api/auth/*).
param(
    [string]$SessaoPath = (Join-Path $PSScriptRoot "sessao.json"),
    [string]$TokenPai = "",
    [string]$BaseUrl = "",
    [string]$ServerId = "",
    [switch]$SomenteTestar
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

function Resolver-Hsm($sessao) {
    $base = Normalizar-BaseUrl $BaseUrl
    $sid = ($ServerId -as [string]).Trim()
    $servidor = $null

    if ($sid -and $sessao.servidores) {
        $servidor = $sessao.servidores | Where-Object { $_.id -eq $sid } | Select-Object -First 1
        if (-not $servidor) { Sair "ERRO: servidor '$sid' nao encontrado em sessao.json." }
        if (-not $base) { $base = Normalizar-BaseUrl $servidor.base_url }
    } elseif ($base -and $sessao.servidores) {
        $servidor = $sessao.servidores | Where-Object {
            (Normalizar-BaseUrl $_.base_url) -eq $base
        } | Select-Object -First 1
    } else {
        $servidor = $sessao.servidores | Where-Object { $_.papel -eq "lab" } | Select-Object -First 1
        if ($servidor) {
            $base = Normalizar-BaseUrl $servidor.base_url
        } elseif ($sessao.endpoints.base_url) {
            $base = Normalizar-BaseUrl $sessao.endpoints.base_url
        }
    }

    if (-not $base) { Sair "ERRO: base_url ausente. Use -BaseUrl ou configure servidores[] em sessao.json." }
    return @{ base = $base; servidor = $servidor }
}

function AtualizarTokenSessao($path, $novoToken, $servidor) {
    $raw = Get-Content -Raw -Encoding UTF8 $path
    $raw = [regex]::Replace($raw, '("token"\s*:\s*")[^"]*(")', "`${1}$novoToken`${2}", 1)
    if ($servidor -and $servidor.id) {
        $sid = [regex]::Escape($servidor.id)
        $pat = '("id"\s*:\s*"' + $sid + '"[\s\S]*?"token"\s*:\s*")[^"]*(")'
        if ($raw -match $pat) {
            $raw = [regex]::Replace($raw, $pat, "`${1}$novoToken`${2}", 1)
        }
    }
    [System.IO.File]::WriteAllText($path, $raw, [System.Text.UTF8Encoding]::new($false))
}

if (-not (Test-Path $SessaoPath)) { Sair "ERRO: sessao.json nao encontrado." }

$sessao = Get-Content -Raw -Encoding UTF8 $SessaoPath | ConvertFrom-Json
$hsm = Resolver-Hsm $sessao
$base = $hsm.base
$servidor = $hsm.servidor

if (-not $TokenPai) {
    if ($servidor -and $servidor.token) {
        $TokenPai = $servidor.token.Trim()
    } else {
        $TokenPai = $sessao.token.Trim()
    }
} else {
    $TokenPai = $TokenPai.Trim()
}

if (-not $TokenPai) { Sair "ERRO: informe -TokenPai ou preencha token em sessao.json." }

Write-Host "HSM: $base"
if ($servidor -and $servidor.nome) { Write-Host "Servidor: $($servidor.nome)" }
Write-Host "Token pai: $($TokenPai.Length) chars"

$testArgs = @{
    SessaoPath = $SessaoPath
    BaseUrl    = $base
}
if ($servidor -and $servidor.id) { $testArgs.ServerId = $servidor.id }

if ($SomenteTestar) {
    & (Join-Path $PSScriptRoot "testar_token.ps1") @testArgs
    exit $LASTEXITCODE
}

$expira = (Get-Date).ToUniversalTime().AddYears(1).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'")
$reqBody = @{
    clientName          = "burn-in"
    expirationDateTime  = $expira
    authorizedEndpoints = @("/api/hardware/*", "/api/sams/*")
    authorizedRoles     = @("ALL")
    authorizedGroups    = @("ALL")
} | ConvertTo-Json -Compress

$genUri = "$base/api/auth/generateToken"
$headers = @{
    Accept         = "application/json"
    "Content-Type" = "application/json"
    Authorization  = "Bearer $TokenPai"
}

Write-Host "Gerando token em $genUri ..."
try {
    $gen = Invoke-RestMethod -Uri $genUri -Method POST -Headers $headers -Body $reqBody -TimeoutSec 30
} catch {
    $code = 0
    if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode }
    Write-Host "FALHA ao gerar token: HTTP $code"
    if ($code -eq 401) {
        $swagger = Url-Swagger $base
        Write-Host @"

O token pai tambem foi recusado (401) neste HSM: $base
O MASTER TOKEN pode estar invalido ou a chave deste equipamento foi rotacionada.

Proximo passo:
  1. No Swagger deste HSM ($swagger), clique Authorize.
  2. Com um JWT valido la, copie e rode:
       .\renovar_token.ps1 -BaseUrl "$base" -TokenPai "SEU_JWT_AQUI"
     (ou -ServerId "id-do-servidor" se estiver em sessao.json)
  3. Ou peca ao admin deste HSM um token com escopo /api/auth/* e /api/hardware/*.
"@
    }
    exit 1
}

$novo = $gen.token
if (-not $novo) { Sair "ERRO: resposta sem campo token." }

Write-Host "Token novo recebido ($($novo.Length) chars, expiresIn=$($gen.expiresIn)s)"
AtualizarTokenSessao $SessaoPath $novo $servidor
Write-Host "sessao.json atualizado (token raiz$(if ($servidor) { " e servidor $($servidor.id)" }))."

& (Join-Path $PSScriptRoot "testar_token.ps1") @testArgs
exit $LASTEXITCODE
