/******************************************************************************
 * burnin.c - Burn-in para chips SAM do HSM950 via REST API
 *
 * O QUE FAZ:
 *   1. Conecta no HSM com Bearer JWT token (documentacao HSM950)
 *   2. Lista todos os chips SAM:  GET  /api/sams/list
 *   3. Envia APDU de operacao:    POST /api/sams/exchangeApdu
 *   4. Mede taxa de sucesso, latencia e erros de cada chip
 *   5. Gera relatorio (burnin_report.csv) e alimenta a dashboard
 *   6. Gera status (burnin_status.json) e alimenta a dashboard
 *   7. Latência é o tempo que o HSM demora para responder a uma chamada do benchmark.
 *
 * COMPILAR:   compilar.bat
 * EXECUTAR:   burnin.exe --base-url http://IP:8080 --token JWT --duration 21600
 ******************************************************************************/

#define _CRT_SECURE_NO_WARNINGS
#define _WIN32_WINNT 0x0601

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <curl/curl.h>
#include <windows.h>
#include <process.h>

/* Pastas ao lado do .exe: dll\ (libcurl) e logs\ (*.log). */
static char g_exe_dir[MAX_PATH];
static char g_logs_dir[MAX_PATH];

static void init_runtime_dirs(void) {
    char buf[MAX_PATH];
    char project[MAX_PATH];
    char *slash;
    const char *folder;
    DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        snprintf(g_exe_dir, sizeof(g_exe_dir), ".");
    else {
        slash = strrchr(buf, '\\');
        if (slash) *slash = 0;
        snprintf(g_exe_dir, sizeof(g_exe_dir), "%s", buf);
    }
    /* dll\burnin.exe + dll\*.dll; logs em <projeto>\logs. */
    snprintf(project, sizeof(project), "%s", g_exe_dir);
    slash = strrchr(project, '\\');
    folder = slash ? slash + 1 : project;
    if (slash && (_stricmp(folder, "dll") == 0 || _stricmp(folder, "bin") == 0))
        *slash = 0;
    snprintf(g_logs_dir, sizeof(g_logs_dir), "%s\\logs", project);
    CreateDirectoryA(g_logs_dir, NULL);
    SetDllDirectoryA(g_exe_dir);
}

static FILE *fopen_log(const char *name, const char *mode) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", g_logs_dir, name);
    return fopen(path, mode);
}

/* ================================================================== */
/* 1. CONFIGURACAO                                                    */
/* ================================================================== */

#define MAX_CSN_LEN   64        /* tamanho do CSN (serial) do chip     */
#define MAX_SAMS      256       /* numero maximo de chips               */
#define MAX_THREADS   64        /* numero maximo de threads             */

typedef struct {
    char   base_url[256];       /* ex.: http://10.100.0.155:8080        */
    char   token[2048];         /* Bearer JWT (pode ser longo)          */
    int    duration_sec;        /* duracao do teste em segundos         */
    int    sam_count;           /* chips a testar (0 = todos)           */
    int    ops_per_cycle;       /* APDUs por ciclo por chip             */
    int    report_interval_sec; /* intervalo de relatorio parcial       */
    int    threads;             /* threads de carga (0 = auto)          */
    int    producao;            /* 1 = lote SAM via /api/hardware/benchmark */
    int    verify_pin;          /* 1 = inclui VERIFY PIN no modo exchangeApdu */
    char   apdu[32];            /* APDU do benchmark (doc: 00C002A008) */
    int    bench_reps;          /* repetitions do POST benchmark (doc: 10) */
    int    bench_timeout_sec;   /* CURLOPT_TIMEOUT do POST benchmark     */
    int    bench_retries;       /* tentativas em timeout/rede/5xx        */
} Config;

static Config cfg = {
    .base_url           = "http://10.100.0.155:8080",
    .token              = "",
    .duration_sec       = 86400,
    .sam_count          = 0,
    .ops_per_cycle      = 100,
    .report_interval_sec = 60,
    .threads            = 0,
    .producao           = 0,
    .verify_pin         = 0,
    .apdu               = "00C002A008",
    .bench_reps         = 10,
    .bench_timeout_sec  = 600,
    .bench_retries      = 3,
};

static volatile int burn_running = 0;

/* APDUs no modo exchangeApdu (so se --per-sam). Burn-in padrao:
 * POST /api/hardware/benchmark com cfg.apdu + cfg.bench_reps.        */
static const char *APDUS[] = {
    "00C002A008",  /* get_data   - GET DATA  */
    "00CA000000",  /* get_status - STATUS    */
    "0020000106",  /* verify_pin - VERIFY    */
};

static int stress_public = 0; /* legado: nao usar. Burn-in = benchmark. */

static int num_apdus(void) {
    return 1;
}

static int agg_hsm_mode(void) {
    return 1;
}

static const char *agg_target_id(void) {
    return "HSM:benchmark";
}

/* ================================================================== */
/* 2. LOG                                                             */
/* ================================================================== */

static FILE *log_fp = NULL;

static ULONGLONG t_burn_start_100ns = 0;

/* Relogio monotônico que, ao contrário de GetTickCount64/QPC, não avança
 * enquanto o Windows está suspenso ou hibernando. */
static ULONGLONG active_time_100ns(void) {
    ULONGLONG value = 0;
    if (QueryUnbiasedInterruptTime(&value))
        return value;
    return GetTickCount64() * 10000ULL;
}

static double burn_elapsed_s(void) {
    if (t_burn_start_100ns == 0) return 0;
    return (double)(active_time_100ns() - t_burn_start_100ns) / 10000000.0;
}

static void log_write(const char *level, const char *fmt, ...) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char buf[4096];
    int n = snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d [%s] ",
                     t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                     t->tm_hour, t->tm_min, t->tm_sec, level);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    puts(buf);
    if (log_fp) { fprintf(log_fp, "%s\n", buf); fflush(log_fp); }
}

#define LOG_INFO(...)  log_write("INFO", __VA_ARGS__)
#define LOG_ERROR(...) log_write("ERRO", __VA_ARGS__)
#define LOG_WARN(...)  log_write("AVISO", __VA_ARGS__)

/* ================================================================== */
/* 3. CLIENTE HTTP (libcurl) com header Bearer token                  */
/* ================================================================== */

struct resp_buf { char *data; size_t len; };

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *user) {
    size_t total = size * nmemb;
    struct resp_buf *rb = (struct resp_buf *)user;
    char *tmp = realloc(rb->data, rb->len + total + 1);
    if (!tmp) return 0;
    rb->data = tmp;
    memcpy(rb->data + rb->len, ptr, total);
    rb->len += total;
    rb->data[rb->len] = 0;
    return total;
}

/* Ultimo erro curl (para registrar o motivo real da falha). */
static char g_last_curl_err[160] = "";

#define MAX_FAIL_CATS 24
typedef struct {
    char cat[80];
    uint64_t n;
    char primeira[24];
    char ultima[24];
    char exemplo[200];
} FailCat;
static FailCat fail_cats[MAX_FAIL_CATS];
static int n_fail_cats = 0;
static uint64_t fail_eventos = 0;

static void json_strip(char *s) {
    for (; s && *s; s++) {
        if (*s == '"' || *s == '\\' || *s == '\n' || *s == '\r') *s = ' ';
    }
}

static const char *fail_categoria(const char *motivo) {
    static char buf[80];
    if (!motivo || !motivo[0]) return "falha na operacao";
    if (strstr(motivo, "timeout") || strstr(motivo, "Timeout") || strstr(motivo, "timed out"))
        return "timeout/rede";
    if (strstr(motivo, "HTTP 401") || strstr(motivo, "HTTP 403"))
        return "autenticacao";
    const char *hp = strstr(motivo, "HTTP ");
    if (hp) {
        int code = atoi(hp + 5);
        if (code >= 500) {
            snprintf(buf, sizeof(buf), "HTTP %d (servidor)", code);
            return buf;
        }
        if (code >= 400) {
            snprintf(buf, sizeof(buf), "HTTP %d (cliente)", code);
            return buf;
        }
    }
    if (strstr(motivo, "conexao") || strstr(motivo, "connection") || strstr(motivo, "Couldn't"))
        return "conexao";
    if (strstr(motivo, "SW ") || strstr(motivo, "status word") || strstr(motivo, "APDU SW"))
        return "APDU SW";
    if (strstr(motivo, "exchangeApdu") || strstr(motivo, "APDU"))
        return "APDU HTTP";
    return "falha na operacao";
}

static void fail_cat_add(const char *cat, const char *ts, const char *ex) {
    fail_eventos++;
    int i;
    for (i = 0; i < n_fail_cats; i++) {
        if (strcmp(fail_cats[i].cat, cat) == 0) {
            fail_cats[i].n++;
            snprintf(fail_cats[i].ultima, sizeof(fail_cats[i].ultima), "%s", ts);
            return;
        }
    }
    if (n_fail_cats >= MAX_FAIL_CATS) return;
    FailCat *c = &fail_cats[n_fail_cats++];
    memset(c, 0, sizeof(*c));
    snprintf(c->cat, sizeof(c->cat), "%s", cat);
    c->n = 1;
    snprintf(c->primeira, sizeof(c->primeira), "%s", ts);
    snprintf(c->ultima, sizeof(c->ultima), "%s", ts);
    snprintf(c->exemplo, sizeof(c->exemplo), "%s", ex ? ex : "");
    json_strip(c->exemplo);
}

static void fail_cats_reset(void) {
    n_fail_cats = 0;
    fail_eventos = 0;
    memset(fail_cats, 0, sizeof(fail_cats));
}

static void fail_explicacao(const char *cat, const char *exemplo,
                            char *oque, size_t oq_n, char *como, size_t co_n) {
    int is_bench = (exemplo && strstr(exemplo, "benchmark") != NULL);
    if (strstr(cat, "timeout")) {
        if (is_bench) {
            snprintf(oque, oq_n,
                     "O HSM nao concluiu o POST /api/hardware/benchmark no prazo do cliente (~%d s).",
                     cfg.bench_timeout_sec > 0 ? cfg.bench_timeout_sec : 600);
            snprintf(como, co_n,
                     "Rede ou HSM/SAM ocupados; curl encerrou com Timeout was reached. "
                     "Conta como falha de operacao (resposta nao chegou a tempo).");
        } else {
            snprintf(oque, oq_n, "O HSM nao respondeu no prazo do cliente HTTP (~15 s).");
            snprintf(como, co_n,
                     "O pedido ficou parado na rede ou o HSM estava ocupado demais para responder.");
        }
    } else if (strstr(cat, "servidor") || strstr(cat, "HTTP 5")) {
        snprintf(oque, oq_n, is_bench
                 ? "O HSM devolveu erro interno (HTTP 5xx) no POST /api/hardware/benchmark."
                 : "O HSM devolveu erro interno (HTTP 5xx).");
        snprintf(como, co_n,
                 "O servico REST estava no ar, mas falhou ao processar o pedido "
                 "(sobrecarga, memoria baixa ou falha interna).");
    } else if (strstr(cat, "autenticacao") || strstr(cat, "HTTP 401") || strstr(cat, "HTTP 403")) {
        snprintf(oque, oq_n, "O HSM recusou o acesso (HTTP 401/403).");
        snprintf(como, co_n,
                 "Token ausente, expirado, ou JWT de outro HSM/chave — o endpoint nao aceitou o Bearer.");
    } else if (strstr(cat, "cliente") || strstr(cat, "HTTP 4")) {
        snprintf(oque, oq_n, "O HSM recusou o pedido (HTTP 4xx).");
        snprintf(como, co_n,
                 "Pedido invalido (corpo/APDU) ou endpoint indisponivel para este token/modo.");
    } else if (strstr(cat, "conexao")) {
        snprintf(oque, oq_n, "Nao foi possivel conectar no HSM.");
        snprintf(como, co_n, "Rede, IP/porta ou o servico HTTP do HSM estava inacessivel.");
    } else if (strstr(cat, "APDU SW")) {
        snprintf(oque, oq_n, "O chip SAM respondeu, mas o status word da APDU nao foi 9000.");
        snprintf(como, co_n,
                 "HTTP 200 so diz que o HSM aceitou o pedido. O diagnostico do chip e o SW "
                 "no fim da resposta (9000 = OK; 6xxx/63Cx = erro/PIN).");
    } else if (strstr(cat, "APDU")) {
        snprintf(oque, oq_n, "Falha ao enviar APDU para o chip SAM.");
        snprintf(como, co_n,
                 "POST /api/sams/exchangeApdu (ou benchmark) nao retornou HTTP 200, "
                 "ou o JSON veio sem status word.");
    } else {
        snprintf(oque, oq_n, "A operacao do burn-in falhou.");
        snprintf(como, co_n, "A API nao retornou sucesso; veja o detalhe registrado no log.");
    }
}

