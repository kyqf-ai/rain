// MiniHttpd v2.3 (IOCP + FastCGI) - 单文件 Windows HTTP 服务器：完成端口高并发，可选 PHP
// 用法: MiniHttpd.exe [端口] [lan] [nobrowser]
//   lan        监听所有网卡（局域网可访问），默认只监听 127.0.0.1
//   nobrowser  启动时不自动打开浏览器
//   min        启动时窗口最小化到任务栏
//   follow     允许跟随指向根目录之外的 junction/symlink（默认拦截）
//   rewrite    伪静态：不存在的路径交给根目录 index.php（WordPress/Laravel 等）
//   nophp      即使存在 php\php-cgi.exe 也不启用 PHP
// 配置文件: exe 同目录 minihttpd.ini（可选）。优先级：命令行参数 > 配置文件 >
//           内置默认值；文件缺失整体按默认运行，某项值无效仅该项回退默认值，
//           其余项照常生效（详见随发行包的模板 minihttpd.ini）
// PHP 调用方式：检测到 php\php-cgi.exe 时自动启用 FastCGI 常驻进程池
// （Nginx + PHP-FPM 架构：进程复用 + 持久连接 + IOCP 全异步 + FIFO 排队），
// 进程池启动失败自动回退到逐请求 CGI 模式。
// v2.2 要点：php-cgi 500 请求自退已禁用（PHP_FCGI_MAX_REQUESTS=0）；spawn 连接
// 改监听 exceptfds 循环重试（Windows 被拒不走 writefds，旧版会白等 8 秒）；
// 补齐 Date/Last-Modified/If-Modified-Since/If-None-Match 列表与 */If-Range/
// bytes=-0→416/请求头超限→431；请求头 20 秒总时限；POST 体按需分配；PHP 输出
// 响应组装改缓冲区窃取（峰值内存 2x→1x）；请求头一次解析 + 原地免拷贝；发送
// 缓冲无锁回收池；FastCGI 客户端断开加固（Conn::up 双向绑定）；请求头/环境
// 传递改一次解析的 Hdrs 结构；目录列表单次遍历。
// v2.3 要点：全部运行期参数外置到 minihttpd.ini（端口/根目录/线程数/各类超时/
// PHP 与 FastCGI 进程池上限/目录列表开关/默认主页等 24 键，命令行参数仍优先）；
// 编码兼容 UTF-8/UTF-16/ANSI；时间值支持 ms/s/m/h 后缀、大小值支持 B/KB/MB/GB
// 后缀；界面新增配置状态行；Keep-Alive 通告超时与实际执行值保持一致；所有
// 默认值与 v2.2 行为完全一致。
#ifdef __MINGW32__
#define __USE_MINGW_ANSI_STDIO 1
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <shellapi.h>
#include "resource.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>
#include <stdarg.h>
#include <limits.h>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#endif

#define WIDE_(s) L##s
#define WIDE(s)  WIDE_(s)
#define SERVER_TOKEN APP_NAME "/" APP_VER_SHORT          /* Server: MiniHttpd/2.0 */
#define PCAP     2048
#define WM_TRAY  (WM_USER + 1)
#define ID_OPEN  1
#define ID_EXIT  2

static wchar_t g_root[PCAP];     // 根目录，末尾无反斜杠
static int     g_port = 8080;
static int     g_lan  = 0;
static SOCKET  g_ls[2] = {INVALID_SOCKET, INVALID_SOCKET};  // [0]=IPv4 [1]=IPv6

/* ---------------- MIME ---------------- */
typedef struct { const char *ext, *type; } Mime;
static const Mime MIMES[] = {
    {"html","text/html; charset=utf-8"}, {"htm","text/html; charset=utf-8"},
    {"css","text/css; charset=utf-8"},
    {"js","text/javascript; charset=utf-8"}, {"mjs","text/javascript; charset=utf-8"},
    {"cjs","text/javascript; charset=utf-8"},
    {"json","application/json; charset=utf-8"}, {"map","application/json; charset=utf-8"},
    {"webmanifest","application/manifest+json"},
    {"xml","application/xml; charset=utf-8"}, {"txt","text/plain; charset=utf-8"},
    {"md","text/plain; charset=utf-8"}, {"csv","text/csv; charset=utf-8"},
    {"yaml","text/plain; charset=utf-8"}, {"yml","text/plain; charset=utf-8"},
    {"ts","text/plain; charset=utf-8"}, {"log","text/plain; charset=utf-8"},
    {"svg","image/svg+xml"}, {"png","image/png"}, {"jpg","image/jpeg"},
    {"jpeg","image/jpeg"}, {"gif","image/gif"}, {"webp","image/webp"},
    {"avif","image/avif"}, {"bmp","image/bmp"}, {"ico","image/x-icon"},
    {"mp3","audio/mpeg"}, {"wav","audio/wav"}, {"ogg","audio/ogg"},
    {"m4a","audio/mp4"}, {"flac","audio/flac"}, {"aac","audio/aac"},
    {"mp4","video/mp4"}, {"webm","video/webm"}, {"mov","video/quicktime"},
    {"mkv","video/x-matroska"}, {"ogv","video/ogg"},
    {"woff","font/woff"}, {"woff2","font/woff2"}, {"ttf","font/ttf"},
    {"otf","font/otf"}, {"eot","application/vnd.ms-fontobject"},
    {"pdf","application/pdf"}, {"zip","application/zip"},
    {"gz","application/gzip"}, {"7z","application/x-7z-compressed"},
    {"wasm","application/wasm"},
    {NULL,NULL}
};

static const char *mime_of(const wchar_t *path) {
    const wchar_t *dot = wcsrchr(path, L'.'), *sl = wcsrchr(path, L'\\');
    if (!dot || (sl && dot < sl)) return "application/octet-stream";
    char ext[16]; int i = 0;
    for (dot++; *dot && i < 15; dot++) {
        if (*dot > 127) return "application/octet-stream";
        ext[i++] = (char)tolower((int)*dot);
    }
    ext[i] = 0;
    for (const Mime *m = MIMES; m->ext; m++)
        if (!strcmp(m->ext, ext)) return m->type;
    return "application/octet-stream";
}

/* ---------------- 简易动态缓冲区 ---------------- */
/*BUF-BEGIN*/
typedef struct { char *p; size_t n, cap; } Buf;

static void b_add(Buf *b, const char *s, size_t len) {
    if (b->n + len + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->n + len + 1) c *= 2;
        char *np = (char *)realloc(b->p, c);
        if (!np) return;
        b->p = np; b->cap = c;
    }
    memcpy(b->p + b->n, s, len);
    b->n += len;
    b->p[b->n] = 0;
}
static void b_str(Buf *b, const char *s) { b_add(b, s, strlen(s)); }
/*BUF-END*/

static void b_esc(Buf *b, const char *s) {           // HTML 转义（安全段整段写入，不逐字符）
    const char *run = s, *p = s;
    for (;;) {
        char c = *p;
        if (c == '<' || c == '>' || c == '&' || c == '"') {
            if (p > run) b_add(b, run, (size_t)(p - run));
            b_add(b, c == '<' ? "&lt;" : c == '>' ? "&gt;" :
                      c == '&' ? "&amp;" : "&quot;", 4);
            run = ++p;
        } else if (!c) {
            if (p > run) b_add(b, run, (size_t)(p - run));
            break;
        } else p++;
    }
}
static void b_urlenc(Buf *b, const char *s) {        // URL 百分号编码（安全段整段写入）
    static const char hex[] = "0123456789ABCDEF";
    const char *run = s, *p = s;
    for (;;) {
        unsigned char c = (unsigned char)*p;
        if (!c) { if (p > run) b_add(b, run, (size_t)(p - run)); break; }
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') { p++; continue; }
        if (p > run) b_add(b, run, (size_t)(p - run));
        { char t[3] = {'%', hex[c >> 4], hex[c & 15]}; b_add(b, t, 3); }
        run = ++p;
    }
}

/* ---------------- 路径处理 ---------------- */
static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 32;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int url_decode(const char *in, char *out, int cap) {
    int o = 0;
    for (; *in; in++) {
        int c = (unsigned char)*in;
        if (c == '%' && hexv(in[1]) >= 0 && hexv(in[2]) >= 0) {
            c = hexv(in[1]) * 16 + hexv(in[2]);
            in += 2;
        }
        if (c == 0 || o >= cap - 1) return -1;
        out[o++] = (char)c;
    }
    out[o] = 0;
    return o;
}

// Windows 保留设备名：CON PRN AUX NUL COM0-9 LPT0-9（含上标 ¹²³）CONIN$ CONOUT$
// Win32 会忽略基名之后的扩展名和基名尾部空格，所以 "con.txt" "nul .x" 同样是设备
static int reserved_name(const wchar_t *s, int len) {
    int m = 0;
    while (m < len && s[m] != L'.') m++;                 // 基名 = 第一个 '.' 之前
    while (m > 0 && s[m - 1] == L' ') m--;               // 去掉基名尾部空格
    if (m == 3 && (!_wcsnicmp(s, L"CON", 3) || !_wcsnicmp(s, L"PRN", 3) ||
                   !_wcsnicmp(s, L"AUX", 3) || !_wcsnicmp(s, L"NUL", 3))) return 1;
    if (m == 4 && (!_wcsnicmp(s, L"COM", 3) || !_wcsnicmp(s, L"LPT", 3))) {
        wchar_t d = s[3];
        if ((d >= L'0' && d <= L'9') || d == 0x00B9 || d == 0x00B2 || d == 0x00B3) return 1;
    }
    if (m == 6 && !_wcsnicmp(s, L"CONIN$", 6)) return 1;
    if (m == 7 && !_wcsnicmp(s, L"CONOUT$", 7)) return 1;
    return 0;
}

// 把"已解码的 UTF-8 URL 路径"解析成本地完整路径；失败（穿越、非法字符、保留设备名）返回 0
static int resolve_dec(const char *dec, wchar_t *out, int cap) {
    wchar_t w[1024];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, dec, -1, w, 1024) <= 0) return 0;

    for (wchar_t *p = w; *p; p++) {                      // 统一分隔符，拦截非法字符
        if (*p == L'/') *p = L'\\';
        wchar_t c = *p;
        if (c < 32 || c == L':' || c == L'*' || c == L'?' ||
            c == L'"' || c == L'<' || c == L'>' || c == L'|') return 0;
    }

    wchar_t *seg = w;                                    // 逐段校验
    for (;;) {
        wchar_t *e = seg;
        while (*e && *e != L'\\') e++;
        int len = (int)(e - seg);
        if (len > 0) {
            int dots = 0;
            for (int i = 0; i < len; i++) if (seg[i] == L'.') dots++;
            if (dots == len && len >= 2) return 0;       // ".." "..." 等
            if (len > 1 || seg[0] != L'.') {             // 单个 "." 放行，其余检查尾部
                wchar_t last = seg[len - 1];
                if (last == L'.' || last == L' ') return 0;   // Win32 会静默去掉尾部点/空格
            }
            if (reserved_name(seg, len)) return 0;
        }
        if (!*e) break;
        seg = e + 1;
    }

    if ((int)(wcslen(g_root) + wcslen(w)) >= cap) return 0;
    wcscpy(out, g_root);
    wcscat(out, w);
    return 1;
}

// 先做百分号解码（结果写入 dec 供后续使用），再解析
static int resolve(const char *raw, wchar_t *out, int cap, char *dec, int deccap) {
    if (url_decode(raw, dec, deccap) < 0) return 0;
    return resolve_dec(dec, out, cap);
}

static void join(wchar_t *out, int cap, const wchar_t *dir, const wchar_t *name) {
    size_t l = wcslen(dir);
    _snwprintf(out, cap, L"%ls%ls%ls", dir, (l && dir[l - 1] == L'\\') ? L"" : L"\\", name);
    out[cap - 1] = 0;
}

/* ======================= 配置文件（v2.3） =======================
   所有无需重编译即可调整的参数集中在 exe 同目录的 minihttpd.ini。
   优先级：命令行参数 > 配置文件 > 内置默认值。
   - 文件缺失 / 不可读 / 为空：整体按默认值运行（界面显示"未找到"）；
   - 某项值无效（非数字 / 超范围 / 单位错误）：该项不生效（保持默认值），
     其余项不受影响；
   - 未知键忽略；[节] 名仅作分组（键名全局唯一，放错节也生效）；
   - 编码兼容 UTF-8（推荐）/ UTF-16LE、BE（记事本"Unicode"）/ ANSI；
   - 注释为整行的 ';' 或 '#'（不支持行内注释，值里的 '#' 不受影响）；
   同一键重复出现时后者生效。
   时间值默认单位为秒（可写 ms/s/m/h 后缀），大小值默认单位为 MB（可写
   B/KB/MB/GB 后缀）。各键含义与取值范围见随发行包的模板 minihttpd.ini。 */

/*CFG-BEGIN*/  /* 纯解析层：不调用任何 Win32 接口，可抽取到 Linux 原生做单元测试
   （含默认值宏：它们就是"配置缺失时的兜底基准"） */
/* ---- 内置默认值（配置文件缺失/无效时的兜底基准，即 v2.2 的各编译期常量） ---- */
#ifndef CGI_TIMEOUT_MS
#define CGI_TIMEOUT_MS   90000                   /* 单个 PHP 脚本最长执行时间 */
#endif
#define CGI_MAX_BODY     (64LL * 1024 * 1024)    /* 请求体上限 */
#define CGI_MAX_OUT      (128ULL * 1024 * 1024)  /* 脚本输出上限（整体缓冲后发送） */
#define CGI_MAX_PROCS    16                      /* CGI 模式同时运行的 php-cgi 进程数上限 */
#define FCGI_MIN_WORKERS    2                    /* 常驻 php-cgi 进程数 */
#define FCGI_MAX_WORKERS    16                   /* 进程池进程数上限 */
#define FCGI_IDLE_KILL_MS   120000               /* 超出常驻数的空闲进程回收时间 */
#define FCGI_SPAWN_TIMEOUT_MS 8000               /* 等待 php-cgi 开始监听的最长时间 */
/* MAXCONN(20000) / IDLE_RECV_MS(15s) / IDLE_SEND_MS(120s) / REQ_TOTAL_MS(20s)
   的默认值直接写在 g_cfg 初值里（见下） */

typedef struct {
    int      port;                   /* 监听端口（1..65535） */
    wchar_t  rootRaw[PCAP];          /* 配置文件里的 root 原文（空 = exe 目录，解析在 cfg_resolve_root） */
    int      lan, follow, rewrite, nophp, nobrowser, startmin, dirlist;
    int      threads;                /* IOCP 工作线程数，0 = 自动（核数+2，4..24） */
    int      maxconn;                /* 最大并发连接数 */
    int      idleRecvMs;             /* 空闲接收超时（keep-alive 空闲断开） */
    int      idleSendMs;             /* 发送 / 长响应超时 */
    int      reqTotalMs;             /* 请求头总时限（防 slowloris 慢速滴字） */
    wchar_t  index[8][28];           /* 缺省主页列表（index.php 单独优先检查，不必写入） */
    int      indexN;
    wchar_t  phpDir[PCAP];           /* PHP 目录：相对网站根目录（默认 php）或绝对路径 */
    int      cgiTimeoutMs;           /* CGI / FastCGI 单脚本超时 */
    long long cgiMaxBody;            /* 请求体上限（字节） */
    unsigned long long cgiMaxOut;    /* 脚本输出上限（字节） */
    int      cgiMaxProcs;
    int      fcgiMin, fcgiMax;       /* FastCGI 进程池下限 / 上限 */
    int      fcgiIdleMs, fcgiSpawnMs;
    /* ---- 加载状态（配置文件里不可写） ---- */
    int      loaded;                 /* 1 = 找到并解析了配置文件 */
    int      bad;                    /* 因值无效而未生效的项数 */
    int      unknown;                /* 被忽略的未知键个数 */
} Cfg;

static Cfg g_cfg = {
    8080,                                              /* port */
    L"",                                               /* rootRaw：空 = exe 目录 */
    0, 0, 0, 0, 0, 0, 1,                               /* lan follow rewrite nophp nobrowser startmin dirlist */
    0,                                                 /* threads：0 = 自动 */
    20000,                                             /* maxconn */
    15000, 120000, 20000,                              /* idleRecvMs idleSendMs reqTotalMs */
    { { L"index.html" }, { L"index.htm" }, { L"default.html" }, { L"default.htm" } },
    4,                                                 /* indexN */
    L"php",                                            /* phpDir */
    CGI_TIMEOUT_MS, CGI_MAX_BODY, CGI_MAX_OUT, CGI_MAX_PROCS,
    FCGI_MIN_WORKERS, FCGI_MAX_WORKERS, FCGI_IDLE_KILL_MS, FCGI_SPAWN_TIMEOUT_MS,
    0, 0, 0                                            /* loaded bad unknown */
};

/* 诊断日志：统一走 OutputDebugStringW，DebugView 可见，不打扰普通用户 */
static void cfg_logf(const wchar_t *fmt, ...) {
    wchar_t buf[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 255, fmt, ap);
    buf[255] = 0;
    va_end(ap);
    OutputDebugStringW(buf);
}

static wchar_t *cfg_trim(wchar_t *s) {                /* 去首尾空白（原地） */
    wchar_t *e;
    while (*s == L' ' || *s == L'\t') s++;
    e = s + wcslen(s);
    while (e > s && (e[-1] == L' ' || e[-1] == L'\t')) e--;
    *e = 0;
    return s;
}

/* 纯整数（可带正负号）；非法字符或溢出返回 0 */
static int cfg_num(const wchar_t *s, long long *out) {
    long long r = 0;
    int neg = 0, any = 0;
    if (*s == L'-') { neg = 1; s++; }
    else if (*s == L'+') s++;
    for (; *s; s++) {
        if (*s < L'0' || *s > L'9') return 0;
        if (r > (LLONG_MAX - 9) / 10) return 0;
        r = r * 10 + (*s - L'0');
        any = 1;
    }
    if (!any) return 0;
    *out = neg ? -r : r;
    return 1;
}

/* 布尔：1/0、true/false、yes/no、on/off（不分大小写）；其余一律无效 */
static int cfg_bool(const wchar_t *s, int *out) {
    static const wchar_t *const T[] = { L"1", L"true", L"yes", L"on", NULL };
    static const wchar_t *const F[] = { L"0", L"false", L"no", L"off", NULL };
    int i;
    for (i = 0; T[i]; i++) if (!_wcsicmp(s, T[i])) { *out = 1; return 1; }
    for (i = 0; F[i]; i++) if (!_wcsicmp(s, F[i])) { *out = 0; return 1; }
    return 0;
}

/* 时间：整数 + 可选单位 ms/s/m/h（默认秒），返回毫秒；负数/未知单位/溢出无效 */
static int cfg_time_ms(const wchar_t *s, long long *outMs) {
    wchar_t digits[24];
    const wchar_t *u;
    long long num, mult = 0;
    size_t dl = 0;
    if (s[0] == L'-') return 0;
    while (s[dl] >= L'0' && s[dl] <= L'9') dl++;
    if (dl == 0 || dl >= sizeof digits / sizeof digits[0]) return 0;
    memcpy(digits, s, dl * sizeof(wchar_t));
    digits[dl] = 0;
    if (!cfg_num(digits, &num)) return 0;
    u = s + dl;
    if (!*u)                                              mult = 1000;
    else if (!_wcsicmp(u, L"ms"))                          mult = 1;
    else if (!_wcsicmp(u, L"s") || !_wcsicmp(u, L"sec"))  mult = 1000;
    else if (!_wcsicmp(u, L"m") || !_wcsicmp(u, L"min"))  mult = 60000;
    else if (!_wcsicmp(u, L"h") || !_wcsicmp(u, L"hr"))   mult = 3600000;
    else return 0;
    if (num > LLONG_MAX / mult) return 0;
    *outMs = num * mult;
    return 1;
}

/* 大小：整数 + 可选单位 B/KB/MB/GB（默认 MB），返回字节 */
static int cfg_size_b(const wchar_t *s, long long *outB) {
    wchar_t digits[24];
    const wchar_t *u;
    long long num, mult = 0;
    size_t dl = 0;
    if (s[0] == L'-') return 0;
    while (s[dl] >= L'0' && s[dl] <= L'9') dl++;
    if (dl == 0 || dl >= sizeof digits / sizeof digits[0]) return 0;
    memcpy(digits, s, dl * sizeof(wchar_t));
    digits[dl] = 0;
    if (!cfg_num(digits, &num)) return 0;
    u = s + dl;
    if (!*u)                                              mult = 1024LL * 1024;
    else if (!_wcsicmp(u, L"b"))                           mult = 1;
    else if (!_wcsicmp(u, L"kb") || !_wcsicmp(u, L"k"))   mult = 1024;
    else if (!_wcsicmp(u, L"mb") || !_wcsicmp(u, L"m"))   mult = 1024LL * 1024;
    else if (!_wcsicmp(u, L"gb") || !_wcsicmp(u, L"g"))   mult = 1024LL * 1024 * 1024;
    else return 0;
    if (num > LLONG_MAX / mult) return 0;
    *outB = num * mult;
    return 1;
}

/* 路径值：去包围引号、'/' 统一为 '\'、去尾部分隔符（盘符根 C:\ 保留）；
   返回长度，0 = 空或太长或含控制字符 */
static int cfg_path(wchar_t *dst, int cap, const wchar_t *src) {
    int n, i;
    while (*src == L' ' || *src == L'\t') src++;
    n = (int)wcslen(src);
    if (n >= 2 && src[0] == L'"' && src[n - 1] == L'"') { n -= 2; src++; }
    if (n <= 0 || n >= cap) return 0;
    for (i = 0; i < n; i++) {
        wchar_t c = src[i];
        if (c == L'/') c = L'\\';
        if (c < 32) return 0;
        dst[i] = c;
    }
    dst[n] = 0;
    while (n > 0 && dst[n - 1] == L'\\' && !(n >= 2 && dst[n - 2] == L':')) dst[--n] = 0;
    return n;
}

/* 绝对路径判定：盘符开头（C:\..）或反斜杠开头（含 UNC \\server\..） */
static int path_is_abs(const wchar_t *p) {
    wchar_t c = (wchar_t)(p[0] | 32);
    return p[0] == L'\\' || ((c >= L'a' && c <= L'z') && p[1] == L':');
}

static void cfg_bad(const wchar_t *key, const wchar_t *val, const wchar_t *why) {
    g_cfg.bad++;
    cfg_logf(L"[MiniHttpd] 配置项无效：%ls=%ls（%ls），该项不生效\r\n", key, val, why);
}

/* 缺省主页列表：逗号分隔、最多 8 项；单项含路径分隔符/非法字符/以'.'开头/
   超长则跳过该项；至少一项可用才应用 */
