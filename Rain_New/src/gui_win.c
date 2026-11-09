/*
 * Rain collector - Windows tray program.
 *
 * A thin Win32 layer around the portable collector (collector.c) and the portable view logic (ui_logic.c):
 *   - status window with a log, minimised to the notification area (tray)
 *   - tray icon whose colour shows the state, balloon messages when communication breaks / recovers
 *   - single instance, optional start with Windows, clean shutdown on logoff / system shutdown
 *   - `rain.exe probe "YYYY-MM-DD HH"` and friends still work from a command prompt (see cli.c)
 *
 * Only wide-character (…W) Win32 functions are used. Text coming from the portable core is UTF-8.
 * Build: -mwindows -finput-charset=UTF-8 -lws2_32 (see build_win.bat). Starts the web UI thread as well.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "collector.h"
#include "config.h"
#include "log.h"
#include "platform.h"
#include "ui_logic.h"
#include "util.h"
#ifndef NO_WEB
#include "web.h"
#endif

#define WM_TRAYICON (WM_APP + 1)
#define WM_SHOWME   (WM_APP + 2)
#define TIMER_UI    1
#define NLINES      7
#define LOGBUF_CH   32768

enum { ID_SYNC = 101, ID_EDITCFG, ID_RELOAD, ID_LOGS, ID_AUTOSTART, ID_SHOW, ID_WEB, ID_EXIT };

static const WCHAR kClass[]  = L"RainCollectorWnd";
static const WCHAR kMutex[]  = L"Local\\RainCollector.SingleInstance";
static const WCHAR kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const WCHAR kRunVal[] = L"RainCollector";
static const char  kIni[]    = "rain.ini";            /* relative to the exe folder (we chdir there at start) */

static HINSTANCE g_inst;
static HWND g_wnd, g_lbl[NLINES], g_log;
static HFONT g_font, g_font_title;
static HICON g_ico_s[4], g_ico_b[4];
static HICON g_ico_app, g_ico_app_s;      /* designed icon from res\rain.rc (resource id 1) */
static NOTIFYICONDATAW g_nid;
static level_t g_level = LV_GRAY;
static char g_tip[120] = "雨量采集";
static cfg_t g_cfg, g_webcfg;
static plat_thread_t *g_thr, *g_web;
static ui_state_t g_ui;
static unsigned long g_logcur;
static WCHAR g_logbuf[LOGBUF_CH];
static int g_loglen;
static UINT g_msg_taskbar;
static int g_quitting, g_first_run;

/* ------------------------------------------------------------------ text helpers */

/* UTF-8 -> UTF-16 into a fixed destination, truncating safely */
static void u8w(const char *s, WCHAR *dst, int dstn)
{
    WCHAR tmp[700];
    if (dstn <= 0) return;
    dst[0] = 0;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, tmp, 700) == 0) return;
    lstrcpynW(dst, tmp, dstn);
}

/* ------------------------------------------------------------------ icons drawn at run time */

static HICON make_icon(level_t lv, int s)
{
    static const DWORD fill[4] = { 0xFF8A93A0, 0xFF2EA05A, 0xFFE8A317, 0xFFD64545 };   /* grey green amber red */
    static const DWORD edge[4] = { 0xFF5A626E, 0xFF1B6B3B, 0xFF9A6B0A, 0xFF8F2424 };
    BITMAPINFO bi;
    void *bits = NULL;
    HDC dc;
    HBITMAP color, mask;
    ICONINFO ii;
    HICON ic;
    DWORD *px;
    BYTE *mbits;
    int x, y, mstride;
    double c, r_out, r_in;

    if (s < 8) s = 16;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = s;
    bi.bmiHeader.biHeight = -s;                 /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dc = GetDC(NULL);
    color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color || !bits) return NULL;

    px = (DWORD *)bits;
    c = (s - 1) / 2.0;
    r_out = s / 2.0 - 0.3;
    r_in = r_out - (s >= 24 ? 2.0 : 1.4);
    for (y = 0; y < s; y++) {
        for (x = 0; x < s; x++) {
            double dx = x - c, dy = y - c, d2 = dx * dx + dy * dy;
            px[y * s + x] = d2 <= r_in * r_in ? fill[lv] : d2 <= r_out * r_out ? edge[lv] : 0x00000000;
        }
    }
    mstride = ((s + 15) / 16) * 2;
    mbits = (BYTE *)calloc((size_t)mstride * (size_t)s, 1);
    mask = CreateBitmap(s, s, 1, 1, mbits);
    free(mbits);
    if (!mask) { DeleteObject(color); return NULL; }

    memset(&ii, 0, sizeof ii);
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    ic = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return ic;
}

