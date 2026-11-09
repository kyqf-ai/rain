#include "web.h"
#include "store.h"
#include "query.h"
#include "log.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0601
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET sock_t;
#  define SOCK_BAD INVALID_SOCKET
#  define sock_close closesocket
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <sys/select.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <pthread.h>
#  include <errno.h>
typedef int sock_t;
#  define SOCK_BAD (-1)
#  define sock_close close
#endif
#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0
#endif

#define REQ_MAX 4096
#define WEB_WORKERS   4        /* concurrent request handlers */
#define WEB_QUEUE_MAX 16       /* accepted sockets waiting for a worker */
#define WEB_SEND_MS   5000     /* per-send timeout: one slow client cannot pin a worker forever */

/* ---- tiny mutex / condition wrappers (worker pool synchronisation) ---- */
#ifdef _WIN32
typedef CRITICAL_SECTION   web_mu_t;
typedef CONDITION_VARIABLE web_cv_t;
static void wmu_init(web_mu_t *m) { InitializeCriticalSection(m); }
static void wmu_done(web_mu_t *m) { DeleteCriticalSection(m); }
static void wmu_lock(web_mu_t *m) { EnterCriticalSection(m); }
static void wmu_unlock(web_mu_t *m) { LeaveCriticalSection(m); }
static void wcv_init(web_cv_t *c) { InitializeConditionVariable(c); }
static void wcv_done(web_cv_t *c) { (void)c; }
static int  wcv_wait_ms(web_cv_t *c, web_mu_t *m, int ms)
{
    return SleepConditionVariableCS(c, m, ms < 0 ? INFINITE : (DWORD)ms) ? 0 : -1;
}
static void wcv_wake(web_cv_t *c) { WakeConditionVariable(c); }
static void wcv_wake_all(web_cv_t *c) { WakeAllConditionVariable(c); }
#else
typedef pthread_mutex_t web_mu_t;
typedef pthread_cond_t  web_cv_t;
static void wmu_init(web_mu_t *m) { pthread_mutex_init(m, NULL); }
static void wmu_done(web_mu_t *m) { pthread_mutex_destroy(m); }
static void wmu_lock(web_mu_t *m) { pthread_mutex_lock(m); }
static void wmu_unlock(web_mu_t *m) { pthread_mutex_unlock(m); }
static void wcv_init(web_cv_t *c) { pthread_cond_init(c, NULL); }
static void wcv_done(web_cv_t *c) { pthread_cond_destroy(c); }
static int  wcv_wait_ms(web_cv_t *c, web_mu_t *m, int ms)
{
    struct timespec ts;
    if (ms < 0) { pthread_cond_wait(c, m); return 0; }
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(c, m, &ts) == ETIMEDOUT ? -1 : 0;
}
static void wcv_wake(web_cv_t *c) { pthread_cond_signal(c); }
static void wcv_wake_all(web_cv_t *c) { pthread_cond_broadcast(c); }
#endif

