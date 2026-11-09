#include "config.h"
#include "collector.h"
#include "store.h"
#include "cli.h"
#ifndef NO_WEB
#include "query.h"
#include "web.h"
#endif
#include "log.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <windows.h>
#endif

/* Sources are UTF-8; a Windows console defaults to the OEM code page, so switch it to match. */
static void console_utf8(void)
{
#ifdef _WIN32
    if (GetConsoleOutputCP()) { SetConsoleOutputCP(CP_UTF8); SetConsoleCP(CP_UTF8); }
#endif
}

static void usage(void)
{
    puts("rain " RAIN_VERSION " - rain gauge collector, storage and viewer\n"
         RAIN_COPYRIGHT "\n"
         "\n"
         "usage: rain [-c rain.ini] <command> [args]\n"
         "\n"
#ifndef NO_WEB
         "  run             collect from the device and serve the web UI (default)\n"
         "  serve           web UI only (reads the database)\n"
         "  report [YYYY-MM-DD]     print one day's 24-hour table to the console\n"
#endif
         "  collect         collect only, no window (console)\n"
         "  once            connect, run one sync cycle, exit\n"
         "  probe \"YYYY-MM-DD HH\"   fetch one hour from the device and show raw + decoded\n"
         "                  (use it to verify byte order against known history; no DB write)\n"
         "  version | help");
}

#ifndef NO_WEB
static int cmd_report(const cfg_t *cfg, const char *date)
{
    char err[200], d[16]; store_t *st; int dev; sbuf_t o; wtime_t t;
    if (!date) { wtime_now(&t); wtime_fmt(&t, WT_DATE, d, sizeof d); date = d; }
    if (!date_ok(date)) { fprintf(stderr, "bad date, use YYYY-MM-DD\n"); return 1; }
    st = store_open(cfg->db, err, sizeof err);
    if (!st) { fprintf(stderr, "%s\n", err); return 1; }
    dev = store_default_device(st);
    if (dev < 0) { fprintf(stderr, "no device in database yet\n"); store_close(st); return 1; }
    sb_init(&o);
    if (q_day_text(st, dev, date, &o) == 0) fputs(o.p, stdout);
    sb_free(&o); store_close(st);
    return 0;
}
#endif

int cli_run(int argc, char **argv)
{
    cfg_t cfg; const char *ini = "rain.ini", *cmd = "run"; int i = 1, rc = 0, loaded;
    cfg_defaults(&cfg);
    console_utf8();
    if (i + 1 < argc && !strcmp(argv[i], "-c")) { ini = argv[i + 1]; i += 2; }
    if (i < argc) cmd = argv[i++];
    if (!strcmp(cmd, "help") || !strcmp(cmd, "-h") || !strcmp(cmd, "--help")) { usage(); return 0; }
    if (!strcmp(cmd, "version")) { puts("rain " RAIN_VERSION "\n" RAIN_COPYRIGHT); return 0; }

    loaded = cfg_load(&cfg, ini);
    log_init(cfg.logdir, 1, cfg.trace);
    if (loaded != 0) LOGW("config file '%s' not found, using defaults", ini);
    plat_init();
    plat_install_stop_handler();

#ifndef NO_WEB
    if (!strcmp(cmd, "run")) {
        plat_thread_t *web = NULL;
        if (cfg.web_enable) web = plat_thread_start(web_thread, &cfg);
        rc = collector_run(&cfg);
        plat_request_stop();
        plat_thread_join(web);
    } else if (!strcmp(cmd, "serve")) rc = web_run(&cfg);
    else if (!strcmp(cmd, "report")) rc = cmd_report(&cfg, i < argc ? argv[i] : NULL);
    else
#endif
    if (!strcmp(cmd, "collect") || !strcmp(cmd, "run")) rc = collector_run(&cfg);
    else if (!strcmp(cmd, "once")) rc = collector_once(&cfg);
    else if (!strcmp(cmd, "probe")) {
        if (i >= argc) { usage(); return 1; }
        rc = collector_probe(&cfg, argv[i]);
    } else { usage(); return 1; }
    return rc;
}