/* ------------------------------------------------------------------ tray */

static void tray_apply(DWORD op, UINT flags)
{
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_wnd;
    g_nid.uID = 1;
    g_nid.uFlags = flags;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_ico_s[g_level];
    u8w(g_tip, g_nid.szTip, 128);
    Shell_NotifyIconW(op, &g_nid);
}
static void tray_add(void)    { tray_apply(NIM_ADD, NIF_MESSAGE | NIF_ICON | NIF_TIP); }
static void tray_update(void) { tray_apply(NIM_MODIFY, NIF_ICON | NIF_TIP); }
static void tray_remove(void) { Shell_NotifyIconW(NIM_DELETE, &g_nid); }

static void tray_balloon(const char *title, const char *text, int warn)
{
    u8w(title, g_nid.szInfoTitle, 64);
    u8w(text, g_nid.szInfo, 256);
    g_nid.dwInfoFlags = warn ? NIIF_WARNING : NIIF_INFO;
    tray_apply(NIM_MODIFY, NIF_ICON | NIF_TIP | NIF_INFO);
}

static void apply_level(level_t lv)
{
    g_level = lv;
    tray_update();
    SendMessageW(g_wnd, WM_SETICON, ICON_SMALL, (LPARAM)g_ico_s[lv]);
    SendMessageW(g_wnd, WM_SETICON, ICON_BIG, (LPARAM)g_ico_b[lv]);
}

/* ------------------------------------------------------------------ autostart (HKCU Run) */

static int autostart_get(void)
{
    HKEY k;
    DWORD type = 0, n = 0;
    LONG r;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    r = RegQueryValueExW(k, kRunVal, NULL, &type, NULL, &n);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static void autostart_set(int on)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    if (on) {
        WCHAR cmd[MAX_PATH + 16];
        DWORD n = GetModuleFileNameW(NULL, cmd + 1, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            cmd[0] = L'"';
            lstrcpynW(cmd + 1 + n, L"\" --tray", 10);
            RegSetValueExW(k, kRunVal, 0, REG_SZ, (const BYTE *)cmd, (DWORD)((lstrlenW(cmd) + 1) * sizeof(WCHAR)));
        }
    } else {
        RegDeleteValueW(k, kRunVal);
    }
    RegCloseKey(k);
}

/* ------------------------------------------------------------------ collector control */

static void load_config(void)
{
    cfg_defaults(&g_cfg);
    if (cfg_load(&g_cfg, kIni) != 0) {
        cfg_write_default(kIni);
        cfg_load(&g_cfg, kIni);
        g_first_run = 1;
    }
    log_init(g_cfg.logdir, 0, g_cfg.trace);
    if (g_first_run) LOGW("rain.ini was missing; a default one was written. Check the serial port (now %s), then press Reload.", g_cfg.port);
}

static void start_collector(void)
{
    collector_reset_status();
    plat_clear_stop();
    g_thr = plat_thread_start(collector_thread, &g_cfg);
    if (!g_thr) LOGE("cannot start the collector thread");
#ifndef NO_WEB
    if (g_cfg.web_enable) {
        g_webcfg = g_cfg;                       /* the web thread keeps its own copy: rain.ini may be reloaded */
        g_web = plat_thread_start(web_thread, &g_webcfg);
        if (!g_web) LOGE("cannot start the web thread");
    }
#endif
}

static void stop_collector(void)
{
    plat_request_stop();
    if (g_thr) { plat_thread_join(g_thr); g_thr = NULL; }
    if (g_web) { plat_thread_join(g_web); g_web = NULL; }
    plat_clear_stop();
}

/* ------------------------------------------------------------------ window content */

static void set_label(int i, const char *u8)
{
    static char cache[NLINES][400];
    WCHAR w[512];
    if (strcmp(cache[i], u8) == 0) return;
    snprintf(cache[i], sizeof cache[i], "%s", u8);
    u8w(u8, w, 512);
    SetWindowTextW(g_lbl[i], w);
}