static void set_send_timeout(sock_t s, int ms)
{
#ifdef _WIN32
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof t);
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000; tv.tv_usec = (long)(ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

static int send_all(sock_t s, const char *p, size_t n)
{
    while (n) {
        int w = (int)send(s, p, (int)(n > 65536 ? 65536 : n), MSG_NOSIGNAL);
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}

static void respond(sock_t s, int code, const char *ctype, const char *extra, const char *body, size_t n)
{
    char head[400];
    const char *msg = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 404 ? "Not Found" : code == 405 ? "Method Not Allowed" : "Internal Server Error";
    int h = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-store\r\n%sConnection: close\r\n\r\n",
                     code, msg, ctype, (unsigned)n, extra ? extra : "");
    send_all(s, head, (size_t)h);
    if (n) send_all(s, body, n);
}
static void respond_err(sock_t s, int code, const char *msg)
{
    char b[200]; int n = snprintf(b, sizeof b, "{\"error\":\"%s\"}", msg);
    respond(s, code, "application/json; charset=utf-8", NULL, b, (size_t)n);
}

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
/* value of key in a query string, percent/plus decoded; 0 if missing */
static int qget(const char *q, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    while (q && *q) {
        const char *amp = strchr(q, '&'); size_t seglen = amp ? (size_t)(amp - q) : strlen(q);
        if (seglen > kl && !strncmp(q, key, kl) && q[kl] == '=') {
            const char *v = q + kl + 1, *end = q + seglen; size_t o = 0;
            while (v < end && o + 1 < n) {
                if (*v == '+') { out[o++] = ' '; v++; }
                else if (*v == '%' && v + 2 < end && hexv(v[1]) >= 0 && hexv(v[2]) >= 0) { out[o++] = (char)(hexv(v[1]) * 16 + hexv(v[2])); v += 3; }
                else out[o++] = *v++;
            }
            out[o] = 0;
            return 1;
        }
        q = amp ? amp + 1 : NULL;
    }
    return 0;
}

static void handle(sock_t cs, store_t *st, const cfg_t *cfg, char *req)
{
    char method[8], target[1024], path[1100], *query, a[64], b[64], c[16]; sbuf_t o; int dev, rc = 0;
    (void)cfg;
    if (sscanf(req, "%7s %1023s", method, target) != 2) { respond_err(cs, 400, "bad request"); return; }
    if (strcmp(method, "GET") != 0) { respond_err(cs, 405, "GET only"); return; }
    snprintf(path, sizeof path, "%s", target);
    query = strchr(path, '?');
    if (query) *query++ = 0;

    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        respond(cs, 200, "text/html; charset=utf-8", NULL, (const char *)web_index_html, web_index_html_len);
        return;
    }
    if (strncmp(path, "/api/", 5) != 0 && strcmp(path, "/export.csv") != 0) { respond_err(cs, 404, "not found"); return; }

    dev = store_default_device(st);
    if (qget(query, "dev", a, sizeof a)) dev = atoi(a);
    sb_init(&o);
    if (!strcmp(path, "/api/ping")) { sb_puts(&o, "{\"ok\":true}"); }
    else if (dev < 0) { sb_puts(&o, "{\"error\":\"no device in database yet\"}"); }
    else if (!strcmp(path, "/api/status")) { rc = q_status_json(st, dev, &o); }
    else if (!strcmp(path, "/api/summary")) { rc = q_summary_json(st, dev, &o); }
    else if (!strcmp(path, "/api/alarms")) { rc = q_alarms_json(st, dev, &o); }
    else if (!strcmp(path, "/api/day")) {
        wtime_t t; if (!qget(query, "date", a, sizeof a)) { wtime_now(&t); wtime_fmt(&t, WT_DATE, a, sizeof a); }
        if (!date_ok(a)) { sb_free(&o); respond_err(cs, 400, "date must be YYYY-MM-DD"); return; }
        rc = q_day_json(st, dev, a, &o);
    } else if (!strcmp(path, "/api/minutes")) {
        wtime_t t;
        if (!qget(query, "hour", a, sizeof a) || strlen(a) != 13 || wtime_parse(a, &t) != 0) { sb_free(&o); respond_err(cs, 400, "hour must be \"YYYY-MM-DD HH\""); return; }
        rc = q_minutes_json(st, dev, a, &o);
    } else if (!strcmp(path, "/api/range") || !strcmp(path, "/export.csv")) {
        wtime_t f, t; long days; int csv = !strcmp(path, "/export.csv");
        if (!qget(query, "from", a, sizeof a) || !qget(query, "to", b, sizeof b) || !date_ok(a) || !date_ok(b)) { sb_free(&o); respond_err(cs, 400, "from/to must be YYYY-MM-DD"); return; }
        if (!qget(query, "by", c, sizeof c)) strcpy(c, "day");
        if (strcmp(c, "hour") && strcmp(c, "day") && strcmp(c, "month")) { sb_free(&o); respond_err(cs, 400, "by must be hour, day or month"); return; }
        wtime_parse(a, &f); wtime_parse(b, &t);
        days = (long)((wtime_epoch(&t) - wtime_epoch(&f)) / 86400) + 1;
        if (days < 1) { sb_free(&o); respond_err(cs, 400, "to is before from"); return; }
        if ((!strcmp(c, "hour") && days > 62) || days > 3700) { sb_free(&o); respond_err(cs, 400, "range too large for this grouping"); return; }
        rc = q_range(st, dev, a, b, c, csv, &o);
        if (rc == 0 && csv) {
            char extra[300]; snprintf(extra, sizeof extra, "Content-Disposition: attachment; filename=\"rain_%s_%s_%s.csv\"\r\n", c, a, b);
            respond(cs, 200, "text/csv; charset=utf-8", extra, o.p, o.n); sb_free(&o); return;
        }
    } else { sb_free(&o); respond_err(cs, 404, "unknown api"); return; }
    if (rc != 0 || o.err) respond_err(cs, 500, "query failed");
    else respond(cs, 200, "application/json; charset=utf-8", NULL, o.p, o.n);
    sb_free(&o);
}