static void cfg_set_index(const wchar_t *v) {
    wchar_t one[32];
    int n = 0, skipped = 0, i;
    const wchar_t *p = v;
    while (p && *p) {
        const wchar_t *e = wcschr(p, L',');
        size_t l = e ? (size_t)(e - p) : wcslen(p);
        int good = (l > 0 && l < sizeof one / sizeof one[0]);
        if (good) {
            wchar_t *t;
            size_t k;
            memcpy(one, p, l * sizeof(wchar_t));
            one[l] = 0;
            t = cfg_trim(one);
            if (t != one) memmove(one, t, (wcslen(t) + 1) * sizeof(wchar_t));
            l = wcslen(one);
            good = l > 0 && one[0] != L'.' &&
                   l < sizeof g_cfg.index[0] / sizeof(wchar_t);
            for (k = 0; good && k < l; k++) {
                wchar_t c = one[k];
                if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
                    c == L'"' || c == L'<' || c == L'>' || c == L'|' || c < 32)
                    good = 0;
            }
        }
        if (good) {
            int dup = 0;
            for (i = 0; i < n; i++)
                if (!_wcsicmp(g_cfg.index[i], one)) { dup = 1; break; }
            if (dup) { /* 重复项：静默忽略 */ }
            else if (n < 8) { wcscpy(g_cfg.index[n], one); n++; }
            else skipped++;
        } else if (l > 0)
            skipped++;
        p = e ? e + 1 : NULL;
    }
    if (n > 0) {
        g_cfg.indexN = n;
        if (skipped)
            cfg_bad(L"index", v, L"某些项被跳过（含分隔符/以.开头/超长/超过 8 项）");
    } else {
        cfg_bad(L"index", v, L"没有可用项（逗号分隔，如 index.html,index.htm）");
    }
}

/* 键值分发：每项严格校验，无效仅该项不生效并计数 */
static void cfg_apply(const wchar_t *key, const wchar_t *val) {
    long long n, ms, by;
    int b;
    if (!*key) return;
    if      (!_wcsicmp(key, L"port")) {
        if (cfg_num(val, &n) && n >= 1 && n <= 65535) g_cfg.port = (int)n;
        else cfg_bad(key, val, L"须为 1..65535 的整数");
    }
    else if (!_wcsicmp(key, L"root")) {
        if (!cfg_path(g_cfg.rootRaw, PCAP, val)) cfg_bad(key, val, L"路径为空或超长");
    }
    else if (!_wcsicmp(key, L"lan")) {
        if (cfg_bool(val, &b)) g_cfg.lan = b; else cfg_bad(key, val, L"须为 1/0、true/false、yes/no、on/off");
    }
    else if (!_wcsicmp(key, L"follow")) {
        if (cfg_bool(val, &b)) g_cfg.follow = b; else cfg_bad(key, val, L"须为 1/0、true/false、yes/no、on/off");
    }
    else if (!_wcsicmp(key, L"rewrite")) {
        if (cfg_bool(val, &b)) g_cfg.rewrite = b; else cfg_bad(key, val, L"须为 1/0、true/false、yes/no、on/off");
    }
    else if (!_wcsicmp(key, L"nophp")) {
        if (cfg_bool(val, &b)) g_cfg.nophp = b; else cfg_bad(key, val, L"须为 1/0、true/false、yes/no、on/off");
    }
    else if (!_wcsicmp(key, L"nobrowser")) {
        if (cfg_bool(val, &b)) g_cfg.nobrowser = b; else cfg_bad(key, val, L"须为 1/0、true/false、yes/no、on/off");
    }
    else if (!_wcsicmp(key, L"start_min")) {
        if (cfg_bool(val, &b)) g_cfg.startmin = b; else cfg_bad(key, val, L"须为 1/0、true/false、yes/no、on/off");
    }
    else if (!_wcsicmp(key, L"dirlist")) {
        if (cfg_bool(val, &b)) g_cfg.dirlist = b; else cfg_bad(key, val, L"须为 1/0、true/false、yes/no、on/off");
    }
    else if (!_wcsicmp(key, L"threads")) {
        if (cfg_num(val, &n) && n >= 0 && n <= 256) g_cfg.threads = (int)n;
        else cfg_bad(key, val, L"须为 0..256 的整数（0=自动）");
    }
    else if (!_wcsicmp(key, L"maxconn")) {
        if (cfg_num(val, &n) && n >= 16 && n <= 1000000) g_cfg.maxconn = (int)n;
        else cfg_bad(key, val, L"须为 16..1000000 的整数");
    }
    else if (!_wcsicmp(key, L"idle_timeout")) {
        if (cfg_time_ms(val, &ms) && ms >= 1000 && ms <= 3600000) g_cfg.idleRecvMs = (int)ms;
        else cfg_bad(key, val, L"须为 1 秒..1 小时（可带 ms/s/m/h）");
    }
    else if (!_wcsicmp(key, L"send_timeout")) {
        if (cfg_time_ms(val, &ms) && ms >= 1000 && ms <= 86400000) g_cfg.idleSendMs = (int)ms;
        else cfg_bad(key, val, L"须为 1 秒..24 小时（可带 ms/s/m/h）");
    }
    else if (!_wcsicmp(key, L"header_timeout")) {
        if (cfg_time_ms(val, &ms) && ms >= 1000 && ms <= 600000) g_cfg.reqTotalMs = (int)ms;
        else cfg_bad(key, val, L"须为 1 秒..10 分钟（可带 ms/s/m/h）");
    }
    else if (!_wcsicmp(key, L"index")) cfg_set_index(val);
    else if (!_wcsicmp(key, L"php_dir")) {
        if (!cfg_path(g_cfg.phpDir, PCAP, val)) cfg_bad(key, val, L"路径为空或超长");
    }
    else if (!_wcsicmp(key, L"php_timeout")) {
        if (cfg_time_ms(val, &ms) && ms >= 1000 && ms <= 3600000) g_cfg.cgiTimeoutMs = (int)ms;
        else cfg_bad(key, val, L"须为 1 秒..1 小时（可带 ms/s/m/h）");
    }
    else if (!_wcsicmp(key, L"php_max_body")) {
        if (cfg_size_b(val, &by) && by >= 1024 * 1024 && by <= 1024LL * 1024 * 1024)
            g_cfg.cgiMaxBody = by;
        else cfg_bad(key, val, L"须为 1MB..1GB（可带 B/KB/MB/GB，默认 MB）");
    }
    else if (!_wcsicmp(key, L"php_max_out")) {
        if (cfg_size_b(val, &by) && by >= 1024 * 1024 && by <= 2LL * 1024 * 1024 * 1024)
            g_cfg.cgiMaxOut = (unsigned long long)by;
        else cfg_bad(key, val, L"须为 1MB..2GB（可带 B/KB/MB/GB，默认 MB）");
    }
    else if (!_wcsicmp(key, L"php_max_procs")) {
        if (cfg_num(val, &n) && n >= 1 && n <= 64) g_cfg.cgiMaxProcs = (int)n;
        else cfg_bad(key, val, L"须为 1..64 的整数");
    }
    else if (!_wcsicmp(key, L"fcgi_min_workers")) {
        if (cfg_num(val, &n) && n >= 1 && n <= 64) g_cfg.fcgiMin = (int)n;
        else cfg_bad(key, val, L"须为 1..64 的整数");
    }
    else if (!_wcsicmp(key, L"fcgi_max_workers")) {
        if (cfg_num(val, &n) && n >= 1 && n <= 64) g_cfg.fcgiMax = (int)n;
        else cfg_bad(key, val, L"须为 1..64 的整数");
    }
    else if (!_wcsicmp(key, L"fcgi_idle_kill")) {
        if (cfg_time_ms(val, &ms) && ms >= 10000 && ms <= 3600000) g_cfg.fcgiIdleMs = (int)ms;
        else cfg_bad(key, val, L"须为 10 秒..1 小时（可带 ms/s/m/h）");
    }
    else if (!_wcsicmp(key, L"fcgi_spawn_timeout")) {
        if (cfg_time_ms(val, &ms) && ms >= 1000 && ms <= 120000) g_cfg.fcgiSpawnMs = (int)ms;
        else cfg_bad(key, val, L"须为 1 秒..2 分钟（可带 ms/s/m/h）");
    }
    else {
        g_cfg.unknown++;
        cfg_logf(L"[MiniHttpd] 配置文件出现未知键：%ls（已忽略）\r\n", key);
    }
}

/* 逐行应用（text 会被原地改动）；末尾做跨项交叉校验 */
static void cfg_parse_lines(wchar_t *text) {
    wchar_t *line = text;
    while (line && *line) {
        wchar_t *eol = wcschr(line, L'\n');
        wchar_t *s, *eq, *key, *val;
        if (eol) *eol = 0;
        {
            size_t l = wcslen(line);
            if (l && line[l - 1] == L'\r') line[l - 1] = 0;
        }
        s = cfg_trim(line);
        if (*s == 0 || *s == L';' || *s == L'#') {
            /* 空行 / 注释 */
        } else if (*s == L'[') {
            /* [节] 名仅作分组：键名全局唯一，放错节也生效 */
        } else if ((eq = wcschr(s, L'=')) != NULL) {
            *eq = 0;
            key = cfg_trim(s);
            val = cfg_trim(eq + 1);
            if (*key)
                cfg_apply(key, val);
            else {
                g_cfg.bad++;
                cfg_logf(L"[MiniHttpd] 配置行缺少键名：%ls\r\n", s);
            }
        } else {
            g_cfg.bad++;
            cfg_logf(L"[MiniHttpd] 配置行缺少 = 号：%ls\r\n", s);
        }
        line = eol ? eol + 1 : NULL;
    }
    if (g_cfg.fcgiMin > g_cfg.fcgiMax) {         /* 下限大于上限：自动对调而非拒绝 */
        int t = g_cfg.fcgiMin;
        g_cfg.fcgiMin = g_cfg.fcgiMax;
        g_cfg.fcgiMax = t;
        cfg_logf(L"[MiniHttpd] fcgi_min_workers > fcgi_max_workers，已自动对调\r\n");
    }
}
/*CFG-END*/

/* ---- 文件读取与编码识别（Windows 部分，不参与原生单测） ---- */
static wchar_t g_exeDir[PCAP];        /* exe 所在目录（长路径）：配置文件与默认网站根目录的基准 */

#define CFG_FILE_MAX  65536           /* 配置文件最大读取字节数（再大的 ini 也用不到） */

static void cfg_load(void) {
    wchar_t path[PCAP];
    unsigned char *raw;
    wchar_t *wbuf = NULL;
    FILE *f;
    size_t len;
    int wl = 0, i;

    _snwprintf(path, PCAP - 1, L"%ls\\\\minihttpd.ini", g_exeDir);
    path[PCAP - 1] = 0;
    f = _wfopen(path, L"rb");
    if (!f) {
        cfg_logf(L"[MiniHttpd] 未找到配置文件 %ls，按内置默认配置运行\r\n", path);
        return;                                   /* 文件不存在：全部默认，照常运行 */
    }
    raw = (unsigned char *)malloc(CFG_FILE_MAX + 1);
    if (!raw) { fclose(f); return; }              /* 内存不足：按默认配置继续 */
    len = fread(raw, 1, CFG_FILE_MAX, f);
    fclose(f);
    if (len == CFG_FILE_MAX)
        cfg_logf(L"[MiniHttpd] 配置文件超过 %d 字节，只读取前 %d 字节\r\n",
                 CFG_FILE_MAX, CFG_FILE_MAX);
    g_cfg.loaded = 1;

    if (len >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {           /* UTF-16LE（记事本"Unicode"） */
        wl = (int)(len / 2) - 1;                                  /* 跳过 BOM */
        if (wl > 0) {
            wbuf = (wchar_t *)malloc((size_t)(wl + 1) * sizeof(wchar_t));
            if (wbuf)
                for (i = 0; i < wl; i++)
                    wbuf[i] = (wchar_t)(raw[2 + i * 2] | (raw[3 + i * 2] << 8));
        }
    } else if (len >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {    /* UTF-16BE */
        wl = (int)(len / 2) - 1;
        if (wl > 0) {
            wbuf = (wchar_t *)malloc((size_t)(wl + 1) * sizeof(wchar_t));
            if (wbuf)
                for (i = 0; i < wl; i++)
                    wbuf[i] = (wchar_t)((raw[2 + i * 2] << 8) | raw[3 + i * 2]);
        }
    } else {
        const unsigned char *p = raw;
        size_t pl = len;
        int cp = CP_UTF8;
        if (len >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
            p += 3;                               /* UTF-8 BOM */
            pl -= 3;
        } else if (pl == 0) {
            /* 空文件：全部默认 */
        } else if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       (const char *)raw, (int)len, NULL, 0) == 0)
            cp = CP_ACP;                          /* 不是合法 UTF-8 → 按 ANSI（老记事本默认） */
        if (pl) {
            wl = MultiByteToWideChar(cp, 0, (const char *)p, (int)pl, NULL, 0);
            if (wl > 0) {
                wbuf = (wchar_t *)malloc((size_t)(wl + 1) * sizeof(wchar_t));
                if (wbuf) {
                    MultiByteToWideChar(cp, 0, (const char *)p, (int)pl, wbuf, wl);
                    wbuf[wl] = 0;
                }
            }
        }
    }
    free(raw);
    if (wbuf) {
        wbuf[wl] = 0;
        cfg_parse_lines(wbuf);
        free(wbuf);
    }
    cfg_logf(L"[MiniHttpd] 配置加载完成：%ls（无效项 %d，忽略未知键 %d）\r\n",
             path, g_cfg.bad, g_cfg.unknown);
}

/* 应用配置的网站根目录：空或 "." → 保持 exe 目录；相对路径基于 exe 目录；
   绝对路径原样。路径不存在 / 不是目录 → 回退 exe 目录（计入无效项）。
   成功后规范化为完整 + 长路径（与 v2.2 的短名目录修复保持同一不变量）。 */
static void cfg_resolve_root(void) {
    wchar_t cand[PCAP], full[PCAP], lng[PCAP], *r = g_cfg.rootRaw;
    DWORD a, fn;
    if (!r[0] || !_wcsicmp(r, L".")) return;
    if (path_is_abs(r))
        _snwprintf(cand, PCAP - 1, L"%ls", r);
    else
        _snwprintf(cand, PCAP - 1, L"%ls\\\\%ls", g_exeDir, r);
    cand[PCAP - 1] = 0;
    fn = GetFullPathNameW(cand, PCAP, full, NULL);
    if (fn == 0 || fn >= PCAP) {
        g_cfg.bad++;
        cfg_logf(L"[MiniHttpd] 配置的根目录无法解析：%ls（使用 exe 目录）\r\n", cand);
        return;
    }
    {
        DWORD ln = GetLongPathNameW(full, lng, PCAP);
        if (ln > 0 && ln < PCAP) wcscpy(full, lng);
    }
    a = GetFileAttributesW(full);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        g_cfg.bad++;
        cfg_logf(L"[MiniHttpd] 配置的根目录不存在：%ls（使用 exe 目录）\r\n", full);
        return;
    }
    wcscpy(g_root, full);
}

/* 界面"配置"状态行的文字（启动时构建一次） */
static void cfg_summary_msg(wchar_t *out, int cap) {
    if (!g_cfg.loaded)
        _snwprintf(out, cap - 1, L"未找到 minihttpd.ini（默认配置）");
    else if (g_cfg.bad > 0)
        _snwprintf(out, cap - 1, L"minihttpd.ini（%d 项无效已用默认）", g_cfg.bad);
    else if (g_cfg.unknown > 0)
        _snwprintf(out, cap - 1, L"minihttpd.ini（忽略 %d 个未知键）", g_cfg.unknown);
    else
        _snwprintf(out, cap - 1, L"minihttpd.ini 已加载");
    out[cap - 1] = 0;
}

/* ---------------- 请求头一次解析（v2.2） ---------------- */
/* 旧版对同一请求反复 strstr 线性扫描（Connection/Range/INM/CL/TE/Expect 各一遍，
   8KB 头要扫几十 KB）；现在 process_buffer 阶段解析一次，后续查找只做指针比较。
   条目直接指向 reqbuf，全程有界（不依赖 NUL 结尾），不拷贝。 */
typedef struct { const char *name; size_t nlen; const char *val; size_t vlen; } HEnt;
enum { HDR_SLOTS = 128 };
typedef struct { HEnt e[HDR_SLOTS]; int n; } Hdrs;

/* 解析 [p, end) 内的头部行（跳过无冒号的行，如请求行）；值只去前导空白。
   返回 0 = 头部数超限（调用方应回 431，避免后续查找漏头造成请求走私）。 */
static int parse_headers(Hdrs *h, const char *p, const char *end) {
    h->n = 0;
    while (p && p < end) {
        const char *eol = (const char *)memchr(p, '\n', (size_t)(end - p));
        const char *lineEnd = eol ? eol : end;
        const char *colon;
        if (lineEnd > p && lineEnd[-1] == '\r') lineEnd--;
        colon = (const char *)memchr(p, ':', (size_t)(lineEnd - p));
        if (colon && colon > p && h->n < HDR_SLOTS) {
            const char *v = colon + 1;
            while (v < lineEnd && (*v == ' ' || *v == '\t')) v++;
            h->e[h->n].name = p;
            h->e[h->n].nlen = (size_t)(colon - p);
            h->e[h->n].val = v;
            h->e[h->n].vlen = (size_t)(lineEnd - v);
            h->n++;
        } else if (colon && colon > p) {
            return 0;                       /* 超过 128 个头：拒绝 */
        }
        p = eol ? eol + 1 : NULL;
    }
    return 1;
}

/* 大小写不敏感取头（首个匹配；返回 NULL 表示不存在），值不拷贝 */
static const HEnt *hdrs_get(const Hdrs *h, const char *name) {
    size_t nl = strlen(name);
    int i;
    for (i = 0; i < h->n; i++)
        if (h->e[i].nlen == nl && !_strnicmp(h->e[i].name, name, nl))
            return &h->e[i];
    return NULL;
}

/* 拷出头值（NUL 结尾，截断到 cap）；返回 0 = 不存在 */
static int hdrs_str(const Hdrs *h, const char *name, char *out, int cap) {
    const HEnt *e = hdrs_get(h, name);
    size_t i;
    if (!e) { out[0] = 0; return 0; }
    for (i = 0; i < e->vlen && i < (size_t)cap - 1; i++) out[i] = e->val[i];
    out[i] = 0;
    return 1;
}

/* ======================= IOCP 引擎 ======================= */
/* MAXCONN / IDLE_RECV_MS / IDLE_SEND_MS / REQ_TOTAL_MS 已移至配置模块的 g_cfg
   （maxconn / idleRecvMs / idleSendMs / reqTotalMs，默认值与 v2.2 相同，
   可在 minihttpd.ini 里用 maxconn / idle_timeout / send_timeout / header_timeout 调整） */
#define RECV_CAP              8192
#define ACCEPTS_PER_LISTENER  32
#define TF_CHUNK              0x800000LL        /* TransmitFile 每块 8MB：每块完成都会刷新空闲计时，慢速下载大文件不会被误杀 */
#define SEND_CHUNK            (1 << 20)         /* WSASend 每次最多 1MB：避免一次锁定过多内存，也让超时按进度刷新 */

#ifndef FILE_NAME_NORMALIZED
#define FILE_NAME_NORMALIZED 0x0
#endif
#ifndef VOLUME_NAME_DOS
#define VOLUME_NAME_DOS 0x0
#endif

enum { OP_ACCEPT = 1, OP_RECV, OP_SEND, OP_FILE, OP_BODY, OP_FSEND, OP_FRECV };
enum { ST_ACCEPT, ST_RECV, ST_SEND, ST_FILE, ST_BODY, ST_CGI };

struct Conn;
struct CgiJob;
struct FcgiUp;
typedef struct { OVERLAPPED ov; int type; struct Conn *c; } Op;   /* ov 必须是第一个成员 */

typedef struct Conn {
    struct Conn *prev, *next;          /* 活动连接链表（供超时扫描） */
    SOCKET s;
    int lidx, state, linked, keep, head, have, hend;
    DWORD last;                        /* 最近一次活动的 tick */
    DWORD rstart;                      /* 当前请求开始读取的时刻（请求头总时限用） */
    Op op;                             /* 同一时刻只有一个挂起操作，一个 Op 足够 */
    char *sbuf; int slen, sofs, scap;  /* 待发送的堆缓冲区：响应头 [+ 正文]（scap=容量，供回池） */
    HANDLE file; long long fstart, fleft; DWORD chunk; int headSent;
    TRANSMIT_FILE_BUFFERS tfb;
    char range[128], inm[256], inr[192];
    FILETIME imsFt; int imsOk;         /* If-Modified-Since 解析结果 */
    char *body; long long blen, bgot; size_t bcap;   /* PHP 请求体（bcap=按需增长的容量） */
    struct CgiJob *cgi;                 /* 等待请求体期间暂存的 CGI 任务 */
    struct FcgiReq *fr;                 /* FastCGI 请求（等待/执行期间有效） */
    struct FcgiUp *up;                  /* 正在为它执行的上游（与 u->c 互为反向指针） */
    char accbuf[2 * (sizeof(struct sockaddr_in6) + 16)];
    char reqbuf[RECV_CAP];
} Conn;

static HANDLE g_iocp;
static CRITICAL_SECTION g_lock;
static Conn *g_head;
static volatile LONG g_nconn, g_reqs, g_missing[2];
static int g_follow = 0;               /* 1 = 允许跟随指向根目录之外的链接 */
static wchar_t g_rootReal[PCAP];
static int g_rootRealLen = 0;

static void process_buffer(Conn *c);
static void response_done(Conn *c);
static void free_job(struct CgiJob *j);
static void dispatch_cgi(Conn *c);
static void post_body(Conn *c);
static void fcgi_free_req(struct FcgiReq *fr);   /* FastCGI：请求参数释放 */

/* FastCGI 前置类型/声明：close_conn 需要在定义点之前取消执行中的请求 */
typedef struct FcgiItem { struct FcgiItem *next; Conn *c; Buf resp; } FcgiItem;
typedef struct FcgiAct {
    FcgiItem *head, *tail;
    struct FcgiUp *freeup;      /* 已无挂起 I/O，可释放的上游连接 */
    struct FcgiWorker *freew;   /* 待 TerminateProcess + 释放的 worker 链（借用 ->next 串联） */
    Conn *oomc;                 /* act 分配失败的客户端（只能在锁外关闭） */
    int want_spawn;
} FcgiAct;
static CRITICAL_SECTION g_fcgiLock;    /* 仅在 fcgi_init() 初始化；c->up/c->fr 非空 ⇒ 必已初始化 */
static void fcgi_cancel_locked(struct FcgiUp *u, FcgiAct *a);
static void fcgi_run_act(FcgiAct *a);
static void fcgi_remove_wait_locked(struct FcgiReq *fr);