/* Faz GET ou POST (se `post` != NULL) e devolve o codigo HTTP.
 * timeout_sec: 0 = 15s (healthCheck); benchmark usa valor maior (HSM demora minutos).
 * Codigos especiais: -1 init falhou, -2 erro curl (timeout/rede).   */
static int http_req(const char *url, const char *post, char **out, long timeout_sec) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(g_last_curl_err, sizeof(g_last_curl_err), "curl_easy_init falhou");
        return -1;
    }

    struct resp_buf rb = { NULL, 0 };
    struct curl_slist *headers = NULL;
    long to = timeout_sec > 0 ? timeout_sec : 15L;
    g_last_curl_err[0] = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rb);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, to);
    /* Benchmark pode ficar sem enviar bytes por >1 min; nao abortar cedo. */
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, to);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);

    /* Autenticacao via Bearer token (documentacao HSM950) */
    if (cfg.token[0]) {
        char auth[2200];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", cfg.token);
        headers = curl_slist_append(headers, auth);
        headers = curl_slist_append(headers, "Accept: application/json");
    }
    if (post) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post);
        headers = curl_slist_append(headers, "Content-Type: application/json");
    }
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (headers) curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        snprintf(g_last_curl_err, sizeof(g_last_curl_err), "%s", curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        free(rb.data);
        if (out) *out = NULL;
        return -2;
    }
    curl_easy_cleanup(curl);
    if (out) *out = rb.data;
    else free(rb.data);
    return (int)http_code;
}

/* ================================================================== */
/* 4. API DO HSM (endpoints da documentacao)                          */
/* ================================================================== */

/* GET /api/hardware/healthCheck - verifica acesso e token.
 * Retorna 1 se saudavel, 0 se token invalido/401, -1 se inacessivel. */
static int hsm_healthcheck(void) {
    char url[512], *body = NULL;
    snprintf(url, sizeof(url), "%s/api/hardware/healthCheck", cfg.base_url);
    int code = http_req(url, NULL, &body, 0);
    if (code == -2) {
        LOG_ERROR("HSM inacessivel em %s", cfg.base_url);
        free(body);
        return -1;
    }
    if (code == 401) {
        LOG_ERROR("Token invalido ou sem acesso (HTTP 401). Configure --token.");
        free(body);
        return 0;
    }
    LOG_INFO("HSM acessivel (HTTP %d)", code);
    free(body);
    return 1;
}

/* Extrai os CSNs (seriais) de um JSON de /api/sams/list.
 * Procura todo par  "csn": "valor".  Devolve quantos encontrou.     */
static int json_extract_csns(const char *json, char csns[MAX_SAMS][MAX_CSN_LEN], int max) {
    int n = 0;
    if (!json) return 0;
    const char *p = json;
    while (n < max && (p = strstr(p, "csn")) != NULL) {
        p = strchr(p, '"');                       /* inicio do valor   */
        if (!p) break;
        p = strchr(p + 1, '"');
        if (!p) break;
        const char *v = p + 1;
        p = strchr(v, '"');                       /* fim do valor      */
        if (!p) break;
        size_t len = (size_t)(p - v);
        if (len >= MAX_CSN_LEN) len = MAX_CSN_LEN - 1;
        memcpy(csns[n], v, len);
        csns[n][len] = 0;
        n++;
    }
    return n;
}

/* GET /api/sams/list - descobre todos os chips do HSM. */
static int hsm_list_sams(char csns[MAX_SAMS][MAX_CSN_LEN]) {
    char url[512], *body = NULL;
    snprintf(url, sizeof(url), "%s/api/sams/list", cfg.base_url);
    int code = http_req(url, NULL, &body, 0);
    int n = 0;
    if (code == 200 && body) {
        n = json_extract_csns(body, csns, MAX_SAMS);
        LOG_INFO("Lista de SAMs: %d chip(s) encontrado(s) via API", n);
    } else {
        LOG_ERROR("Falha ao listar SAMs (HTTP %d). Verifique o token.", code);
    }
    free(body);
    return n;
}

static int json_get_string(const char *json, const char *key, char *out, size_t out_sz);
static double json_get_number(const char *json, const char *key);
static int apdu_response_ok(int http_code, const char *body, char *motivo, size_t motivo_len);

/* GET /api/sams/details/{csn} - diagnostico individual de um chip.
 * Loga campos uteis do JSON (nao so HTTP 200).                      */
static int hsm_sam_details(const char *csn) {
    char url[512], *body = NULL;
    snprintf(url, sizeof(url), "%s/api/sams/details/%s", cfg.base_url, csn);
    int code = http_req(url, NULL, &body, 0);
    if (code == 200 && body) {
        char state[64] = "", err[96] = "";
        json_get_string(body, "state", state, sizeof(state));
        if (!state[0]) json_get_string(body, "status", state, sizeof(state));
        json_get_string(body, "error", err, sizeof(err));
        if (!err[0]) json_get_string(body, "lastError", err, sizeof(err));
        LOG_INFO("Diagnostico %s HTTP 200 state=%s err=%s",
                 csn, state[0] ? state : "?", err[0] ? err : "-");
        free(body);
        return 1;
    }
    LOG_WARN("Diagnostico %s falhou (HTTP %d)", csn, code);
    free(body);
    return 0;
}

/* POST /api/sams/initSams - inicializacao de SAMs (provisionamento).
 * Retorna 1 se o HSM retornou HTTP 200, 0 caso contrario.           */
static int hsm_init_sams(const char *src_group, const char *dst_group) {
    char url[512], payload[1024], *body = NULL;
    snprintf(url, sizeof(url), "%s/api/sams/initSams", cfg.base_url);
    snprintf(payload, sizeof(payload),
             "{\"sourceSamGroup\":\"%s\",\"destinationSamGroup\":\"%s\",\"sourceSamLevel\":1,\"destinationSamLevel\":1}",
             src_group, dst_group);
    int code = http_req(url, payload, &body, 0);
    if (code == 200) {
        LOG_INFO("initSams OK (HTTP 200)");
        free(body);
        return 1;
    }
    LOG_WARN("initSams falhou (HTTP %d). Grupos inexistentes sao ignorados.", code);
    free(body);
    return 0;
}

/* POST /api/sams/exchangeApdu - envia um APDU para um chip.
 * Sucesso so com HTTP 200 E status word ISO 7816 9000 (ou 61xx).    */
static int hsm_exchange_apdu(const char *csn, const char *apdu,
                             char *motivo, size_t motivo_len) {
    char url[512], payload[1024], *body = NULL;
    snprintf(url, sizeof(url), "%s/api/sams/exchangeApdu", cfg.base_url);
    snprintf(payload, sizeof(payload), "{\"csn\":\"%s\",\"messageId\":0,\"data\":\"%s\"}", csn, apdu);
    int code = http_req(url, payload, &body, 0);
    int ok = apdu_response_ok(code, body, motivo, motivo_len);
    free(body);
    return ok;
}

/* Estresse publico sem token: bombeia o endpoint /api/hardware/healthCheck.
 * Faz ate 3 tentativas com backoff curto para falhas transitórias.
 * Retorna 1 se HTTP 200, 0 caso contrario. motivo recebe causa da falha. */
static int hsm_stress_public(char *motivo, size_t motivo_len) {
    char url[512];
    snprintf(url, sizeof(url), "%s/api/hardware/healthCheck", cfg.base_url);

    const int max_attempts = 3;
    int last_code = 0;
    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        if (!burn_running) break;
        char *body = NULL;
        last_code = http_req(url, NULL, &body, 0);
        free(body);
        if (last_code == 200) {
            if (motivo && motivo_len) motivo[0] = 0;
            return 1;
        }
        /* Retry so em erro transitório (timeout/rede/5xx). */
        if (attempt < max_attempts && burn_running) {
            if (last_code == -2 || last_code >= 500 || last_code == 0)
                Sleep(200 * attempt);
            else
                break; /* 4xx nao adianta retry */
        }
    }

    if (motivo && motivo_len) {
        if (last_code == -2)
            snprintf(motivo, motivo_len, "timeout/rede: %s (apos %d tentativas)",
                     g_last_curl_err[0] ? g_last_curl_err : "curl", max_attempts);
        else if (last_code < 0)
            snprintf(motivo, motivo_len, "erro de conexao (code %d)", last_code);
        else
            snprintf(motivo, motivo_len, "HTTP %d em /api/hardware/healthCheck", last_code);
    }
    return 0;
}

/* Soma de erros no JSON do benchmark (capacity/regression). -1 = campo ausente. */
static double benchmark_error_count(const char *json) {
    static const char *keys[] = {
        "errorCount", "error_count", "totalErrors", "failedCount",
        "failures", "errors"
    };
    int i;
    if (!json) return -1;
    for (i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++) {
        double v = json_get_number(json, keys[i]);
        if (v >= 0)
            return v;
    }
    return -1;
}

/* POST /api/hardware/benchmark
 * Corpo documentado: {"apdu":"00C002A008","repetitions":10}
 * O HSM manda o APDU em todos os SAMs do soquete.
 * Falha se HTTP != 200, se error counts > 0, ou se houver SW != 9000/61xx.
 * Retry so em timeout/rede/5xx (igual healthCheck). */