static int read_request(sock_t cs, char *buf, size_t cap)
{
    size_t len = 0;
    int64_t deadline = plat_mono_ms() + 5000;   /* total budget: one slow client must not stall the server */
    while (len + 1 < cap) {
        fd_set rf; struct timeval tv; int r; int64_t left;
        left = deadline - plat_mono_ms();
        if (left <= 0) return -1;
        FD_ZERO(&rf); FD_SET(cs, &rf);
        tv.tv_sec = (long)(left / 1000); tv.tv_usec = (long)(left % 1000) * 1000;
        if (tv.tv_sec > 2) { tv.tv_sec = 2; tv.tv_usec = 0; }   /* wake up often enough to notice the deadline */
        r = select((int)cs + 1, &rf, NULL, NULL, &tv);
        if (r <= 0) return -1;
        r = (int)recv(cs, buf + len, (int)(cap - 1 - len), 0);
        if (r <= 0) return -1;
        len += (size_t)r; buf[len] = 0;
        if (strstr(buf, "\r\n\r\n") || strstr(buf, "\n\n")) return 0;
    }
    return -1;
}

/* One shared sqlite handle (opened with SQLITE_OPEN_FULLMUTEX) serves all workers;
   the DB layer serialises access, so no extra lock is needed around queries. */
typedef struct {
    const cfg_t *cfg;
    store_t *st;
    sock_t q[WEB_QUEUE_MAX];
    int head, tail, count, stopping, nworkers;
    web_mu_t mu;
    web_cv_t cv_job, cv_space;
    plat_thread_t *workers[WEB_WORKERS];
} web_srv_t;

static void web_worker(void *p)
{
    web_srv_t *sv = (web_srv_t *)p;
    char req[REQ_MAX];
    for (;;) {
        sock_t cs;
        wmu_lock(&sv->mu);
        while (!sv->count && !sv->stopping) wcv_wait_ms(&sv->cv_job, &sv->mu, -1);
        if (sv->stopping) {                    /* shutdown: drop queued sockets, do not serve them */
            while (sv->count) {
                cs = sv->q[sv->head];
                sv->head = (sv->head + 1) % WEB_QUEUE_MAX;
                sv->count--;
                sock_close(cs);
            }
            wmu_unlock(&sv->mu);
            return;
        }
        cs = sv->q[sv->head];
        sv->head = (sv->head + 1) % WEB_QUEUE_MAX;
        sv->count--;
        wcv_wake(&sv->cv_space);
        wmu_unlock(&sv->mu);
        if (read_request(cs, req, sizeof req) == 0) handle(cs, sv->st, sv->cfg, req);
        sock_close(cs);
    }
}