/* ---------------- 发送缓冲池（无锁 SLIST，v2.2） ---------------- */
/* 小响应（错误页/文件头/PHP 输出）的 sbuf 免除每请求 malloc/free；
   只回收 4KB..64KB 的块，更大的直接 free（防内存滞留）；
   节点数封顶，突发后的闲置占用有上界。 */
#define BP_MAX_CAP   (64 * 1024)
#define BP_MAX_NODES 256
typedef struct _BPoolNode {
    SLIST_ENTRY next;               /* winnt.h：struct { struct _SLIST_ENTRY *Next; } */
    char *p;
    size_t cap;
} BPoolNode;
static SLIST_HEADER g_bp;
static volatile LONG g_bpN;

static Buf bp_get(void) {
    BPoolNode *n = (BPoolNode *)InterlockedPopEntrySList(&g_bp);
    Buf b;
    if (n) {
        InterlockedDecrement(&g_bpN);
        b.p = n->p; b.cap = n->cap; b.n = 0;
        free(n);
        return b;
    }
    b.p = NULL; b.n = 0; b.cap = 0;
    return b;
}

static void bp_put(char *p, size_t cap) {
    BPoolNode *n;
    if (!p) return;
    if (cap < 4096 || cap > BP_MAX_CAP || g_bpN >= BP_MAX_NODES) { free(p); return; }
    n = (BPoolNode *)malloc(sizeof *n);
    if (!n) { free(p); return; }
    n->p = p; n->cap = cap;
    InterlockedPushEntrySList(&g_bp, &n->next);
    InterlockedIncrement(&g_bpN);
}

/* ---------------- 连接生命周期 ---------------- */
static void link_conn(Conn *c) {
    EnterCriticalSection(&g_lock);
    c->prev = NULL;
    c->next = g_head;
    if (g_head) g_head->prev = c;
    g_head = c;
    c->linked = 1;
    LeaveCriticalSection(&g_lock);
    InterlockedIncrement(&g_nconn);
}

static void close_conn(Conn *c) {
    if (c->linked) {
        EnterCriticalSection(&g_lock);
        if (c->prev) c->prev->next = c->next; else g_head = c->next;
        if (c->next) c->next->prev = c->prev;
        LeaveCriticalSection(&g_lock);
        InterlockedDecrement(&g_nconn);
    }
    /* FastCGI 执行期间客户端断开（当前版本无自然触发路径，此为加固：
       未来加入主动断开检测等新路径时不会变成 u->c 悬空） */
    if (c->up) {
        FcgiAct a;
        memset(&a, 0, sizeof a);
        EnterCriticalSection(&g_fcgiLock);
        fcgi_cancel_locked(c->up, &a);
        c->up = NULL;
        LeaveCriticalSection(&g_fcgiLock);
        fcgi_run_act(&a);
    }
    if (c->fr) {                       /* 还在排队：先摘出队列再释放 */
        EnterCriticalSection(&g_fcgiLock);
        fcgi_remove_wait_locked(c->fr);
        LeaveCriticalSection(&g_fcgiLock);
        fcgi_free_req(c->fr);
    }
    if (c->file != INVALID_HANDLE_VALUE) CloseHandle(c->file);
    bp_put(c->sbuf, (size_t)c->scap);
    free(c->body);
    if (c->cgi) free_job(c->cgi);
    if (c->s != INVALID_SOCKET) {
        if (c->linked) shutdown(c->s, SD_SEND);
        closesocket(c->s);
    }
    free(c);
}

/* ---------------- 投递异步操作（投递后绝不再碰 c） ---------------- */
static void post_recv(Conn *c) {
    WSABUF b;
    DWORD n = 0, fl = 0;
    b.buf = c->reqbuf + c->have;
    b.len = (ULONG)(RECV_CAP - 1 - c->have);
    c->state = ST_RECV;
    c->last = GetTickCount();
    memset(&c->op.ov, 0, sizeof c->op.ov);
    c->op.type = OP_RECV;
    if (WSARecv(c->s, &b, 1, &n, &fl, &c->op.ov, NULL) == SOCKET_ERROR &&
        WSAGetLastError() != WSA_IO_PENDING) close_conn(c);
}

static void post_send(Conn *c) {
    WSABUF b;
    DWORD n = 0;
    b.buf = c->sbuf + c->sofs;
    b.len = (ULONG)(c->slen - c->sofs > SEND_CHUNK ? SEND_CHUNK : c->slen - c->sofs);
    c->state = ST_SEND;
    c->last = GetTickCount();
    memset(&c->op.ov, 0, sizeof c->op.ov);
    c->op.type = OP_SEND;
    if (WSASend(c->s, &b, 1, &n, 0, &c->op.ov, NULL) == SOCKET_ERROR &&
        WSAGetLastError() != WSA_IO_PENDING) close_conn(c);
}

static void post_file(Conn *c) {
    LARGE_INTEGER li;
    c->chunk = (DWORD)(c->fleft > TF_CHUNK ? TF_CHUNK : c->fleft);
    c->state = ST_FILE;
    c->last = GetTickCount();
    li.QuadPart = c->fstart;
    SetFilePointerEx(c->file, li, NULL, FILE_BEGIN);          /* 与 OVERLAPPED 偏移双保险 */
    memset(&c->op.ov, 0, sizeof c->op.ov);
    c->op.ov.Offset = (DWORD)(c->fstart & 0xFFFFFFFFLL);
    c->op.ov.OffsetHigh = (DWORD)(c->fstart >> 32);
    c->op.type = OP_FILE;
    TRANSMIT_FILE_BUFFERS *tb = NULL;
    if (!c->headSent && c->sbuf) {                            /* 响应头随首块一起发出 */
        memset(&c->tfb, 0, sizeof c->tfb);
        c->tfb.Head = c->sbuf;
        c->tfb.HeadLength = (DWORD)c->slen;
        tb = &c->tfb;
        c->headSent = 1;
    }
    if (!TransmitFile(c->s, c->file, c->chunk, 0, &c->op.ov, tb, 0) &&
        WSAGetLastError() != WSA_IO_PENDING) close_conn(c);
}

/* ---------------- 响应构造 ---------------- */
static const char *status_text(int c) {
    switch (c) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 504: return "Gateway Timeout";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 416: return "Range Not Satisfiable";
    case 431: return "Request Header Fields Too Large";
    default:  return "Error";
    }
}

/* ---------------- HTTP 日期（v2.2：Date / Last-Modified / 条件请求） ---------------- */
static int fmt_http_date(const SYSTEMTIME *st, const char *prefix, char *out, size_t cap) {
    static const char *const WD[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *const MO[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    if (st->wMonth < 1 || st->wMonth > 12 || st->wDayOfWeek > 6) return 0;
    return snprintf(out, cap, "%s%s, %02d %s %04d %02d:%02d:%02d GMT\r\n",
                    prefix, WD[st->wDayOfWeek], st->wDay, MO[st->wMonth - 1], st->wYear,
                    st->wHour, st->wMinute, st->wSecond);
}

/* "Date: ..." 响应头（每次现算，约两百纳秒，远低于一次系统调用） */
static int date_line(char *out, size_t cap) {
    SYSTEMTIME st;
    GetSystemTime(&st);
    return fmt_http_date(&st, "Date: ", out, cap);
}

/* "Last-Modified: ..."（FILETIME 本身就是 UTC，直接转即可） */
static int filetime_line(const FILETIME *ft, char *out, size_t cap) {
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(ft, &st)) return 0;
    return fmt_http_date(&st, "Last-Modified: ", out, cap);
}

/* 解析 HTTP 日期：IMF-fixdate "Sun, 06 Nov 1994 08:49:37 GMT"（浏览器实际只发这种，
   同时宽容 asctime "Sun Nov  6 08:49:37 1994"）；返回 0 = 解析失败 */
static int parse_http_date(const char *s, SYSTEMTIME *st) {
    static const char *const MO[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    char mon[8] = "";
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0, i, monIdx = 0;
    if (!s) return 0;
    for (i = 0; s[i]; i++)
        if ((unsigned char)s[i] < 32 || (unsigned char)s[i] > 126) return 0;
    if (sscanf(s, "%*3[A-Za-z], %d %3[A-Za-z] %d %d:%d:%d",
               &day, mon, &year, &hh, &mm, &ss) != 6 &&
        sscanf(s, "%*3[A-Za-z] %3[A-Za-z] %d %d:%d:%d %d",
               mon, &day, &hh, &mm, &ss, &year) != 6)
        return 0;
    for (i = 0; i < 12; i++) if (!_strnicmp(mon, MO[i], 3)) { monIdx = i + 1; break; }
    if (year < 1601 || year > 9999 || monIdx == 0 || day < 1 || day > 31) return 0;
    if (hh > 23 || mm > 59 || ss > 60) return 0;
    memset(st, 0, sizeof *st);
    st->wYear = (WORD)year; st->wMonth = (WORD)monIdx; st->wDay = (WORD)day;
    st->wHour = (WORD)hh; st->wMinute = (WORD)mm; st->wSecond = (WORD)ss;
    return 1;
}

static ULONGLONG ft_key(const FILETIME *ft) {          /* 截到秒的比较键 */
    return ((((ULONGLONG)ft->dwHighDateTime) << 32) | ft->dwLowDateTime) / 10000000ULL;
}
static int filetime_le(const FILETIME *a, const FILETIME *b) { return ft_key(a) <= ft_key(b); }

/* If-None-Match 弱匹配：'*' 匹配任意存在；列表项去 W/ 前缀后与 etag（含引号）比较 */
static int etag_match(const char *list, const char *etag) {
    const char *p = list;
    size_t el = strlen(etag);
    while (*p) {
        const char *t, *te;
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        t = p;
        while (*p && *p != ',') p++;
        te = p;
        while (te > t && (te[-1] == ' ' || te[-1] == '\t')) te--;
        if (*t == '*') return 1;
        if ((t[0] == 'W' || t[0] == 'w') && t[1] == '/') t += 2;
        if ((size_t)(te - t) == el && !strncmp(t, etag, el)) return 1;
    }
    return 0;
}

/* If-Range 匹配：值为 ETag（去 W/ 的宽松强比较）或 HTTP-date（修改时间精确到秒相等）；
   解析不了当不匹配处理 → 忽略 Range 回 200 全量（安全侧，防旧偏移续传损坏文件） */
static int if_range_match(const Conn *c, const char *etag, const FILETIME *ft) {
    const char *t = c->inr;
    if (!t[0]) return 1;                       /* 无 If-Range：Range 直接生效 */
    if (t[0] == '"' || ((t[0] == 'W' || t[0] == 'w') && t[1] == '/')) {
        if (t[0] != '"') t += 2;
        return strlen(t) == strlen(etag) && !strcmp(t, etag);
    }
    {
        SYSTEMTIME st;
        FILETIME f2;
        if (parse_http_date(t, &st) && SystemTimeToFileTime(&st, &f2))
            return ft_key(ft) == ft_key(&f2);
    }
    return 0;
}

/* Connection 头；keep-alive 时通告的空闲超时与实际执行的 idle_timeout 保持一致 */
static const char *keep_line(Conn *c, char *buf, size_t cap) {
    if (c->keep)
        snprintf(buf, cap, "Connection: keep-alive\r\nKeep-Alive: timeout=%d\r\n",
                 g_cfg.idleRecvMs / 1000);
    else
        snprintf(buf, cap, "Connection: close\r\n");
    return buf;
}

static void make_head(Buf *b, Conn *c, int code, const char *type, long long len, const char *extra) {
    char h[2900], d[64], ka[64];
    int n;
    date_line(d, sizeof d);
    n = snprintf(h, sizeof h,
        "HTTP/1.1 %d %s\r\nServer: " SERVER_TOKEN "\r\n%sContent-Type: %s\r\n"
        "Content-Length: %lld\r\nAccept-Ranges: bytes\r\nCache-Control: no-cache\r\n%s%s\r\n",
        code, status_text(code), d, type, len, keep_line(c, ka, sizeof ka),
        extra ? extra : "");
    if (n > 0 && n < (int)sizeof h) b_add(b, h, (size_t)n);
}

/* 304 精简头（RFC 7232 §4.1）：不带 Content-Length / Accept-Ranges，只带验证器类头 */
static void make_head_304(Buf *b, Conn *c, const char *extra) {
    char h[1400], d[64], ka[64];
    int n;
    date_line(d, sizeof d);
    n = snprintf(h, sizeof h,
        "HTTP/1.1 304 Not Modified\r\nServer: " SERVER_TOKEN "\r\n%s%s%s\r\n",
        d, extra ? extra : "", keep_line(c, ka, sizeof ka));
    if (n > 0 && n < (int)sizeof h) b_add(b, h, (size_t)n);
}

/* 整个响应（头+正文）放进一块堆内存，一次异步发送；调用后不得再使用 c */
static void reply_buf(Conn *c, int code, const char *type, const char *body,
                      size_t len, const char *extra) {
    Buf b = bp_get();
    make_head(&b, c, code, type, (long long)len, extra);
    if (!c->head && len) b_add(&b, body, len);
    if (!b.p) { close_conn(c); return; }
    c->sbuf = b.p;
    c->slen = (int)b.n;
    c->scap = (int)b.cap;
    c->sofs = 0;
    post_send(c);
}

/* 304：无正文，精简头（v2.2） */
static void reply_304(Conn *c, const char *extra) {
    Buf b = bp_get();
    make_head_304(&b, c, extra);
    if (!b.p) { close_conn(c); return; }
    c->sbuf = b.p;
    c->slen = (int)b.n;
    c->scap = (int)b.cap;
    c->sofs = 0;
    post_send(c);
}

static void reply_error(Conn *c, int code, const char *extra) {
    char b[512];
    int n = snprintf(b, sizeof b,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>%d %s</title></head>"
        "<body style=\"font-family:sans-serif;text-align:center;margin-top:15vh\">"
        "<h1>%d %s</h1><hr><small>MiniHttpd</small></body></html>",
        code, status_text(code), code, status_text(code));
    reply_buf(c, code, "text/html; charset=utf-8", b, (size_t)n, extra);
}

/* ---------------- 真实路径校验（防 junction / symlink 穿透） ---------------- */
static int real_path_inside(HANDLE h) {
    wchar_t buf[PCAP];
    if (g_follow || g_rootRealLen == 0) return 1;
    DWORD n = GetFinalPathNameByHandleW(h, buf, PCAP, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n == 0 || n >= PCAP) return 0;                        /* 取不到就按"不安全"处理 */
    if (_wcsnicmp(buf, g_rootReal, (size_t)g_rootRealLen) != 0) return 0;
    wchar_t t = buf[g_rootRealLen];
    return t == 0 || t == L'\\' || g_rootReal[g_rootRealLen - 1] == L'\\';
}

static int dir_inside(const wchar_t *p) {
    if (g_follow || g_rootRealLen == 0) return 1;
    HANDLE h = CreateFileW(p, FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = real_path_inside(h);
    CloseHandle(h);
    return ok;
}

static void init_real_root(void) {
    wchar_t p[PCAP];
    _snwprintf(p, PCAP - 2, L"%ls\\", g_root);
    p[PCAP - 1] = 0;
    HANDLE h = CreateFileW(p, FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD n = GetFinalPathNameByHandleW(h, g_rootReal, PCAP, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0 || n >= PCAP) { g_rootReal[0] = 0; return; }
    size_t l = wcslen(g_rootReal);                            /* 去尾部 '\'，盘符根 "\\?\C:\" 除外 */
    if (l > 0 && g_rootReal[l - 1] == L'\\' && !(l >= 2 && g_rootReal[l - 2] == L':'))
        g_rootReal[--l] = 0;
    g_rootRealLen = (int)l;
}

/* ---------------- 发送文件（TransmitFile + Range + 条件请求） ---------------- */
/* 返回 0 = 什么都没发，由调用者回复 403；返回 1 = 已投递（或已关闭连接），调用者必须直接返回 */
static int serve_file(Conn *c, const wchar_t *wp, int code) {
    HANDLE h = CreateFileW(wp, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!real_path_inside(h)) { CloseHandle(h); return 0; }

    BY_HANDLE_FILE_INFORMATION bi;
    if (!GetFileInformationByHandle(h, &bi)) { CloseHandle(h); return 0; }
    long long size = ((long long)bi.nFileSizeHigh << 32) | bi.nFileSizeLow;
    long long start = 0, end = size - 1;
    int part = 0, el = 0;
    char etag[96] = "", extra[512] = "";

    if (code == 200) {
        snprintf(etag, sizeof etag, "\"%llx-%llx\"", (unsigned long long)size,
                 ((unsigned long long)bi.ftLastWriteTime.dwHighDateTime << 32) |
                  bi.ftLastWriteTime.dwLowDateTime);
        el = snprintf(extra, sizeof extra, "ETag: W/%s\r\n", etag);
        el += filetime_line(&bi.ftLastWriteTime, extra + el, sizeof extra - el);

        /* 条件请求（RFC 7232）：If-None-Match 优先于 If-Modified-Since */
        if (c->inm[0] ? etag_match(c->inm, etag)
                      : (c->imsOk && filetime_le(&bi.ftLastWriteTime, &c->imsFt))) {
            CloseHandle(h);
            reply_304(c, extra);
            return 1;
        }

        /* Range（RFC 7233）：If-Range 不匹配则忽略 Range 回 200 全量（防旧偏移续传损坏文件）；
           非数字的区间直接忽略（回 200），bytes=-0 语法有效但不可满足 → 416 */
        if (c->range[0] && size > 0 && !strncmp(c->range, "bytes=", 6) &&
            !strchr(c->range, ',') && if_range_match(c, etag, &bi.ftLastWriteTime)) {
            const char *r = c->range + 6, *d = strchr(r, '-');
            if (d) {
                const char *q;
                int okd = 1;
                if (d == r) {                                     /* bytes=-N 后缀区间 */
                    for (q = d + 1; *q; q++)
                        if (*q < '0' || *q > '9') { okd = 0; break; }
                    if (okd && d[1]) {
                        long long n = _atoi64(d + 1);
                        if (n > 0) { start = size - n; if (start < 0) start = 0; part = 1; }
                        else { part = 1; start = size; }          /* -0：不可满足 */
                    }
                } else {                                          /* bytes=A-  或 A-B */
                    for (q = r; q < d; q++)
                        if (*q < '0' || *q > '9') { okd = 0; break; }
                    if (okd) {
                        start = _atoi64(r);
                        if (d[1] >= '0' && d[1] <= '9') end = _atoi64(d + 1);
                        part = 1;
                    }
                }
            }
            if (part) {
                if (end >= size) end = size - 1;
                if (start < 0 || start > end || start >= size) {
                    snprintf(extra, sizeof extra, "Content-Range: bytes */%lld\r\n", size);
                    CloseHandle(h);
                    reply_buf(c, 416, "text/plain", "", 0, extra);
                    return 1;
                }
                code = 206;
                snprintf(extra + el, sizeof extra - el, "Content-Range: bytes %lld-%lld/%lld\r\n",
                         start, end, size);
            }
        }
    }

    Buf hb = bp_get();
    make_head(&hb, c, code, mime_of(wp), size > 0 ? end - start + 1 : 0, extra);
    if (!hb.p) { CloseHandle(h); close_conn(c); return 1; }
    c->sbuf = hb.p;
    c->slen = (int)hb.n;
    c->scap = (int)hb.cap;
    c->sofs = 0;

    if (c->head || size == 0) {                               /* 只有头 */
        CloseHandle(h);
        post_send(c);
        return 1;
    }
    c->file = h;
    c->fstart = start;
    c->fleft = end - start + 1;
    c->headSent = 0;
    post_file(c);
    return 1;
}

static void send_404(Conn *c) {
    wchar_t p[PCAP];
    join(p, PCAP, g_root, L"404.html");
    DWORD a = GetFileAttributesW(p);
    if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY) &&
        serve_file(c, p, 404)) return;
    reply_error(c, 404, NULL);
}

/* ---------------- 目录列表（v2.2：单次遍历） ---------------- */
static void list_dir(Conn *c, const wchar_t *dir, const char *dec) {
    Buf out = {0}, d = {0}, f = {0};
    b_str(&out, "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
              "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
              "<title>Index of ");
    b_esc(&out, dec);
    b_str(&out, "</title><style>body{font:15px/1.8 system-ui,'Microsoft YaHei',sans-serif;"
              "max-width:860px;margin:2em auto;padding:0 1em}a{text-decoration:none}"
              "ul{padding:0}li{list-style:none}.s{color:#888;margin-left:1em;font-size:12px}"
              "</style></head><body><h2>Index of ");
    b_esc(&out, dec);
    b_str(&out, "</h2><ul>");
    if (strcmp(dec, "/")) b_str(&out, "<li><a href=\"../\">../</a></li>");

    wchar_t pat[PCAP];
    join(pat, PCAP, dir, L"*");
    {   /* 单次遍历：目录条目进 d、文件条目进 f，最后拼成“目录在前”
           （旧版把整个目录枚举两遍，大目录开销双倍；FindExInfoBasic 还可免取 8.3 短名） */
        WIN32_FIND_DATAW fd;
        HANDLE fl = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
        if (fl != INVALID_HANDLE_VALUE) {
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;
                int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                char name[1024];
                if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL))
                    continue;
                Buf *o = isdir ? &d : &f;
                b_str(o, "<li><a href=\"");
                b_urlenc(o, name);
                if (isdir) b_str(o, "/");
                b_str(o, "\">");
                b_esc(o, name);
                if (isdir) b_str(o, "/");
                b_str(o, "</a>");
                if (!isdir) {
                    unsigned long long fs = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                    char t[64];
                    snprintf(t, sizeof t, "<span class=\"s\">%llu KB</span>", (fs + 1023) / 1024);
                    b_str(o, t);
                }
                b_str(o, "</li>");
            } while (FindNextFileW(fl, &fd));
            FindClose(fl);
        }
    }
    if (d.p) b_add(&out, d.p, d.n);
    if (f.p) b_add(&out, f.p, f.n);
    b_str(&out, "</ul><hr><small>MiniHttpd</small></body></html>");
    reply_buf(c, 200, "text/html; charset=utf-8", out.p ? out.p : "", out.n, NULL);
    free(d.p);
    free(f.p);
    free(out.p);
}

/* ======================= PHP (CGI) 支持 ======================= */
/* CGI_TIMEOUT_MS / CGI_MAX_BODY / CGI_MAX_OUT / CGI_MAX_PROCS 已移至配置模块
   （g_cfg.cgiTimeoutMs / cgiMaxBody / cgiMaxOut / cgiMaxProcs，默认值不变，
   可在 minihttpd.ini 里用 php_timeout / php_max_body / php_max_out / php_max_procs 调整） */

#ifndef PROC_THREAD_ATTRIBUTE_HANDLE_LIST
#define PROC_THREAD_ATTRIBUTE_HANDLE_LIST 0x00020002
#endif

