# Burn-in HSM / SAM

Ferramenta de burn-in via **API REST** do HSM (`hsmrest`): executa benchmarks APDU por longos períodos, grava status/relatório e expõe um dashboard local.

Alvo atual de lab: `http://10.100.0.155:8080` (configurável em `sessao.json`).

## Requisitos

- Windows
- [MinGW](https://winlibs.com) em `C:\msys64\mingw64` (ou ajuste `compilar.bat`)
- libcurl (vem com o MinGW / DLLs em `dll\`)
- Python 3 (só para o dashboard)
- JWT válido **deste** HSM (Bearer) — não use chave SSH

## Configuração (`sessao.json`)

Toda a execução sai daqui:

| Campo | Uso |
|--------|-----|
| `servidor_ativo_id` / `servidores[]` | Qual HSM usar (`base_url` + `token`) |
| `token` | JWT Bearer (raiz e/ou por servidor) |
| `duracao_programada_s` | Duração do burn-in (ex.: 86400 = 24 h) |
| `endpoints` | healthCheck, benchmark, APDU, repetitions |
| `execucao` | ops, threads, timeout, produção, etc. |

**Token:** string JWT em 3 partes (`eyJ... . eyJ... . assinatura`).  
Se o HSM responder **401 Unauthorized**, o JWT não é aceito por aquele aparelho (assinatura/chave diferente). Peça ao admin um MASTER novo **desse** IP; SSH / chave OpenSSH **não** autentica a API.

## Compilar

```bat
compilar.bat
```

Gera `dll\burnin.exe` e organiza DLLs/logs.

## Rodar o burn-in

```powershell
cd <pasta-do-projeto>
.\rodar.ps1
```

Lê `sessao.json` e chama `dll\burnin.exe` com `--base-url`, `--token`, duração, etc.

### Testar / renovar token

```powershell
.\testar_token.ps1 -BaseUrl "http://10.100.0.155:8080" -Token "SEU_JWT"
.\renovar_token.ps1 -BaseUrl "http://10.100.0.155:8080" -TokenPai "JWT_ACEITO_PELO_HSM"
```

`renovar_token.ps1` só funciona se o **TokenPai** já for aceito pelo HSM (`POST /api/auth/generateToken`).

Swagger do equipamento: `http://IP:8080/swagger-ui/index.html`  
Authorize → cole o JWT → `POST /api/hardware/benchmark` deve retornar **200**.

## Dashboard

Sobe um servidor local em **http://localhost:8000** (não confundir com a porta **8080** do HSM).

| Ação | Como |
|------|------|
| Abrir | Duplo clique em `abrir_dashboard.bat` ou atalho **Burn-in Dashboard** |
| Parar | `parar_dashboard.bat` |
| Subir no login do Windows | `instalar_inicio_automatico.bat` (uma vez) |
| Remover auto-start | `.\instalar_inicio_automatico.ps1 -Remover` |

## Estrutura útil

```
burnin.c              código do burn-in
dll\burnin.exe        binário + DLLs
sessao.json           config + token
rodar.ps1             sobe o burn-in
iniciar_burnin.bat    atalho Windows para rodar.ps1
testar_token.ps1      valida JWT no benchmark
renovar_token.ps1     gera JWT novo (com TokenPai válido)
dashboard\            UI + serve_dashboard.py
organizar.ps1         move exe/dll/log para pastas
```

## Artefatos gerados (não versionar como fonte)

- `burnin_status.json`, `burnin_report.csv`, `burnin_falhas.txt`
- `logs\`, `resultados\`
- `build_*.txt`

## Segurança

- Não grave chave **SSH** em `sessao.json` nem no repositório.
- JWT é credencial: evite commit público; prefira token só no ambiente de lab.
- O dashboard escuta em `localhost` neste PC.

## Operação de rotina

No dashboard (`http://localhost:8000`), abra **Operação de rotina**:

1. Preencha lote / produto / câmara / operador / duração e o HSM ativo → **Salvar lote / HSM** (grava `sessao.json`).
2. Se precisar de JWT novo: cole um TokenPai aceito por esse HSM → **Renovar JWT** (ou `renovar_token.ps1`).
3. **Ping HSM** (rede) → **Testar JWT** → se OK, **Iniciar Burn-in**.
4. Ao terminar: **Exportar ZIP** ou histórico em **Histórico de runs** (`resultados\`).

Atalhos do dia a dia:

- Dashboard no login: `instalar_inicio_automatico.bat` (uma vez)
- Abrir a tela: atalho **Burn-in Dashboard** ou `abrir_dashboard.bat`
- Burn-in sem dashboard: `iniciar_burnin.bat` ou `rodar.ps1`

## Fluxo rápido

1. Compilar (`compilar.bat`)
2. Garantir JWT aceito no HSM (Swagger = 200)
3. Atualizar lote/HSM na rotina da dashboard (ou `sessao.json`)
4. `iniciar_burnin.bat` / `rodar.ps1` ou Iniciar na dashboard (`:8000`)