static int hsm_benchmark(char *motivo, size_t motivo_len) {
    char url[512], payload[256], *body = NULL;
    static int logged_sample = 0;
    double nerr;
    int max_attempts = cfg.bench_retries > 0 ? cfg.bench_retries : 1;
    long to = cfg.bench_timeout_sec > 0 ? (long)cfg.bench_timeout_sec : 600L;
    int last_code = 0;
    int suspend_recovery_used = 0;

    snprintf(url, sizeof(url), "%s/api/hardware/benchmark", cfg.base_url);
    snprintf(payload, sizeof(payload),
             "{\"apdu\":\"%s\",\"repetitions\":%d}", cfg.apdu, cfg.bench_reps);

    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        if (!burn_running) break;
        free(body);
        body = NULL;
        ULONGLONG request_start = active_time_100ns();
        last_code = http_req(url, payload, &body, to);
        double request_active_ms =
            (double)(active_time_100ns() - request_start) / 10000.0;
        /* libcurl pode voltar com timeout imediatamente após o computador
         * despertar. Se o tempo efetivamente ativo ficou muito abaixo do
         * timeout, a chamada foi interrompida pela suspensão, não pelo HSM. */
        if (last_code == -2 && burn_running && !suspend_recovery_used &&
            request_active_ms + 5000.0 < (double)to * 1000.0) {
            suspend_recovery_used = 1;
            LOG_WARN("Windows retomado de suspensao durante o benchmark; "
                     "repetindo a chamada sem registrar falha.");
            attempt--;
            Sleep(500);
            continue;
        }
        if (!logged_sample && body && body[0]) {
            logged_sample = 1;
            LOG_INFO("Amostra /api/hardware/benchmark (%.180s)", body);
        }
        if (last_code == 200) {
            nerr = benchmark_error_count(body);
            if (nerr > 0) {
                if (motivo && motivo_len)
                    snprintf(motivo, motivo_len, "benchmark: %.0f erro(s) nos SAMs", nerr);
                free(body);
                return 0;
            }
            /* Benchmark pode devolver so metricas (sem SW). Sem SW, HTTP 200 vale. */
            if (body && body[0]) {
                char dummy[256];
                if (apdu_response_ok(200, body, dummy, sizeof(dummy))) {
                    free(body);
                    return 1;
                }
                if (strstr(dummy, "SW ausente")) {
                    free(body);
                    return 1;
                }
                if (motivo && motivo_len)
                    snprintf(motivo, motivo_len, "%s", dummy);
                free(body);
                return 0;
            }
            free(body);
            return 1;
        }
        if (last_code == 401) {
            static int logged_401 = 0;
            if (!logged_401) {
                logged_401 = 1;
                LOG_ERROR("HSM recusou o POST (HTTP 401 Unauthorized).");
                LOG_ERROR("O Bearer foi enviado, mas este HSM nao aceitou o JWT (invalido, revogado ou de outro aparelho).");
                LOG_ERROR("No Swagger deste HSM: Authorize + Execute em benchmarkTest deve dar HTTP 200.");
                LOG_ERROR("Encerrando para nao lotar de erro.");
            }
            burn_running = 0;
            break;
        }
        /* Retry so em erro transitório (timeout/rede/5xx). */
        if (attempt < max_attempts && burn_running) {
            if (last_code == -2 || last_code >= 500 || last_code == 0) {
                LOG_WARN("benchmark tentativa %d/%d falhou (code %d); retry...",
                         attempt, max_attempts, last_code);
                Sleep(200 * attempt);
            } else {
                break; /* 4xx nao adianta retry */
            }
        }
    }

    if (motivo && motivo_len) {
        if (last_code == -2)
            snprintf(motivo, motivo_len,
                     "timeout/rede em /api/hardware/benchmark: %s (apos %d tentativas)",
                     g_last_curl_err[0] ? g_last_curl_err : "curl", max_attempts);
        else if (last_code < 0)
            snprintf(motivo, motivo_len, "erro de conexao (code %d)", last_code);
        else
            snprintf(motivo, motivo_len, "HTTP %d em /api/hardware/benchmark", last_code);
    }
    free(body);
    return 0;
}

/* Lê um campo numerico generico de um JSON (ex.: temperatura).
 * Devolve o valor ou -1 se o campo nao existir.                     */
static double json_get_number(const char *json, const char *key) {
    if (!json) return -1;
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p = strchr(p + strlen(needle), ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') p++;              /* valores numericos podem vir como string */
    return atof(p);
}

/* Lê uma string de um JSON. Devolve 0 se achou, -1 caso contrario.     */
static int json_get_string(const char *json, const char *key,
                           char *out, size_t out_sz) {
    if (!json) return -1;
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p = strchr(p + strlen(needle), ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    size_t len = 0;
    while (p[len] && p[len] != '"') len++;
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, p, len);
    out[len] = 0;
    return 0;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_sw_hex4(const char *p, unsigned *sw_out) {
    int n[4];
    if (!p) return -1;
    for (int i = 0; i < 4; i++) {
        n[i] = hex_nibble(p[i]);
        if (n[i] < 0) return -1;
    }
    *sw_out = (unsigned)((n[0] << 12) | (n[1] << 8) | (n[2] << 4) | n[3]);
    return 0;
}

/* ISO 7816: 9000 = OK; 61xx = OK com mais dados. */
static int apdu_sw_ok(unsigned sw) {
    return sw == 0x9000 || (sw & 0xFF00u) == 0x6100u;
}

/* Tira os hex de um valor e le o SW (ultimos 2 bytes = 4 hex). */
static int sw_from_hex_blob(const char *val, unsigned *sw_out) {
    char hex[512];
    int h = 0;
    if (!val) return -1;
    for (const char *p = val; *p && h < (int)sizeof(hex) - 1; p++) {
        if (hex_nibble(*p) >= 0) hex[h++] = *p;
    }
    hex[h] = 0;
    if (h < 4) return -1;
    return parse_sw_hex4(hex + h - 4, sw_out);
}

/* HTTP 200 nao basta: o chip fala pelo status word no JSON. */
static int apdu_response_ok(int http_code, const char *body,
                            char *motivo, size_t motivo_len) {
    static int logged_sample = 0;
    unsigned sw = 0;
    char val[512];
    const char *hex_keys[] = { "data", "response", "apdu", "recvData", "rApdu", "message", NULL };

    if (!logged_sample && body && body[0]) {
        logged_sample = 1;
        LOG_INFO("Amostra exchangeApdu (%.180s)", body);
    }

    if (http_code == -2) {
        if (motivo && motivo_len)
            snprintf(motivo, motivo_len, "timeout/rede em exchangeApdu: %s",
                     g_last_curl_err[0] ? g_last_curl_err : "curl");
        return 0;
    }
    if (http_code != 200) {
        if (motivo && motivo_len)
            snprintf(motivo, motivo_len, "HTTP %d em exchangeApdu", http_code);
        return 0;
    }
    if (!body || !body[0]) {
        if (motivo && motivo_len)
            snprintf(motivo, motivo_len, "APDU SW ausente: HTTP 200 sem corpo");
        return 0;
    }

    if (json_get_string(body, "sw", val, sizeof(val)) == 0 ||
        json_get_string(body, "statusWord", val, sizeof(val)) == 0) {
        const char *p = val;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        while (*p == ' ') p++;
        if (parse_sw_hex4(p, &sw) == 0)
            goto got_sw;
    }
    {
        double sw_n = json_get_number(body, "sw");
        if (sw_n >= 0 && sw_n <= 65535.0) {
            sw = (unsigned)sw_n;
            goto got_sw;
        }
    }
    {
        double sw1 = json_get_number(body, "sw1");
        double sw2 = json_get_number(body, "sw2");
        if (sw1 >= 0 && sw2 >= 0 && sw1 <= 255 && sw2 <= 255) {
            sw = ((unsigned)sw1 << 8) | (unsigned)sw2;
            goto got_sw;
        }
    }
    for (int i = 0; hex_keys[i]; i++) {
        if (json_get_string(body, hex_keys[i], val, sizeof(val)) != 0) continue;
        if (sw_from_hex_blob(val, &sw) == 0) goto got_sw;
    }

    if (motivo && motivo_len)
        snprintf(motivo, motivo_len, "APDU SW ausente no JSON (HTTP 200)");
    return 0;

got_sw:
    if (apdu_sw_ok(sw)) return 1;
    if (motivo && motivo_len)
        snprintf(motivo, motivo_len, "APDU SW %04X (esperado 9000)", sw);
    return 0;
}

/* "1976 MB", "1.9G", "40 MB" -> megabytes. */
static double mem_str_to_mb(const char *s) {
    if (!s) return -1;
    while (*s == ' ' || *s == '\t' || *s == '"') s++;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s) return -1;
    while (*end == ' ') end++;
    if (*end == 'G' || *end == 'g') return v * 1024.0;
    if (*end == 'K' || *end == 'k') return v / 1024.0;
    return v;
}

/* Lê used/total/free só de dentro de "memoryInfo":{...}. */
static double json_meminfo_field(const char *json, const char *key) {
    const char *sec = strstr(json, "\"memoryInfo\"");
    if (!sec) return -1;
    const char *obj = strchr(sec, '{');
    if (!obj) return -1;
    /* memoryInfo contem um objeto "properties":{} antes dos valores.
     * O primeiro '}' fecha properties, nao memoryInfo. Limite a busca
     * pela proxima secao do healthCheck. */
    const char *end = strstr(obj, "\"diskInfo\"");
    if (!end) end = obj + strlen(obj);
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = obj;
    while (p && p < end) {
        p = strstr(p, needle);
        if (!p || p >= end) return -1;
        p = strchr(p + strlen(needle), ':');
        if (!p || p >= end) return -1;
        p++;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        return mem_str_to_mb(p);
    }
    return -1;
}

/* Dados reais do HSM lidos do /api/hardware/healthCheck */
typedef struct {
    int    http_code;          /* -1 se inacessivel                     */
    int    numberOfSams;       /* total de SAMs configurados            */
    int    numberOfOperative;  /* SAMs operacionais                     */
    char   alias[64];          /* alias configurado                     */
    char   uptime[32];         /* ex.: "8 days"                         */
    double load1, load5, load15;   /* load average                      */
    double mem_used_mb, mem_total_mb, mem_free_mb;
    double cpu_cores;
    double openSessions, maxSessions, sessionErrors, inQueue;
    double temp;               /* temperatura (-1 se nao existir)       */
    int    is_healthy;         /* 1/0/-1  (campo isHealthy da API)      */
    char   health_msg[192];
} HsmHealth;

/* GET /api/hardware/healthCheck - extrai os indicadores reais do HSM.  */
static HsmHealth hsm_read_health(void) {
    HsmHealth h;
    memset(&h, 0, sizeof(h));
    h.http_code = -1;
    h.numberOfSams = -1;
    h.numberOfOperative = -1;
    h.load1 = h.load5 = h.load15 = -1;
    h.mem_used_mb = h.mem_total_mb = h.mem_free_mb = -1;
    h.cpu_cores = -1;
    h.openSessions = h.maxSessions = h.sessionErrors = h.inQueue = -1;
    h.temp = -1;
    h.is_healthy = -1;
    h.health_msg[0] = 0;

    char url[512], *body = NULL;
    snprintf(url, sizeof(url), "%s/api/hardware/healthCheck", cfg.base_url);
    /* Sob benchmark, o healthCheck deste HSM pode levar mais de 15 s. */
    int code = http_req(url, NULL, &body, 60L);
    if (code == 200 && body) {
        h.http_code = 200;
        h.numberOfSams = (int)json_get_number(body, "numberOfSams");
        h.numberOfOperative = (int)json_get_number(body, "numberOfOperativeSams");
        json_get_string(body, "alias", h.alias, sizeof(h.alias));
        /* uptime real vem aninhado: "uptime":{"uptime":"8 days",...} */
        if (json_get_string(body, "uptime", h.uptime, sizeof(h.uptime)) != 0) {
            char tmp[32];
            const char *p = strstr(body, "\"uptime\":\"");
            if (p) {
                p += strlen("\"uptime\":\"");
                size_t l = 0;
                while (p[l] && p[l] != '"' && l < sizeof(tmp) - 1) l++;
                memcpy(tmp, p, l); tmp[l] = 0;
                snprintf(h.uptime, sizeof(h.uptime), "%s", tmp);
            }
        }
        h.load1 = json_get_number(body, "loadAverage1");
        h.load5 = json_get_number(body, "loadAverage5");
        h.load15 = json_get_number(body, "loadAverage15");
        h.mem_total_mb = json_meminfo_field(body, "total");
        h.mem_used_mb = json_meminfo_field(body, "used");
        h.mem_free_mb = json_meminfo_field(body, "free");
        if (h.mem_free_mb < 0 && h.mem_total_mb > 0 && h.mem_used_mb >= 0)
            h.mem_free_mb = h.mem_total_mb - h.mem_used_mb;
        if (strstr(body, "\"isHealthy\":true") || strstr(body, "\"isHealthy\": true"))
            h.is_healthy = 1;
        else if (strstr(body, "\"isHealthy\":false") || strstr(body, "\"isHealthy\": false"))
            h.is_healthy = 0;
        json_get_string(body, "message", h.health_msg, sizeof(h.health_msg));
        h.cpu_cores = json_get_number(body, "cores");
        h.openSessions = json_get_number(body, "openSessions");
        h.maxSessions = json_get_number(body, "maxSessions");
        h.sessionErrors = json_get_number(body, "sessionErrors");
        h.inQueue = json_get_number(body, "inQueue");
        static const char *tkeys[] = { "temperature", "temp_c", "cpu_temp", "cpu_temperature", "temp" };
        for (size_t i = 0; i < sizeof(tkeys) / sizeof(tkeys[0]); i++) {
            h.temp = json_get_number(body, tkeys[i]);
            if (h.temp > 0) break;
        }
    }
    free(body);
    return h;
}

/* GET /api/hardware/healthCheck - tenta extrair a temperatura.
 * Como o nome do campo pode variar, procura varios.                 */
/* ================================================================== */
/* 5. METRICAS POR CHIP                                               */
/* ================================================================== */

enum { RT_CAP = 256, HIST_CAP = 100000 };

typedef struct {
    char id[MAX_CSN_LEN + 16];
    uint64_t total, ok, fail;
    double lat_sum, lat_max;
    double *latencies;          /* todas as latencias (percentis)      */
    size_t lat_count, lat_cap;
    double temp_sum;            /* somatoria de temperaturas           */
    int temp_count;
    double temp_max;
    double first_half_ok, first_half_total;   /* degradacao 1a metade  */
    double second_half_ok, second_half_total; /* degradacao 2a metade  */
    double half_boundary;       /* fronteira (ops) entre as metades    */
    double rt_lat[RT_CAP];      /* janela rolante p/ dashboard         */
    size_t rt_lat_head, rt_lat_count;
    double rt_temp[RT_CAP];
    size_t rt_temp_head, rt_temp_count;
    double *hist_time, *hist_temp;   /* curva termica                  */
    size_t hist_count, hist_cap;
} Metrics;

static double t_start_epoch = 0;

static void met_init(Metrics *m, const char *csn, double half) {
    memset(m, 0, sizeof(*m));
    snprintf(m->id, sizeof(m->id), "SAM:%s", csn[0] ? csn : "?");
    m->half_boundary = half;
    m->lat_cap = 1048576;
    m->latencies = (double *)malloc(m->lat_cap * sizeof(double));
    if (!m->latencies) m->lat_cap = 0;
    m->hist_cap = HIST_CAP;
    m->hist_time = (double *)malloc(m->hist_cap * sizeof(double));
    m->hist_temp = (double *)malloc(m->hist_cap * sizeof(double));
    if (!m->hist_time || !m->hist_temp) m->hist_cap = 0;
}

static void met_add(Metrics *m, int success, double lat_ms) {
    m->total++;
    if (success) m->ok++; else m->fail++;
    m->lat_sum += lat_ms;
    if (lat_ms > m->lat_max) m->lat_max = lat_ms;
    if (m->latencies && m->lat_count < m->lat_cap)
        m->latencies[m->lat_count++] = lat_ms;

    m->rt_lat[m->rt_lat_head] = lat_ms;
    m->rt_lat_head = (m->rt_lat_head + 1) % RT_CAP;
    if (m->rt_lat_count < RT_CAP) m->rt_lat_count++;

    /* Metades por tempo (1a vs 2a metade da duracao), nao por ops estimadas. */
    if (cfg.duration_sec > 0) {
        double half_t = (double)cfg.duration_sec / 2.0;
        if (burn_elapsed_s() <= half_t) {
            m->first_half_total++;
            if (success) m->first_half_ok++;
        } else {
            m->second_half_total++;
            if (success) m->second_half_ok++;
        }
    }
}

static void met_add_temp(Metrics *m, double temp) {
    m->temp_sum += temp;
    m->temp_count++;
    if (temp > m->temp_max) m->temp_max = temp;
    m->rt_temp[m->rt_temp_head] = temp;
    m->rt_temp_head = (m->rt_temp_head + 1) % RT_CAP;
    if (m->rt_temp_count < RT_CAP) m->rt_temp_count++;
    if (m->hist_time && m->hist_temp && m->hist_count < m->hist_cap) {
        m->hist_time[m->hist_count] = (double)time(NULL);
        m->hist_temp[m->hist_count] = temp;
        m->hist_count++;
    }
    /* Anexa na curva termica da dashboard: rel, temp, total, ok, fail */
    FILE *ht = fopen_log("burnin_history.log", "a");
    if (ht) {
        double rel = (double)time(NULL) - t_start_epoch;
        fprintf(ht, "%.1f,%.1f,%llu,%llu,%llu\n", rel, temp, m->total, m->ok, m->fail);
        fclose(ht);
    }
}

static void met_add_error(Metrics *m, const char *motivo) {
    FILE *he = fopen_log("burnin_errors.log", "a");
    if (he) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        double rel = (double)now - t_start_epoch;
        const char *desc = (motivo && motivo[0]) ? motivo : "falha na operacao";
        char ts[32];
        snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                 t->tm_hour, t->tm_min, t->tm_sec);
        const char *cat = fail_categoria(desc);
        fail_cat_add(cat, ts, desc);
        fprintf(he, "%s,%.1f,%s,%s,%s\n", ts, rel, m->id, cat, desc);
        fclose(he);
    }
}