static int g_phpOk = 0, g_phpIni = 0, g_nophp = 0, g_rewrite = 0;
static wchar_t g_phpExe[PCAP], g_phpDir[PCAP];
static HANDLE g_job, g_cgiSem;

typedef struct CgiJob {
    Conn *c;
    wchar_t *env;                      /* 环境变量块 */
    wchar_t script[PCAP], dir[PCAP];
    char *body; size_t blen;           /* dispatch 之后才归 job 所有 */
} CgiJob;

static void free_job(struct CgiJob *j) {
    if (!j) return;
    free(j->env);
    free(j->body);
    free(j);
}

/* 检测 PHP：默认网站根目录的 php\php-cgi.exe（php_dir 可配，见 minihttpd.ini） */
static void php_init(void) {
    wchar_t ini[PCAP];
    DWORD a;
    if (g_nophp) return;
    if (path_is_abs(g_cfg.phpDir)) {                     /* 配置了绝对路径（minihttpd.ini: php_dir） */
        _snwprintf(g_phpDir, PCAP - 1, L"%ls", g_cfg.phpDir);
        g_phpDir[PCAP - 1] = 0;
    } else {
        join(g_phpDir, PCAP, g_root, g_cfg.phpDir);      /* 默认 php：网站根目录的 php 子目录 */
    }
    join(g_phpExe, PCAP, g_phpDir, L"php-cgi.exe");
    a = GetFileAttributesW(g_phpExe);
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) return;
    {                                                           /* 统一成长路径，便于和请求路径比较 */
        wchar_t lp[PCAP];
        DWORD ln = GetLongPathNameW(g_phpDir, lp, PCAP);
        if (ln > 0 && ln < PCAP) wcscpy(g_phpDir, lp);
    }
    join(ini, PCAP, g_phpDir, L"php.ini");
    a = GetFileAttributesW(ini);
    g_phpIni = (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY));
    g_cgiSem = CreateSemaphoreW(NULL, g_cfg.cgiMaxProcs, g_cfg.cgiMaxProcs, NULL);
    g_job = CreateJobObjectW(NULL, NULL);                       /* 服务退出时自动结束所有 php-cgi */
    if (g_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    g_phpOk = (g_cgiSem != NULL);
}

/* 请求的真实路径是否落在 php\ 目录之内（该目录含 php.ini 等，禁止被浏览器直接访问）。
   g_root 启动时已长化，请求路径只要不含 '~' 就不可能有 8.3 短名，
   免去旧版每请求一次 GetLongPathNameW 的逐级目录枚举 */
static int in_php_dir(const wchar_t *wp) {
    wchar_t full[PCAP], lng[PCAP];
    DWORD n = GetFullPathNameW(wp, PCAP, full, NULL);          /* 规范化 "\.\" 之类 */
    size_t l = wcslen(g_phpDir);
    if (n == 0 || n >= PCAP) return 1;
    if (wcschr(full, L'~')) {                                  /* 可能含短名才展开（PHP~1 之类） */
        DWORD m = GetLongPathNameW(full, lng, PCAP);
        if (m > 0 && m < PCAP) wcscpy(full, lng);
    }
    return _wcsnicmp(full, g_phpDir, l) == 0 && (full[l] == 0 || full[l] == L'\\');
}

static int ends_with_php(const char *s) {
    size_t l = strlen(s);
    return l >= 4 && !_stricmp(s + l - 4, ".php");
}

/* 从 from 起，找第一个以 .php 结尾的路径段，返回 ".php" 之后的位置；找不到返回 NULL */
static const char *find_php_end(const char *start, const char *from) {
    for (const char *p = from; *p; p++) {
        if (p[0] == '.' && (p[1] | 32) == 'p' && (p[2] | 32) == 'h' && (p[3] | 32) == 'p' &&
            (p[4] == '/' || p[4] == '\\' || p[4] == 0) &&
            p > start && p[-1] != '/' && p[-1] != '\\')
            return p + 4;
    }
    return NULL;
}

/* ---------------- 环境变量块 ---------------- */
typedef struct { wchar_t *p; size_t n, cap; } WBuf;

static int wb_put(WBuf *w, const wchar_t *s, size_t len) {
    if (w->n + len + 1 > w->cap) {
        size_t cap = w->cap ? w->cap : 4096;
        while (cap < w->n + len + 1) cap *= 2;
        wchar_t *np = (wchar_t *)realloc(w->p, cap * sizeof(wchar_t));
        if (!np) return 0;
        w->p = np; w->cap = cap;
    }
    memcpy(w->p + w->n, s, len * sizeof(wchar_t));
    w->n += len;
    return 1;
}

static wchar_t *a2w(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static int wb_var(WBuf *w, const wchar_t *name, const wchar_t *val) {      /* NAME=val\0 */
    return wb_put(w, name, wcslen(name)) && wb_put(w, L"=", 1) && wb_put(w, val, wcslen(val) + 1);
}

static int wb_vara(WBuf *w, const wchar_t *name, const char *val) {
    wchar_t *v = a2w(val);
    int r;
    if (!v) return 0;
    r = wb_var(w, name, v);
    free(v);
    return r;
}

static int wb_has(const WBuf *w, const wchar_t *name, size_t nlen) {
    size_t i = 0;
    while (i < w->n) {
        const wchar_t *e = w->p + i;
        size_t l = wcslen(e);
        if (l == 0) break;
        if (l > nlen && !_wcsnicmp(e, name, nlen) && e[nlen] == L'=') return 1;
        i += l + 1;
    }
    return 0;
}

static wchar_t *build_cgi_env(Conn *c, const Hdrs *hs, const char *method, const char *script_url,
                              const char *path_info, const char *query, const char *uri,
                              const wchar_t *script, long long blen) {
    WBuf w = {0};
    wchar_t t[64];
    char host[256], ct[256], addr[96] = "", port[16] = "";
    int ok = 1, i;
    wchar_t *inh;

    ok &= wb_var(&w, L"GATEWAY_INTERFACE", L"CGI/1.1");
    ok &= wb_var(&w, L"SERVER_SOFTWARE", WIDE(SERVER_TOKEN));
    ok &= wb_var(&w, L"SERVER_PROTOCOL", L"HTTP/1.1");
    ok &= wb_var(&w, L"REQUEST_SCHEME", L"http");
    ok &= wb_var(&w, L"REDIRECT_STATUS", L"200");               /* php-cgi 的 cgi.force_redirect 要求 */

    host[0] = 0;
    hdrs_str(hs, "Host", host, sizeof host);
    {
        char *e;
        if (host[0] == '[') { e = strchr(host, ']'); if (e) e[1] = 0; }
        else { e = strchr(host, ':'); if (e) *e = 0; }
    }
    if (!host[0]) strcpy(host, "localhost");
    ok &= wb_vara(&w, L"SERVER_NAME", host);
    _snwprintf(t, 63, L"%d", g_port); t[63] = 0;
    ok &= wb_var(&w, L"SERVER_PORT", t);
    ok &= wb_vara(&w, L"REQUEST_METHOD", method);
    ok &= wb_vara(&w, L"REQUEST_URI", uri);
    ok &= wb_vara(&w, L"QUERY_STRING", query);
    ok &= wb_vara(&w, L"SCRIPT_NAME", script_url);
    ok &= wb_var(&w, L"SCRIPT_FILENAME", script);
    ok &= wb_var(&w, L"DOCUMENT_ROOT", g_root);
    ok &= wb_var(&w, L"PHPRC", g_phpDir);                      /* 让 php-cgi 在自己目录找 php.ini */
    if (path_info[0]) ok &= wb_vara(&w, L"PATH_INFO", path_info);

    {
        struct sockaddr_storage ss;
        int sl = (int)sizeof ss;
        if (getpeername(c->s, (struct sockaddr *)&ss, &sl) == 0 &&
            getnameinfo((struct sockaddr *)&ss, sl, addr, sizeof addr, port, sizeof port,
                        NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
            ok &= wb_vara(&w, L"REMOTE_ADDR", addr);
            ok &= wb_vara(&w, L"REMOTE_PORT", port);
        }
    }
    if (hdrs_str(hs, "Content-Type", ct, sizeof ct)) ok &= wb_vara(&w, L"CONTENT_TYPE", ct);
    if (blen > 0 || !strcmp(method, "POST")) {
        _snwprintf(t, 63, L"%ld", (long)blen); t[63] = 0;
        ok &= wb_var(&w, L"CONTENT_LENGTH", t);
    }

    /* 请求头 -> HTTP_* ；忽略含下划线的头名（防伪造）和 Proxy（httpoxy）；
       直接迭代一次解析好的条目，不再重扫原始文本 */
    for (i = 0; i < hs->n; i++) {
        const char *p = hs->e[i].name, *v = hs->e[i].val;
        size_t nl = hs->e[i].nlen, vl = hs->e[i].vlen, k = 5, j;
        wchar_t name[160];
        int good = nl < 120;
        if (good) {
            wcscpy(name, L"HTTP_");
            for (j = 0; j < nl; j++) {
                unsigned char ch = (unsigned char)p[j];
                if (ch == '_' || ch <= 32 || ch >= 127 || ch == '=') { good = 0; break; }
                name[k++] = (ch == '-') ? L'_' : (wchar_t)toupper(ch);
            }
            name[k] = 0;
        }
        if (good && _wcsicmp(name, L"HTTP_CONTENT_TYPE") && _wcsicmp(name, L"HTTP_CONTENT_LENGTH") &&
            _wcsicmp(name, L"HTTP_PROXY") && !wb_has(&w, name, wcslen(name))) {
            char val[RECV_CAP];               /* 与 v2.1 一致：超长 Cookie 等头值不截断 */
            while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;   /* 去尾空白 */
            if (vl >= sizeof val) vl = sizeof val - 1;
            memcpy(val, v, vl);
            val[vl] = 0;
            ok &= wb_vara(&w, name, val);
        }
    }

    inh = GetEnvironmentStringsW();                              /* 继承系统环境（SystemRoot、PATH、TEMP…） */
    if (inh) {
        for (wchar_t *e = inh; *e; e += wcslen(e) + 1) {
            wchar_t *eq = wcschr(e + (*e == L'=' ? 1 : 0), L'=');
            if (!eq || eq == e) continue;
            if (wb_has(&w, e, (size_t)(eq - e))) continue;
            ok &= wb_put(&w, e, wcslen(e) + 1);
        }
        FreeEnvironmentStringsW(inh);
    }
    ok &= wb_put(&w, L"", 1);                                    /* 块末尾的额外 '\0' */
    if (!ok) { free(w.p); return NULL; }
    return w.p;
}

/* ---------------- 运行 php-cgi ---------------- */
typedef struct { HANDLE h; char *p; size_t n; } WriterArg;

static DWORD WINAPI cgi_writer(LPVOID a) {                      /* 单独线程写 stdin，避免和读 stdout 互相卡死 */
    WriterArg *w = (WriterArg *)a;
    size_t off = 0;
    while (off < w->n) {
        size_t left = w->n - off;
        DWORD chunk = (DWORD)(left > 65536 ? 65536 : left), got = 0;
        if (!WriteFile(w->h, w->p + off, chunk, &got, NULL) || got == 0) break;
        off += got;
    }
    CloseHandle(w->h);
    free(w->p);
    free(w);
    return 0;
}

typedef struct { HANDLE proc; volatile LONG fired; } KillCtx;

static VOID CALLBACK cgi_kill_cb(PVOID p, BOOLEAN timedOut) {
    KillCtx *k = (KillCtx *)p;
    (void)timedOut;
    InterlockedExchange(&k->fired, 1);
    TerminateProcess(k->proc, 1);
}

/* 返回 0=成功 1=无法启动 2=超时 3=输出过大 */
static int cgi_run(CgiJob *j, Buf *out, DWORD *werr, DWORD *exitcode) {
    SECURITY_ATTRIBUTES sa;
    HANDLE inR = NULL, inW = NULL, outR = NULL, outW = NULL, nul = INVALID_HANDLE_VALUE;
    HANDLE inh[3], wt = NULL, tm = NULL;
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    SIZE_T asz = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST al = NULL;
    wchar_t cmd[PCAP + 4];
    KillCtx kc;
    int rc = 1, started = 0, toobig = 0;

    *werr = 0; *exitcode = 0;
    memset(&pi, 0, sizeof pi);
    memset(&si, 0, sizeof si);
    sa.nLength = sizeof sa; sa.lpSecurityDescriptor = NULL; sa.bInheritHandle = TRUE;

    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0)) { *werr = GetLastError(); goto done; }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (nul == INVALID_HANDLE_VALUE) { *werr = GetLastError(); goto done; }

    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.StartupInfo.hStdInput = inR;
    si.StartupInfo.hStdOutput = outW;
    si.StartupInfo.hStdError = nul;                              /* stderr 丢弃，避免污染 HTTP 输出 */

    InitializeProcThreadAttributeList(NULL, 1, 0, &asz);
    al = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, asz);
    if (!al || !InitializeProcThreadAttributeList(al, 1, 0, &asz)) { *werr = GetLastError(); goto done; }
    inh[0] = inR; inh[1] = outW; inh[2] = nul;                   /* 只继承这三个句柄，客户端套接字不会泄漏给子进程 */
    if (!UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inh, sizeof inh, NULL, NULL)) {
        *werr = GetLastError(); goto done;
    }
    si.lpAttributeList = al;

    /* 注：_snwprintf 的 size 参数最多写 size-1 字符 + NUL，这里 cmd[PCAP+3]=0 是
       双保险（数组容量 PCAP+4，索引 PCAP+3 合法）；行为正确但易误读 */
    _snwprintf(cmd, PCAP + 3, L"\"%ls\"", g_phpExe);
    cmd[PCAP + 3] = 0;
    if (!CreateProcessW(g_phpExe, cmd, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                        j->env, j->dir, &si.StartupInfo, &pi)) {
        *werr = GetLastError(); goto done;
    }
    started = 1;
    CloseHandle(inR);  inR = NULL;
    CloseHandle(outW); outW = NULL;
    CloseHandle(nul);  nul = INVALID_HANDLE_VALUE;
    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    kc.proc = pi.hProcess; kc.fired = 0;
    CreateTimerQueueTimer(&tm, NULL, cgi_kill_cb, &kc, g_cfg.cgiTimeoutMs, 0, WT_EXECUTEONLYONCE);

    if (j->blen > 0 && j->body) {
        WriterArg *wa = (WriterArg *)malloc(sizeof *wa);
        if (wa) {
            wa->h = inW; wa->p = j->body; wa->n = j->blen;
            wt = CreateThread(NULL, 0, cgi_writer, wa, 0, NULL);
            if (wt) { inW = NULL; j->body = NULL; } else free(wa);
        }
    }
    if (inW) { CloseHandle(inW); inW = NULL; }

    {
        static const DWORD BUFSZ = 65536;
        char *tmp = (char *)malloc(BUFSZ);
        DWORD n;
        if (tmp) {
            while (ReadFile(outR, tmp, BUFSZ, &n, NULL) && n) {
                if (out->n + n > g_cfg.cgiMaxOut) { toobig = 1; TerminateProcess(pi.hProcess, 1); break; }
                b_add(out, tmp, n);
            }
            free(tmp);
        }
    }
    if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    GetExitCodeProcess(pi.hProcess, exitcode);
    if (tm) { DeleteTimerQueueTimer(NULL, tm, INVALID_HANDLE_VALUE); tm = NULL; }
    if (wt) { WaitForSingleObject(wt, 5000); CloseHandle(wt); wt = NULL; }
    rc = kc.fired ? 2 : (toobig ? 3 : 0);

done:
    if (started) CloseHandle(pi.hProcess);
    if (al) { DeleteProcThreadAttributeList(al); HeapFree(GetProcessHeap(), 0, al); }
    if (inR) CloseHandle(inR);
    if (inW) CloseHandle(inW);
    if (outR) CloseHandle(outR);
    if (outW) CloseHandle(outW);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    return rc;
}

/* ---------------- 把 CGI 输出转换成 HTTP 响应 ---------------- */
static int hdr_is(const char *name, size_t nl, const char *want) {
    return strlen(want) == nl && !_strnicmp(name, want, nl);
}

/* v2.2：直接“窃取” o 的缓冲区组装响应 —— 头部写在前端、正文就地 memmove 一次，
   免去旧版整块 memcpy（PHP 大输出时峰值内存 2x → 1x）。 */
static void cgi_make_response(Conn *c, Buf *o, Buf *resp) {
    const char *s = o->p;
    size_t n = o->n, pos = 0, body = 0, bl;
    int code = 0, has_ct = 0, has_cc = 0, has_loc = 0, found = 0;
    char reason[64] = "", head[512], d[64], ka[64];
    Buf h = {0}, hd = {0};
    int hn;

    while (pos < n) {
        const char *nlp = (const char *)memchr(s + pos, '\n', n - pos);
        size_t k = nlp ? (size_t)(nlp - s) : n, le = k;
        if (le > pos && s[le - 1] == '\r') le--;
        if (le == pos) { body = nlp ? k + 1 : n; found = 1; break; }      /* 空行：头部结束 */
        {
            const char *line = s + pos, *colon;
            size_t ll = le - pos;
            colon = (const char *)memchr(line, ':', ll);
            if (colon && colon > line) {
                size_t nl = (size_t)(colon - line);
                const char *v = colon + 1;
                size_t vl = ll - nl - 1;
                while (vl && (*v == ' ' || *v == '\t')) { v++; vl--; }
                if (hdr_is(line, nl, "Status")) {
                    char tmp[128];
                    size_t m = vl < sizeof tmp - 1 ? vl : sizeof tmp - 1;
                    const char *sp;
                    memcpy(tmp, v, m); tmp[m] = 0;
                    code = atoi(tmp);
                    sp = strchr(tmp, ' ');
                    if (sp) { while (*sp == ' ') sp++; snprintf(reason, sizeof reason, "%s", sp); }
                } else if (hdr_is(line, nl, "Content-Length") || hdr_is(line, nl, "Transfer-Encoding") ||
                           hdr_is(line, nl, "Connection") || hdr_is(line, nl, "Keep-Alive")) {
                    /* 由服务器重新生成 */
                } else {
                    if (hdr_is(line, nl, "Content-Type")) has_ct = 1;
                    else if (hdr_is(line, nl, "Cache-Control")) has_cc = 1;
                    else if (hdr_is(line, nl, "Location")) has_loc = 1;
                    b_add(&h, line, ll);
                    b_add(&h, "\r\n", 2);
                }
            }
        }
        pos = nlp ? k + 1 : n;
    }
    if (!found) {                                                      /* 没有头部：整个输出当 HTML 正文 */
        free(h.p); h.p = NULL; h.n = h.cap = 0;
        has_ct = has_cc = has_loc = 0; code = 0; reason[0] = 0; body = 0;
    }
    if (!code) code = has_loc ? 302 : 200;
    if (!reason[0]) snprintf(reason, sizeof reason, "%s", status_text(code));
    bl = n - body;

    hn = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nServer: " SERVER_TOKEN "\r\n", code, reason);
    b_add(&hd, head, (size_t)hn);
    date_line(d, sizeof d);
    b_add(&hd, d, (size_t)strlen(d));
    if (h.p) b_add(&hd, h.p, h.n);
    if (!has_ct) b_str(&hd, "Content-Type: text/html; charset=UTF-8\r\n");
    if (!has_cc) b_str(&hd, "Cache-Control: no-cache\r\n");
    hn = snprintf(head, sizeof head, "Content-Length: %llu\r\n%s\r\n",
                  (unsigned long long)(c->head ? 0 : bl), keep_line(c, ka, sizeof ka));
    b_add(&hd, head, (size_t)hn);

    if (o->p) {                                                        /* 窃取 o 的缓冲区就位 */
        size_t want = hd.n + (c->head ? 0 : bl);
        char *base = o->p;
        if (want + 1 > o->cap) {
            char *np = (char *)realloc(base, want + 64);
            if (!np) {
                free(hd.p); free(h.p);
                resp->p = NULL; resp->n = resp->cap = 0;
                return;                                                /* OOM：调用方关连接 */
            }
            base = np;
            o->cap = want + 64;
        }
        if (!c->head && bl) memmove(base + hd.n, base + body, bl);
        memcpy(base, hd.p, hd.n);
        o->n = 0;
        resp->p = base;
        resp->n = want;
        resp->cap = o->cap;
        resp->p[resp->n] = 0;
        o->p = NULL;
        o->cap = 0;
    } else {
        *resp = hd;                                                    /* o 为空（防御）：头就是全部 */
        if (c->head) resp->n = hd.n;
    }
    free(h.p);
}

static void put_page(Buf *r, Conn *c, int code, const char *title, const char *msg) {
    char b[1024];
    int n = snprintf(b, sizeof b,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>%d %s</title></head>"
        "<body style=\"font-family:sans-serif;margin:8vh auto;max-width:720px;padding:0 1em\">"
        "<h2>%d %s</h2><p>%s</p><hr><small>MiniHttpd</small></body></html>",
        code, title, code, title, msg);
    make_head(r, c, code, "text/html; charset=utf-8", n, NULL);
    if (!c->head) b_add(r, b, (size_t)n);
}

/* ---------------- 线程池中执行一个 CGI 请求 ---------------- */
static DWORD WINAPI cgi_work(LPVOID p) {
    CgiJob *j = (CgiJob *)p;
    Conn *c = j->c;
    Buf out = {0}, resp = {0};
    DWORD werr = 0, ec = 0;
    int rc;

    WaitForSingleObject(g_cgiSem, INFINITE);
    rc = cgi_run(j, &out, &werr, &ec);
    ReleaseSemaphore(g_cgiSem, 1, NULL);
    free_job(j);

    if (rc == 0 && out.n > 0) {
        cgi_make_response(c, &out, &resp);      /* resp 窃取 out 的缓冲区，免复制 */
    } else {
        int code = 500;
        const char *title = "Internal Server Error";
        resp = bp_get();
        if (rc == 1) { char msg[320];
            snprintf(msg, sizeof msg,
                "无法启动 php-cgi.exe（Windows 错误码 %lu）。请确认 php 目录完整，并已安装 Visual C++ 运行库。",
                (unsigned long)werr);
            put_page(&resp, c, code, title, msg);
        } else if (rc == 2) { char msg[96]; code = 504; title = "Gateway Timeout";
            snprintf(msg, sizeof msg, "PHP 脚本执行超过 %d 秒，已被终止。", g_cfg.cgiTimeoutMs / 1000);
            put_page(&resp, c, code, title, msg);
        } else if (rc == 3) { char msg[96];
            snprintf(msg, sizeof msg, "PHP 输出超过 %u MB，已被终止。", (unsigned)(g_cfg.cgiMaxOut >> 20));
            put_page(&resp, c, code, title, msg);
        } else { char msg[320];
            snprintf(msg, sizeof msg,
                "php-cgi 没有返回任何内容（退出码 %lu）。请检查 php.ini 与脚本是否存在致命错误。", (unsigned long)ec);
            put_page(&resp, c, code, title, msg);
        }
    }
    free(out.p);                               /* 被窃取时为 NULL，free(NULL) 无害 */
    if (!resp.p) { close_conn(c); return 0; }
    c->sbuf = resp.p;
    c->slen = (int)resp.n;
    c->scap = (int)resp.cap;
    c->sofs = 0;
    post_send(c);                                                /* 此后 c 归 IOCP 工作线程，不再触碰 */
    return 0;
}

