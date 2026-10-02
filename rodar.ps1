$root = Split-Path -Parent $MyInvocation.MyCommand.Path
& (Join-Path $root "organizar.ps1")
$sessaoPath = Join-Path $root "sessao.json"
$exePath = Join-Path $root "dll\burnin.exe"

function Sair($msg) {
    Write-Host "ERRO: $msg"
    Read-Host "Pressione Enter para sair"
    exit 1
}

function DataHoraPadrao {
    # Mesmo padrao de sessao.json: YYYY-MM-DD HH:MM:SS (hora local).
    return (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
}

function GravarCampoSessao($path, $campo, $valor) {
    $raw = Get-Content -Raw -Encoding UTF8 $path
    $pat = '"' + [regex]::Escape($campo) + '"\s*:\s*"[^"]*"'
    if ($raw -notmatch $pat) { return }
    $novo = '"' + $campo + '":  "' + $valor + '"'
    $atualizado = [regex]::Replace($raw, $pat, $novo, 1)
    [System.IO.File]::WriteAllText($path, $atualizado, [System.Text.UTF8Encoding]::new($false))
}

function Ou($valor, $padrao) {
    if ($null -eq $valor -or "$valor" -eq "") { return $padrao }
    return $valor
}

if (-not (Test-Path $sessaoPath)) { Sair "sessao.json nao encontrado em $root" }
if (-not (Test-Path $exePath))    { Sair "burnin.exe nao encontrado em dll\. Rode compilar.bat." }

try {
    $sessao = Get-Content -Raw -Encoding UTF8 $sessaoPath | ConvertFrom-Json
} catch {
    Sair "sessao.json invalido: $($_.Exception.Message)"
}

# Toda a config sai do sessao.json; nada fica chumbado aqui.
$serverId = [string]$sessao.servidor_ativo_id
if ($serverId) {
    $lab = $sessao.servidores | Where-Object { $_.id -eq $serverId } | Select-Object -First 1
    if (-not $lab) { Sair "servidor_ativo_id '$serverId' nao encontrado em sessao.json." }
} else {
    $lab = $sessao.servidores | Where-Object { $_.papel -eq "lab" } | Select-Object -First 1
}

$base = Ou $lab.base_url $sessao.endpoints.base_url
$token = Ou $lab.token $sessao.token
$dur  = [int](Ou $sessao.duracao_programada_s 86400)
$apdu = [string](Ou $sessao.endpoints.apdu "00C002A008")
$reps = [int](Ou $sessao.endpoints.repetitions 10)

$exec = $sessao.execucao
$ops     = [int](Ou $exec.ops_por_ciclo 100)
$threads = [int](Ou $exec.threads 8)
$interv  = [int](Ou $exec.report_interval_s 60)
$sams    = [int](Ou $exec.sams 0)
$producao = [bool](Ou $exec.producao $false)
$timeout = [int](Ou $exec.timeout_s 600)
$retries = [int](Ou $exec.bench_retries 3)

$base = ([string]$base).Trim()    
$token = ([string]$token).Trim()
$apdu = $apdu.Trim()

if (-not $base)  { Sair "base_url ausente (servidores[].base_url ou endpoints.base_url)." }
if (-not $token) { Sair "token ausente (servidores[].token ou token na raiz)." }
if (($token.ToCharArray() | Where-Object { $_ -eq '.' }).Count -ne 2) {
    Sair "token nao tem as 3 partes de um JWT. Copie o valor inteiro para sessao.json."
}

# Authorization: Bearer <jwt> e montado no burnin.exe a partir de --token.
$argList = @(
    "--base-url", $base,
    "--token", $token,
    "--duration", "$dur",
    "--ops", "$ops",
    "--apdu", $apdu,
    "--repetitions", "$reps",
    "--timeout", "$timeout",
    "--bench-retries", "$retries",
    "--threads", "$threads",
    "--report-interval", "$interv",
    "--sams", "$sams"
)
if ($producao) { $argList += "--producao" }

Write-Host "Burn-in SAM fisico em $base"
Write-Host "Modo: $(if ($producao) { 'producao (qualifica lote)' } else { 'lab (burn-in)' })"
Write-Host "POST /api/hardware/benchmark { `"apdu`": `"$apdu`", `"repetitions`": $reps }"
Write-Host "Timeout HTTP: $timeout s | Retries: $retries"
Write-Host "Authorization: Bearer (token de sessao.json, $($token.Length) chars)"
Write-Host "Duracao: $dur s | Ops/ciclo: $ops | Threads: $threads | Relatorio: $interv s | SAMs: $sams"

$inicio = DataHoraPadrao
GravarCampoSessao $sessaoPath "horario_inicio" $inicio
GravarCampoSessao $sessaoPath "horario_fim" ""
GravarCampoSessao $sessaoPath "atualizado_em" $inicio
Write-Host "horario_inicio: $inicio"

$dllPath = Join-Path $root "dll"
if (Test-Path $dllPath) {
    $env:PATH = $dllPath + ";" + $env:PATH
}
$proc = Start-Process -FilePath $exePath `
    -WorkingDirectory $root `
    -ArgumentList $argList -NoNewWindow -Wait -PassThru
Write-Host "Exit code: $($proc.ExitCode)"
$fim = DataHoraPadrao
GravarCampoSessao $sessaoPath "horario_fim" $fim
GravarCampoSessao $sessaoPath "atualizado_em" $fim
Write-Host "horario_fim: $fim"
Read-Host "Pressione Enter para sair"