/* Reinicia o log de erros da sessao atual (evita misturar testes antigos). */
static void errors_log_reset(void) {
    fail_cats_reset();
    FILE *he = fopen_log("burnin_errors.log", "w");
    if (he) {
        fprintf(he, "# session_start_epoch=%.0f\n", t_start_epoch);
        fprintf(he, "# formato: data_hora,segundos,alvo,categoria,detalhe\n");
        fclose(he);
    }
}

/* Taxa de sucesso precisa: 100 * ok / (ok+fail). Sem arredondamento artificial. */
static double met_success_pct(uint64_t ok, uint64_t fail) {
    uint64_t total = ok + fail;
    if (!total) return 0;
    double sr = 100.0 * (double)ok / (double)total;
    if (sr > 100.0) sr = 100.0;
    if (sr < 0.0) sr = 0.0;
    return sr;
}

static double met_stddev(Metrics *m) {
    if (m->lat_count == 0) return 0.0;
    double mean = m->lat_count ? m->lat_sum / m->lat_count : 0;
    double sum_sq = 0;
    for (size_t i = 0; i < m->lat_count; i++) {
        double d = m->latencies[i] - mean;
        sum_sq += d * d;
    }
    return sqrt(sum_sq / m->lat_count);
}

static void met_report(Metrics *m, double elapsed) {
    uint64_t ok = m->ok, fail = m->fail;
    uint64_t total = ok + fail;
    double sr = met_success_pct(ok, fail);
    double al = total ? m->lat_sum / total : 0;
    LOG_INFO("=== %s ===", m->id);
    LOG_INFO("  Decorrido: %.1f s  Total: %llu  OK: %llu  Fail: %llu  Taxa: %.2f%%",
             elapsed, (unsigned long long)total, (unsigned long long)ok,
             (unsigned long long)fail, sr);
    LOG_INFO("  Latencia: avg %.3f ms | desvio %.3f ms | max %.3f ms",
             al, met_stddev(m), m->lat_max);
    if (m->temp_count > 0)
        LOG_INFO("  Temperatura: media %.1f C | max %.1f C (%d leituras)",
                 m->temp_sum / m->temp_count, m->temp_max, m->temp_count);
    if (m->first_half_total > 0 && m->second_half_total > 0) {
        double sr1 = 100.0 * m->first_half_ok / m->first_half_total;
        double sr2 = 100.0 * m->second_half_ok / m->second_half_total;
        LOG_INFO("  Degradacao: 1a metade %.2f%% vs 2a metade %.2f%% (%s)",
                 sr1, sr2, sr2 < sr1 - 0.5 ? "DEGRADANDO" : "estavel");
    }
}

/* ================================================================== */
/* 6. TIMER                                                           */
/* ================================================================== */

static void tmr_init(void) {}
static double tmr_elapsed(ULONGLONG s) {
    return (double)(active_time_100ns() - s) / 10000.0;
}

/* ================================================================== */
/* 7. THREADS DE BURN-IN POR CHIP                                     */
/* ================================================================== */

typedef struct {
    int idx;                    /* indice da thread (1..N)             */
    char csns[MAX_SAMS][MAX_CSN_LEN];  /* chips atribuidos a thread    */
    int n_csns;
    Metrics met;
    volatile int running;
    HANDLE th;
} SamThread;

static SamThread *threads = NULL;
static int nthr = 0;

/* Para o teste no relogio monotonico (GetTickCount64), nao num Sleep
 * unico de varias horas — esse Sleep pode nao acordar na hora certa. */