static void cgi_abort(Conn *c, int code, const char *extra) {
    if (c->cgi) { free_job(c->cgi); c->cgi = NULL; }
    free(c->body); c->body = NULL;
    c->blen = c->bgot = 0; c->bcap = 0;
    if (code == 400 || code == 411 || code == 413) c->keep = 0;  /* 请求体没读完，连接不可复用 */
    reply_error(c, code, extra);
}

static void dispatch_cgi(Conn *c) {
    CgiJob *j = c->cgi;
    c->cgi = NULL;
    j->body = c->body; j->blen = (size_t)c->blen;
    c->body = NULL; c->blen = c->bgot = 0; c->bcap = 0;
    c->state = ST_CGI;
    c->last = GetTickCount();
    if (!QueueUserWorkItem(cgi_work, j, WT_EXECUTELONGFUNCTION)) { free_job(j); reply_error(c, 500, NULL); }
}

/* 请求体按需增长（v2.2）：慢速 POST 不再一次性占用 Content-Length 全量内存。
   容量从 64KB 起步倍增，终量不超过 Content-Length（≤64MB）。 */
static int body_reserve(Conn *c, size_t need) {
    size_t want = (size_t)c->bgot + need, ncap;
    char *np;
    if (want <= c->bcap) return 1;
    ncap = c->bcap ? c->bcap : 65536;
    while (ncap < want) ncap *= 2;
    if (ncap > (size_t)c->blen) ncap = (size_t)c->blen;
    if (ncap < want) ncap = want;
    np = (char *)realloc(c->body, ncap);
    if (!np) return 0;
    c->body = np;
    c->bcap = ncap;
    return 1;
}

static void post_body(Conn *c) {
    WSABUF b;
    DWORD n = 0, fl = 0;
    long long left = c->blen - c->bgot;
    size_t take = (size_t)(left > (1 << 20) ? (1 << 20) : left);
    if (!body_reserve(c, take)) { close_conn(c); return; }   /* 内存不足：直接断开 */
    b.buf = c->body + c->bgot;
    b.len = (ULONG)take;
    c->state = ST_BODY;
    c->last = GetTickCount();
    memset(&c->op.ov, 0, sizeof c->op.ov);
    c->op.type = OP_BODY;
    if (WSARecv(c->s, &b, 1, &n, &fl, &c->op.ov, NULL) == SOCKET_ERROR &&
        WSAGetLastError() != WSA_IO_PENDING) close_conn(c);
}

/* ---------------- PHP 请求公共预检（CGI / FastCGI 共用） ---------------- */
/* 返回 -1 = 已回复错误；否则返回请求体长度。
   Content-Length 严格校验（RFC 7230）：纯数字、无正负号、无内部空白；
   重复的 Content-Length 字面必须完全相同，否则 400（防请求走私）。 */
static long long php_precheck(Conn *c, const Hdrs *hs, const char *method) {
    static const char *const okm[] = {"GET", "HEAD", "POST", "PUT", "DELETE", "PATCH", "OPTIONS", NULL};
    const HEnt *e;
    int i, good = 0;

    for (i = 0; okm[i]; i++) if (!strcmp(method, okm[i])) good = 1;
    if (!good) { c->keep = 0; reply_error(c, 405, "Allow: GET, HEAD, POST, PUT, DELETE, PATCH, OPTIONS\r\n"); return -1; }

    if (hdrs_get(hs, "Transfer-Encoding")) { c->keep = 0; reply_error(c, 411, NULL); return -1; }
    e = hdrs_get(hs, "Content-Length");
    if (e) {
        long long blen = 0;
        size_t k;
        if (e->vlen == 0 || e->vlen > 15) { c->keep = 0; reply_error(c, 400, NULL); return -1; }
        for (k = 0; k < e->vlen; k++) {
            if (e->val[k] < '0' || e->val[k] > '9') { c->keep = 0; reply_error(c, 400, NULL); return -1; }
            blen = blen * 10 + (e->val[k] - '0');
        }
        for (i = 0; i < hs->n; i++) {                 /* 重复 CL：字面不同即 400 */
            const HEnt *d = &hs->e[i];
            if (d->nlen == 14 && !_strnicmp(d->name, "Content-Length", 14) &&
                (d->vlen != e->vlen || memcmp(d->val, e->val, e->vlen) != 0)) {
                c->keep = 0; reply_error(c, 400, NULL); return -1;
            }
        }
        if (blen > g_cfg.cgiMaxBody) { c->keep = 0; reply_error(c, 413, NULL); return -1; }
        return blen;
    }
    return 0;
}

/* 调用后调用者必须直接返回 */
static void start_cgi(Conn *c, const Hdrs *hs, const char *method, const wchar_t *script,
                      const char *script_url, const char *path_info, const char *query, const char *uri) {
    long long blen = php_precheck(c, hs, method);
    CgiJob *j;
    wchar_t *sl;

    if (blen < 0) return;
    if (!dir_inside(script)) { if (blen > 0) c->keep = 0; reply_error(c, 403, NULL); return; }

    j = (CgiJob *)calloc(1, sizeof *j);
    if (!j) { if (blen > 0) c->keep = 0; reply_error(c, 500, NULL); return; }
    j->c = c;
    wcsncpy(j->script, script, PCAP - 1);
    wcscpy(j->dir, j->script);
    sl = wcsrchr(j->dir, L'\\');
    if (sl) *sl = 0;
    j->env = build_cgi_env(c, hs, method, script_url, path_info, query, uri, script, blen);
    if (!j->env) { free_job(j); if (blen > 0) c->keep = 0; reply_error(c, 500, NULL); return; }
    c->cgi = j;
    c->blen = blen;
    c->bgot = 0;

    if (blen > 0) {
        size_t avail, take;
        avail = (size_t)(c->have - c->hend);                   /* 请求头之后已经收到的字节就是请求体开头 */
        take = avail < (size_t)blen ? avail : (size_t)blen;
        if (take && !body_reserve(c, take)) { cgi_abort(c, 500, NULL); return; }
        if (take) memcpy(c->body, c->reqbuf + c->hend, take);
        c->bgot = (long long)take;
        c->hend += (int)take;                                  /* 这些字节算作已消费，不属于下一个请求 */
        if (c->bgot < blen) {
            const HEnt *ex = hdrs_get(hs, "Expect");
            if (ex) {
                char hv[64];
                size_t cl = ex->vlen < sizeof hv - 1 ? ex->vlen : sizeof hv - 1;
                memcpy(hv, ex->val, cl);
                hv[cl] = 0;
                for (char *q = hv; *q; q++) *q = (char)tolower((unsigned char)*q);
                if (strstr(hv, "100-continue")) send(c->s, "HTTP/1.1 100 Continue\r\n\r\n", 25, 0);
            }
            post_body(c);
            return;
        }
    }
    dispatch_cgi(c);
}

/* ======================= PHP (FastCGI) 支持 =======================
   对标 Nginx + PHP-FPM 的架构（v2.1 起，检测到 php\php-cgi.exe 即自动启用）：
   - 常驻进程池：php-cgi.exe 以 "-b 127.0.0.1:端口" 的 FastCGI 监听模式运行，
     按需扩容（最多 fcgi_max_workers 个，默认 16），空闲超时自动缩容，免除逐请求 fork；
   - 每个进程一条持久连接（FCGI_KEEP_CONN），请求间复用，连接挂在同一个 IOCP 上，
     全链路异步，执行期间不占用任何线程（对比 CGI 模式每请求阻塞一个线程池线程）；
   - 上游全忙时请求进入 FIFO 等待队列，有空闲连接立即派发（等价 nginx upstream 排队）；
   - 上游零输出断开时 GET/HEAD 自动重试一次（等价 fastcgi_next_upstream）；
   - 进程池彻底无法启动时自动回退到逐请求 CGI 模式。
   锁纪律：g_fcgiLock 期间绝不调用 close_conn / reply_*（它们会碰 g_lock），
   对客户端的响应一律经 FcgiAct 延迟到解锁之后执行，避免锁嵌套死锁。 */
/* FCGI_MIN_WORKERS / FCGI_MAX_WORKERS / FCGI_IDLE_KILL_MS / FCGI_SPAWN_TIMEOUT_MS
   已移至配置模块（g_cfg.fcgiMin / fcgiMax / fcgiIdleMs / fcgiSpawnMs，默认值不变，
   可在 minihttpd.ini 里用 fcgi_min_workers / fcgi_max_workers / fcgi_idle_kill /
   fcgi_spawn_timeout 调整） */
#define FCGI_SPAWN_ATTEMPTS 2                   /* 每次启动的换端口重试次数 */
#define FCGI_MAX_RECORD     65535               /* 单个 FastCGI 记录内容上限（协议规定） */
#define FCGI_RECV_CAP       65536               /* 上游接收缓冲 */

/* ---- 纯协议层：不依赖 Windows，单独抽出即可在主机上做单元测试 ---- */
/*PROTO-BEGIN*/
#define FCGI_STDERR_KEEP    4096                /* 保留的 stderr 字节数（诊断用） */

enum {
    FCGI_BEGIN_REQUEST = 1, FCGI_ABORT_REQUEST = 2, FCGI_END_REQUEST = 3,
    FCGI_PARAMS = 4, FCGI_STDIN = 5, FCGI_STDOUT = 6, FCGI_STDERR = 7,
    FCGI_DATA = 8, FCGI_GET_VALUES = 9, FCGI_GET_VALUES_RESULT = 10,
    FCGI_UNKNOWN_TYPE = 11
};
#define FCGI_RESPONDER  1
#define FCGI_KEEP_CONN  1

typedef struct {                /* FastCGI 记录增量解析状态 */
    unsigned char rhdr[8];      /* 正在拼的记录头 */
    int rhave;                  /* 已收到的头字节数 */
    int rtype, rcontent, rpad;  /* 当前记录的类型/内容长/填充长 */
    int rleftC, rleftP;         /* 剩余待收的内容/填充字节数 */
    unsigned char rbody[8];     /* END_REQUEST 内容缓冲 */
    int rbodyHave;
} FcgiParse;

/* 记录头：requestId 恒为 1（每条连接同时只跑一个请求，php-cgi 也是单请求串行） */
static void fcgi_hdr(unsigned char *h, int type, int len) {
    h[0] = 1; h[1] = (unsigned char)type;
    h[2] = 0; h[3] = 1;
    h[4] = (unsigned char)(len >> 8); h[5] = (unsigned char)(len & 0xFF);
    h[6] = 0; h[7] = 0;         /* padding = 0（php-cgi 发来的记录带填充，解析端支持） */
}

/* name/value 长度前缀：<128 一字节；否则 4 字节大端且最高位置 1 */
static void fcgi_put_len(Buf *b, size_t len) {
    if (len < 128) {
        unsigned char t = (unsigned char)len;
        b_add(b, (const char *)&t, 1);
    } else {
        unsigned char t[4] = { (unsigned char)(0x80 | (len >> 24)), (unsigned char)(len >> 16),
                               (unsigned char)(len >> 8), (unsigned char)len };
        b_add(b, (const char *)t, 4);
    }
}

static int fcgi_put_nv(Buf *b, const char *name, const char *val, size_t vlen) {
    size_t want = strlen(name) + vlen + (strlen(name) < 128 ? 1 : 4) + (vlen < 128 ? 1 : 4);
    size_t before = b->n;
    fcgi_put_len(b, strlen(name));
    fcgi_put_len(b, vlen);
    b_add(b, name, strlen(name));
    b_add(b, val, vlen);
    return b->n == before + want;      /* OOM 时 b_add 不追加，长度对不上即失败 */
}

/* 增量解析 FastCGI 记录流（对端=php-cgi）。
   返回 1=收到 END_REQUEST；0=需要更多数据；-1=致命错误（*why: 1=输出超限 0=协议错误）。
   STDOUT/STDERR 采用流式累积，记录可以任意拆分到达；padding 单独跳过。
   注意：记录完成检查在“是否还需要数据”之前 —— END_REQUEST 总是最后一个记录，
   若它的最后一个字节恰好在某次喂数的末尾，必须立即返回 1 而不是等下一包。 */
static int fcgi_feed(FcgiParse *s, Buf *out, Buf *err, int *appStatus, int *protoStatus,
                     const char *p, size_t nn, unsigned long long maxOut, int *why) {
    *why = 0;
    for (;;) {
        if (s->rhave < 8) {                             /* 拼记录头 */
            size_t k;
            if (!nn) return 0;
            k = nn < (size_t)(8 - s->rhave) ? nn : (size_t)(8 - s->rhave);
            memcpy(s->rhdr + s->rhave, p, k);
            s->rhave += (int)k; p += k; nn -= k;
            if (s->rhave < 8) return 0;
            if (s->rhdr[0] != 1) return -1;                     /* 版本号不对 */
            if (((s->rhdr[2] << 8) | s->rhdr[3]) != 1) return -1; /* 请求号不对 */
            s->rtype    = s->rhdr[1];
            s->rcontent = (s->rhdr[4] << 8) | s->rhdr[5];
            s->rpad     = s->rhdr[6];
            s->rleftC   = s->rcontent;
            s->rleftP   = s->rpad;
            if (s->rtype == FCGI_END_REQUEST && s->rcontent != 8) return -1;
            continue;
        }
        if (s->rleftC > 0) {                            /* 收内容 */
            size_t k;
            if (!nn) return 0;
            k = nn < (size_t)s->rleftC ? nn : (size_t)s->rleftC;
            switch (s->rtype) {
            case FCGI_STDOUT:
                if (out->n + k > maxOut) { *why = 1; return -1; }
                b_add(out, p, k);
                break;
            case FCGI_STDERR:
                if (err->n < FCGI_STDERR_KEEP) {        /* 只保留开头一段做诊断 */
                    size_t t = FCGI_STDERR_KEEP - err->n;
                    if (k < t) t = k;
                    b_add(err, p, t);
                }
                break;
            case FCGI_END_REQUEST:
                memcpy(s->rbody + s->rbodyHave, p, k);
                s->rbodyHave += (int)k;
                break;
            default:                                    /* PARAMS/DATA 等：按字节丢弃 */
                break;
            }
            p += k; nn -= k; s->rleftC -= (int)k;
            continue;
        }
        if (s->rleftP > 0) {                            /* 跳填充 */
            size_t k;
            if (!nn) return 0;
            k = nn < (size_t)s->rleftP ? nn : (size_t)s->rleftP;
            p += k; nn -= k; s->rleftP -= (int)k;
            continue;
        }
        /* 当前记录完整（即使本次喂数已用尽也立即处理） */
        if (s->rtype == FCGI_END_REQUEST) {
            if (s->rbodyHave != 8) return -1;
            *appStatus   = (s->rbody[0] << 24) | (s->rbody[1] << 16) | (s->rbody[2] << 8) | s->rbody[3];
            *protoStatus = s->rbody[4];
            return 1;
        }
        s->rhave = 0;
        s->rbodyHave = 0;
        /* nn 可能为 0：下一轮 rhave<8 且 nn==0 → 正确返回 0 */
    }
}
/*PROTO-END*/

/* ---- 池结构与全局状态 ---- */
typedef struct FcgiWorker FcgiWorker;
typedef struct FcgiUp FcgiUp;

typedef struct FcgiReq {         /* 一个待执行的 PHP 请求（挂在客户端 Conn 上） */
    struct FcgiReq *qnext;       /* FIFO 等待队列链 */
    Conn *c;
    char *params;                /* BEGIN+PARAMS(+空STDIN) 已编码字节流 */
    size_t plen;
    int retried;                 /* 上游零输出断开时只允许重试一次 */
    DWORD queued;                /* 进入等待队列的时刻（排队超时用） */
} FcgiReq;

typedef struct { char *p; size_t len; } FcgiSeg;

typedef struct { OVERLAPPED ov; int type; FcgiUp *u; } FOp;   /* type = OP_FSEND/OP_FRECV，ov 必须是第一个成员 */

struct FcgiUp {                  /* 与某个 php-cgi 进程的持久连接 */
    FcgiWorker *w;
    SOCKET s;
    int dead, busy;
    Conn *c;                     /* busy 期间服务的客户端连接 */
    DWORD started, idleSince;
    FcgiSeg segs[3];             /* 当前发送批次未确认的段 */
    int nsegs;
    unsigned char shdr[8], ehdr[8];
    char *ps, *pe;               /* 参数流游标 */
    char *bs, *be;               /* 请求体游标 */
    int endSent;                 /* 空 STDIN 结束记录是否已入队 */
    char *rb;                    /* 接收缓冲 */
    FcgiParse pp;
    Buf out, err;                /* STDOUT / STDERR 累积 */
    int appStatus, protoStatus;
    FOp op;                      /* 同一时刻只有一个挂起操作 */
};

struct FcgiWorker {
    FcgiWorker *prev, *next;
    HANDLE proc;
    int port;
    FcgiUp *up;                  /* 其唯一持久连接（php-cgi 单线程串行，一进程一连接） */
};

/* 延迟到 g_fcgiLock 之外执行的动作（回复客户端 / 回收资源 / 补进程）
   —— 类型定义已前置到 close_conn 之前，见文件前部 */
static wchar_t *g_fcgiEnv;         /* 子进程环境块（继承父环境 + PHP_FCGI_MAX_REQUESTS=0，fcgi_init 构建一次） */
static FcgiWorker *g_fcgiWorkers;
static FcgiReq *g_fcgiWaitHead, *g_fcgiWaitTail;
static volatile LONG g_fcgiN, g_fcgiBusy, g_fcgiQ, g_spawning, g_fcgiFails;
static int g_fcgiOk = 0;         /* 1 = FastCGI 模式（启动失败自动回退 CGI） */
static char g_rootUtf8[PCAP * 3];/* DOCUMENT_ROOT（UTF-8，启动时转换一次） */

static void fcgi_dead_locked(FcgiUp *u, FcgiAct *a, int can_retry, int code, const char *msg);
static void fcgi_run_act(FcgiAct *a);
static void fcgi_spawn_async(void);
static DWORD WINAPI fcgi_spawn_thread(LPVOID unused);

/* ---- 基础工具 ---- */
static void fcgi_free_req(FcgiReq *fr) {
    if (!fr) return;
    free(fr->params);
    free(fr);
}

/* 只允许在"该连接已无挂起 I/O"时调用（记录头里那个 op 的完成包已处理完） */
static void fcgi_free_up(FcgiUp *u) {
    if (u->s != INVALID_SOCKET) closesocket(u->s);
    free(u->rb);
    free(u->out.p);
    free(u->err.p);
    free(u);
}

/* 把已构建好的响应挂到延迟动作上（锁内调用；分配失败记 oomc，锁外关闭） */
static void act_send(FcgiAct *a, Conn *c, Buf *resp) {
    FcgiItem *it = (FcgiItem *)malloc(sizeof *it);
    if (!it) { free(resp->p); a->oomc = c; return; }
    it->next = NULL;
    it->c = c;
    it->resp = *resp;
    if (a->tail) a->tail->next = it; else a->head = it;
    a->tail = it;
}

/* ---- 等待队列（FIFO） ---- */
static void fcgi_push_wait_locked(FcgiReq *fr, int head) {
    fr->qnext = NULL;
    fr->queued = GetTickCount();
    if (head) {
        fr->qnext = g_fcgiWaitHead;
        g_fcgiWaitHead = fr;
        if (!g_fcgiWaitTail) g_fcgiWaitTail = fr;
    } else {
        if (g_fcgiWaitTail) g_fcgiWaitTail->qnext = fr; else g_fcgiWaitHead = fr;
        g_fcgiWaitTail = fr;
    }
    InterlockedIncrement(&g_fcgiQ);
}

static void fcgi_remove_wait_locked(FcgiReq *fr) {
    FcgiReq *prev = NULL, *p = g_fcgiWaitHead;
    while (p && p != fr) { prev = p; p = p->qnext; }
    if (!p) return;
    if (prev) prev->qnext = fr->qnext; else g_fcgiWaitHead = fr->qnext;
    if (g_fcgiWaitTail == fr) g_fcgiWaitTail = prev;
    fr->qnext = NULL;
    InterlockedDecrement(&g_fcgiQ);
}

static Conn *fcgi_pop_wait_locked(void) {
    FcgiReq *fr = g_fcgiWaitHead;
    Conn *c;
    if (!fr) return NULL;
    g_fcgiWaitHead = fr->qnext;
    if (!g_fcgiWaitHead) g_fcgiWaitTail = NULL;
    fr->qnext = NULL;
    InterlockedDecrement(&g_fcgiQ);
    c = fr->c;
    return c;
}

static int need_spawn_locked(void) {
    LONG n;
    if (g_spawning >= 2) return 0;                    /* 限制并发 spawn，防 fork 风暴 */
    n = g_fcgiN + g_spawning;
    if (n < g_cfg.fcgiMin) return 1;
    if (g_fcgiWaitHead && n < g_cfg.fcgiMax) return 1;  /* 有排队且未到上限 → 扩容 */
    return 0;
}

/* ---- 发送状态机 ----
   BEGIN+PARAMS（+无请求体时的空 STDIN）在 build 阶段就编成一块；
   请求体按 ≤64KB 的 STDIN 记录流出，最后一片顺带空 STDIN 结束记录，
   典型 GET 请求只需一次 WSASend。 */