int web_run(const cfg_t *cfg)
{
    char err[200]; sock_t ls = SOCK_BAD; struct sockaddr_in addr; int one = 1, i, rc = 1;
    web_srv_t sv;
    memset(&sv, 0, sizeof sv);
    sv.cfg = cfg;
    wmu_init(&sv.mu); wcv_init(&sv.cv_job); wcv_init(&sv.cv_space);
#ifdef _WIN32
    WSADATA wsa; if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { LOGE("WSAStartup failed"); goto out; }
#endif
    sv.st = store_open(cfg->db, err, sizeof err);
    if (!sv.st) { LOGE("web: %s", err); goto out; }
    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == SOCK_BAD) { LOGE("web: socket() failed"); goto out; }
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET; addr.sin_port = htons((unsigned short)cfg->web_port);
    addr.sin_addr.s_addr = inet_addr(cfg->web_bind);
    if (addr.sin_addr.s_addr == INADDR_NONE) { LOGE("web: bad bind address '%s'", cfg->web_bind); goto out; }
    if (bind(ls, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(ls, 32) != 0) {
        LOGE("web: cannot listen on %s:%d (port in use?)", cfg->web_bind, cfg->web_port);
        goto out;
    }
    for (i = 0; i < WEB_WORKERS; i++) {
        sv.workers[i] = plat_thread_start(web_worker, &sv);
        if (sv.workers[i]) sv.nworkers++;
    }
    if (!sv.nworkers) LOGE("web: no worker thread started; serving synchronously");
    LOGI("web UI: http://%s:%d/ (%d workers)", strcmp(cfg->web_bind, "0.0.0.0") ? cfg->web_bind : "localhost",
         cfg->web_port, sv.nworkers);
    while (!plat_stop_requested()) {
        fd_set rf; struct timeval tv; sock_t cs; char req[REQ_MAX];
        FD_ZERO(&rf); FD_SET(ls, &rf); tv.tv_sec = 0; tv.tv_usec = 200000;
        if (select((int)ls + 1, &rf, NULL, NULL, &tv) <= 0) continue;
        cs = accept(ls, NULL, NULL);
        if (cs == SOCK_BAD) continue;
        set_send_timeout(cs, WEB_SEND_MS);
        if (!sv.nworkers) {                    /* fallback: no pool, serve inline */
            if (read_request(cs, req, sizeof req) == 0) handle(cs, sv.st, cfg, req);
            sock_close(cs);
            continue;
        }
        wmu_lock(&sv.mu);
        while (sv.count == WEB_QUEUE_MAX && !plat_stop_requested()) wcv_wait_ms(&sv.cv_space, &sv.mu, 200);
        if (plat_stop_requested()) { wmu_unlock(&sv.mu); sock_close(cs); break; }
        sv.q[sv.tail] = cs; sv.tail = (sv.tail + 1) % WEB_QUEUE_MAX; sv.count++;
        wcv_wake(&sv.cv_job);
        wmu_unlock(&sv.mu);
    }
    wmu_lock(&sv.mu);
    sv.stopping = 1;
    wcv_wake_all(&sv.cv_job); wcv_wake_all(&sv.cv_space);
    wmu_unlock(&sv.mu);
    for (i = 0; i < WEB_WORKERS; i++) if (sv.workers[i]) plat_thread_join(sv.workers[i]);
    sock_close(ls); ls = SOCK_BAD;
    rc = 0;
out:
    if (ls != SOCK_BAD) sock_close(ls);
    if (sv.st) store_close(sv.st);
    wcv_done(&sv.cv_job); wcv_done(&sv.cv_space); wmu_done(&sv.mu);
#ifdef _WIN32
    WSACleanup();
#endif
    return rc;
}

void web_thread(void *p) { web_run((const cfg_t *)p); }