static int burn_stop_requested(void) {
    FILE *f = fopen("burnin.stop", "r");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int burn_time_up(void) {
    if (cfg.duration_sec <= 0 || t_burn_start_100ns == 0) return 0;
    return burn_elapsed_s() >= (double)cfg.duration_sec;
}

/* Cache dos indicadores reais do HSM, atualizado periodicamente pela
 * health_thread para nao bloquear a escrita do status a cada 1s.      */
static HsmHealth g_hsm;
static volatile int g_hsm_valid = 0;
static double g_session_errors_start = -1;

/* sessionErrors e um contador acumulado do HSM. Para avaliar esta execucao,
 * interessa apenas quanto ele aumentou desde o inicio do teste. */
static double hsm_new_session_errors(HsmHealth h) {
    if (g_session_errors_start < 0 || h.sessionErrors < 0) return -1;
    if (h.sessionErrors >= g_session_errors_start)
        return h.sessionErrors - g_session_errors_start;
    /* Se o contador foi zerado por reinicio do servico, os erros atuais
     * ocorreram depois desse reinicio e ainda pertencem ao teste. */
    return h.sessionErrors;
}

/* Variacao real dos indicadores do HSM ao longo do teste (min/max/media).
 * Acumulados pela health_thread a cada leitura do healthCheck.          */
static double hsm_load1_min = 0, hsm_load1_max = 0, hsm_load1_sum = 0, hsm_load1_n = 0;
static double hsm_mem_min = 0, hsm_mem_max = 0, hsm_mem_sum = 0, hsm_mem_n = 0;
static double hsm_sess_min = 0, hsm_sess_max = 0, hsm_sess_sum = 0, hsm_sess_n = 0;

static void hsm_telemetry_reset(void) {
    hsm_load1_min = hsm_load1_max = hsm_load1_sum = hsm_load1_n = 0;
    hsm_mem_min  = hsm_mem_max  = hsm_mem_sum  = hsm_mem_n  = 0;
    hsm_sess_min = hsm_sess_max = hsm_sess_sum = hsm_sess_n = 0;
}

static void hsm_telemetry_accum(HsmHealth *h) {
    if (h->load1 > 0) {
        if (!hsm_load1_n || h->load1 < hsm_load1_min) hsm_load1_min = h->load1;
        if (h->load1 > hsm_load1_max) hsm_load1_max = h->load1;
        hsm_load1_sum += h->load1; hsm_load1_n++;
    }
    if (h->mem_used_mb > 0) {
        if (!hsm_mem_n || h->mem_used_mb < hsm_mem_min) hsm_mem_min = h->mem_used_mb;
        if (h->mem_used_mb > hsm_mem_max) hsm_mem_max = h->mem_used_mb;
        hsm_mem_sum += h->mem_used_mb; hsm_mem_n++;
    }
    if (h->openSessions >= 0) {
        if (!hsm_sess_n || h->openSessions < hsm_sess_min) hsm_sess_min = h->openSessions;
        if (h->openSessions > hsm_sess_max) hsm_sess_max = h->openSessions;
        hsm_sess_sum += h->openSessions; hsm_sess_n++;
    }
}

static unsigned __stdcall health_thread(void *arg) {
    (void)arg;
    while (burn_running) {
        HsmHealth sample = hsm_read_health();
        if (sample.http_code == 200) {
            /* Uma falha temporaria do healthCheck nao deve apagar a
             * ultima telemetria valida exibida pela dashboard. */
            g_hsm = sample;
            g_hsm_valid = 1;
            hsm_telemetry_accum(&g_hsm);
        } else if (!g_hsm_valid) {
            /* Antes da primeira resposta valida, preserve o estado
             * "nao informado" (-1) em vez de exibir zeros falsos. */
            g_hsm = sample;
        }
        if (burn_time_up() || burn_stop_requested()) {
            burn_running = 0;
            break;
        }
        Sleep(5000);   /* atualiza os dados reais a cada 5s */
    }
    return 0;
}

/* Agrega as metricas de todas as threads em um unico alvo (modo publico:
 * o alvo real e o proprio HSM via healthCheck, nao chips individuais).
 * Snapshot ok/fail primeiro e deriva total = ok+fail para a taxa nao
 * passar de 100% sob leitura concorrente.                              */
static Metrics met_aggregate(const char *id) {
    Metrics a;
    memset(&a, 0, sizeof(a));
    snprintf(a.id, sizeof(a.id), "%s", id);
    uint64_t ok = 0, fail = 0; double lat_sum = 0;
    for (int i = 0; i < nthr; i++) {
        Metrics *m = &threads[i].met;
        uint64_t mok = m->ok, mfail = m->fail;
        ok += mok;
        fail += mfail;
        lat_sum += m->lat_sum;
        if (m->lat_max > a.lat_max) a.lat_max = m->lat_max;
        a.temp_sum += m->temp_sum; a.temp_count += m->temp_count;
        if (m->temp_max > a.temp_max) a.temp_max = m->temp_max;
        a.first_half_total += m->first_half_total; a.first_half_ok += m->first_half_ok;
        a.second_half_total += m->second_half_total; a.second_half_ok += m->second_half_ok;
    }
    a.ok = ok;
    a.fail = fail;
    a.total = ok + fail;
    a.lat_sum = a.total ? lat_sum / a.total : 0;   /* media simples p/ escrita */
    return a;
}

/* Burn-in dos SAMs fisicos: POST /api/hardware/benchmark (GET DATA x N). */
static void exercise_one_sam(Metrics *met, const char *csn) {
    (void)csn;
    for (int r = 0; r < cfg.ops_per_cycle && burn_running; r++) {
        ULONGLONG t0 = active_time_100ns();
        char motivo[256] = "";
        int ok = hsm_benchmark(motivo, sizeof(motivo));
        double ms = tmr_elapsed(t0);
        met_add(met, ok, ms);
        if (!ok) met_add_error(met, motivo);
    }
}

static unsigned __stdcall thread_wrap(void *arg) {
    SamThread *st = (SamThread *)arg;
    if (!agg_hsm_mode())
        LOG_INFO("%s: iniciando burn-in", st->met.id);
    if (!agg_hsm_mode())
        for (int i = 0; i < st->n_csns && burn_running; i++)
            hsm_sam_details(st->csns[i]);
    while (burn_running && st->running && !burn_time_up() && !burn_stop_requested()) {
        if (agg_hsm_mode()) {
            exercise_one_sam(&st->met, st->met.id);
        } else {
            for (int i = 0; i < st->n_csns && burn_running && !burn_time_up() && !burn_stop_requested(); i++)
                exercise_one_sam(&st->met, st->csns[i]);
            if (st->idx == 1 && burn_running) {
                char motivo[256] = "";
                if (!hsm_benchmark(motivo, sizeof(motivo)))
                    LOG_WARN("Benchmark do HSM falhou: %s", motivo[0] ? motivo : "HTTP != 200");
            }
        }
    }
    return 0;
}

/* ================================================================== */
/* 8. DASHBOARD EM TEMPO REAL                                         */
/* ================================================================== */

static double rt_avg(const double *a, size_t n) {
    if (!n) return 0;
    double s = 0;
    for (size_t i = 0; i < n; i++) s += a[i];
    return s / n;
}

static double rt_stddev(const double *a, size_t n) {
    if (!n) return 0;
    double mean = rt_avg(a, n);
    double sum_sq = 0;
    for (size_t i = 0; i < n; i++) {
        double d = a[i] - mean;
        sum_sq += d * d;
    }
    return sqrt(sum_sq / n);
}

static void overall_ok_fail(uint64_t *ok, uint64_t *fail, double *sr1, double *sr2) {
    *ok = 0; *fail = 0; *sr1 = -1; *sr2 = -1;
    if (!threads || nthr <= 0) return;
    if (agg_hsm_mode()) {
        Metrics agg = met_aggregate(agg_target_id());
        *ok = agg.ok;
        *fail = agg.fail;
        *sr1 = agg.first_half_total ? 100.0 * agg.first_half_ok / agg.first_half_total : -1;
        *sr2 = agg.second_half_total ? 100.0 * agg.second_half_ok / agg.second_half_total : -1;
        return;
    }
    double fh_ok = 0, fh_tot = 0, sh_ok = 0, sh_tot = 0;
    for (int i = 0; i < nthr; i++) {
        *ok += threads[i].met.ok;
        *fail += threads[i].met.fail;
        fh_ok += threads[i].met.first_half_ok;
        fh_tot += threads[i].met.first_half_total;
        sh_ok += threads[i].met.second_half_ok;
        sh_tot += threads[i].met.second_half_total;
    }
    *sr1 = fh_tot ? 100.0 * fh_ok / fh_tot : -1;
    *sr2 = sh_tot ? 100.0 * sh_ok / sh_tot : -1;
}

/* Criterios auditaveis do lote (espelhados no JSON / relatorio). */
#define MAX_VERDICT_CRIT 10
typedef struct {
    char nome[64];
    int  ok;                 /* 1 = passou, 0 = falhou, -1 = N/A */
    char detalhe[192];
} VerdictCrit;
static VerdictCrit g_verdict_crit[MAX_VERDICT_CRIT];
static int g_verdict_ncrit = 0;

static void verdict_add(const char *nome, int ok, const char *fmt, ...) {
    if (g_verdict_ncrit >= MAX_VERDICT_CRIT) return;
    VerdictCrit *c = &g_verdict_crit[g_verdict_ncrit++];
    snprintf(c->nome, sizeof(c->nome), "%s", nome);
    c->ok = ok;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->detalhe, sizeof(c->detalhe), fmt, ap);
    va_end(ap);
    json_strip(c->nome);
    json_strip(c->detalhe);
}

static const char *resultado_unidade(uint64_t total, uint64_t fail,
                                     double sr1, double sr2, int completa) {
    if (stress_public) return "NAO QUALIFICADO";
    if (!cfg.producao) {
        if (total == 0) return "SEM DADOS";
        if (fail > 0) return "FALHOU";
        if (sr1 >= 0 && sr2 >= 0 && sr2 < sr1 - 0.5) return "FALHOU";
        if (!completa) return "INCOMPLETO";
        return "CONCLUIDO";
    }
    if (total == 0) return "SEM DADOS";
    if (fail > 0) return "REPROVADO";
    if (sr1 >= 0 && sr2 >= 0 && sr2 < sr1 - 0.5) return "REPROVADO";
    if (!completa) return "INCOMPLETO";
    return "APROVADO";
}

/* Veredicto do lote. APROVADO so em modo producao (APDU), duracao completa,
 * 0 falhas, SAMs operacionais, sem erros de sessao e sem degradacao.
 * healthCheck publico nunca aprova o lote. Carga/memoria/fila nao entram. */
static void compute_verdict(double elapsed, double dur, int running,
                            char *result, size_t result_n,
                            char *motivo, size_t motivo_n) {
    uint64_t ok = 0, fail = 0;
    double sr1 = -1, sr2 = -1;
    overall_ok_fail(&ok, &fail, &sr1, &sr2);
    uint64_t total = ok + fail;
    int completa = (dur > 0) && (elapsed >= dur * 0.98);
    HsmHealth hh = g_hsm;
    double new_session_errors = hsm_new_session_errors(hh);
    g_verdict_ncrit = 0;

    verdict_add("Burn-in SAM fisico", 1, "POST /api/hardware/benchmark %s x%d",
                cfg.apdu, cfg.bench_reps);
    if (cfg.producao)
        verdict_add("Qualificacao de lote", 1, "--producao (APROVADO/REPROVADO)");
    else
        verdict_add("Qualificacao de lote", 0, "desligada (so burn-in, nao e teste de producao)");

    if (running)
        verdict_add("Duracao completa", -1, "%.0f / %.0f s (em execucao)", elapsed, dur);
    else
        verdict_add("Duracao completa", completa ? 1 : 0,
                    "%.0f / %.0f s (exige >= 98%%)", elapsed, dur);

    verdict_add("Operacoes registradas", total > 0 ? 1 : 0, "%llu",
                (unsigned long long)total);

    if (total == 0)
        verdict_add("Zero falhas de operacao", -1, "sem dados");
    else
        verdict_add("Zero falhas de operacao", fail == 0 ? 1 : 0,
                    "%llu fail em %llu (taxa %.4f%%)",
                    (unsigned long long)fail, (unsigned long long)total,
                    met_success_pct(ok, fail));

    if (hh.numberOfSams > 0 && hh.numberOfOperative >= 0)
        verdict_add("SAMs todos operacionais",
                    hh.numberOfOperative >= hh.numberOfSams ? 1 : 0,
                    "%d / %d", hh.numberOfOperative, hh.numberOfSams);
    else
        verdict_add("SAMs todos operacionais", -1, "API nao informou");

    if (new_session_errors >= 0)
        verdict_add("Sem novos erros de sessao", new_session_errors > 0 ? 0 : 1,
                    "inicio %.0f, atual %.0f, novos %.0f",
                    g_session_errors_start, hh.sessionErrors, new_session_errors);
    else
        verdict_add("Sem novos erros de sessao", -1, "API nao informou inicio/atual");

    if (sr1 >= 0 && sr2 >= 0)
        verdict_add("Sem degradacao (2a metade)",
                    (sr2 < sr1 - 0.5) ? 0 : 1,
                    "1a metade %.2f%% vs 2a %.2f%% (limite -0.5 pp)", sr1, sr2);
    else
        verdict_add("Sem degradacao (2a metade)", -1, "ainda sem 2a metade");

    if (hh.is_healthy == 0)
        verdict_add("HSM isHealthy", 0, "%s",
                    hh.health_msg[0] ? hh.health_msg : "false");
    else if (hh.is_healthy == 1)
        verdict_add("HSM isHealthy", 1, "true");
    else
        verdict_add("HSM isHealthy", -1, "campo ausente");

    if (running) {
        snprintf(result, result_n, "EM EXECUCAO");
        snprintf(motivo, motivo_n,
                 "Aguardando fim da duracao programada (%.0f / %.0f s)", elapsed, dur);
        json_strip(motivo);
        return;
    }
    if (stress_public) {
        snprintf(result, result_n, "NAO QUALIFICADO");
        snprintf(motivo, motivo_n,
                 "Modo publico (healthCheck): nao exercita chips SAM. O lote nao pode ser APROVADO.");
        json_strip(motivo);
        return;
    }
    /* Sem --producao: burn-in dos chips, sem carimbo APROVADO de lote. */
    if (!cfg.producao) {
        if (total == 0) {
            snprintf(result, result_n, "SEM DADOS");
            snprintf(motivo, motivo_n, "Nenhuma operacao registrada");
        } else if (fail > 0) {
            snprintf(result, result_n, "FALHOU");
            snprintf(motivo, motivo_n, "%llu falhas em %llu no POST /api/hardware/benchmark",
                     (unsigned long long)fail, (unsigned long long)total);
        } else if (!completa) {
            snprintf(result, result_n, "INCOMPLETO");
            snprintf(motivo, motivo_n, "Parou em %.0f s de %.0f s", elapsed, dur);
        } else {
            snprintf(result, result_n, "CONCLUIDO");
            snprintf(motivo, motivo_n,
                     "Burn-in SAM fisico ok (benchmark GET DATA). Nao e qualificacao de lote.");
        }
        json_strip(motivo);
        return;
    }
    if (total == 0) {
        snprintf(result, result_n, "SEM DADOS");
        snprintf(motivo, motivo_n, "Nenhuma operacao registrada");
        json_strip(motivo);
        return;
    }
    if (fail > 0) {
        snprintf(result, result_n, "REPROVADO");
        snprintf(motivo, motivo_n, "%llu operacoes falharam em %llu (taxa %.4f%%)",
                 (unsigned long long)fail, (unsigned long long)total, met_success_pct(ok, fail));
        json_strip(motivo);
        return;
    }
    if (hh.numberOfSams > 0 && hh.numberOfOperative >= 0 &&
        hh.numberOfOperative < hh.numberOfSams) {
        snprintf(result, result_n, "REPROVADO");
        snprintf(motivo, motivo_n, "SAMs operacionais %d/%d ao final",
                 hh.numberOfOperative, hh.numberOfSams);
        json_strip(motivo);
        return;
    }
    if (new_session_errors > 0) {
        snprintf(result, result_n, "REPROVADO");
        snprintf(motivo, motivo_n,
                 "Novos erros de sessao durante o teste: %.0f (inicio %.0f, final %.0f)",
                 new_session_errors, g_session_errors_start, hh.sessionErrors);
        json_strip(motivo);
        return;
    }
    if (hh.is_healthy == 0) {
        snprintf(result, result_n, "REPROVADO");
        snprintf(motivo, motivo_n, "HSM isHealthy=false: %s",
                 hh.health_msg[0] ? hh.health_msg : "nao saudavel");
        json_strip(motivo);
        return;
    }
    if (sr1 >= 0 && sr2 >= 0 && sr2 < sr1 - 0.5) {
        snprintf(result, result_n, "REPROVADO");
        snprintf(motivo, motivo_n, "Degradacao: 1a metade %.2f%% vs 2a metade %.2f%%", sr1, sr2);
        json_strip(motivo);
        return;
    }
    if (!completa) {
        snprintf(result, result_n, "INCOMPLETO");
        snprintf(motivo, motivo_n,
                 "Parou em %.0f s de %.0f s programados. APROVADO so apos a duracao completa",
                 elapsed, dur);
        json_strip(motivo);
        return;
    }
    snprintf(result, result_n, "APROVADO");
    snprintf(motivo, motivo_n,
             "Duracao completa (%.0f s), 0 falhas, sem degradacao, SAMs operacionais, modo APDU",
             elapsed);
    json_strip(motivo);
}