static int fcgi_send_more(FcgiUp *u) {
    WSABUF wb[3];
    DWORD n = 0;
    for (;;) {
        if (!u->nsegs) {
            if (u->ps < u->pe) {                      /* 参数流 */
                size_t k = (size_t)(u->pe - u->ps);
                if (k > SEND_CHUNK) k = SEND_CHUNK;
                u->segs[0].p = u->ps; u->segs[0].len = k;
                u->nsegs = 1;
                u->ps += k;
            } else if (u->bs < u->be) {               /* 请求体 → STDIN 记录 */
                size_t k = (size_t)(u->be - u->bs);
                if (k > FCGI_MAX_RECORD) k = FCGI_MAX_RECORD;
                fcgi_hdr(u->shdr, FCGI_STDIN, (int)k);
                u->segs[0].p = (char *)u->shdr; u->segs[0].len = 8;
                u->segs[1].p = u->bs;      u->segs[1].len = k;
                u->nsegs = 2;
                u->bs += k;
                if (u->bs == u->be && !u->endSent) {  /* 最后一片顺带结束记录，少一次系统调用 */
                    fcgi_hdr(u->ehdr, FCGI_STDIN, 0);
                    u->segs[2].p = (char *)u->ehdr; u->segs[2].len = 8;
                    u->nsegs = 3;
                    u->endSent = 1;
                }
            } else if (!u->endSent) {                 /* 孤立的空 STDIN 结束记录 */
                fcgi_hdr(u->ehdr, FCGI_STDIN, 0);
                u->segs[0].p = (char *)u->ehdr; u->segs[0].len = 8;
                u->nsegs = 1;
                u->endSent = 1;
            } else {
                /* 请求已全部发出 → 转接收 */
                WSABUF rb;
                DWORD rn = 0, fl = 0;
                rb.buf = u->rb; rb.len = FCGI_RECV_CAP;
                u->op.type = OP_FRECV;
                memset(&u->op.ov, 0, sizeof u->op.ov);
                if (WSARecv(u->s, &rb, 1, &rn, &fl, &u->op.ov, NULL) == 0 ||
                    WSAGetLastError() == WSA_IO_PENDING) return 0;
                return -1;
            }
        }
        {
            int i;
            for (i = 0; i < u->nsegs; i++) {
                wb[i].buf = u->segs[i].p;
                wb[i].len = (ULONG)u->segs[i].len;
            }
            u->op.type = OP_FSEND;
            memset(&u->op.ov, 0, sizeof u->op.ov);
            if (WSASend(u->s, wb, (DWORD)u->nsegs, &n, 0, &u->op.ov, NULL) == 0 ||
                WSAGetLastError() == WSA_IO_PENDING) return 1;
            return -1;
        }
    }
}

/* WSASend 完成后按确认字节数消费发送段 */
static int fcgi_send_done(FcgiUp *u, DWORD n) {
    size_t left = n;
    int i = 0, j, rem;
    while (left && i < u->nsegs) {
        size_t k = u->segs[i].len;
        if (k > left) k = left;
        u->segs[i].p += k;
        u->segs[i].len -= k;
        left -= k;
        if (u->segs[i].len == 0) i++;
    }
    if (i) {
        rem = u->nsegs - i;
        for (j = 0; j < rem; j++) u->segs[j] = u->segs[i + j];
        u->nsegs = rem;
    }
    return fcgi_send_more(u);
}

/* ---- 请求生命周期 ---- */
static void act_freew(FcgiAct *a, FcgiWorker *w) {          /* 待回收 worker 入链（借用 ->next） */
    w->next = a->freew;
    a->freew = w;
}

static void fcgi_start_locked(FcgiUp *u, Conn *c, FcgiAct *a) {
    FcgiReq *fr = c->fr;
    u->busy = 1;
    InterlockedIncrement(&g_fcgiBusy);
    u->c = c;
    c->up = u;                          /* 双向绑定：close_conn 能找到并取消执行中的请求 */
    u->started = GetTickCount();
    free(u->out.p); free(u->err.p);
    memset(&u->out, 0, sizeof u->out);
    memset(&u->err, 0, sizeof u->err);
    memset(&u->pp, 0, sizeof u->pp);
    u->appStatus = u->protoStatus = 0;
    u->ps = fr->params;
    u->pe = fr->params + fr->plen;
    u->bs = c->body;
    u->be = c->body + (size_t)c->blen;
    u->endSent = (c->blen <= 0);      /* 无请求体时空 STDIN 已并入参数块 */
    u->nsegs = 0;
    if (fcgi_send_more(u) < 0)
        fcgi_dead_locked(u, a, 1, 0, NULL);
}

/* 上游连接失效（php 崩溃/连接断开/协议错误/输出超限）。
   挂起 op 已完成或从未发出 → 由调用方经 act->freeup 释放。 */
static void fcgi_dead_locked(FcgiUp *u, FcgiAct *a, int can_retry, int code, const char *msg) {
    FcgiWorker *w = u->w;
    Conn *c;
    u->dead = 1;
    if (w) {                                     /* 摘除 worker，php 进程交给锁外善后 */
        if (w->prev) w->prev->next = w->next; else g_fcgiWorkers = w->next;
        if (w->next) w->next->prev = w->prev;
        act_freew(a, w);
        InterlockedDecrement(&g_fcgiN);
    }
    c = u->c;
    u->c = NULL;
    if (c) c->up = NULL;
    if (u->busy) { u->busy = 0; InterlockedDecrement(&g_fcgiBusy); }
    a->freeup = u;
    if (c && c->fr) {
        if (can_retry && !c->fr->retried && u->out.n == 0) {
            /* 等价 nginx fastcgi_next_upstream：零输出断开 → 换个进程重跑一次 */
            c->fr->retried = 1;
            fcgi_push_wait_locked(c->fr, 1);
        } else {
            Buf r = bp_get();
            if (!code) code = 502;
            if (!msg) msg = "php-cgi 进程在处理请求时意外退出，请检查 PHP 环境与脚本。";
            put_page(&r, c, code, code == 502 ? "Bad Gateway" : "Internal Server Error", msg);
            act_send(a, c, &r);
        }
    }
    if (need_spawn_locked()) a->want_spawn = 1;
}

/* 脚本异常退出时的诊断页（带 stderr 尾巴） */
static void fcgi_err_page(Buf *r, Conn *c, FcgiUp *u) {
    Buf b = {0};
    char t[32];
    b_str(&b, "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>500 Internal Server Error</title></head>"
              "<body style=\"font-family:sans-serif;margin:8vh auto;max-width:720px;padding:0 1em\">"
              "<h2>500 Internal Server Error</h2><p>PHP 脚本异常退出（退出码 ");
    snprintf(t, sizeof t, "%d", u->appStatus);
    b_str(&b, t);
    b_str(&b, "），没有产生任何输出。</p>");
    if (u->err.p && u->err.n) {
        b_str(&b, "<pre style=\"background:#f6f6f6;padding:8px;overflow:auto;text-align:left\">");
        b_esc(&b, u->err.p);
        b_str(&b, "</pre>");
    }
    b_str(&b, "<hr><small>MiniHttpd</small></body></html>");
    make_head(r, c, 500, "text/html; charset=utf-8", (long long)b.n, NULL);
    if (!c->head && b.p) b_add(r, b.p, b.n);
    free(b.p);
}

/* 收到 END_REQUEST：构建响应并立即衔接队列里的下一个请求 */
static void fcgi_finish_locked(FcgiUp *u, FcgiAct *a) {
    Conn *c = u->c;
    Conn *nc;
    u->c = NULL;
    if (c) c->up = NULL;
    u->busy = 0;
    InterlockedDecrement(&g_fcgiBusy);
    u->idleSince = GetTickCount();
    if (c) {
        Buf r = bp_get();
        if (u->protoStatus != 0) {
            put_page(&r, c, 502, "Bad Gateway", "FastCGI 协议错误（protocolStatus 非 0）。");
            act_send(a, c, &r);
        } else if (u->out.n == 0 && u->appStatus != 0) {
            fcgi_err_page(&r, c, u);
            act_send(a, c, &r);
        } else if (u->out.n == 0) {
            make_head(&r, c, 200, "text/html; charset=UTF-8", 0, NULL);   /* 空输出按 200 处理 */
            act_send(a, c, &r);
        } else {
            cgi_make_response(c, &u->out, &r);      /* 与 CGI 模式完全一致；r 窃取 out 免复制 */
            act_send(a, c, &r);
        }
    }
    free(u->out.p); free(u->err.p);
    memset(&u->out, 0, sizeof u->out);
    memset(&u->err, 0, sizeof u->err);
    u->appStatus = u->protoStatus = 0;
    nc = fcgi_pop_wait_locked();
    if (nc) fcgi_start_locked(u, nc, a);            /* 失败会走 fcgi_dead_locked 善后 */
}

/* ---- IOCP 完成回调（工作线程，完成键 = 3） ---- */
static void fcgi_on_io(FcgiUp *u, int type, BOOL ok, DWORD n) {
    FcgiAct a;
    if (!u) return;
    memset(&a, 0, sizeof a);
    EnterCriticalSection(&g_fcgiLock);
    if (u->dead) {                                 /* 已被定时器/前一次完成判定死亡 */
        LeaveCriticalSection(&g_fcgiLock);
        fcgi_free_up(u);
        return;
    }
    if (type == OP_FSEND) {
        if (!ok) {
            fcgi_dead_locked(u, &a, 1, 0, NULL);
        } else {
            if (fcgi_send_done(u, n) < 0) fcgi_dead_locked(u, &a, 1, 0, NULL);
        }
    } else { /* OP_FRECV */
        if (!ok || n == 0) {
            fcgi_dead_locked(u, &a, 1, 0, NULL);   /* EOF / 错误 */
        } else {
            int why = 0;
            int r = fcgi_feed(&u->pp, &u->out, &u->err, &u->appStatus, &u->protoStatus,
                              u->rb, n, g_cfg.cgiMaxOut, &why);
            if (r < 0) {
                if (why == 1) {
                    char msg[96];
                    snprintf(msg, sizeof msg, "PHP 输出超过 %u MB，已被终止。", (unsigned)(g_cfg.cgiMaxOut >> 20));
                    fcgi_dead_locked(u, &a, 0, 500, msg);
                } else
                    fcgi_dead_locked(u, &a, 0, 502, "FastCGI 协议错误（上游返回了无法解析的数据）。");
            } else if (r == 1) {
                fcgi_finish_locked(u, &a);
            } else {
                WSABUF rb;
                DWORD rn = 0, fl = 0;
                rb.buf = u->rb; rb.len = FCGI_RECV_CAP;
                u->op.type = OP_FRECV;
                memset(&u->op.ov, 0, sizeof u->op.ov);
                if (!(WSARecv(u->s, &rb, 1, &rn, &fl, &u->op.ov, NULL) == 0 ||
                      WSAGetLastError() == WSA_IO_PENDING))
                    fcgi_dead_locked(u, &a, 1, 0, NULL);
            }
        }
    }
    LeaveCriticalSection(&g_fcgiLock);
    fcgi_run_act(&a);
}
/* ---- 延迟动作执行（锁外）：回复客户端 / 回收资源 / 补进程 ---- */
static void fcgi_run_act(FcgiAct *a) {
    if (a->freeup) {
        fcgi_free_up(a->freeup);
        a->freeup = NULL;
    }
    while (a->freew) {
        FcgiWorker *w = a->freew;
        a->freew = w->next;
        if (w->proc) {
            TerminateProcess(w->proc, 1);
            CloseHandle(w->proc);
        }
        free(w);
    }
    while (a->head) {
        FcgiItem *it = a->head;
        a->head = it->next;
        if (!it->resp.p || !it->c) {
            if (it->c) close_conn(it->c);
        } else {
            it->c->sbuf = it->resp.p;
            it->c->slen = (int)it->resp.n;
            it->c->scap = (int)it->resp.cap;
            it->c->sofs = 0;
            post_send(it->c);          /* 立即失败时 post_send 内部会关闭连接 */
        }
        free(it);
    }
    if (a->oomc) close_conn(a->oomc);
    if (a->want_spawn) fcgi_spawn_async();
}

/* ---- 进程启动 ---- */
/* g_spawning 的递减遍布所有提前返回路径（QueueUserWorkItem 失败/连接失败/
   CIOP 挂接失败/分配失败/正常完成），新增退出路径时勿漏 */
static void fcgi_spawn_async(void) {
    InterlockedIncrement(&g_spawning);
    if (!QueueUserWorkItem(fcgi_spawn_thread, NULL, WT_EXECUTELONGFUNCTION)) {
        InterlockedDecrement(&g_spawning);
        InterlockedIncrement(&g_fcgiFails);
    }
}

/* 先占一个临时套接字拿到空闲端口再关闭，把端口交给 php-cgi 绑定。
   本机回环上这个窗口极小，即使撞上也会在连接阶段失败并换端口重试。 */
static int fcgi_pick_port(void) {
    struct sockaddr_in a, g;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int port = 0, gl = (int)sizeof g;
    if (s == INVALID_SOCKET) return 0;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7F000001);
    a.sin_port = 0;
    if (bind(s, (struct sockaddr *)&a, sizeof a) == 0 &&
        getsockname(s, (struct sockaddr *)&g, &gl) == 0)
        port = ntohs(g.sin_port);
    closesocket(s);
    return port;
}

/* 子进程环境块：继承父进程 + PHP_FCGI_MAX_REQUESTS=0。
   php-cgi 的 FastCGI 监听模式默认处理 500 个请求后自退（PHP 源码 fastcgi.c 的
   max_requests 默认值），进程池场景必须禁用；用户已设置该变量时不覆盖。
   返回 NULL = 直接用继承环境（不传块）。只在 fcgi_init（单线程）调用一次。 */
static wchar_t *fcgi_child_env(void) {
    wchar_t *inh = GetEnvironmentStringsW(), *blk = NULL;
    size_t total = 0, add;
    if (!inh) return NULL;
    if (GetEnvironmentVariableW(L"PHP_FCGI_MAX_REQUESTS", NULL, 0) != 0) {
        FreeEnvironmentStringsW(inh);                     /* 用户已设置：尊重用户 */
        return NULL;
    }
    for (wchar_t *e = inh; *e; e += wcslen(e) + 1) total += wcslen(e) + 1;
    add = wcslen(L"PHP_FCGI_MAX_REQUESTS=0") + 1;
    blk = (wchar_t *)malloc((total + add + 1) * sizeof(wchar_t));
    if (blk) {
        memcpy(blk, inh, total * sizeof(wchar_t));
        memcpy(blk + total, L"PHP_FCGI_MAX_REQUESTS=0", add * sizeof(wchar_t));
        blk[total + add] = 0;                            /* 环境块末尾的双 '\0' */
    }
    FreeEnvironmentStringsW(inh);
    return blk;
}

static DWORD WINAPI fcgi_spawn_thread(LPVOID unused) {
    HANDLE proc = NULL;
    SOCKET s = INVALID_SOCKET;
    int port = 0, ok = 0, att;
    wchar_t cmd[PCAP * 2];

    (void)unused;
    for (att = 0; att < FCGI_SPAWN_ATTEMPTS && !ok; att++) {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        int t;

        port = fcgi_pick_port();
        if (!port) break;
        memset(&si, 0, sizeof si);
        memset(&pi, 0, sizeof pi);
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        _snwprintf(cmd, PCAP * 2 - 1, L"\"%ls\" -b 127.0.0.1:%d -c \"%ls\"",
                   g_phpExe, port, g_phpDir);
        cmd[PCAP * 2 - 1] = 0;
        if (!CreateProcessW(g_phpExe, cmd, NULL, NULL, FALSE,
                            CREATE_NO_WINDOW | (g_fcgiEnv ? CREATE_UNICODE_ENVIRONMENT : 0),
                            g_fcgiEnv, g_phpDir, &si, &pi)) {
            Sleep(150);
            continue;
        }
        CloseHandle(pi.hThread);
        if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
        if (proc) { TerminateProcess(proc, 1); CloseHandle(proc); }   /* 上一次尝试的残骸 */
        proc = pi.hProcess;

        /* 非阻塞连接 + select 轮询，等 php-cgi 真正开始监听（v2.2 重写）：
           Windows 上“连接被拒”经 exceptfds 报告，writefds 永远不会就绪；
           旧版只监听 writefds → php-cgi 启动稍慢（杀毒扫描/冷启动）就白等整个
           超时窗口后换端口重来，进程池永远起不来。现在两路都听、被拒就
           重建套接字立即重连，php-cgi 一监听成功即收敛（回环上被拒→重连约 1ms 一轮）。 */
        for (t = 0; t < g_cfg.fcgiSpawnMs && !ok; t += 100) {
            fd_set w, e;
            struct timeval tv;
            int se, err = 0, el = sizeof err;
            if (s == INVALID_SOCKET) {
                u_long nb = 1;
                struct sockaddr_in a;
                s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
                if (s == INVALID_SOCKET) break;
                ioctlsocket(s, FIONBIO, &nb);
                memset(&a, 0, sizeof a);
                a.sin_family = AF_INET;
                a.sin_addr.s_addr = htonl(0x7F000001);
                a.sin_port = htons((u_short)port);
                if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) { ok = 1; break; }
                if (WSAGetLastError() != WSAEWOULDBLOCK) {
                    closesocket(s);
                    s = INVALID_SOCKET;
                    break;
                }
            }
            if (WaitForSingleObject(proc, 0) == WAIT_OBJECT_0) break;  /* php-cgi 启动即退 */
            FD_ZERO(&w); FD_ZERO(&e);
            FD_SET(s, &w); FD_SET(s, &e);
            tv.tv_sec = 0;
            tv.tv_usec = 100 * 1000;
            se = select(0, NULL, &w, &e, &tv);
            if (se > 0) {
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &el);
                if (err == 0 && !FD_ISSET(s, &e)) { ok = 1; break; }    /* 连上了 */
                closesocket(s);                       /* 被拒：套接字作废，下轮重建重连 */
                s = INVALID_SOCKET;
            }
            /* se == 0：100ms 内无结果，继续等 */
        }
        if (!ok) {
            if (s != INVALID_SOCKET) { closesocket(s); s = INVALID_SOCKET; }
            Sleep(150);
        }
    }

    if (!ok) {
        if (proc) { TerminateProcess(proc, 1); CloseHandle(proc); }
        InterlockedIncrement(&g_fcgiFails);
        InterlockedDecrement(&g_spawning);
        return 0;
    }
    {
        u_long blkmode = 0;
        int one = 1;
        FcgiWorker *w;
        FcgiUp *u;
        FcgiAct a;
        Conn *nc;
        /* 恢复阻塞模式：IOCP + 阻塞套接字是标准组合；非阻塞模式下
           overlapped 收发偶尔会直接回 WSAEWOULDBLOCK 而不是挂起 */
        ioctlsocket(s, FIONBIO, &blkmode);
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        if (!CreateIoCompletionPort((HANDLE)s, g_iocp, 3, 0)) {
            closesocket(s);
            TerminateProcess(proc, 1);
            CloseHandle(proc);
            InterlockedIncrement(&g_fcgiFails);
            InterlockedDecrement(&g_spawning);
            return 0;
        }
        w = (FcgiWorker *)calloc(1, sizeof *w);
        u = (FcgiUp *)calloc(1, sizeof *u);
        if (!w || !u || !(u->rb = (char *)malloc(FCGI_RECV_CAP))) {
            if (u) { u->s = s; fcgi_free_up(u); } else closesocket(s);
            free(w);
            TerminateProcess(proc, 1);
            CloseHandle(proc);
            InterlockedIncrement(&g_fcgiFails);
            InterlockedDecrement(&g_spawning);
            return 0;
        }
        w->proc = proc;
        w->port = port;
        w->up = u;
        u->w = w;
        u->s = s;
        u->idleSince = GetTickCount();
        u->op.u = u;

        memset(&a, 0, sizeof a);
        EnterCriticalSection(&g_fcgiLock);
        w->next = g_fcgiWorkers;
        if (w->next) w->next->prev = w;
        g_fcgiWorkers = w;
        InterlockedIncrement(&g_fcgiN);
        InterlockedExchange(&g_fcgiFails, 0);
        nc = fcgi_pop_wait_locked();                    /* 有排队请求立即开跑 */
        if (nc) fcgi_start_locked(u, nc, &a);
        if (need_spawn_locked()) a.want_spawn = 1;      /* 队列没排完继续补 */
        LeaveCriticalSection(&g_fcgiLock);
        fcgi_run_act(&a);
    }
    InterlockedDecrement(&g_spawning);
    return 0;
}

/* ---- 派发：请求体已收齐 → 上 FastCGI 池 ---- */
static void fcgi_attach(Conn *c) {
    FcgiAct a;
    FcgiUp *u = NULL;
    FcgiWorker *w;

    c->state = ST_CGI;                                  /* 等价 CGI 等待期：客户端无挂起操作 */
    c->last = GetTickCount();
    memset(&a, 0, sizeof a);
    EnterCriticalSection(&g_fcgiLock);
    if (!g_fcgiOk) {                                    /* 已回退 CGI（不应走到，防御） */
        LeaveCriticalSection(&g_fcgiLock);
        {
            Buf r = bp_get();
            put_page(&r, c, 502, "Bad Gateway", "FastCGI 进程池不可用。");
            if (r.p) {
                c->sbuf = r.p; c->slen = (int)r.n; c->scap = (int)r.cap; c->sofs = 0;
                post_send(c);
                return;
            }
            close_conn(c);
            return;
        }
    }
    for (w = g_fcgiWorkers; w; w = w->next)
        if (w->up && !w->up->busy && !w->up->dead) { u = w->up; break; }
    if (u) {
        fcgi_start_locked(u, c, &a);
    } else {
        fcgi_push_wait_locked(c->fr, 0);                /* 全忙 → FIFO 排队 */
        if (need_spawn_locked()) a.want_spawn = 1;      /* 且未到上限 → 扩容 */
    }
    LeaveCriticalSection(&g_fcgiLock);
    fcgi_run_act(&a);
}

