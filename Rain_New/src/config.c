#include "config.h"
#include "log.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cfg_defaults(cfg_t *c)
{
    memset(c, 0, sizeof *c);
#ifdef _WIN32
    strcpy(c->port, "COM1");
#else
    strcpy(c->port, "/dev/ttyS0");
#endif
    c->baud = 9600;
    c->unit = 100;
    c->poll_sec = 60; c->backfill_hours = 72; c->max_fetch = 24; c->retries = 3;
    c->timeout_ms = 1500; c->reconnect_sec = 10; c->status_hist_sec = 600;
    strcpy(c->endian, "auto"); c->max_per_min = 1000;
    c->web_enable = 1; strcpy(c->web_bind, "127.0.0.1"); c->web_port = 9099;
    strcpy(c->db, "rain.db"); strcpy(c->logdir, "logs");
}

static void sset(char *dst, size_t n, const char *v) { strncpy(dst, v, n - 1); dst[n - 1] = 0; }

static int cfg_set(cfg_t *c, const char *k, const char *v)
{
    if (!strcmp(k, "serial.port")) sset(c->port, sizeof c->port, v);
    else if (!strcmp(k, "serial.baud")) c->baud = atoi(v);
    else if (!strcmp(k, "serial.dtr")) c->dtr = atoi(v);
    else if (!strcmp(k, "serial.rts")) c->rts = atoi(v);
    else if (!strcmp(k, "device.machine")) sset(c->machine, sizeof c->machine, v);
    else if (!strcmp(k, "device.name")) sset(c->name, sizeof c->name, v);
    else if (!strcmp(k, "device.unit")) c->unit = atoi(v);
    else if (!strcmp(k, "collect.poll_sec")) c->poll_sec = atoi(v);
    else if (!strcmp(k, "collect.backfill_hours")) c->backfill_hours = atoi(v);
    else if (!strcmp(k, "collect.max_fetch")) c->max_fetch = atoi(v);
    else if (!strcmp(k, "collect.retries")) c->retries = atoi(v);
    else if (!strcmp(k, "collect.timeout_ms")) c->timeout_ms = atoi(v);
    else if (!strcmp(k, "collect.reconnect_sec")) c->reconnect_sec = atoi(v);
    else if (!strcmp(k, "collect.status_hist_sec")) c->status_hist_sec = atoi(v);
    else if (!strcmp(k, "collect.endian")) sset(c->endian, sizeof c->endian, v);
    else if (!strcmp(k, "collect.max_per_min")) c->max_per_min = atoi(v);
    else if (!strcmp(k, "web.enable")) c->web_enable = atoi(v);
    else if (!strcmp(k, "web.bind")) sset(c->web_bind, sizeof c->web_bind, v);
    else if (!strcmp(k, "web.port")) c->web_port = atoi(v);
    else if (!strcmp(k, "paths.db")) sset(c->db, sizeof c->db, v);
    else if (!strcmp(k, "paths.logdir")) sset(c->logdir, sizeof c->logdir, v);
    else if (!strcmp(k, "paths.trace")) c->trace = atoi(v);
    else return -1;
    return 0;
}

static char *trim(char *s)
{
    char *e;
    while (*s && isspace((unsigned char)*s)) s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

int cfg_load(cfg_t *c, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512], sec[32] = "", key[96]; int ln = 0;
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line), *eq; size_t i;
        ln++;
        if (ln == 1 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3;  /* BOM */
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (e) { *e = 0; sset(sec, sizeof sec, s + 1); for (i = 0; sec[i]; i++) sec[i] = (char)tolower((unsigned char)sec[i]); }
            continue;
        }
        eq = strchr(s, '=');
        if (!eq) { LOGW("config line %d ignored", ln); continue; }
        *eq = 0;
        { char *v = eq + 1, *q;                        /* inline comment: whitespace followed by ';' ('#' only starts a full-line comment) */
          for (q = v; *q; q++) if (*q == ';' && q > v && (q[-1] == ' ' || q[-1] == '\t')) { *q = 0; break; } }
        snprintf(key, sizeof key, "%s.%s", sec, trim(s));
        for (i = 0; key[i]; i++) key[i] = (char)tolower((unsigned char)key[i]);
        if (cfg_set(c, key, trim(eq + 1)) != 0) LOGW("config: unknown key '%s' (line %d)", key, ln);
    }
    fclose(f);
    if (c->unit < 1) c->unit = 100;
    if (c->poll_sec < 5) c->poll_sec = 5;
    if (c->retries < 1) c->retries = 1;
    if (c->timeout_ms < 200) c->timeout_ms = 200;
    if (c->backfill_hours < 1) c->backfill_hours = 1;
    if (c->max_fetch < 1) c->max_fetch = 1;
    return 0;
}

endian_t cfg_endian(const cfg_t *c)
{
    if (!strcmp(c->endian, "le")) return EN_LE;
    if (!strcmp(c->endian, "be")) return EN_BE;
    return EN_AUTO;
}

/* Keep in sync with rain.ini.sample (tests/test_config.c checks that the generated file parses to the defaults). */
int cfg_write_default(const char *path)
{
    cfg_t d; FILE *f;
    cfg_defaults(&d);
    f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f,
        "; rain.ini - generated on first start. Edit and use 'Reload' in the program (or restart it).\r\n"
        "; Keep this file UTF-8 or ASCII. A ';' after a value (preceded by a space) starts a trailing comment.\r\n"
        "\r\n[serial]\r\nport=%s\r\nbaud=9600\r\ndtr=0\r\nrts=0\r\n"
        "\r\n[device]\r\n; expected machine id, e.g. SRY1C-2211-00001; empty = accept whatever the device reports\r\nmachine=\r\nname=\r\n"
        "unit=100                ; tip counts per mm\r\n"
        "\r\n[collect]\r\npoll_sec=60\r\nbackfill_hours=72\r\nmax_fetch=24\r\nretries=3\r\ntimeout_ms=1500\r\nreconnect_sec=10\r\n"
        "status_hist_sec=600\r\nendian=auto             ; auto | le | be\r\nmax_per_min=1000\r\n"
        "\r\n[paths]\r\ndb=rain.db\r\nlogdir=logs\r\ntrace=0                 ; 1 = log every byte sent/received\r\n",
        d.port);
    fclose(f);
    return 0;
}