static void drain_log(void)
{
    char line[300];
    WCHAR w[700];
    int changed = 0;
    while (log_ring_next(&g_logcur, line, sizeof line)) {
        int n;
        u8w(line, w, 700);
        n = lstrlenW(w);
        if (g_loglen + n + 4 >= LOGBUF_CH) {              /* drop the oldest half, at a line boundary */
            int drop = g_loglen / 2;
            while (drop < g_loglen && g_logbuf[drop] != L'\n') drop++;
            if (drop < g_loglen) drop++;
            memmove(g_logbuf, g_logbuf + drop, (size_t)(g_loglen - drop) * sizeof(WCHAR));
            g_loglen -= drop;
        }
        memcpy(g_logbuf + g_loglen, w, (size_t)n * sizeof(WCHAR));
        g_loglen += n;
        g_logbuf[g_loglen++] = L'\r';
        g_logbuf[g_loglen++] = L'\n';
        changed = 1;
    }
    if (changed) {
        g_logbuf[g_loglen] = 0;
        SetWindowTextW(g_log, g_logbuf);
        SendMessageW(g_log, EM_SETSEL, (WPARAM)g_loglen, (LPARAM)g_loglen);
        SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
    }
}

static void refresh_ui(void)
{
    col_status_t cs;
    ui_view_t v;
    char buf[400];
    collector_get_status(&cs);
    ui_compute(&g_ui, &cs, g_cfg.port, g_cfg.poll_sec, plat_mono_ms(), &v);

    snprintf(buf, sizeof buf, "状态：%s%s%s", v.status, v.detail[0] ? " —— " : "", v.detail);
    set_label(0, buf);
    set_label(1, v.device);
    set_label(2, v.contact);
    set_label(3, v.today);
    set_label(4, v.power);
    set_label(5, v.clock);
    snprintf(buf, sizeof buf, "运行统计：已同步 %lu 次，通信正常帧 %lu，CRC 错误 %lu", cs.cycles, cs.frames_ok, cs.crc_errors);
    set_label(6, buf);

    if (v.level != g_level) { snprintf(g_tip, sizeof g_tip, "%s", v.tip); apply_level(v.level); }
    else if (strcmp(v.tip, g_tip) != 0) { snprintf(g_tip, sizeof g_tip, "%s", v.tip); tray_update(); }
    if (v.balloon) tray_balloon(v.balloon_title, v.balloon_text, v.balloon == 2);
    drain_log();
}

static void show_window(void)
{
    ShowWindow(g_wnd, SW_SHOW);
    ShowWindow(g_wnd, SW_RESTORE);
    SetForegroundWindow(g_wnd);
}

static void open_in_shell(const WCHAR *verb, const WCHAR *file, const WCHAR *params)
{
    ShellExecuteW(g_wnd, verb, file, params, NULL, SW_SHOWNORMAL);
}