/* ---- 定时器（timer_thread 每 2 秒调一次；v2.2：一次处理所有到期项） ---- */
static void fcgi_timer(void) {
    FcgiAct a;
    DWORD now = GetTickCount();
    FcgiWorker *w;

    if (!g_fcgiOk && !g_fcgiWorkers && !g_fcgiWaitHead) return;
    memset(&a, 0, sizeof a);
    EnterCriticalSection(&g_fcgiLock);

    /* 1) 执行中的请求超时：杀进程 + 504（全部到期项一次处理；
          旧版每 tick 只杀一个，多个脚本失控时恢复要拖很久） */
    for (w = g_fcgiWorkers; w; ) {
        FcgiWorker *wn = w->next;
        FcgiUp *u = w->up;
        if (u && u->busy && !u->dead && now - u->started > (DWORD)g_cfg.cgiTimeoutMs) {
            Conn *c = u->c;
            u->dead = 1;
            u->c = NULL;
            if (c) c->up = NULL;
            u->busy = 0;
            InterlockedDecrement(&g_fcgiBusy);
            if (w->prev) w->prev->next = w->next; else g_fcgiWorkers = w->next;
            if (w->next) w->next->prev = w->prev;
            act_freew(&a, w);
            InterlockedDecrement(&g_fcgiN);
            if (c) {
                Buf r = bp_get();
                char msg[96];
                snprintf(msg, sizeof msg, "PHP 脚本执行超过 %d 秒，已被终止。", g_cfg.cgiTimeoutMs / 1000);
                put_page(&r, c, 504, "Gateway Timeout", msg);
                act_send(&a, c, &r);
            }
            /* 只取消 I/O，socket 延迟到完成包处理后再关，避免句柄复用错乱。
               CancelIoEx 必须在锁内：u 的释放由随后的完成包路径（fcgi_on_io）执行，
               挪到锁外则可能晚于 fcgi_on_io 对 u 的释放，变成 use-after-free */
            CancelIoEx((HANDLE)u->s, NULL);
        }
        w = wn;
    }

    /* 2) 排队超时：504（全部到期项一次处理，单次遍历摘除） */
    {
        FcgiReq *fr = g_fcgiWaitHead, *next;
        while (fr) {
            next = fr->qnext;                     /* 先存 next：remove 会改写链 */
            if (now - fr->queued > (DWORD)g_cfg.cgiTimeoutMs) {
                Conn *c = fr->c;
                fcgi_remove_wait_locked(fr);
                if (c) {
                    Buf r = bp_get();
                    put_page(&r, c, 504, "Gateway Timeout", "FastCGI 进程池繁忙，请求等待超时。");
                    act_send(&a, c, &r);
                }
            }
            fr = next;
        }
    }

    /* 3) 空闲缩容（超出常驻数的空闲进程，每 tick 温和回收一个） */
    {
        int n = 0;
        for (w = g_fcgiWorkers; w; w = w->next) n++;
        if (n > g_cfg.fcgiMin) {
            for (w = g_fcgiWorkers; w; w = w->next) {
                FcgiUp *u = w->up;
                if (u && !u->busy && !u->dead && now - u->idleSince > (DWORD)g_cfg.fcgiIdleMs) {
                    if (w->prev) w->prev->next = w->next; else g_fcgiWorkers = w->next;
                    if (w->next) w->next->prev = w->prev;
                    act_freew(&a, w);                      /* 锁外 TerminateProcess */
                    a.freeup = u;                          /* 空闲连接无挂起 I/O，直接释放 */
                    InterlockedDecrement(&g_fcgiN);
                    break;
                }
            }
        }
    }

    /* 4) 补进程：低于常驻数，或有排队且未到上限（超时杀掉的进程立即补齐） */
    if (need_spawn_locked()) a.want_spawn = 1;

    /* 5) 兜底：池彻底起不来 → 回退逐请求 CGI，排队中的请求直接 502 */
    if (g_fcgiOk && g_fcgiN == 0 && g_spawning == 0 && g_fcgiFails >= 4) {
        g_fcgiOk = 0;
        while (g_fcgiWaitHead) {
            FcgiReq *fr = g_fcgiWaitHead;
            Conn *c = fr->c;
            fcgi_remove_wait_locked(fr);
            if (c) {
                Buf r = bp_get();
                put_page(&r, c, 502, "Bad Gateway",
                         "FastCGI 进程池无法启动，已自动回退 CGI 模式，请重新发起请求。");
                act_send(&a, c, &r);
            }
        }
    }

    LeaveCriticalSection(&g_fcgiLock);
    fcgi_run_act(&a);
}

/* 客户端在 FastCGI 执行期间断开（close_conn 调用）：放弃该请求并杀掉正忙的上游。
   只在 g_fcgiLock 内调用；u 本身不在此释放，交给 CancelIoEx 触发的
   完成包（fcgi_on_io 看到 dead 后 fcgi_free_up），与超时路径同一套善后。 */
static void fcgi_cancel_locked(FcgiUp *u, FcgiAct *a) {
    FcgiWorker *w = u->w;
    u->dead = 1;
    u->c = NULL;
    if (u->busy) { u->busy = 0; InterlockedDecrement(&g_fcgiBusy); }
    if (w) {
        if (w->prev) w->prev->next = w->next; else g_fcgiWorkers = w->next;
        if (w->next) w->next->prev = w->prev;
        act_freew(a, w);
        InterlockedDecrement(&g_fcgiN);
    }
    CancelIoEx((HANDLE)u->s, NULL);
    if (need_spawn_locked()) a->want_spawn = 1;
}

/* ---- PARAMS 构建（UTF-8 字节流，一次编码完成） ---- */
static char *w2u(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *p;
    if (n <= 0) return NULL;
    p = (char *)malloc((size_t)n);
    if (!p) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, p, n, NULL, NULL);
    return p;
}

/* 返回 malloc 的 [BEGIN_REQUEST][PARAMS...][空PARAMS][空STDIN(仅无请求体时)]，
   *outLen 为长度；失败返回 NULL。语义与 build_cgi_env 完全对应。
   v2.2：直接迭代一次解析好的 Hdrs；去重表动态增长（旧版 48 条封顶，
   超限后新头仍会发送但不再去重，与注释意图相悳）。 */
static char *build_fcgi_params(Conn *c, const Hdrs *hs, const char *method,
                               const char *script_url, const char *path_info,
                               const char *query, const char *uri,
                               const wchar_t *script, long long blen, size_t *outLen) {
    Buf nv = {0}, out = {0}, seen = {0};
    char host[256], ct[256], addr[96] = "", port[16] = "", t[32];
    char *sf = NULL;
    unsigned char bh[8], bc[8] = { 0, FCGI_RESPONDER, FCGI_KEEP_CONN, 0, 0, 0, 0, 0 };
    unsigned char ph[8];
    size_t off;
    int i;

    if (!fcgi_put_nv(&nv, "GATEWAY_INTERFACE", "FastCGI/1.0", 14) ||
        !fcgi_put_nv(&nv, "SERVER_SOFTWARE", SERVER_TOKEN, strlen(SERVER_TOKEN)) ||
        !fcgi_put_nv(&nv, "SERVER_PROTOCOL", "HTTP/1.1", 8) ||
        !fcgi_put_nv(&nv, "REQUEST_SCHEME", "http", 4) ||
        !fcgi_put_nv(&nv, "REDIRECT_STATUS", "200", 3) ||
        !fcgi_put_nv(&nv, "REQUEST_METHOD", method, strlen(method)) ||
        !fcgi_put_nv(&nv, "REQUEST_URI", uri, strlen(uri)) ||
        !fcgi_put_nv(&nv, "QUERY_STRING", query, strlen(query)) ||
        !fcgi_put_nv(&nv, "SCRIPT_NAME", script_url, strlen(script_url)))
        goto fail;

    host[0] = 0;
    hdrs_str(hs, "Host", host, sizeof host);
    {
        char *e;
        if (host[0] == '[') { e = strchr(host, ']'); if (e) e[1] = 0; }
        else { e = strchr(host, ':'); if (e) *e = 0; }
    }
    if (!host[0]) strcpy(host, "localhost");
    if (!fcgi_put_nv(&nv, "SERVER_NAME", host, strlen(host))) goto fail;
    snprintf(t, sizeof t, "%d", g_port);
    if (!fcgi_put_nv(&nv, "SERVER_PORT", t, strlen(t))) goto fail;

    sf = w2u(script);
    if (!sf || !fcgi_put_nv(&nv, "SCRIPT_FILENAME", sf, strlen(sf))) goto fail;
    if (!fcgi_put_nv(&nv, "DOCUMENT_ROOT", g_rootUtf8, strlen(g_rootUtf8))) goto fail;
    if (path_info[0] && !fcgi_put_nv(&nv, "PATH_INFO", path_info, strlen(path_info))) goto fail;

    {
        struct sockaddr_storage ss;
        int sl = (int)sizeof ss;
        if (getpeername(c->s, (struct sockaddr *)&ss, &sl) == 0 &&
            getnameinfo((struct sockaddr *)&ss, sl, addr, sizeof addr, port, sizeof port,
                        NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
            if (!fcgi_put_nv(&nv, "REMOTE_ADDR", addr, strlen(addr)) ||
                !fcgi_put_nv(&nv, "REMOTE_PORT", port, strlen(port))) goto fail;
        }
    }
    if (hdrs_str(hs, "Content-Type", ct, sizeof ct) &&
        !fcgi_put_nv(&nv, "CONTENT_TYPE", ct, strlen(ct))) goto fail;
    if (blen > 0 || !strcmp(method, "POST")) {
        snprintf(t, sizeof t, "%lld", blen);
        if (!fcgi_put_nv(&nv, "CONTENT_LENGTH", t, strlen(t))) goto fail;
    }

    /* 请求头 → HTTP_*：与 CGI 模式同规则（去重、拦 Proxy/下划线/非 ASCII）。
       seen 为 "NAME\0NAME\0…" 的动态表，无条数上限 */
    for (i = 0; i < hs->n; i++) {
        const char *p = hs->e[i].name, *v = hs->e[i].val;
        size_t nl = hs->e[i].nlen, vl = hs->e[i].vlen, k = 5, j;
        int good = nl < 120, dup = 0;
        char name[160];
        if (good) {
            memcpy(name, "HTTP_", 5);
            for (j = 0; j < nl; j++) {
                unsigned char ch = (unsigned char)p[j];
                if (ch == '_' || ch <= 32 || ch >= 127 || ch == '=') { good = 0; break; }
                name[k++] = (ch == '-') ? '_' : (char)toupper(ch);
            }
            name[k] = 0;
        }
        if (good && strcmp(name, "HTTP_CONTENT_TYPE") && strcmp(name, "HTTP_CONTENT_LENGTH") &&
            strcmp(name, "HTTP_PROXY")) {
            char val[1024];
            while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
            if (vl >= sizeof val) vl = sizeof val - 1;
            memcpy(val, v, vl);
            val[vl] = 0;
            {   /* 动态去重表 */
                const char *q = seen.p, *end = seen.p + seen.n;
                size_t nml = strlen(name);
                while (q < end) {
                    size_t l = strlen(q);
                    if (l == nml && !strcmp(q, name)) { dup = 1; break; }
                    q += l + 1;
                }
            }
            if (!dup) {
                b_add(&seen, name, strlen(name) + 1);
                if (!fcgi_put_nv(&nv, name, val, vl)) goto fail;
            }
        }
    }

    if (!nv.p) goto fail;

    /* BEGIN_REQUEST 记录 */
    fcgi_hdr(bh, FCGI_BEGIN_REQUEST, 8);
    b_add(&out, (const char *)bh, 8);
    b_add(&out, (const char *)bc, 8);
    /* PARAMS 记录（请求头有 8KB 上限，参数总量必然远小于单记录上限，此处防御性分片） */
    off = 0;
    do {
        size_t k = nv.n - off;
        if (k > FCGI_MAX_RECORD) k = FCGI_MAX_RECORD;
        fcgi_hdr(ph, FCGI_PARAMS, (int)k);
        b_add(&out, (const char *)ph, 8);
        if (k) b_add(&out, nv.p + off, k);
        off += k;
    } while (off < nv.n);
    fcgi_hdr(ph, FCGI_PARAMS, 0);                       /* 空 PARAMS = 参数结束 */
    b_add(&out, (const char *)ph, 8);
    if (blen <= 0) {                                    /* 无请求体：空 STDIN 一并带上，
                                                           典型 GET 一次 WSASend 全发完 */
        fcgi_hdr(ph, FCGI_STDIN, 0);
        b_add(&out, (const char *)ph, 8);
    }
    free(nv.p);
    free(sf);
    free(seen.p);
    if (!out.p) return NULL;
    *outLen = out.n;
    return out.p;

fail:
    free(nv.p);
    free(out.p);
    free(sf);
    free(seen.p);
    return NULL;
}

/* ---- 客户端入口（与 start_cgi 同构） ---- */
static void fcgi_abort(Conn *c, int code) {
    if (c->fr) { fcgi_free_req(c->fr); c->fr = NULL; }
    free(c->body);
    c->body = NULL;
    c->blen = c->bgot = 0; c->bcap = 0;
    if (code == 400 || code == 411 || code == 413) c->keep = 0;   /* 请求体没读完，连接不可复用 */
    reply_error(c, code, NULL);
}

static void start_fastcgi(Conn *c, const Hdrs *hs, const char *method, const wchar_t *script,
                          const char *script_url, const char *path_info, const char *query,
                          const char *uri) {
    long long blen = php_precheck(c, hs, method);
    char *params;
    size_t plen;
    FcgiReq *fr;

    if (blen < 0) return;                               /* 已回复错误 */
    if (!dir_inside(script)) { if (blen > 0) c->keep = 0; reply_error(c, 403, NULL); return; }

    params = build_fcgi_params(c, hs, method, script_url, path_info, query, uri, script, blen, &plen);
    if (!params) { if (blen > 0) c->keep = 0; reply_error(c, 500, NULL); return; }
    fr = (FcgiReq *)calloc(1, sizeof *fr);
    if (!fr) { free(params); if (blen > 0) c->keep = 0; reply_error(c, 500, NULL); return; }
    fr->c = c;
    fr->params = params;
    fr->plen = plen;
    c->fr = fr;
    c->blen = blen;
    c->bgot = 0;

    if (blen > 0) {
        size_t avail, take;
        avail = (size_t)(c->have - c->hend);            /* 请求头之后已收到的就是请求体开头 */
        take = avail < (size_t)blen ? avail : (size_t)blen;
        if (take && !body_reserve(c, take)) { fcgi_abort(c, 500); return; }
        if (take) memcpy(c->body, c->reqbuf + c->hend, take);
        c->bgot = (long long)take;
        c->hend += (int)take;
        if (c->bgot < blen) {
            const HEnt *ex = hdrs_get(hs, "Expect");
            if (ex) {
                char hv[64];
                size_t cl = ex->vlen < sizeof hv - 1 ? ex->vlen : sizeof hv - 1;
                memcpy(hv, ex->val, cl);
                hv[cl] = 0;
                for (char *q = hv; *q; q++) *q = (char)tolower((unsigned char)*q);
                if (strstr(hv, "100-continue")) send(c->s, "HTTP/1.1 100 Continue\r\n\r\n", 25, 0);
            }
            post_body(c);
            return;
        }
    }
    fcgi_attach(c);
}

/* PHP 统一入口：FastCGI 优先，CGI 兜底 */
static void start_php(Conn *c, const Hdrs *hs, const char *method, const wchar_t *script,
                      const char *script_url, const char *path_info, const char *query,
                      const char *uri) {
    if (g_fcgiOk)
        start_fastcgi(c, hs, method, script, script_url, path_info, query, uri);
    else
        start_cgi(c, hs, method, script, script_url, path_info, query, uri);
}

/* 请求体收齐后的统一分发 */
static void dispatch_php(Conn *c) {
    if (g_fcgiOk) fcgi_attach(c);
    else dispatch_cgi(c);
}

/* 在 IOCP 引擎就绪后调用（需要 g_iocp） */
static void fcgi_init(void) {
    int i;
    if (!g_phpOk) return;
    InitializeCriticalSection(&g_fcgiLock);
    g_fcgiEnv = fcgi_child_env();       /* 禁用 php-cgi 默认的 500 请求自退 */
    WideCharToMultiByte(CP_UTF8, 0, g_root, -1, g_rootUtf8, sizeof g_rootUtf8, NULL, NULL);
    g_fcgiOk = 1;                       /* 池起不来时由 fcgi_timer 自动回退 CGI */
    for (i = 0; i < g_cfg.fcgiMin; i++) fcgi_spawn_async();
}

/* ---------------- 请求处理：必须以"投递了一个操作 / 关闭了连接"结束 ---------------- */
/* 缺省主页列表在 g_cfg.index（minihttpd.ini 的 index 键，默认 4 项；
   目录下的 index.php 在此之前单独优先检查） */

/* 非 GET/HEAD 的请求携带的请求体不会被读取，所以出错时必须关闭连接 */
static void fail_req(Conn *c, int is_gh, int code, const char *extra) {
    if (!is_gh) c->keep = 0;
    reply_error(c, code, extra);
}

static void handle_request(Conn *c, int hend) {
    char method[16], target[2048], uri[2048], ver[16] = "", dec[2048], conn[64], sd[2048], su[2200];
    char ims[64];
    const char *req = c->reqbuf, *query = "";
    wchar_t wp[PCAP], tmp[PCAP], ws[PCAP];
    Hdrs hs;
    const char *hstart;
    int n, is_gh;
    DWORD at;
    SYSTEMTIME st;

    InterlockedIncrement(&g_reqs);
    c->head = 0; c->range[0] = 0; c->inm[0] = 0; c->inr[0] = 0; c->imsOk = 0;
    n = sscanf(req, "%15s %2047s %15s", method, target, ver);
    c->keep = strcmp(ver, "HTTP/1.0") != 0;
    if (n < 2) { c->keep = 0; reply_error(c, 400, NULL); return; }
    if (strlen(target) >= sizeof target - 1) { c->keep = 0; reply_error(c, 414, NULL); return; }

    /* 头部一次解析（跳过请求行；条目指向 reqbuf，全程有界不拷贝）。
       超过 128 个头直接 431，避免后续查找漏头造成请求走私 */
    hstart = (const char *)memchr(req, '\n', (size_t)hend);
    if (!parse_headers(&hs, hstart ? hstart + 1 : req, req + hend - 4)) {
        c->keep = 0; reply_error(c, 431, NULL); return;
    }

    if (hdrs_str(&hs, "Connection", conn, sizeof conn)) {
        for (char *p = conn; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (strstr(conn, "close")) c->keep = 0;
        else if (strstr(conn, "keep-alive")) c->keep = 1;
    }

    c->head = !strcmp(method, "HEAD");
    is_gh = c->head || !strcmp(method, "GET");
    if (target[0] != '/') { c->keep = 0; reply_error(c, 400, NULL); return; }

    { char *h = strchr(target, '#'); if (h) *h = 0; }
    snprintf(uri, sizeof uri, "%s", target);                  /* 完整 URI（含查询串），给 REQUEST_URI 用 */
    { char *q = strchr(target, '?'); if (q) { query = uri + (q - target) + 1; *q = 0; } }

    hdrs_str(&hs, "Range", c->range, sizeof c->range);
    hdrs_str(&hs, "If-None-Match", c->inm, sizeof c->inm);
    hdrs_str(&hs, "If-Range", c->inr, sizeof c->inr);
    if (hdrs_str(&hs, "If-Modified-Since", ims, sizeof ims) && parse_http_date(ims, &st)) {
        c->imsOk = 1;
        SystemTimeToFileTime(&st, &c->imsFt);
    }
    if (is_gh) {                                              /* GET/HEAD 带了请求体：不读取它，所以不能复用连接 */
        const HEnt *te = hdrs_get(&hs, "Transfer-Encoding");
        const HEnt *cl = hdrs_get(&hs, "Content-Length");
        if (te || (cl && !(cl->vlen == 1 && cl->val[0] == '0'))) c->keep = 0;
    }

    if (!resolve(target, wp, PCAP, dec, sizeof dec)) { fail_req(c, is_gh, 403, NULL); return; }
    at = GetFileAttributesW(wp);

    if (g_phpOk) {
        const char *pe;
        DWORD a2;
        if (in_php_dir(wp)) { fail_req(c, is_gh, 403, NULL); return; }       /* php\ 目录本身不对外开放 */

        /* 1) 直接请求 .php 文件 */
        if (at != INVALID_FILE_ATTRIBUTES && !(at & FILE_ATTRIBUTE_DIRECTORY) && ends_with_php(dec)) {
            start_php(c, &hs, method, wp, dec, "", query, uri);
            return;
        }
        /* 2) script.php/path/info：脚本存在，其后的部分作为 PATH_INFO */
        if (at == INVALID_FILE_ATTRIBUTES) {
            const char *from = dec;
            for (int tries = 0; tries < 8 && (pe = find_php_end(dec, from)) != NULL; tries++) {
                size_t sl = (size_t)(pe - dec);
                from = pe;
                if (*pe != '/' || sl >= sizeof sd) continue;
                memcpy(sd, dec, sl);
                sd[sl] = 0;
                if (!resolve_dec(sd, ws, PCAP)) continue;
                a2 = GetFileAttributesW(ws);
                if (a2 != INVALID_FILE_ATTRIBUTES && !(a2 & FILE_ATTRIBUTE_DIRECTORY)) {
                    if (in_php_dir(ws)) { fail_req(c, is_gh, 403, NULL); return; }
                    start_php(c, &hs, method, ws, sd, pe, query, uri);
                    return;
                }
            }
        }
        /* 3) 伪静态（rewrite）：不存在的路径交给根目录的 index.php */
        if (at == INVALID_FILE_ATTRIBUTES && g_rewrite) {
            join(ws, PCAP, g_root, L"index.php");
            a2 = GetFileAttributesW(ws);
            if (a2 != INVALID_FILE_ATTRIBUTES && !(a2 & FILE_ATTRIBUTE_DIRECTORY)) {
                start_php(c, &hs, method, ws, "/index.php", "", query, uri);
                return;
            }
        }
    }

    if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY)) {
        if (!dir_inside(wp)) { fail_req(c, is_gh, 403, NULL); return; }
        if (target[strlen(target) - 1] != '/') {              /* /dir -> /dir/ */
            char loc[2200];
            if (!is_gh) { c->keep = 0; reply_error(c, 405, "Allow: GET, HEAD\r\n"); return; }
            const char *t = target;
            while (*t == '/' || *t == '\\') t++;                /* 合并开头的多个斜杠，防止 //host 被浏览器当成外站 */
            snprintf(loc, sizeof loc, "Location: /%s/\r\n", t);
            reply_buf(c, 301, "text/plain", "", 0, loc);
            return;
        }
        if (g_phpOk) {                                        /* 目录下有 index.php 就执行它（优先于 index.html） */
            join(tmp, PCAP, wp, L"index.php");
            DWORD a = GetFileAttributesW(tmp);
            if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
                snprintf(su, sizeof su, "%sindex.php", dec);
                start_php(c, &hs, method, tmp, su, "", query, uri);
                return;
            }
        }
        if (!is_gh) { c->keep = 0; reply_error(c, 405, "Allow: GET, HEAD\r\n"); return; }
        for (int i = 0; i < g_cfg.indexN; i++) {              /* 缺省主页 */
            join(tmp, PCAP, wp, g_cfg.index[i]);
            DWORD a = GetFileAttributesW(tmp);
            if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
                if (!serve_file(c, tmp, 200)) reply_error(c, 403, NULL);
                return;
            }
        }
        if (!g_cfg.dirlist) { fail_req(c, is_gh, 403, NULL); return; }  /* dirlist=0：无缺省主页时 403（同 nginx autoindex off） */
        list_dir(c, wp, dec);
        return;
    }

    if (!is_gh) { c->keep = 0; reply_error(c, 405, "Allow: GET, HEAD\r\n"); return; }
    if (at == INVALID_FILE_ATTRIBUTES) { send_404(c); return; }
    if (!serve_file(c, wp, 200)) reply_error(c, 403, NULL);
}

/* ---------------- 请求缓冲区处理（支持流水线） ---------------- */
static void process_buffer(Conn *c) {
    c->reqbuf[c->have] = 0;                     /* NUL 兜底（sscanf / 局部拷贝的终点） */
    char *e = strstr(c->reqbuf, "\r\n\r\n");
    if (!e) {
        if (c->have >= RECV_CAP - 1) { c->keep = 0; c->head = 0; reply_error(c, 431, NULL); }
        else post_recv(c);
        return;
    }
    c->hend = (int)(e - c->reqbuf) + 4;
    handle_request(c, c->hend);                 /* v2.2：原地解析，免掉旧版整个头部的 8KB 拷贝 */
}