static void write_status_json(double elapsed, double dur) {
    HsmHealth hh = g_hsm;              /* usa o cache (health_thread) */
    FILE *js = fopen("burnin_status.json", "w");
    if (!js) return;
    fprintf(js, "{\n");
    fprintf(js, "  \"running\": %d,\n", burn_running ? 1 : 0);
    fprintf(js, "  \"elapsed_s\": %.1f,\n", elapsed);
    fprintf(js, "  \"duration_s\": %.0f,\n", dur);
    fprintf(js, "  \"start_epoch\": %.0f,\n", t_start_epoch);
    fprintf(js, "  \"now_epoch\": %.0f,\n", (double)time(NULL));
    fprintf(js, "  \"interface\": \"HTTP\",\n");
    fprintf(js, "  \"target\": \"%s\",\n", cfg.base_url);
    fprintf(js, "  \"modo\": \"%s\",\n", cfg.producao ? "producao" : "lab");
    fprintf(js, "  \"timeout_s\": %d,\n",
            cfg.bench_timeout_sec > 0 ? cfg.bench_timeout_sec : 600);
    fprintf(js, "  \"bench_retries\": %d,\n",
            cfg.bench_retries > 0 ? cfg.bench_retries : 1);
    {
        char ver[64], mot[256];
        compute_verdict(elapsed, dur, burn_running ? 1 : 0, ver, sizeof(ver), mot, sizeof(mot));
        fprintf(js, "  \"veredicto\": \"%s\",\n", ver);
        fprintf(js, "  \"veredicto_motivo\": \"%s\",\n", mot);
        fprintf(js, "  \"duration_completa\": %d,\n",
                (!burn_running && dur > 0 && elapsed >= dur * 0.98) ? 1 : 0);
        fprintf(js, "  \"veredicto_criterios\": [\n");
        for (int i = 0; i < g_verdict_ncrit; i++) {
            fprintf(js, "    {\"nome\":\"%s\",\"ok\":%d,\"detalhe\":\"%s\"}%s\n",
                    g_verdict_crit[i].nome,
                    g_verdict_crit[i].ok,
                    g_verdict_crit[i].detalhe,
                    (i + 1 < g_verdict_ncrit) ? "," : "");
        }
        fprintf(js, "  ],\n");
    }
    fprintf(js, "  \"hsm\": {\n");
    fprintf(js, "    \"alias\": \"%s\",\n", hh.alias[0] ? hh.alias : "?");
    fprintf(js, "    \"numberOfSams\": %d,\n", hh.numberOfSams);
    fprintf(js, "    \"numberOfOperative\": %d,\n", hh.numberOfOperative);
    fprintf(js, "    \"uptime\": \"%s\",\n", hh.uptime[0] ? hh.uptime : "?");
    fprintf(js, "    \"load1\": %.2f,\n", hh.load1);
    fprintf(js, "    \"load5\": %.2f,\n", hh.load5);
    fprintf(js, "    \"load15\": %.2f,\n", hh.load15);
    fprintf(js, "    \"mem_used_mb\": %.0f,\n", hh.mem_used_mb);
    fprintf(js, "    \"mem_total_mb\": %.0f,\n", hh.mem_total_mb);
    fprintf(js, "    \"mem_free_mb\": %.0f,\n", hh.mem_free_mb);
    fprintf(js, "    \"is_healthy\": %d,\n", hh.is_healthy);
    fprintf(js, "    \"health_msg\": \"%s\",\n", hh.health_msg[0] ? hh.health_msg : "");
    fprintf(js, "    \"cpu_cores\": %.0f,\n", hh.cpu_cores);
    fprintf(js, "    \"openSessions\": %.0f,\n", hh.openSessions);
    fprintf(js, "    \"maxSessions\": %.0f,\n", hh.maxSessions);
    fprintf(js, "    \"sessionErrors\": %.0f,\n", hh.sessionErrors);
    fprintf(js, "    \"sessionErrorsStart\": %.0f,\n", g_session_errors_start);
    fprintf(js, "    \"sessionErrorsNew\": %.0f,\n", hsm_new_session_errors(hh));
    fprintf(js, "    \"inQueue\": %.0f,\n", hh.inQueue);
    fprintf(js, "    \"temp_c\": %.1f\n", hh.temp);
    fprintf(js, "  },\n");
    fprintf(js, "  \"sams\": [\n");
    if (agg_hsm_mode()) {
        /* Um alvo: healthCheck (lab) ou benchmark GET DATA (producao). */
        Metrics agg = met_aggregate(agg_target_id());
        double sr = met_success_pct(agg.ok, agg.fail);
        double al = agg.total ? agg.lat_sum : 0;
        double ops = elapsed > 0 ? (double)agg.total / elapsed : 0;
        double tavg = agg.temp_count ? agg.temp_sum / agg.temp_count : 0;
        fprintf(js, "    {\n");
        fprintf(js, "      \"id\": \"%s\",\n", agg.id);
        fprintf(js, "      \"total\": %llu,\n", (unsigned long long)agg.total);
        fprintf(js, "      \"ok\": %llu,\n", (unsigned long long)agg.ok);
        fprintf(js, "      \"fail\": %llu,\n", (unsigned long long)agg.fail);
        fprintf(js, "      \"success_pct\": %.6f,\n", sr);
        fprintf(js, "      \"lat_avg_ms\": %.3f,\n", al);
        fprintf(js, "      \"lat_stddev_ms\": 0.000,\n");
        fprintf(js, "      \"lat_max_ms\": %.3f,\n", agg.lat_max);
        fprintf(js, "      \"ops_per_sec\": %.2f,\n", ops);
        fprintf(js, "      \"temp_avg_c\": %.1f,\n", tavg);
        fprintf(js, "      \"temp_rt_c\": %.1f,\n", tavg);
        fprintf(js, "      \"lat_rt_ms\": %.3f,\n", al);
        fprintf(js, "      \"degradacao_1a_metade_pct\": %.2f,\n",
                agg.first_half_total ? 100.0 * agg.first_half_ok / agg.first_half_total : -1);
        fprintf(js, "      \"degradacao_2a_metade_pct\": %.2f\n",
                agg.second_half_total ? 100.0 * agg.second_half_ok / agg.second_half_total : -1);
        fprintf(js, "    }\n");
    } else {
    for (int i = 0; i < nthr; i++) {
        Metrics *m = &threads[i].met;
        uint64_t ok = m->ok, fail = m->fail;
        uint64_t total = ok + fail;
        double sr = met_success_pct(ok, fail);
        double al = total ? m->lat_sum / total : 0;
        double ops = elapsed > 0 ? (double)total / elapsed : 0;
        double tavg = m->temp_count ? m->temp_sum / m->temp_count : 0;
        fprintf(js, "    {\n");
        fprintf(js, "      \"id\": \"%s\",\n", m->id);
        fprintf(js, "      \"total\": %llu,\n", (unsigned long long)total);
        fprintf(js, "      \"ok\": %llu,\n", (unsigned long long)ok);
        fprintf(js, "      \"fail\": %llu,\n", (unsigned long long)fail);
        fprintf(js, "      \"success_pct\": %.6f,\n", sr);
        fprintf(js, "      \"lat_avg_ms\": %.3f,\n", al);
        fprintf(js, "      \"lat_stddev_ms\": %.3f,\n", rt_stddev(m->rt_lat, m->rt_lat_count));
        fprintf(js, "      \"lat_max_ms\": %.3f,\n", m->lat_max);
        fprintf(js, "      \"ops_per_sec\": %.2f,\n", ops);
        fprintf(js, "      \"temp_avg_c\": %.1f,\n", tavg);
        fprintf(js, "      \"temp_rt_c\": %.1f,\n", rt_avg(m->rt_temp, m->rt_temp_count));
        fprintf(js, "      \"lat_rt_ms\": %.3f,\n", rt_avg(m->rt_lat, m->rt_lat_count));
        fprintf(js, "      \"degradacao_1a_metade_pct\": %.2f,\n",
                m->first_half_total ? 100.0 * m->first_half_ok / m->first_half_total : -1);
        fprintf(js, "      \"degradacao_2a_metade_pct\": %.2f\n",
                m->second_half_total ? 100.0 * m->second_half_ok / m->second_half_total : -1);
        fprintf(js, "    }%s\n", (i + 1 < nthr) ? "," : "");
    }
    }
    fprintf(js, "  ],\n");
    fprintf(js, "  \"falhas_total\": %llu,\n", (unsigned long long)fail_eventos);
    fprintf(js, "  \"falhas_resumo\": [\n");
    for (int i = 0; i < n_fail_cats; i++) {
        fprintf(js, "    {\"tipo\":\"%s\",\"qtd\":%llu,\"primeira\":\"%s\",\"ultima\":\"%s\",\"exemplo\":\"%s\"}%s\n",
                fail_cats[i].cat,
                (unsigned long long)fail_cats[i].n,
                fail_cats[i].primeira,
                fail_cats[i].ultima,
                fail_cats[i].exemplo,
                (i + 1 < n_fail_cats) ? "," : "");
    }
    fprintf(js, "  ]\n}\n");
    fclose(js);
}