static void tray_menu(void)
{
    HMENU m = CreatePopupMenu();
    POINT pt;
    if (!m) return;
    AppendMenuW(m, MF_STRING, ID_SHOW, L"显示窗口");
    AppendMenuW(m, MF_STRING, ID_SYNC, L"立即同步");
#ifndef NO_WEB
    if (g_cfg.web_enable) AppendMenuW(m, MF_STRING, ID_WEB, L"打开网页");
#endif
    AppendMenuW(m, MF_STRING, ID_LOGS, L"打开日志目录");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (autostart_get() ? MF_CHECKED : 0), ID_AUTOSTART, L"开机自动启动");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"退出");
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);                 /* required, otherwise the menu does not close when clicking elsewhere */
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_wnd, NULL);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void do_command(int id)
{
    WCHAR wdir[300];
    switch (id) {
    case ID_SHOW: show_window(); break;
    case ID_SYNC: collector_poke(); LOGI("manual sync requested"); break;
#ifndef NO_WEB
    case ID_WEB:
    {
        char a[96]; WCHAR w[128];
        snprintf(a, sizeof a, "http://%s:%d/", strcmp(g_cfg.web_bind, "0.0.0.0") ? g_cfg.web_bind : "127.0.0.1", g_cfg.web_port);
        u8w(a, w, 128);
        open_in_shell(L"open", w, NULL);
        break;
    }
#endif
    case ID_EDITCFG: open_in_shell(L"open", L"notepad.exe", L"rain.ini"); break;
    case ID_RELOAD:
        LOGI("reloading configuration");
        stop_collector();
        load_config();
        start_collector();
        break;
    case ID_LOGS:
        u8w(g_cfg.logdir, wdir, 300);
        open_in_shell(L"open", wdir, NULL);
        break;
    case ID_AUTOSTART: autostart_set(!autostart_get()); break;
    case ID_EXIT:
        if (MessageBoxW(g_wnd, L"退出后将停止采集。确定要退出吗？", L"雨量采集", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            g_quitting = 1;
            DestroyWindow(g_wnd);
        }
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------ window procedure */

static HWND make_child(HWND parent, const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent, (HMENU)(UINT_PTR)id, g_inst, NULL);
    if (c && g_font) SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

/* same face as the system message font, scaled / emboldened */
static HFONT make_font(const LOGFONTW *base, int pct, int weight)
{
    LOGFONTW lf = *base;
    lf.lfHeight = base->lfHeight * pct / 100;
    lf.lfWeight = weight;
    return CreateFontIndirectW(&lf);
}

static void create_controls(HWND hwnd)
{
    static const WCHAR *btn[4] = { L"立即同步", L"编辑配置", L"重新加载配置", L"日志目录" };
    static const int btn_id[4] = { ID_SYNC, ID_EDITCFG, ID_RELOAD, ID_LOGS };
    NONCLIENTMETRICSW ncm;
    HWND c;
    int i;

    memset(&ncm, 0, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0)) {
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        g_font_title = make_font(&ncm.lfMessageFont, 170, FW_BOLD);
    }
    if (!g_font) g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (!g_font_title) g_font_title = g_font;

    /* header: title on the left, version on the right, thin rule underneath */
    c = make_child(hwnd, L"STATIC", L"雨量采集", SS_LEFT | SS_NOPREFIX, 16, 12, 320, 32, 0);
    if (c && g_font_title) SendMessageW(c, WM_SETFONT, (WPARAM)g_font_title, TRUE);
    c = make_child(hwnd, L"STATIC", L"v" RAIN_VERSION_W, SS_RIGHT | SS_NOPREFIX, 368, 18, 196, 26, 0);
    if (c && g_font_title) SendMessageW(c, WM_SETFONT, (WPARAM)g_font_title, TRUE);
    make_child(hwnd, L"STATIC", L"", SS_ETCHEDHORZ, 16, 54, 548, 2, 0);

    for (i = 0; i < NLINES; i++) g_lbl[i] = make_child(hwnd, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 16, 66 + 22 * i, 548, 20, 0);

    make_child(hwnd, L"STATIC", L"", SS_ETCHEDHORZ, 16, 226, 548, 2, 0);

    for (i = 0; i < 4; i++) make_child(hwnd, L"BUTTON", btn[i], BS_PUSHBUTTON | WS_TABSTOP, 16 + i * 140, 236, 128, 30, btn_id[i]);

    make_child(hwnd, L"STATIC", L"", SS_ETCHEDHORZ, 16, 278, 548, 2, 0);
    make_child(hwnd, L"STATIC", L"运行日志", SS_LEFT | SS_NOPREFIX, 16, 284, 548, 20, 0);

    g_log = make_child(hwnd, L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 16, 306, 548, 148, 0);

    make_child(hwnd, L"STATIC", L"", SS_ETCHEDHORZ, 16, 460, 548, 2, 0);
    make_child(hwnd, L"STATIC", L"rain v" RAIN_VERSION_W L" · 数据库 rain.db · " RAIN_COPYRIGHT_W,
               SS_LEFT | SS_NOPREFIX, 16, 464, 548, 20, 0);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_msg_taskbar && msg == g_msg_taskbar) { tray_add(); return 0; }       /* explorer.exe restarted */
    switch (msg) {
    case WM_CREATE:
        g_wnd = hwnd;
        create_controls(hwnd);
        tray_add();
        SetTimer(hwnd, TIMER_UI, 1000, NULL);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_UI) refresh_ui();
        return 0;
    case WM_COMMAND:
        do_command((int)LOWORD(wp));
        return 0;
    case WM_TRAYICON:
        if ((UINT)lp == WM_LBUTTONUP || (UINT)lp == WM_LBUTTONDBLCLK) show_window();
        else if ((UINT)lp == WM_RBUTTONUP) tray_menu();
        return 0;
    case WM_SHOWME:
        show_window();
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE) { ShowWindow(hwnd, SW_HIDE); return 0; }   /* minimise = to the tray */
        break;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) ShowWindow(hwnd, SW_HIDE);                         /* e.g. Win+D */
        break;
    case WM_CLOSE:
        if (!g_quitting) { ShowWindow(hwnd, SW_HIDE); return 0; }                    /* the X button also only hides */
        break;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) { g_quitting = 1; stop_collector(); tray_remove(); }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_UI);
        stop_collector();
        tray_remove();
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ entry */