static void response_done(Conn *c) {
    bp_put(c->sbuf, (size_t)c->scap);           /* 小块回发送缓冲池，大块直接 free */
    c->sbuf = NULL;
    c->slen = c->sofs = 0; c->scap = 0;
    free(c->body); c->body = NULL; c->blen = c->bgot = 0; c->bcap = 0;
    if (c->cgi) { free_job(c->cgi); c->cgi = NULL; }
    if (c->fr) { fcgi_free_req(c->fr); c->fr = NULL; }
    if (c->file != INVALID_HANDLE_VALUE) { CloseHandle(c->file); c->file = INVALID_HANDLE_VALUE; }
    if (!c->keep) { close_conn(c); return; }
    memmove(c->reqbuf, c->reqbuf + c->hend, (size_t)(c->have - c->hend));
    c->have -= c->hend;
    c->hend = 0;
    c->rstart = GetTickCount();                 /* 下一个请求的头部总时限起点 */
    process_buffer(c);                                        /* 缓冲区里可能已有下一个请求 */
}

/* ---------------- accept ---------------- */
static int post_accept(int lidx) {
    Conn *c = (Conn *)calloc(1, sizeof *c);
    if (!c) return 0;
    c->file = INVALID_HANDLE_VALUE;
    c->lidx = lidx;
    c->state = ST_ACCEPT;
    c->s = WSASocketW(lidx == 0 ? AF_INET : AF_INET6, SOCK_STREAM, IPPROTO_TCP,
                      NULL, 0, WSA_FLAG_OVERLAPPED);
    if (c->s == INVALID_SOCKET) { free(c); return 0; }
    CreateIoCompletionPort((HANDLE)c->s, g_iocp, 2, 0);
    c->op.c = c;
    c->op.type = OP_ACCEPT;
    DWORD got = 0;
    if (!AcceptEx(g_ls[lidx], c->s, c->accbuf, 0,
                  sizeof(struct sockaddr_in6) + 16, sizeof(struct sockaddr_in6) + 16,
                  &got, &c->op.ov) && WSAGetLastError() != ERROR_IO_PENDING) {
        closesocket(c->s);
        free(c);
        return 0;
    }
    return 1;
}

static void on_accept(Conn *c, BOOL ok) {
    int lidx = c->lidx;
    if (!post_accept(lidx)) InterlockedIncrement(&g_missing[lidx]);   /* 先补一个，保持队列满 */
    if (!ok) { closesocket(c->s); free(c); return; }
    setsockopt(c->s, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (const char *)&g_ls[lidx], sizeof(SOCKET));
    if (g_nconn >= g_cfg.maxconn) { close_conn(c); return; }
    {
        int one = 1;
        setsockopt(c->s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    }
    c->have = 0;
    link_conn(c);
    c->rstart = GetTickCount();          /* 首个请求的头部总时限起点 */
    post_recv(c);
}

/* ---------------- 工作线程 / 定时线程 ---------------- */
static DWORD WINAPI worker_thread(LPVOID unused) {
    (void)unused;
    for (;;) {
        DWORD n = 0;
        ULONG_PTR key = 0;
        OVERLAPPED *ov = NULL;
        BOOL ok = GetQueuedCompletionStatus(g_iocp, &n, &key, &ov, INFINITE);
        if (!ov) { if (!ok) Sleep(10); continue; }
        if (key == 3) {                     /* FastCGI 上游 I/O 完成包 */
            FOp *fop = (FOp *)ov;
            fcgi_on_io(fop->u, fop->type, ok, n);
            continue;
        }
        Op *op = (Op *)ov;
        Conn *c = op->c;
        c->last = GetTickCount();
        switch (op->type) {
        case OP_ACCEPT:
            on_accept(c, ok);
            break;
        case OP_RECV:
            if (!ok || n == 0) close_conn(c);
            else { c->have += (int)n; process_buffer(c); }
            break;
        case OP_SEND:
            if (!ok || n == 0) close_conn(c);
            else {
                c->sofs += (int)n;
                if (c->sofs < c->slen) post_send(c);
                else response_done(c);
            }
            break;
        case OP_BODY:
            if (!ok || n == 0) close_conn(c);
            else { c->bgot += n; if (c->bgot < c->blen) post_body(c); else dispatch_php(c); }
            break;
        case OP_FILE:
            if (!ok) close_conn(c);
            else {
                c->fstart += c->chunk;
                c->fleft -= c->chunk;
                if (c->fleft > 0) post_file(c);
                else response_done(c);
            }
            break;
        }
    }
    return 0;
}

static DWORD WINAPI timer_thread(LPVOID unused) {
    (void)unused;
    for (;;) {
        Sleep(2000);
        DWORD now = GetTickCount();
        EnterCriticalSection(&g_lock);
        for (Conn *c = g_head; c; c = c->next) {
            DWORD lim = (c->state == ST_RECV || c->state == ST_BODY) ? g_cfg.idleRecvMs : g_cfg.idleSendMs;
            /* 空闲超时，或 slowloris：请求头必须在总时限内收完
               （每收一字节就刷新空闲计时挡不住慢速滴字攻击） */
            if ((now - c->last > lim) ||
                (c->state == ST_RECV && now - c->rstart > (DWORD)g_cfg.reqTotalMs))
                CancelIoEx((HANDLE)c->s, NULL);  /* 挂起操作以 ABORTED 完成 -> 关闭 */
        }
        LeaveCriticalSection(&g_lock);
        fcgi_timer();                               /* FastCGI：超时/缩容/补进程/CGI 回退 */
        for (int i = 0; i < 2; i++)                           /* 补回失败的 accept */
            while (g_ls[i] != INVALID_SOCKET && g_missing[i] > 0 && post_accept(i))
                InterlockedDecrement(&g_missing[i]);
    }
    return 0;
}

/* ---------------- 监听 + 引擎启动 ---------------- */
/* IPv4 必须成功；同端口的 IPv6 尽力而为（避免 localhost 先走 ::1 被拒绝而拖慢） */
static int start_server(void) {
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w)) return 0;
    for (int p = g_port; p < g_port + 20 && p < 65536; p++) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return 0;
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(g_lan ? INADDR_ANY : INADDR_LOOPBACK);
        a.sin_port = htons((u_short)p);
        if (bind(s, (struct sockaddr *)&a, sizeof a) == 0 && listen(s, SOMAXCONN) == 0) {
            g_ls[0] = s;
            g_port = p;
            SOCKET s6 = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
            if (s6 != INVALID_SOCKET) {
                DWORD on = 1;
                struct sockaddr_in6 a6;
                memset(&a6, 0, sizeof a6);
                setsockopt(s6, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&on, sizeof on);
                a6.sin6_family = AF_INET6;
                a6.sin6_port = htons((u_short)p);
                if (!g_lan) a6.sin6_addr.s6_addr[15] = 1;     /* ::1 */
                if (bind(s6, (struct sockaddr *)&a6, sizeof a6) == 0 && listen(s6, SOMAXCONN) == 0)
                    g_ls[1] = s6;
                else
                    closesocket(s6);
            }
            return 1;
        }
        closesocket(s);
    }
    return 0;
}

static int engine_start(void) {
    SYSTEM_INFO si;
    int nw;
    GetSystemInfo(&si);
    if (g_cfg.threads > 0) {
        nw = g_cfg.threads;              /* 配置文件指定线程数（已校验 1..256） */
    } else {
        /* IOCP 工作线程只处理完成包 + 非阻塞投递，核数 + 2 即可
           （旧版核数×2，多核机器上线程过多上下文切换开销反而增加） */
        nw = (int)si.dwNumberOfProcessors + 2;
        if (nw < 4) nw = 4;
        if (nw > 24) nw = 24;
    }

    InitializeCriticalSection(&g_lock);
    InitializeSListHead(&g_bp);                  /* 发送缓冲池（单线程阶段初始化） */
    g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!g_iocp) return 0;
    for (int i = 0; i < 2; i++)
        if (g_ls[i] != INVALID_SOCKET) CreateIoCompletionPort((HANDLE)g_ls[i], g_iocp, 1, 0);
    for (int i = 0; i < nw; i++) {
        HANDLE t = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    int posted = 0;
    for (int i = 0; i < 2; i++) {
        if (g_ls[i] == INVALID_SOCKET) continue;
        for (int k = 0; k < ACCEPTS_PER_LISTENER; k++) if (post_accept(i)) posted++;
    }
    if (!posted) return 0;
    {
        HANDLE t = CreateThread(NULL, 0, timer_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return 1;
}

/* ---------------- 窗口 + 托盘 ---------------- */
#define ID_SHOW      3
#define ID_BTN_OPEN  101
#define ID_BTN_EXIT  102

#define CLR_BG       RGB(255, 255, 255)
#define CLR_TEXT     RGB(40, 44, 52)
#define CLR_SUBTEXT  RGB(110, 116, 128)
#define CLR_LINE     RGB(230, 232, 236)
#define CLR_OK       RGB(39, 201, 63)
#define CLR_HDR_TEXT RGB(255, 255, 255)
#define CLR_HDR_SUB  RGB(205, 222, 250)

static HFONT  g_fontTitle, g_fontBody, g_fontSmall;
static HBRUSH g_brBg;
static HICON  g_appIcon;
static HWND   g_hTitle, g_hSub, g_hAddr, g_hRoot, g_hScope, g_hStat, g_hPhp, g_hCfg;
static int    g_dpi = 96;
static UINT   g_wmTaskbar;                  /* Explorer 重启后会广播 TaskbarCreated，需要重新添加托盘图标 */

#define S(v) MulDiv((v), g_dpi, 96)         /* 逻辑像素 -> 实际像素（按系统缩放比例） */

static void open_browser(void) {
    wchar_t url[64];
    _snwprintf(url, 63, L"http://localhost:%d/", g_port);
    url[63] = 0;
    ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
}

static void show_window(HWND h) {
    ShowWindow(h, SW_RESTORE);
    SetForegroundWindow(h);
}

static void tray_add(HWND h) {
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = h;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_appIcon;
    _snwprintf(nid.szTip, 127, L"MiniHttpd v" WIDE(APP_VER_SHORT) L" - port %d", g_port);
    Shell_NotifyIconW(NIM_ADD, &nid);
}

/* 参数是 96 DPI 下的逻辑坐标，这里统一按系统缩放放大 */
static HWND mk(HWND p, const wchar_t *cls, const wchar_t *txt, DWORD style,
               int x, int y, int w, int h, int id, HFONT font) {
    HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                             p, (HMENU)(UINT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_wmTaskbar && m == g_wmTaskbar) { tray_add(h); return 0; }
    switch (m) {
    case WM_CREATE: {
        LOGFONTW lf;
        memset(&lf, 0, sizeof lf);
        lf.lfHeight = -S(17); lf.lfWeight = FW_SEMIBOLD; lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy(lf.lfFaceName, L"Microsoft YaHei UI");
        g_fontTitle = CreateFontIndirectW(&lf);
        lf.lfHeight = -S(14); lf.lfWeight = FW_NORMAL;
        g_fontBody = CreateFontIndirectW(&lf);
        lf.lfHeight = -S(13);
        g_fontSmall = CreateFontIndirectW(&lf);
        g_brBg = CreateSolidBrush(CLR_BG);

        wchar_t t[PCAP + 32];
        g_hTitle = mk(h, L"STATIC", L"MiniHttpd  v" WIDE(APP_VER_SHORT), SS_LEFT, 24, 18, 240, 26, 0, g_fontTitle);
        g_hSub   = mk(h, L"STATIC", L"静态文件服务器 · IOCP 运行中", SS_LEFT, 24, 46, 240, 18, 0, g_fontSmall);

        int y = 96;
        mk(h, L"STATIC", L"地址", SS_LEFT, 24, y, 50, 18, 0, g_fontSmall);
        _snwprintf(t, PCAP + 31, L"http://localhost:%d/", g_port);
        g_hAddr = mk(h, L"STATIC", t, SS_LEFT, 90, y, 260, 18, 0, g_fontBody);

        y += 28;
        mk(h, L"STATIC", L"目录", SS_LEFT, 24, y, 50, 18, 0, g_fontSmall);
        g_hRoot = mk(h, L"STATIC", g_root, SS_LEFT | SS_PATHELLIPSIS, 90, y, 266, 18, 0, g_fontBody);

        y += 28;
        mk(h, L"STATIC", L"范围", SS_LEFT, 24, y, 50, 18, 0, g_fontSmall);
        g_hScope = mk(h, L"STATIC", g_lan ? (g_phpOk ? L"局域网可访问 · 含 PHP" : L"局域网可访问") : L"仅本机访问",
                      SS_LEFT, 90, y, 260, 18, 0, g_fontBody);

        y += 28;
        mk(h, L"STATIC", L"状态", SS_LEFT, 24, y, 50, 18, 0, g_fontSmall);
        g_hStat = mk(h, L"STATIC", L"连接 0  ·  请求 0", SS_LEFT, 90, y, 260, 18, 0, g_fontBody);

        y += 28;
        mk(h, L"STATIC", L"PHP", SS_LEFT, 24, y, 50, 18, 0, g_fontSmall);
        g_hPhp = mk(h, L"STATIC",
                    g_fcgiOk ? (g_phpIni ? L"已启用 (FastCGI)" : L"已启用 (FastCGI) · 未找到 php.ini")
                    : g_phpOk ? (g_phpIni ? L"已启用 (CGI)" : L"已启用 (CGI) · 未找到 php.ini")
                              : L"未启用（把 PHP 放进 php 子目录）",
                    SS_LEFT, 90, y, 270, 18, 0, g_fontBody);

        y += 28;
        mk(h, L"STATIC", L"配置", SS_LEFT, 24, y, 50, 18, 0, g_fontSmall);
        {
            wchar_t cm[96];
            cfg_summary_msg(cm, 96);
            g_hCfg = mk(h, L"STATIC", cm, SS_LEFT, 90, y, 260, 18, 0, g_fontBody);
        }

        mk(h, L"BUTTON", L"打开浏览器", BS_PUSHBUTTON | WS_TABSTOP, 24, 280, 150, 32, ID_BTN_OPEN, g_fontBody);
        mk(h, L"BUTTON", L"退出服务", BS_PUSHBUTTON | WS_TABSTOP, 198, 280, 150, 32, ID_BTN_EXIT, g_fontBody);
        SetTimer(h, 1, 500, NULL);
        return 0;
    }
    case WM_TIMER: {
        static LONG lc = -1, lr = -1, fw = -1, fb = -1, fq = -1;
        LONG c1 = g_nconn, r1 = g_reqs;
        if (c1 != lc || r1 != lr) {
            wchar_t t[64];
            _snwprintf(t, 63, L"连接 %ld  ·  请求 %ld", c1, r1);
            t[63] = 0;
            SetWindowTextW(g_hStat, t);
            lc = c1; lr = r1;
        }
        if (g_phpOk) {                                /* FastCGI 进程池实时状态 */
            LONG w1 = g_fcgiN, b1 = g_fcgiBusy, q1 = g_fcgiQ;
            if (!g_fcgiOk) {
                if (fw != -2) { SetWindowTextW(g_hPhp, L"FastCGI 故障 · 已回退 CGI 模式"); fw = -2; }
            } else if (w1 != fw || b1 != fb || q1 != fq) {
                wchar_t t[96];
                if (!w1 && g_spawning)
                    _snwprintf(t, 95, L"FastCGI 启动中…");
                else
                    _snwprintf(t, 95, L"FastCGI · %ld 进程 · %ld 忙%ls", w1, b1,
                               q1 ? L" · 排队中" : L"");
                t[95] = 0;
                SetWindowTextW(g_hPhp, t);
                fw = w1; fb = b1; fq = q1;
            }
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        HWND ctl = (HWND)l;
        if (ctl == g_hTitle || ctl == g_hSub) {                /* 标题栏渐变上的文字：白色、透明背景 */
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, ctl == g_hTitle ? CLR_HDR_TEXT : CLR_HDR_SUB);
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }
        SetBkColor(dc, CLR_BG);
        SetTextColor(dc, (ctl == g_hAddr || ctl == g_hRoot || ctl == g_hScope || ctl == g_hStat || ctl == g_hPhp || ctl == g_hCfg)
                          ? CLR_TEXT : CLR_SUBTEXT);
        return (LRESULT)g_brBg;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);

        int hdr = S(78), ico = S(36), mgn = S(24);
        for (int i = 0; i < hdr; i++) {
            double t = (double)i / hdr;
            int r = (int)(44 + (30 - 44) * t), g = (int)(98 + (70 - 98) * t), b = (int)(224 + (170 - 224) * t);
            HBRUSH br = CreateSolidBrush(RGB(r, g, b));
            RECT line = {0, i, rc.right, i + 1};
            FillRect(dc, &line, br);
            DeleteObject(br);
        }
        if (g_appIcon) DrawIconEx(dc, rc.right - mgn - ico, S(21), g_appIcon, ico, ico, 0, NULL, DI_NORMAL);

        RECT body = {0, hdr, rc.right, rc.bottom};
        FillRect(dc, &body, g_brBg);

        HPEN pen = CreatePen(PS_SOLID, 1, CLR_LINE);
        HPEN old = (HPEN)SelectObject(dc, pen);
        MoveToEx(dc, mgn, S(262), NULL);
        LineTo(dc, rc.right - mgn, S(262));
        SelectObject(dc, old);
        DeleteObject(pen);

        HBRUSH dot = CreateSolidBrush(CLR_OK);
        HBRUSH oldb = (HBRUSH)SelectObject(dc, dot);
        HPEN oldp = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, rc.right - mgn - ico - S(16), S(21) + S(12), rc.right - mgn - ico - S(4), S(21) + S(24));
        SelectObject(dc, oldb);
        SelectObject(dc, oldp);
        DeleteObject(dot);

        EndPaint(h, &ps);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == ID_BTN_OPEN) open_browser();
        else if (LOWORD(w) == ID_BTN_EXIT) DestroyWindow(h);
        return 0;
    case WM_TRAY:
        if (l == WM_RBUTTONUP || l == WM_CONTEXTMENU) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, ID_SHOW, L"显示窗口");
            AppendMenuW(menu, MF_STRING, ID_OPEN, L"在浏览器中打开");
            AppendMenuW(menu, MF_STRING, ID_EXIT, L"退出");
            SetForegroundWindow(h);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, h, NULL);
            DestroyMenu(menu);
            PostMessageW(h, WM_NULL, 0, 0);                    /* 让托盘菜单在点击别处时正常收起 */
            if (cmd == ID_SHOW) show_window(h);
            else if (cmd == ID_OPEN) open_browser();
            else if (cmd == ID_EXIT) DestroyWindow(h);
        } else if (l == WM_LBUTTONDBLCLK) {
            show_window(h);
        }
        return 0;
    case WM_DESTROY: {
        NOTIFYICONDATAW nid;
        KillTimer(h, 1);
        memset(&nid, 0, sizeof nid);
        nid.cbSize = sizeof nid;
        nid.hWnd = h;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show) {
    (void)hp; (void)show;
    GetModuleFileNameW(NULL, g_exeDir, PCAP);
    {
        wchar_t *sl = wcsrchr(g_exeDir, L'\\');
        if (sl) *sl = 0;
    }
    {   /* 展开为长路径（v2.2 起）：否则从 8.3 短名目录（如 C:\PROGRA~1\）运行时，
           请求路径基于短名根目录构建，而 g_phpDir 已长化，两者比较必然失败，
           php\ 目录保护（禁止下载 php.ini 等）会完全失效 */
        wchar_t lp[PCAP];
        DWORD ln = GetLongPathNameW(g_exeDir, lp, PCAP);
        if (ln > 0 && ln < PCAP) wcscpy(g_exeDir, lp);
    }
    wcscpy(g_root, g_exeDir);       /* 默认网站根目录 = exe 目录（可被 minihttpd.ini 的 root 覆盖） */

    cfg_load();                     /* v2.3：读 exe 同目录 minihttpd.ini；缺失/无效项用默认值兜底 */
    cfg_resolve_root();             /* 应用配置的根目录（无效则回退 exe 目录） */
    g_port = g_cfg.port;
    g_lan = g_cfg.lan;
    g_follow = g_cfg.follow;
    g_rewrite = g_cfg.rewrite;
    g_nophp = g_cfg.nophp;

    if (cmd) {                      /* 命令行参数：优先级高于配置文件 */
        const char *p = cmd;
        while (*p && !isdigit((unsigned char)*p)) p++;
        if (*p) { int v = atoi(p); if (v > 0 && v < 65536) g_port = v; }
        if (strstr(cmd, "lan")) g_lan = 1;
        if (strstr(cmd, "nobrowser")) g_cfg.nobrowser = 1;
        if (strstr(cmd, "min")) g_cfg.startmin = 1;
        if (strstr(cmd, "follow")) g_follow = 1;
        if (strstr(cmd, "rewrite")) g_rewrite = 1;
        if (strstr(cmd, "nophp")) g_nophp = 1;
    }

    init_real_root();
    php_init();

    if (!start_server() || !engine_start()) {
        MessageBoxW(NULL, L"无法启动服务：端口范围已被占用，或 IOCP 初始化失败。",
                    L"MiniHttpd", MB_ICONERROR);
        return 1;
    }
    fcgi_init();                                    /* IOCP 就绪后再拉起 FastCGI 进程池 */

    {
        HDC sdc = GetDC(NULL);                                 /* 系统 DPI（manifest 声明为系统级 DPI 感知） */
        if (sdc) { int d = GetDeviceCaps(sdc, LOGPIXELSX); if (d >= 96) g_dpi = d; ReleaseDC(NULL, sdc); }
    }
    g_appIcon = LoadIconW(hi, MAKEINTRESOURCEW(IDI_ICON1));
    if (!g_appIcon) g_appIcon = LoadIconW(NULL, MAKEINTRESOURCEW(32512));
    g_wmTaskbar = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.lpszClassName = L"MiniHttpdWnd";
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    wc.hIcon = g_appIcon;
    wc.hbrBackground = NULL;
    RegisterClassW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = {0, 0, S(372), S(328)};
    AdjustWindowRect(&rc, style, FALSE);
    HWND hw = CreateWindowExW(0, wc.lpszClassName, L"MiniHttpd v" WIDE(APP_VER_SHORT), style,
                              CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                              NULL, NULL, hi, NULL);
    if (!hw) {
        MessageBoxW(NULL, L"无法创建主窗口。", L"MiniHttpd", MB_ICONERROR);
        return 1;
    }
    SendMessageW(hw, WM_SETICON, ICON_SMALL, (LPARAM)g_appIcon);
    SendMessageW(hw, WM_SETICON, ICON_BIG, (LPARAM)g_appIcon);

    tray_add(hw);

    ShowWindow(hw, g_cfg.startmin ? SW_SHOWMINNOACTIVE : SW_SHOW);
    UpdateWindow(hw);
    if (!g_cfg.nobrowser) open_browser();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hw, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