static void write_falhas_relatorio(double elapsed) {
    FILE *f = fopen("burnin_falhas.txt", "w");
    if (!f) return;
    char ver[64], mot[256];
    compute_verdict(elapsed, (double)cfg.duration_sec, 0, ver, sizeof(ver), mot, sizeof(mot));
    fprintf(f, "RELATORIO DE FALHAS - BURN-IN HSM/SAM\n");
    fprintf(f, "=====================================\n");
    fprintf(f, "Modo: %s\n",
            cfg.producao ? "producao (qualifica lote)" : "burn-in SAM fisico (benchmark)");
    fprintf(f, "Veredicto: %s\n", ver);
    fprintf(f, "Motivo: %s\n", mot);
    fprintf(f, "Duracao: %.0f / %d s\n", elapsed, cfg.duration_sec);
    fprintf(f, "Total de falhas de operacao: %llu\n\n", (unsigned long long)fail_eventos);

    fprintf(f, "O QUE ACONTECEU\n");
    if (fail_eventos == 0) {
        fprintf(f, "Nenhuma falha de operacao foi registrada.\n");
        if (stress_public)
            fprintf(f, "Carga: GET /api/hardware/healthCheck (nao exercita SAM; nao qualifica lote).\n");
        else
            fprintf(f, "Carga: POST /api/hardware/benchmark {apdu:%s, repetitions:%d}.\n",
                    cfg.apdu, cfg.bench_reps);
        fprintf(f, "Todas as chamadas a API retornaram sucesso.\n");
        fprintf(f, "\nFALHAS POR TIPO\n");
        fprintf(f, "(nenhuma — zero falhas de operacao)\n");
        fprintf(f, "\nLista cronologica: burnin_errors.log\n");
        fclose(f);
        LOG_INFO("Relatorio de falhas salvo em burnin_falhas.txt (0 falhas)");
        return;
    }
    fprintf(f, "%llu falha(s) de operacao nesta sessao.\n", (unsigned long long)fail_eventos);
    if (!stress_public)
        fprintf(f, "Carga: POST /api/hardware/benchmark {apdu:%s, repetitions:%d}.\n",
                cfg.apdu, cfg.bench_reps);
    else
        fprintf(f, "Carga: GET /api/hardware/healthCheck.\n");
    fprintf(f, "Significado de cada tipo abaixo (sem repetir a lista cronologica).\n");

    fprintf(f, "\nFALHAS POR TIPO\n");
    for (int i = 0; i < n_fail_cats; i++) {
        char oque[256], como[256];
        fail_explicacao(fail_cats[i].cat, fail_cats[i].exemplo,
                        oque, sizeof(oque), como, sizeof(como));
        fprintf(f, "\n%d) %s — %llu ocorrencia(s)\n", i + 1, fail_cats[i].cat,
                (unsigned long long)fail_cats[i].n);
        fprintf(f, "   O que: %s\n", oque);
        fprintf(f, "   Como: %s\n", como);
        fprintf(f, "   Primeira: %s\n", fail_cats[i].primeira);
        fprintf(f, "   Ultima: %s\n", fail_cats[i].ultima);
        fprintf(f, "   Exemplo: %s\n", fail_cats[i].exemplo);
    }
    fprintf(f, "\nLista cronologica: burnin_errors.log\n");
    fclose(f);
    LOG_INFO("Relatorio de falhas salvo em burnin_falhas.txt (%llu falhas)",
             (unsigned long long)fail_eventos);
}

static unsigned __stdcall status_thread(void *arg) {
    (void)arg;
    while (burn_running) {
        if (burn_time_up() || burn_stop_requested()) burn_running = 0;
        write_status_json(burn_elapsed_s(), (double)cfg.duration_sec);
        if (!burn_running) break;
        Sleep(1000);
    }
    write_status_json(burn_elapsed_s(), (double)cfg.duration_sec);
    return 0;
}
static unsigned __stdcall report_thread(void *arg) {
    (void)arg;
    while (burn_running) {
        Sleep(cfg.report_interval_sec * 1000);
        if (burn_time_up() || burn_stop_requested()) burn_running = 0;
        if (!burn_running) break;
        double elap = burn_elapsed_s();
        /* Indicadores reais do HSM (cache atualizado pela health_thread) */
        HsmHealth h = g_hsm;
        if (h.temp > 0)
            for (int i = 0; i < nthr; i++)
                met_add_temp(&threads[i].met, h.temp);
        LOG_INFO("--- PARCIAL (%.1f s) | HSM REAL: %d/%d SAMs op | load %.2f/%.2f/%.2f | mem %.0f/%.0f MB | sess %0.f/%0.f err %0.f fila %0.f%s%s ---",
                 elap, h.numberOfOperative, h.numberOfSams,
                 h.load1, h.load5, h.load15,
                 h.mem_used_mb, h.mem_total_mb,
                 h.openSessions, h.maxSessions, h.sessionErrors, h.inQueue,
                 h.temp > 0 ? " | temp " : "", h.temp > 0 ? "" : "");
        if (h.temp > 0) LOG_INFO("   Temp HSM: %.1f C", h.temp);
        if (agg_hsm_mode()) {
            Metrics agg = met_aggregate(agg_target_id());
            met_report(&agg, elap);
        } else {
            for (int i = 0; i < nthr; i++) met_report(&threads[i].met, elap);
        }
        LOG_INFO("--- FIM PARCIAL ---");
    }
    return 0;
}

/* ================================================================== */
/* 9. ARGUMENTOS DE LINHA DE COMANDO                                  */
/* ================================================================== */

static void str_trim_inplace(char *s) {
    char *start, *end;
    if (!s || !s[0]) return;
    start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
        *--end = 0;
}

/* Carrega defaults de sessao.json (cwd). CLI sobrescreve depois. */
static void load_sessao_defaults(void) {
    FILE *f = fopen("sessao.json", "rb");
    char *buf = NULL;
    long sz;
    char tmp[2048];
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    sz = ftell(f);
    if (sz <= 0 || sz > 2 * 1024 * 1024) { fclose(f); return; }
    rewind(f);
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return;
    }
    buf[sz] = 0;
    fclose(f);

    if (!cfg.token[0] && json_get_string(buf, "token", tmp, sizeof(tmp)) == 0) {
        str_trim_inplace(tmp);
        if (tmp[0])
            snprintf(cfg.token, sizeof(cfg.token), "%s", tmp);
    }
    if (json_get_string(buf, "apdu", tmp, sizeof(tmp)) == 0 && tmp[0]) {
        str_trim_inplace(tmp);
        snprintf(cfg.apdu, sizeof(cfg.apdu), "%s", tmp);
    }
    {
        double reps = json_get_number(buf, "repetitions");
        if (reps > 0) cfg.bench_reps = (int)reps;
    }
    {
        double to = json_get_number(buf, "timeout_s");
        if (to > 0) cfg.bench_timeout_sec = (int)to;
    }
    {
        double retries = json_get_number(buf, "bench_retries");
        if (retries > 0) cfg.bench_retries = (int)retries;
    }
    {
        double dur = json_get_number(buf, "duracao_programada_s");
        if (dur > 0) cfg.duration_sec = (int)dur;
    }
    if (json_get_string(buf, "base_url", tmp, sizeof(tmp)) == 0 && tmp[0]) {
        str_trim_inplace(tmp);
        if (strncmp(tmp, "http", 4) == 0)
            snprintf(cfg.base_url, sizeof(cfg.base_url), "%s", tmp);
    }
    free(buf);
}

static void print_usage(const char *name) {
    printf("Uso: %s [opcoes]\n\n", name);
    printf("Opcoes:\n");
    printf("  --base-url <url>     URL base do HSM (default: http://10.100.0.155:8080)\n");
    printf("  --token <jwt>        Bearer (obrigatorio no HSM: benchmark da 401 sem ele)\n");
    printf("  --producao           Alem do burn-in, qualifica lote (APROVADO/REPROVADO)\n");
    printf("  --apdu <hex>         APDU do benchmark (default: 00C002A008 = GET DATA)\n");
    printf("  --repetitions <n>    Repeticoes do HSM por POST (default: 10)\n");
    printf("  --timeout <s>        Timeout HTTP do POST benchmark (default: 600)\n");
    printf("  --bench-retries <n>  Tentativas em timeout/rede/5xx (default: 3)\n");
    printf("  --verify-pin         Legado (nao usado no burn-in benchmark)\n");
    printf("  --duration <s>       Duracao em segundos (default: 86400 = 24h)\n");
    printf("  --sams <n>           Limite de CSN no inventario (0 = todos)\n");
    printf("  --ops <n>            POSTs benchmark por ciclo (default: 100)\n");
    printf("  --threads <n>        Threads de carga (default: 8)\n");
    printf("  --report-interval <n> Segundos entre relatorios (default: 60)\n");
    printf("  --help / -h\n\n");
    printf("Burn-in SAM fisico (sem qualificacao de lote):\n");
    printf("  %s --base-url http://10.100.0.155:8080 --token <jwt> --duration 3600\n", name);
}

static int parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--base-url") == 0 && i + 1 < argc)
            strncpy(cfg.base_url, argv[++i], sizeof(cfg.base_url) - 1);
        else if (strcmp(argv[i], "--token") == 0 && i + 1 < argc) {
            strncpy(cfg.token, argv[++i], sizeof(cfg.token) - 1);
            cfg.token[sizeof(cfg.token) - 1] = 0;
            str_trim_inplace(cfg.token);
        }
        else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            cfg.duration_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "--sams") == 0 && i + 1 < argc)
            cfg.sam_count = atoi(argv[++i]);
        else if (strcmp(argv[i], "--ops") == 0 && i + 1 < argc)
            cfg.ops_per_cycle = atoi(argv[++i]);
        else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
            cfg.threads = atoi(argv[++i]);
        else if (strcmp(argv[i], "--report-interval") == 0 && i + 1 < argc)
            cfg.report_interval_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "--producao") == 0)
            cfg.producao = 1;
        else if (strcmp(argv[i], "--verify-pin") == 0)
            cfg.verify_pin = 1;
        else if (strcmp(argv[i], "--apdu") == 0 && i + 1 < argc)
            strncpy(cfg.apdu, argv[++i], sizeof(cfg.apdu) - 1);
        else if (strcmp(argv[i], "--repetitions") == 0 && i + 1 < argc)
            cfg.bench_reps = atoi(argv[++i]);
        else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
            cfg.bench_timeout_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "--bench-retries") == 0 && i + 1 < argc)
            cfg.bench_retries = atoi(argv[++i]);
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }
    return 1;
}

/* ================================================================== */
/* 10. MAIN                                                           */
/* ================================================================== */