static int run_cli(int argc, LPWSTR *wargv)
{
    char **av = (char **)calloc((size_t)argc + 1, sizeof *av);
    int i, rc;
    if (!av) return 1;
    for (i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        av[i] = (char *)calloc((size_t)(n > 0 ? n : 1), 1);
        if (av[i] && n > 0) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, av[i], n, NULL, NULL);
    }
    {   /* A GUI-subsystem exe has no console. If stdout was redirected (rain.exe probe ... > file.txt) the handle is valid and
           the C runtime already uses it; only when there is none do we attach to the console that started us. */
        HANDLE ho = GetStdHandle(STD_OUTPUT_HANDLE);
        if ((ho == NULL || ho == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }
    }
    rc = cli_run(argc, av);
    return rc;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WCHAR exe[MAX_PATH];
    LPWSTR *wargv;
    WNDCLASSEXW wc;
    MSG msg;
    HANDLE mutex;
    DWORD n, style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc;
    int argc = 0, i, start_hidden = 0, w, h, lv, ssz, bsz;
    (void)prev; (void)cmdline; (void)show;

    g_inst = inst;
    plat_init();

    n = GetModuleFileNameW(NULL, exe, MAX_PATH);          /* work in the exe's folder: rain.ini, rain.db, logs\ live there */
    if (n > 0 && n < MAX_PATH) {
        WCHAR *p = exe + n;
        while (p > exe && p[-1] != L'\\' && p[-1] != L'/') p--;
        if (p > exe) { *p = 0; SetCurrentDirectoryW(exe); }
    }

    wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (wargv) {
        for (i = 1; i < argc; i++) if (lstrcmpW(wargv[i], L"--tray") == 0) start_hidden = 1;
        if (argc > 1 && lstrcmpW(wargv[1], L"--tray") != 0 && lstrcmpW(wargv[1], L"--show") != 0)
            return run_cli(argc, wargv);                  /* diagnostics: rain.exe probe "2026-11-05 20" */
    }

    mutex = CreateMutexW(NULL, FALSE, kMutex);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {   /* already running: just bring it to the front */
        HWND other = FindWindowW(kClass, NULL);
        if (other) PostMessageW(other, WM_SHOWME, 0, 0);
        return 0;
    }

    ui_state_init(&g_ui);
    load_config();

    ssz = GetSystemMetrics(SM_CXSMICON);
    bsz = GetSystemMetrics(SM_CXICON);
    for (lv = 0; lv < 4; lv++) {
        g_ico_s[lv] = make_icon((level_t)lv, ssz);
        g_ico_b[lv] = make_icon((level_t)lv, bsz);
        if (!g_ico_s[lv]) g_ico_s[lv] = LoadIconW(NULL, MAKEINTRESOURCEW(32512));
        if (!g_ico_b[lv]) g_ico_b[lv] = g_ico_s[lv];
    }

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    g_ico_app   = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, bsz, bsz, LR_DEFAULTCOLOR);
    g_ico_app_s = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, ssz, ssz, LR_DEFAULTCOLOR);
    wc.hIcon = g_ico_app ? g_ico_app : g_ico_b[LV_GRAY];
    wc.hIconSm = g_ico_app_s ? g_ico_app_s : g_ico_s[LV_GRAY];
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = (HBRUSH)(UINT_PTR)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc)) return 1;
    g_msg_taskbar = RegisterWindowMessageW(L"TaskbarCreated");

    rc.left = 0; rc.top = 0; rc.right = 580; rc.bottom = 500;
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    g_wnd = CreateWindowExW(0, kClass, L"雨量采集  v" RAIN_VERSION_W, style,
                            (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h,
                            NULL, NULL, inst, NULL);
    if (!g_wnd) return 1;

    start_collector();
    refresh_ui();
    if (!start_hidden || g_first_run) { ShowWindow(g_wnd, SW_SHOW); UpdateWindow(g_wnd); }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    stop_collector();
    for (lv = 0; lv < 4; lv++) { if (g_ico_s[lv]) DestroyIcon(g_ico_s[lv]); if (g_ico_b[lv] && g_ico_b[lv] != g_ico_s[lv]) DestroyIcon(g_ico_b[lv]); }
    if (g_ico_app) DestroyIcon(g_ico_app);
    if (g_ico_app_s) DestroyIcon(g_ico_app_s);
    if (g_font) DeleteObject(g_font);
    if (g_font_title && g_font_title != g_font) DeleteObject(g_font_title);
    if (mutex) CloseHandle(mutex);
    return (int)msg.wParam;
}