int main(int argc, char **argv) {
    init_runtime_dirs();
    tmr_init();
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    log_fp = fopen_log("burnin.log", "a");
    if (!log_fp) log_fp = stderr;

    /* sessao.json entra primeiro; o argv manda por cima. */
    load_sessao_defaults();
    if (!parse_args(argc, argv)) return 0;
    str_trim_inplace(cfg.token);
    str_trim_inplace(cfg.base_url);
    str_trim_inplace(cfg.apdu);

    /* Evita que a suspensão automática do Windows interrompa uma chamada
     * longa do benchmark. A tela ainda pode ser desligada normalmente. */
    EXECUTION_STATE power_state =
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);

    t_start_epoch = (double)time(NULL);
    errors_log_reset();
    remove("burnin.stop");

    LOG_INFO("==============================================");
    LOG_INFO("BURN-IN SAM FISICO");
    LOG_INFO("POST /api/hardware/benchmark {\"apdu\":\"%s\",\"repetitions\":%d}",
             cfg.apdu, cfg.bench_reps);
    LOG_INFO("Timeout HTTP: %d s  Retries: %d",
             cfg.bench_timeout_sec > 0 ? cfg.bench_timeout_sec : 600,
             cfg.bench_retries > 0 ? cfg.bench_retries : 1);
    LOG_INFO("Alvo: %s", cfg.base_url);
    LOG_INFO("Token: %s (%u chars)",
             cfg.token[0] ? "configurado" : "AUSENTE",
             (unsigned)strlen(cfg.token));
    if (cfg.token[0]) {
        int dots = 0;
        for (const char *p = cfg.token; *p; p++)
            if (*p == '.') dots++;
        if (dots != 2)
            LOG_WARN("Token com %d ponto(s): nao tem as 3 partes de um JWT (pode ter vindo cortado).", dots);
    }
    LOG_INFO("Duracao: %d s  Ops/ciclo: %d", cfg.duration_sec, cfg.ops_per_cycle);
    if (power_state)
        LOG_INFO("Suspensao automatica do Windows bloqueada durante o teste.");
    else
        LOG_WARN("Windows nao permitiu bloquear a suspensao automatica.");
    LOG_INFO("==============================================");

    if (hsm_healthcheck() <= 0)
        LOG_WARN("GET /api/hardware/healthCheck falhou. Seguindo com o POST de SAM.");

    hsm_telemetry_reset();
    {
        HsmHealth initial_health = hsm_read_health();
        if (initial_health.http_code == 200) {
            g_hsm = initial_health;
            g_hsm_valid = 1;
            g_session_errors_start = initial_health.sessionErrors;
            hsm_telemetry_accum(&g_hsm);
            LOG_INFO("Contador de erros de sessao no inicio: %.0f",
                     g_session_errors_start);
        } else {
            LOG_WARN("Nao foi possivel registrar o contador inicial de erros de sessao.");
        }
    }

    /* Burn-in = so POST /api/hardware/benchmark {apdu, repetitions}. */
    static char all_csns[MAX_SAMS][MAX_CSN_LEN];
    snprintf(all_csns[0], MAX_CSN_LEN, "HSM:benchmark");
    int total = 1;

    /* Passo 3: distribui os chips entre as threads (round-robin) */
    if (agg_hsm_mode()) {
        nthr = cfg.threads > 0 ? cfg.threads : 8;
        if (nthr > MAX_THREADS) nthr = MAX_THREADS;
    } else {
        nthr = total < MAX_THREADS ? total : MAX_THREADS;
    }
    threads = (SamThread *)calloc(nthr, sizeof(SamThread));
    if (!threads) { LOG_ERROR("Sem memoria."); goto fim; }

    uint64_t expected = (uint64_t)cfg.duration_sec * (uint64_t)cfg.ops_per_cycle * (uint64_t)num_apdus();
    double half = (double)(expected / 2);

    t_burn_start_100ns = active_time_100ns();
    burn_running = 1;
    for (int i = 0; i < nthr; i++) {
        SamThread *st = &threads[i];
        st->idx = i + 1;
        for (int j = i; j < total; j += nthr) {
            snprintf(st->csns[st->n_csns], MAX_CSN_LEN, "%s", all_csns[j]);
            st->n_csns++;
        }
        met_init(&st->met, st->csns[0], half);
        st->running = 1;
        st->th = (HANDLE)_beginthreadex(NULL, 0, thread_wrap, st, 0, NULL);
    }

    HANDLE hRep = (HANDLE)_beginthreadex(NULL, 0, report_thread, NULL, 0, NULL);
    HANDLE hSta = (HANDLE)_beginthreadex(NULL, 0, status_thread, NULL, 0, NULL);
    HANDLE hHea = (HANDLE)_beginthreadex(NULL, 0, health_thread, NULL, 0, NULL);

    LOG_INFO("Rodando por %d segundos sobre %d chip(s)...", cfg.duration_sec, total);
    while (burn_running && !burn_time_up() && !burn_stop_requested())
        Sleep(200);
    if (burn_stop_requested()) {
        LOG_INFO("Parada pedida (dashboard). Encerrando.");
        burn_running = 0;
    } else if (burn_time_up())
        LOG_INFO("Duracao esgotada (%d s). Encerrando.", cfg.duration_sec);

    burn_running = 0;
    for (int i = 0; i < nthr; i++) threads[i].running = 0;
    write_status_json(burn_elapsed_s(), (double)cfg.duration_sec);

    WaitForSingleObject(hRep, 5000); CloseHandle(hRep);
    WaitForSingleObject(hSta, 5000); CloseHandle(hSta);
    WaitForSingleObject(hHea, 20000); CloseHandle(hHea);
    for (int i = 0; i < nthr; i++) {
        WaitForSingleObject(threads[i].th, 20000);
        CloseHandle(threads[i].th);
    }

    double total_t = burn_elapsed_s();
    HsmHealth hf = hsm_read_health();
    if (hf.http_code == 200) {
        g_hsm = hf;
        g_hsm_valid = 1;
        hsm_telemetry_accum(&g_hsm);
    } else {
        hf = g_hsm;
        LOG_WARN("Leitura final do healthCheck falhou; usando a ultima leitura valida.");
    }
    LOG_INFO("==============================================");
    LOG_INFO("BURN-IN FINALIZADO");
    if (agg_hsm_mode()) {
        Metrics agg = met_aggregate(agg_target_id());
        met_report(&agg, total_t);
    } else {
        for (int i = 0; i < nthr; i++) met_report(&threads[i].met, total_t);
    }
    LOG_INFO("==============================================");

    /* Passo 4: relatorio final */
    FILE *csv = fopen("burnin_report.csv", "w");
    if (csv) {
        time_t now = time(NULL);
        struct tm *ti = localtime(&now);
        fprintf(csv, "# Relatorio de Burn-in HSM950 (SAMS)\n");
        fprintf(csv, "# Gerado em: %04d-%02d-%02d %02d:%02d:%02d\n",
                ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday,
                ti->tm_hour, ti->tm_min, ti->tm_sec);
        fprintf(csv, "# Alvo: %s\n", cfg.base_url);
        fprintf(csv, "# Modo: %s\n",
                cfg.producao ? "producao (qualifica lote)" : "burn-in SAM fisico (benchmark)");
        fprintf(csv, "# Duracao programada: %d s\n", cfg.duration_sec);
        fprintf(csv, "# Duracao real: %.1f s\n", total_t);
        {
            char ver[64], mot[256];
            compute_verdict(total_t, (double)cfg.duration_sec, 0, ver, sizeof(ver), mot, sizeof(mot));
            fprintf(csv, "# Veredicto: %s\n", ver);
            fprintf(csv, "# Motivo: %s\n", mot);
            LOG_INFO("Veredicto: %s — %s", ver, mot);
        }
        fprintf(csv, "# Ops por ciclo: %d\n", cfg.ops_per_cycle);
        fprintf(csv, "# HSM real: %s | %d/%d SAMs op | uptime %s | load %.2f/%.2f/%.2f | mem %.0f/%.0f MB | sess %0.f/%0.f\n",
                hf.alias[0] ? hf.alias : "?", hf.numberOfOperative, hf.numberOfSams,
                hf.uptime[0] ? hf.uptime : "?", hf.load1, hf.load5, hf.load15,
                hf.mem_used_mb, hf.mem_total_mb, hf.openSessions, hf.maxSessions);
        fprintf(csv, "# Erros de sessao: inicio %.0f | final %.0f | novos %.0f\n",
                g_session_errors_start, hf.sessionErrors, hsm_new_session_errors(hf));
        fprintf(csv, "# Variacao no teste: load1 min %.2f max %.2f media %.2f (%d leituras) | mem min %.0f max %.0f media %.0f MB | sess min %0.f max %0.f\n",
                hsm_load1_min, hsm_load1_max, hsm_load1_n ? hsm_load1_sum / hsm_load1_n : 0, (int)hsm_load1_n,
                hsm_mem_min, hsm_mem_max, hsm_mem_n ? hsm_mem_sum / hsm_mem_n : 0,
                hsm_sess_min, hsm_sess_max);
        fprintf(csv, "#\n");
        fprintf(csv, "sam_id,total,ok,fail,sucesso_pct,lat_avg_ms,lat_stddev_ms,lat_max_ms,ops_por_segundo,temp_avg_c,temp_max_c,degradacao_1a_metade_pct,degradacao_2a_metade_pct,resultado\n");
        if (agg_hsm_mode()) {
            Metrics m = met_aggregate(agg_target_id());
            double sr = met_success_pct(m.ok, m.fail);
            double al = m.total ? m.lat_sum : 0;
            double ops = total_t > 0 ? m.total / total_t : 0;
            double tavg = m.temp_count ? m.temp_sum / m.temp_count : -1;
            double sr1 = m.first_half_total ? 100.0 * m.first_half_ok / m.first_half_total : -1;
            double sr2 = m.second_half_total ? 100.0 * m.second_half_ok / m.second_half_total : -1;
            if (sr1 > 100.0) sr1 = 100.0;
            if (sr2 > 100.0) sr2 = 100.0;
            const char *result = resultado_unidade(
                m.total, m.fail, sr1, sr2,
                total_t >= cfg.duration_sec * 0.98);
            fprintf(csv, "%s,%llu,%llu,%llu,%.6f,%.3f,0.000,%.3f,%.2f,%.1f,%.1f,%.2f,%.2f,%s\n",
                    m.id, (unsigned long long)m.total, (unsigned long long)m.ok,
                    (unsigned long long)m.fail, sr, al,
                    m.lat_max, ops, tavg, m.temp_max, sr1, sr2, result);
        } else {
        for (int i = 0; i < nthr; i++) {
            Metrics *m = &threads[i].met;
            uint64_t ok = m->ok, fail = m->fail;
            uint64_t total = ok + fail;
            double sr = met_success_pct(ok, fail);
            double al = total ? m->lat_sum / total : 0;
            double ops = total_t > 0 ? (double)total / total_t : 0;
            double tavg = m->temp_count ? m->temp_sum / m->temp_count : -1;
            double sr1 = m->first_half_total ? 100.0 * m->first_half_ok / m->first_half_total : -1;
            double sr2 = m->second_half_total ? 100.0 * m->second_half_ok / m->second_half_total : -1;
            if (sr1 > 100.0) sr1 = 100.0;
            if (sr2 > 100.0) sr2 = 100.0;
            const char *result = resultado_unidade(
                total, fail, sr1, sr2,
                total_t >= cfg.duration_sec * 0.98);
            fprintf(csv, "%s,%llu,%llu,%llu,%.6f,%.3f,%.3f,%.3f,%.2f,%.1f,%.1f,%.2f,%.2f,%s\n",
                    m->id, (unsigned long long)total, (unsigned long long)ok,
                    (unsigned long long)fail, sr, al,
                    met_stddev(m),
                    m->lat_max, ops, tavg, m->temp_max, sr1, sr2, result);
        }
        }
        fclose(csv);
        LOG_INFO("Relatorio salvo em burnin_report.csv");
    }
    write_status_json(burn_elapsed_s(), (double)cfg.duration_sec);
    write_falhas_relatorio(burn_elapsed_s());

fim:
    SetThreadExecutionState(ES_CONTINUOUS);
    if (threads) {
        for (int i = 0; i < nthr; i++) {
            free(threads[i].met.latencies);
            free(threads[i].met.hist_time);
            free(threads[i].met.hist_temp);
        }
        free(threads);
    }
    if (log_fp && log_fp != stderr) fclose(log_fp);
    WSACleanup();
    return 0;
}
