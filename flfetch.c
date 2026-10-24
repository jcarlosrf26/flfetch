/*
 * flfetch - el fetch de FLinux (sucesor de neofetch, descontinuado)
 *
 * Ficha del sistema estilo neofetch con el logo ASCII de FLinux y,
 * a diferencia de neofetch, CPU/RAM/Swap EN VIVO estilo htop:
 * la pantalla se queda abierta y las barras se mueven cada segundo.
 *
 * Pensado para FLinux / Tiny Core (i686): un solo binario C con
 * ncurses, sin dependencias pesadas. Teclas: q = salir, r = refrescar.
 * Modo de un disparo: flfetch --once (o sin terminal) imprime la
 * ficha estatica y sale, como el neofetch clasico.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <pwd.h>
#include <time.h>
#include <locale.h>
#include <langinfo.h>
#include <signal.h>
#include <sys/utsname.h>
#include <sys/ioctl.h>
#include <ncurses.h>

/* ------------------------------------------------------------------ */
/*  Logo: banner ASCII «FLINUX» del arranque de FLinux                 */
/* ------------------------------------------------------------------ */
static const char *LOGO[] = {
    " $$$$$$$$\\ $$\\       $$\\",
    " $$  _____|$$ |      \\__|",
    " $$ |      $$ |      $$\\ $$$$$$$$\\  $$\\   $$\\ $$\\   $$\\",
    " $$$$$\\    $$ |      $$ |$$  __$$\\ $$ |  $$ |\\$$\\ $$  |",
    " $$  __|   $$ |      $$ |$$ |  $$ |$$ |  $$ | \\$$$$  /",
    " $$ |      $$ |      $$ |$$ |  $$ |$$ |  $$ | $$  $$<",
    " $$ |      $$$$$$$$\\ $$ |$$ |  $$ |\\$$$$$$  |$$  /\\$$\\",
};
#define LOGO_H 7
#define LOGO_W 55

/* Marco del icono oficial: borde + franja amarilla (5) + hueco + logo */
#define SPINE_W 5
#define FRAME_W (LOGO_W + 9)          /* 1 borde + 5 franja + 1 + 55 + 1 + 1 */
#define COL_GAP 2                     /* hueco entre marco y ficha       */
#define MIN_COLS (FRAME_W + COL_GAP + 40)
#define MIN_ROWS 21

/* ------------------------------------------------------------------ */
/*  Datos del sistema                                                  */
/* ------------------------------------------------------------------ */
typedef struct {
    char user[64];
    char host[128];
    char os[160];          /* "FLinux 17.2 i686" */
    char kernel[96];
    char uptime[96];
    long uptime_secs;
    char shell[64];
    char res[48];
    char wm[64];
    char term[64];
    char cpu[192];         /* modelo (nucleos) @ GHz */
    int  has_temp;
    int  temp_c;
    double cpu_pct;        /* -1 si aun no hay muestra */
    unsigned long long mem_total, mem_used;
    unsigned long long swp_total, swp_used;
} info_t;

typedef struct { unsigned long long total, idle; } cput_t;

static int g_utf8 = 0;     /* la locale activa es UTF-8 */

static const char *S_o(void)    { return g_utf8 ? "\xc3\xb3" : "\xf3"; }        /* ó */
static const char *S_mid(void)  { return g_utf8 ? "\xc2\xb7" : "\xb7"; }        /* · */
static const char *S_lq(void)   { return g_utf8 ? "\xc2\xab" : "\xab"; }        /* « */
static const char *S_rq(void)   { return g_utf8 ? "\xc2\xbb" : "\xbb"; }        /* » */
static const char *S_deg(void)  { return g_utf8 ? "\xc2\xb0" : "\xb0"; }        /* ° */

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t'))
        s[--n] = 0;
    memmove(s, s + strspn(s, " \t"), strlen(s) + 1 - strspn(s, " \t"));
}

static int read_file(const char *path, char *buf, size_t sz)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sz - 1, f);
    fclose(f);
    buf[n] = 0;
    return 0;
}

/* busca "clave: valor" o "clave=valor" (con o sin comillas) en un texto */
static int grab(const char *text, const char *key, char sep,
                char *out, size_t sz)
{
    size_t klen = strlen(key);
    const char *p = text;
    while (p && *p) {
        const char *q = p + klen;
        while (*q == ' ' || *q == '\t') q++;
        if (strncmp(p, key, klen) == 0 && *q == sep) {
            p = q + 1;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '"') p++;
            size_t i = 0;
            while (*p && *p != '\n' && *p != '"' && i < sz - 1)
                out[i++] = *p++;
            out[i] = 0;
            trim(out);
            return 0;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return -1;
}

static cput_t read_cpu(void)
{
    cput_t c = {0, 0};
    char buf[512];
    if (read_file("/proc/stat", buf, sizeof buf) == 0) {
        unsigned long long v[10] = {0};
        char *p = strstr(buf, "cpu ");
        if (p) {
            int n = sscanf(p + 4, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                           &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
            for (int i = 0; i < n; i++) c.total += v[i];
            c.idle = v[3] + v[4];
        }
    }
    return c;
}

static double cpu_pct_between(cput_t a, cput_t b)
{
    unsigned long long dt = b.total - a.total;
    unsigned long long di = b.idle - a.idle;
    if (dt == 0) return 0.0;
    double p = 100.0 * (double)(dt - di) / (double)dt;
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    return p;
}

static void fmt_uptime(long secs, char *out, size_t sz)
{
    long d = secs / 86400, h = (secs % 86400) / 3600, m = (secs % 3600) / 60;
    const char *ia = g_utf8 ? "\xc3\xad" : "\xed";   /* la i de dias */
    if (d > 0)
        snprintf(out, sz, "%ld d%sa%s, %ld hora%s, %ld minuto%s",
                 d, ia, d == 1 ? "" : "s", h, h == 1 ? "" : "s",
                 m, m == 1 ? "" : "s");
    else if (h > 0)
        snprintf(out, sz, "%ld hora%s, %ld minuto%s",
                 h, h == 1 ? "" : "s", m, m == 1 ? "" : "s");
    else
        snprintf(out, sz, "%ld minuto%s", m, m == 1 ? "" : "s");
}

/* memoria en formato legible: "1.3 GiB" o "512 MiB" */
static void fmt_mem(unsigned long long kb, char *out, size_t sz)
{
    if (kb >= 1048576ULL)
        snprintf(out, sz, "%.1f GiB", kb / 1048576.0);
    else
        snprintf(out, sz, "%llu MiB", kb / 1024ULL);
}

static void get_resolution(info_t *in)
{
    strcpy(in->res, "-");
    /* 1) xdpyinfo si hay DISPLAY */
    if (getenv("DISPLAY")) {
        FILE *p = popen("xdpyinfo 2>/dev/null", "r");
        if (p) {
            char line[256];
            while (fgets(line, sizeof line, p)) {
                char *d = strstr(line, "dimensions:");
                if (d) {
                    int w, h;
                    if (sscanf(d + 11, "%dx%d", &w, &h) == 2 && w > 0) {
                        snprintf(in->res, sizeof in->res, "%dx%d", w, h);
                        break;
                    }
                }
            }
            pclose(p);
            if (strcmp(in->res, "-") != 0) return;
        }
        /* 2) la linea -screen de Xvesa en ~/.xsession */
        const char *home = getenv("HOME");
        if (home) {
            char path[512], buf[8192];
            snprintf(path, sizeof path, "%s/.xsession", home);
            if (read_file(path, buf, sizeof buf) == 0) {
                char *s = strstr(buf, "-screen ");
                if (s) {
                    int w, h, b;
                    if (sscanf(s + 8, "%dx%dx%d", &w, &h, &b) >= 2 && w > 0) {
                        snprintf(in->res, sizeof in->res, "%dx%d", w, h);
                        return;
                    }
                }
            }
        }
    }
    /* 3) framebuffer */
    char buf[64];
    if (read_file("/sys/class/graphics/fb0/virtual_size", buf, sizeof buf) == 0) {
        int w, h;
        if (sscanf(buf, "%d,%d", &w, &h) == 2 && w > 0)
            snprintf(in->res, sizeof in->res, "%dx%d", w, h);
    }
}

static int get_temp(void)
{
    char type[128], val[64], path[256];
    int fallback = -1;
    for (int i = 0; i < 10; i++) {
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/type", i);
        if (read_file(path, type, sizeof type) != 0) continue;
        trim(type);
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", i);
        if (read_file(path, val, sizeof val) != 0) continue;
        long milli = atol(val);
        if (milli <= 0 || milli > 150000) continue;
        int c = (int)(milli / 1000);
        if (strstr(type, "cpu") || strstr(type, "coretemp") ||
            strstr(type, "x86_pkg") || strstr(type, "core"))
            return c;
        if (fallback < 0) fallback = c;
    }
    /* hwmon: temp1_input */
    for (int i = 0; i < 6 && fallback < 0; i++) {
        snprintf(path, sizeof path, "/sys/class/hwmon/hwmon%d/temp1_input", i);
        if (read_file(path, val, sizeof val) == 0) {
            long milli = atol(val);
            if (milli > 0 && milli <= 150000) fallback = (int)(milli / 1000);
        }
    }
    return fallback;
}

static void collect_static(info_t *in)
{
    struct utsname un;
    uname(&un);

    const char *u = getenv("USER");
    if (!u || !*u) {
        struct passwd *pw = getpwuid(getuid());
        u = pw ? pw->pw_name : "tc";
    }
    snprintf(in->user, sizeof in->user, "%s", u);
    if (gethostname(in->host, sizeof in->host) != 0)
        snprintf(in->host, sizeof in->host, "%s", un.nodename);

    /* SO: /etc/os-release (FLinux trae NAME=FLinux VERSION_ID=17.2) */
    char buf[4096], name[96] = "", ver[64] = "";
    if (read_file("/etc/os-release", buf, sizeof buf) == 0) {
        grab(buf, "NAME", '=', name, sizeof name);
        grab(buf, "VERSION_ID", '=', ver, sizeof ver);
    }
    if (!name[0]) strcpy(name, "FLinux");
    snprintf(in->os, sizeof in->os, "%s%s%s %s",
             name, ver[0] ? " " : "", ver, un.machine);

    snprintf(in->kernel, sizeof in->kernel, "%s", un.release);

    const char *sh = getenv("SHELL");
    if (!sh || !*sh) sh = "/bin/sh";
    const char *base = strrchr(sh, '/');
    snprintf(in->shell, sizeof in->shell, "%s", base ? base + 1 : sh);

    get_resolution(in);

    /* WM: /etc/sysconfig/desktop */
    char wm[64] = "";
    if (read_file("/etc/sysconfig/desktop", wm, sizeof wm) == 0) trim(wm);
    if      (!strcasecmp(wm, "flwm"))    strcpy(in->wm, "FLWM");
    else if (!strcasecmp(wm, "openbox")) strcpy(in->wm, "Openbox");
    else if (!strcasecmp(wm, "icewm"))   strcpy(in->wm, "IceWM");
    else if (!strcasecmp(wm, "jwm"))     strcpy(in->wm, "JWM");
    else if (wm[0]) {
        snprintf(in->wm, sizeof in->wm, "%s", wm);
        in->wm[0] = toupper((unsigned char)in->wm[0]);
    } else strcpy(in->wm, "-");

    const char *t = getenv("TERM");
    snprintf(in->term, sizeof in->term, "%s", (t && *t) ? t : "-");

    /* CPU: modelo, nucleos y frecuencia desde /proc/cpuinfo */
    char ci[65536];
    char model[128] = "";
    int cores = 0;
    double mhz = 0;
    if (read_file("/proc/cpuinfo", ci, sizeof ci) == 0) {
        grab(ci, "model name", ':', model, sizeof model);
        char *p = ci;
        while ((p = strstr(p, "processor")) != NULL) { cores++; p += 9; }
        char mhzs[32] = "";
        if (grab(ci, "cpu MHz", ':', mhzs, sizeof mhzs) == 0)
            mhz = atof(mhzs);
        /* colapsa espacios dobles del modelo */
        char clean[128]; int j = 0, sp = 0;
        for (int i = 0; model[i] && j < (int)sizeof clean - 1; i++) {
            if (model[i] == ' ' || model[i] == '\t') { if (sp) continue; sp = 1; }
            else sp = 0;
            clean[j++] = model[i];
        }
        clean[j] = 0; trim(clean);
        snprintf(model, sizeof model, "%s", clean);
    }
    if (!model[0]) strcpy(model, un.machine);
    if (cores <= 0) cores = 1;
    if (mhz > 0)
        snprintf(in->cpu, sizeof in->cpu, "%s (%d) @ %.2f GHz", model, cores, mhz / 1000.0);
    else
        snprintf(in->cpu, sizeof in->cpu, "%s (%d)", model, cores);

    in->has_temp = 0;
    in->temp_c = get_temp();
    if (in->temp_c >= 0) in->has_temp = 1;
}

static void collect_dynamic(info_t *in)
{
    char buf[2048];
    /* uptime */
    if (read_file("/proc/uptime", buf, sizeof buf) == 0) {
        double up = atof(buf);
        in->uptime_secs = (long)up;
        fmt_uptime(in->uptime_secs, in->uptime, sizeof in->uptime);
    }
    /* memoria y swap */
    unsigned long long mt = 0, ma = 0, st = 0, sf = 0;
    if (read_file("/proc/meminfo", buf, sizeof buf) == 0) {
        char tmp[64];
        if (grab(buf, "MemTotal", ':', tmp, sizeof tmp) == 0)  mt = strtoull(tmp, NULL, 10);
        if (grab(buf, "MemAvailable", ':', tmp, sizeof tmp) == 0) ma = strtoull(tmp, NULL, 10);
        if (grab(buf, "SwapTotal", ':', tmp, sizeof tmp) == 0) st = strtoull(tmp, NULL, 10);
        if (grab(buf, "SwapFree", ':', tmp, sizeof tmp) == 0)  sf = strtoull(tmp, NULL, 10);
    }
    in->mem_total = mt;
    in->mem_used = (mt >= ma) ? mt - ma : 0;
    in->swp_total = st;
    in->swp_used = (st >= sf) ? st - sf : 0;
    /* temperatura (puede variar) */
    if (in->has_temp) {
        int tc = get_temp();
        if (tc >= 0) in->temp_c = tc;
    }
}

/* ------------------------------------------------------------------ */
/*  Interfaz en vivo (ncurses)                                         */
/* ------------------------------------------------------------------ */
enum {
    CP_LABEL = 1, CP_VALUE, CP_USER, CP_HOST, CP_SEP, CP_LOGO, CP_DIM,
    CP_HEAD, CP_LIVE, CP_DOT, CP_BORD, CP_TOPBAR, CP_SPINE, CP_SQIN, CP_EMPTY,
    CP_GRAD = 30,          /* 30..39 degradado de barras (fondo) */
    CP_PAL  = 44           /* 44..59 paleta (<= 63: rxvt solo da 64 pares) */
};

static int g_colors = 0;   /* la terminal admite colores */
static int g_c256 = 0;     /* la terminal admite 256 colores */

static void init_colors(void)
{
    g_colors = has_colors();
    if (!g_colors) return;
    start_color();
    int defbg = (use_default_colors() == OK) ? -1 : COLOR_BLACK;
    g_c256 = (COLORS >= 256);

    if (g_c256) {
        init_pair(CP_LABEL,  208, defbg);
        init_pair(CP_VALUE,  255, defbg);
        init_pair(CP_USER,   208, defbg);
        init_pair(CP_HOST,   220, defbg);
        init_pair(CP_SEP,    208, defbg);
        init_pair(CP_LOGO,   230, defbg);
        init_pair(CP_DIM,    242, defbg);
        init_pair(CP_HEAD,   220, defbg);
        init_pair(CP_LIVE,   196, defbg);
        init_pair(CP_DOT,    196, 196);
        init_pair(CP_BORD,   245, defbg);
        init_pair(CP_TOPBAR, 235, 246);
        init_pair(CP_SPINE,    0, 221);
        init_pair(CP_SQIN,     0, 230);
        init_pair(CP_EMPTY,  238, 238);
        static const short ramp[10] = {46, 82, 118, 154, 190, 226, 214, 208, 202, 196};
        for (int i = 0; i < 10; i++) init_pair(CP_GRAD + i, ramp[i], ramp[i]);
        static const short pal[16] = {235, 160, 40, 184, 33, 170, 44, 254,
                                      242, 203, 84, 227, 75, 213, 87, 15};
        for (int i = 0; i < 16; i++) init_pair(CP_PAL + i, pal[i], pal[i]);
    } else {
        init_pair(CP_LABEL,  COLOR_YELLOW, defbg);
        init_pair(CP_VALUE,  COLOR_WHITE,  defbg);
        init_pair(CP_USER,   COLOR_YELLOW, defbg);
        init_pair(CP_HOST,   COLOR_YELLOW, defbg);
        init_pair(CP_SEP,    COLOR_YELLOW, defbg);
        init_pair(CP_LOGO,   COLOR_WHITE,  defbg);
        init_pair(CP_DIM,    COLOR_WHITE,  defbg);
        init_pair(CP_HEAD,   COLOR_YELLOW, defbg);
        init_pair(CP_LIVE,   COLOR_RED,    defbg);
        init_pair(CP_DOT,    COLOR_RED,    COLOR_RED);
        init_pair(CP_BORD,   COLOR_WHITE,  defbg);
        init_pair(CP_TOPBAR, COLOR_BLACK,  COLOR_WHITE);
        init_pair(CP_SPINE,  COLOR_BLACK,  COLOR_YELLOW);
        init_pair(CP_SQIN,   COLOR_BLACK,  COLOR_WHITE);
        init_pair(CP_EMPTY,  COLOR_WHITE,  defbg);
        static const short ramp[10] = {COLOR_GREEN, COLOR_GREEN, COLOR_GREEN,
                                       COLOR_GREEN, COLOR_GREEN, COLOR_YELLOW,
                                       COLOR_YELLOW, COLOR_YELLOW, COLOR_RED, COLOR_RED};
        for (int i = 0; i < 10; i++) init_pair(CP_GRAD + i, ramp[i], ramp[i]);
        for (int i = 0; i < 16; i++) {
            short c = (short)(i % 8);
            if (COLORS >= 16 && i >= 8) c = (short)i;
            init_pair(CP_PAL + i, c, c);
        }
    }
}

/* pinta n celdas con un par (fondo) */
static void cells(WINDOW *w, int y, int x, int n, int pair)
{
    if (g_colors) wattron(w, COLOR_PAIR(pair));
    else if (pair == CP_SPINE || pair == CP_TOPBAR || pair >= CP_GRAD) wattron(w, A_REVERSE);
    wmove(w, y, x);
    for (int i = 0; i < n; i++) waddch(w, ' ');
    if (g_colors) wattroff(w, COLOR_PAIR(pair));
    else if (pair == CP_SPINE || pair == CP_TOPBAR || pair >= CP_GRAD) wattroff(w, A_REVERSE);
}

static void put_pair(WINDOW *w, int y, int x, const char *s, int pair, int bold)
{
    int at = bold ? A_BOLD : 0;
    if (g_colors) at |= COLOR_PAIR(pair);
    wattron(w, at);
    mvwaddstr(w, y, x, s);
    wattroff(w, at);
}

typedef struct {
    int frame_h;      /* alto del marco                      */
    int info_rows;    /* filas de la ficha (sin seccion vivo) */
    int live_row;     /* fila (en winR) de la cabecera EN VIVO */
    int right_w;      /* ancho de la columna derecha          */
    int bar_w;        /* ancho de las barras                  */
    int up_row;       /* fila del valor uptime                */
    int temp_row;     /* fila de la temperatura (-1 si no hay) */
    int val_x;        /* columna donde empiezan los valores   */
} layout_t;

static void compute_layout(const info_t *in, int cols, layout_t *L)
{
    L->info_rows = 10 + (in->has_temp ? 1 : 0);
    L->live_row = L->info_rows + 1;
    L->frame_h = L->live_row + 1 + 3 + 2;   /* cabecera + 3 barras + hueco + pista */
    L->right_w = cols - (FRAME_W + COL_GAP);
    L->up_row = 4;
    L->temp_row = in->has_temp ? 10 : -1;
    L->val_x = 0;                            /* los valores van tras la etiqueta */
    /* detalle mas largo de las barras */
    char d1[96], d2[96], a[32], b[32];
    fmt_mem(in->mem_used, a, sizeof a); fmt_mem(in->mem_total, b, sizeof b);
    snprintf(d1, sizeof d1, "%s / %s", a, b);
    fmt_mem(in->swp_used, a, sizeof a); fmt_mem(in->swp_total, b, sizeof b);
    snprintf(d2, sizeof d2, "%s / %s", a, b);
    int det = 5;                              /* "100%" */
    if ((int)strlen(d1) > det) det = (int)strlen(d1);
    if ((int)strlen(d2) > det) det = (int)strlen(d2);
    L->bar_w = L->right_w - (5 + 2 + 1 + det);
    if (L->bar_w < 10) L->bar_w = 10;
}

static void draw_frame(WINDOW *w, const layout_t *L)
{
    werase(w);
    /* borde */
    if (g_colors) wattron(w, COLOR_PAIR(CP_BORD));
    wborder(w, 0, 0, 0, 0, 0, 0, 0, 0);
    if (g_colors) wattroff(w, COLOR_PAIR(CP_BORD));
    /* franja amarilla del icono, a todo el alto interior */
    for (int y = 1; y < L->frame_h - 1; y++)
        cells(w, y, 1, SPINE_W, CP_SPINE);
    /* cuadradito de la esquina (ventana del icono) */
    cells(w, 2, 2, 3, CP_EMPTY);              /* exterior oscuro (256) */
    if (!g_c256) { /* en modo basico se dibuja con espacios del fondo por defecto */
        wattron(w, A_REVERSE); mvwaddch(w, 2, 2, ' '); wattroff(w, A_REVERSE);
    }
    cells(w, 2, 3, 1, CP_SQIN);               /* centro crema */
    /* barra superior gris */
    for (int y = 1; y <= 2; y++)
        cells(w, y, 1 + SPINE_W + 1, FRAME_W - (1 + SPINE_W + 1) - 1, CP_TOPBAR);
    /* logo centrado en el interior */
    int interior_y = 3, interior_h = L->frame_h - 2 - interior_y;
    int ly = interior_y + (interior_h - LOGO_H) / 2;
    int lx = 1 + SPINE_W + 1 + (FRAME_W - (1 + SPINE_W + 1) - 1 - LOGO_W) / 2;
    for (int i = 0; i < LOGO_H; i++)
        put_pair(w, ly + i, lx, LOGO[i], CP_LOGO, 1);
    /* paleta de colores bajo el marco: 8 + 8 */
    for (int i = 0; i < 8; i++) {
        cells(w, L->frame_h,     1 + i * 3, 3, CP_PAL + i);
        cells(w, L->frame_h + 1, 1 + i * 3, 3, CP_PAL + 8 + i);
    }
    wnoutrefresh(w);
}

static void kv(WINDOW *w, int y, const char *label, const char *value)
{
    put_pair(w, y, 0, label, CP_LABEL, 1);
    put_pair(w, y, (int)strlen(label), value, CP_VALUE, 0);
    wmove(w, y, (int)strlen(label) + (int)strlen(value));
    wclrtoeol(w);
}

static void draw_info_static(WINDOW *w, const info_t *in, const layout_t *L)
{
    werase(w);
    /* usuario@hostname */
    int x = 0;
    put_pair(w, 0, x, in->user, CP_USER, 1); x += (int)strlen(in->user);
    put_pair(w, 0, x, "@", CP_DIM, 0);       x += 1;
    put_pair(w, 0, x, in->host, CP_HOST, 1); x += (int)strlen(in->host);
    /* separador */
    wattron(w, COLOR_PAIR(CP_SEP));
    mvwhline(w, 1, 0, ACS_HLINE, x);
    wattroff(w, COLOR_PAIR(CP_SEP));

    char lab[64];
    snprintf(lab, sizeof lab, "SO: ");                    kv(w, 2, lab, in->os);
    snprintf(lab, sizeof lab, "Kernel: ");                kv(w, 3, lab, in->kernel);
    snprintf(lab, sizeof lab, "Uptime: ");                kv(w, 4, lab, in->uptime);
    snprintf(lab, sizeof lab, "Shell: ");                 kv(w, 5, lab, in->shell);
    snprintf(lab, sizeof lab, "Resoluci%s: ", S_o());     kv(w, 6, lab, in->res);
    snprintf(lab, sizeof lab, "WM: ");                    kv(w, 7, lab, in->wm);
    snprintf(lab, sizeof lab, "Terminal: ");              kv(w, 8, lab, in->term);
    snprintf(lab, sizeof lab, "CPU: ");                   kv(w, 9, lab, in->cpu);
    if (in->has_temp) {
        char tv[32];
        snprintf(tv, sizeof tv, "%d %sC", in->temp_c, S_deg());
        snprintf(lab, sizeof lab, "Temp. CPU: ");         kv(w, 10, lab, tv);
    }

    /* cabecera EN VIVO */
    int y = L->live_row;
    wattron(w, COLOR_PAIR(CP_SEP));
    mvwhline(w, y, 0, ACS_HLINE, 2);
    wattroff(w, COLOR_PAIR(CP_SEP));
    x = 3;
    put_pair(w, y, x, "EN VIVO", CP_HEAD, 1);   x += 8;
    cells(w, y, x, 1, CP_DOT);                  x += 2;   /* punto rojo */
    put_pair(w, y, x, "LIVE", CP_LIVE, 1);      x += 4;
    char mid[64];
    snprintf(mid, sizeof mid, " %s se actualiza cada 1 s ", S_mid());
    put_pair(w, y, x, mid, CP_DIM, 0);          x += (int)strlen(mid);
    wattron(w, COLOR_PAIR(CP_SEP));
    mvwhline(w, y, x, ACS_HLINE, 3);
    wattroff(w, COLOR_PAIR(CP_SEP));

    /* pista de teclas */
    char hint[96];
    snprintf(hint, sizeof hint, "%sq%s para salir %s %sr%s refrescar ahora",
             S_lq(), S_rq(), S_mid(), S_lq(), S_rq());
    put_pair(w, L->live_row + 5, 0, hint, CP_DIM, 0);
    wnoutrefresh(w);
}

static void draw_meter(WINDOW *w, int y, const char *label,
                       double pct, const char *detail, int bar_w)
{
    char lab[8];
    snprintf(lab, sizeof lab, "%-4s ", label);
    put_pair(w, y, 0, lab, CP_LABEL, 1);
    int x = 5;
    put_pair(w, y, x, "[", CP_DIM, 0); x += 1;
    int filled = (int)(pct / 100.0 * bar_w + 0.5);
    if (filled < 0) filled = 0;
    if (filled > bar_w) filled = bar_w;
    for (int i = 0; i < bar_w; i++) {
        if (i < filled) {
            int idx = (int)((double)i / bar_w * 9.99);
            cells(w, y, x + i, 1, CP_GRAD + idx);
        } else {
            if (g_c256) cells(w, y, x + i, 1, CP_EMPTY);
            else {
                wattron(w, A_DIM);
                mvwaddch(w, y, x + i, ACS_CKBOARD);
                wattroff(w, A_DIM);
            }
        }
    }
    x += bar_w;
    put_pair(w, y, x, "]", CP_DIM, 0); x += 2;
    put_pair(w, y, x, detail, CP_VALUE, 1);
    wmove(w, y, x + (int)strlen(detail));
    wclrtoeol(w);
}

static void draw_dynamic(WINDOW *w, const info_t *in, const layout_t *L)
{
    /* uptime y temperatura se refrescan en su linea */
    kv(w, L->up_row, "Uptime: ", in->uptime);
    if (in->has_temp && L->temp_row >= 0) {
        char tv[32];
        snprintf(tv, sizeof tv, "%d %sC", in->temp_c, S_deg());
        kv(w, L->temp_row, "Temp. CPU: ", tv);
    }
    /* barras */
    char det[80], a[32], b[32], pcts[16];
    if (in->cpu_pct < 0) snprintf(pcts, sizeof pcts, "--%%");
    else snprintf(pcts, sizeof pcts, "%.0f%%", in->cpu_pct);
    draw_meter(w, L->live_row + 1, "CPU", in->cpu_pct < 0 ? 0 : in->cpu_pct,
               pcts, L->bar_w);
    fmt_mem(in->mem_used, a, sizeof a); fmt_mem(in->mem_total, b, sizeof b);
    snprintf(det, sizeof det, "%s / %s", a, b);
    double rp = in->mem_total ? 100.0 * (double)in->mem_used / (double)in->mem_total : 0;
    draw_meter(w, L->live_row + 2, "RAM", rp, det, L->bar_w);
    if (in->swp_total > 0) {
        fmt_mem(in->swp_used, a, sizeof a); fmt_mem(in->swp_total, b, sizeof b);
        snprintf(det, sizeof det, "%s / %s", a, b);
        double sp = 100.0 * (double)in->swp_used / (double)in->swp_total;
        draw_meter(w, L->live_row + 3, "Swap", sp, det, L->bar_w);
    } else {
        draw_meter(w, L->live_row + 3, "Swap", 0, "-", L->bar_w);
    }
    wnoutrefresh(w);
}

static volatile sig_atomic_t g_quit = 0;
static void on_sig(int s) { (void)s; g_quit = 1; }

static void draw_too_small(void)
{
    erase();
    char l1[128], l2[128];
    snprintf(l1, sizeof l1, "Terminal demasiado peque%sa", g_utf8 ? "\xc3\xb1" : "\xf1");
    snprintf(l2, sizeof l2, "flfetch necesita al menos %d x %d (ahora %d x %d)",
             MIN_COLS, MIN_ROWS, COLS, LINES);
    int y = LINES / 2;
    mvaddstr(y - 1, (COLS - (int)strlen(l1)) / 2, l1);
    mvaddstr(y,     (COLS - (int)strlen(l2)) / 2, l2);
    mvaddstr(y + 2, (COLS - 22) / 2, "Pulsa q para salir");
    wnoutrefresh(stdscr);
}

static int run_live(info_t *in)
{
    /* ncurses con la TERM pedida; si el terminfo no existe, reintentos */
    SCREEN *scr = NULL;
    const char *fallbacks[] = { NULL, "xterm", "rxvt", "vt100", NULL };
    for (int i = 0; fallbacks[i] || i == 0; i++) {
        if (fallbacks[i]) setenv("TERM", fallbacks[i], 1);
        scr = newterm(NULL, stdout, stdin);
        if (scr) break;
        if (!fallbacks[i]) continue;
    }
    if (!scr) {
        fprintf(stderr, "flfetch: no se pudo abrir la terminal\n");
        return 1;
    }
    set_term(scr);
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    init_colors();

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    /* primera muestra de CPU con una pausa breve: primera foto util */
    cput_t prev = read_cpu();
    collect_dynamic(in);
    {
        struct timespec ts = {0, 200 * 1000 * 1000};
        nanosleep(&ts, NULL);
        cput_t now = read_cpu();
        in->cpu_pct = cpu_pct_between(prev, now);
        prev = now;
    }

    WINDOW *winL = NULL, *winR = NULL;
    layout_t L;
    int laid_out = 0, too_small = 0;

    timeout(100);
    time_t last_tick = time(NULL);
    while (!g_quit) {
        int need = (COLS < MIN_COLS || LINES < MIN_ROWS);
        if (need != too_small || !laid_out) {
            too_small = need;
            if (winL) { delwin(winL); winL = NULL; }
            if (winR) { delwin(winR); winR = NULL; }
            laid_out = 1;
            werase(stdscr);
            wnoutrefresh(stdscr);
            if (!too_small) {
                compute_layout(in, COLS, &L);
                winL = newwin(L.frame_h + 2, FRAME_W, 0, 0);
                winR = newwin(L.frame_h + 2, L.right_w, 0, FRAME_W + COL_GAP);
                draw_frame(winL, &L);
                draw_info_static(winR, in, &L);
                draw_dynamic(winR, in, &L);
            } else {
                draw_too_small();
            }
            doupdate();
        }
        int ch = getch();
        if (ch == 'q' || ch == 'Q') break;
        if (ch == KEY_RESIZE) { laid_out = 0; continue; }
        time_t now_t = time(NULL);
        if (ch == 'r' || ch == 'R' || now_t != last_tick) {
            last_tick = now_t;
            cput_t now = read_cpu();
            in->cpu_pct = cpu_pct_between(prev, now);
            prev = now;
            collect_dynamic(in);
            if (!too_small && winR) {
                draw_dynamic(winR, in, &L);
                doupdate();
            }
        }
    }
    if (winL) delwin(winL);
    if (winR) delwin(winR);
    endwin();
    delscreen(scr);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Modo de un disparo (--once o sin terminal): ficha estatica ANSI    */
/* ------------------------------------------------------------------ */
static int o256;                 /* la terminal admite 256 colores */
static int oplain;               /* sin color (TERM=dumb o NO_COLOR) */

static void o_fg(int c256, int cbasic)
{
    if (oplain) return;
    if (o256) printf("\033[38;5;%dm", c256);
    else printf("\033[%dm", cbasic);
}
static void o_bg(int c256, int cbasic)
{
    if (oplain) return;
    if (o256) printf("\033[48;5;%dm", c256);
    else printf("\033[%dm", cbasic);
}
static void o_bold(void) { if (!oplain) printf("\033[1m"); }
static void o_reset(void) { if (!oplain) printf("\033[0m"); }

static const short RAMP256[10] = {46, 82, 118, 154, 190, 226, 214, 208, 202, 196};
static const short RAMPBAS[10] = {42, 42, 42, 42, 42, 43, 43, 43, 41, 41};
static const short PAL256[16] = {235, 160, 40, 184, 33, 170, 44, 254,
                                 242, 203, 84, 227, 75, 213, 87, 15};
static const short PALBAS[16] = {40, 41, 42, 43, 44, 45, 46, 47,
                                 100, 101, 102, 103, 104, 105, 106, 107};

/* una celda del marco en modo once */
static void once_frame_cell(int y, int x, const layout_t *L, int ly, int lx)
{
    if (y == 0 || y == L->frame_h - 1) {                 /* filas del borde */
        if (x == 0 || x == FRAME_W - 1) { o_fg(245, 37); putchar('+'); }
        else { o_fg(245, 37); putchar('-'); }
        o_reset(); return;
    }
    if (x == 0 || x == FRAME_W - 1) { o_fg(245, 37); putchar('|'); o_reset(); return; }
    if (x >= 1 && x <= SPINE_W) {                       /* franja amarilla */
        if (y == 2 && x >= 2 && x <= 4) {               /* cuadradito */
            if (x == 3) o_bg(230, 47); else o_bg(0, 40);
            putchar(' '); o_reset(); return;
        }
        o_bg(221, 43); putchar(' '); o_reset(); return;
    }
    if ((y == 1 || y == 2) && x >= SPINE_W + 2) {        /* barra superior */
        o_bg(246, 47); putchar(' '); o_reset(); return;
    }
    if (y >= ly && y < ly + LOGO_H) {                   /* logo */
        const char *line = LOGO[y - ly];
        int li = x - lx;
        if (li >= 0 && li < (int)strlen(line) && line[li] != ' ') {
            o_bold(); o_fg(230, 37); putchar(line[li]); o_reset(); return;
        }
    }
    putchar(' ');
}

static void once_meter(const char *label, double pct, const char *detail, int bar_w)
{
    o_bold(); o_fg(208, 33); printf("%-4s ", label); o_reset();
    o_fg(242, 37); putchar('['); o_reset();
    int filled = (int)(pct / 100.0 * bar_w + 0.5);
    if (filled < 0) filled = 0;
    if (filled > bar_w) filled = bar_w;
    for (int i = 0; i < bar_w; i++) {
        if (i < filled) {
            int idx = (int)((double)i / bar_w * 9.99);
            if (oplain) putchar('#');
            else { o_bg(RAMP256[idx], RAMPBAS[idx]); putchar(' '); o_reset(); }
        } else {
            if (oplain) putchar('-');
            else { o_bg(238, 100); putchar(' '); o_reset(); }
        }
    }
    o_fg(242, 37); putchar(']'); o_reset();
    o_bold(); o_fg(255, 37); printf(" %s", detail); o_reset();
}

static int print_once(info_t *in)
{
    const char *term = getenv("TERM");
    oplain = (!term || !strcmp(term, "dumb") || getenv("NO_COLOR") != NULL);
    o256 = (term && strstr(term, "256") != NULL);

    int cols = 124;
    if (isatty(STDOUT_FILENO)) {
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= MIN_COLS)
            cols = ws.ws_col;
    }
    if (cols > 200) cols = 200;

    /* CPU: dos muestras con pausa breve */
    cput_t a = read_cpu();
    {
        struct timespec ts = {0, 200 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    cput_t b = read_cpu();
    in->cpu_pct = cpu_pct_between(a, b);
    collect_dynamic(in);

    layout_t L;
    compute_layout(in, cols, &L);
    int total_rows = L.frame_h + 2;
    int ly = 3 + (L.frame_h - 5 - LOGO_H) / 2;
    int lx = 1 + SPINE_W + 1 + (FRAME_W - (1 + SPINE_W + 1) - 1 - LOGO_W) / 2;

    /* lineas de la derecha, ya compuestas */
    char kvbuf[16][512];
    int nright = 0;
    char tmp[256], lab[64];
    /* 0: usuario@host */
    {
        int n = 0;
        if (!oplain) n += snprintf(kvbuf[0] + n, sizeof kvbuf[0] - n, "\033[1m");
        if (!oplain) n += snprintf(kvbuf[0] + n, sizeof kvbuf[0] - n, "\033[%sm", o256 ? "38;5;208" : "33");
        n += snprintf(kvbuf[0] + n, sizeof kvbuf[0] - n, "%s", in->user);
        if (!oplain) n += snprintf(kvbuf[0] + n, sizeof kvbuf[0] - n, "\033[0m");
        n += snprintf(kvbuf[0] + n, sizeof kvbuf[0] - n, "@");
        if (!oplain) n += snprintf(kvbuf[0] + n, sizeof kvbuf[0] - n, "\033[1m");
        if (!oplain) n += snprintf(kvbuf[0] + n, sizeof kvbuf[0] - n, "\033[%sm", o256 ? "38;5;220" : "33");
        n += snprintf(kvbuf[0] + n, sizeof kvbuf[0] - n, "%s", in->host);
        if (!oplain) n += snprintf(kvbuf[0] + n, sizeof kvbuf[0] - n, "\033[0m");
        kvbuf[0][n] = 0;
    }
    nright = 1;
    /* 1: separador */
    {
        int len = (int)strlen(in->user) + 1 + (int)strlen(in->host);
        int n = 0;
        if (!oplain) n += snprintf(kvbuf[1] + n, sizeof kvbuf[1] - n, "\033[%sm", o256 ? "38;5;208" : "33");
        for (int i = 0; i < len; i++)
            n += snprintf(kvbuf[1] + n, sizeof kvbuf[1] - n, "%s", g_utf8 ? "\xe2\x94\x80" : "-");
        if (!oplain) n += snprintf(kvbuf[1] + n, sizeof kvbuf[1] - n, "\033[0m");
        kvbuf[1][n] = 0;
    }
    nright = 2;
#define KVLINE(idx, LABEL, VAL) do { \
    int n = 0; \
    if (!oplain) n += snprintf(kvbuf[idx] + n, sizeof kvbuf[idx] - n, "\033[1m\033[%sm", o256 ? "38;5;208" : "33"); \
    n += snprintf(kvbuf[idx] + n, sizeof kvbuf[idx] - n, "%s", LABEL); \
    if (!oplain) n += snprintf(kvbuf[idx] + n, sizeof kvbuf[idx] - n, "\033[0m"); \
    n += snprintf(kvbuf[idx] + n, sizeof kvbuf[idx] - n, "%s", VAL); \
    kvbuf[idx][n] = 0; } while (0)
    snprintf(lab, sizeof lab, "SO: ");                 KVLINE(2, lab, in->os);
    snprintf(lab, sizeof lab, "Kernel: ");             KVLINE(3, lab, in->kernel);
    snprintf(lab, sizeof lab, "Uptime: ");             KVLINE(4, lab, in->uptime);
    snprintf(lab, sizeof lab, "Shell: ");              KVLINE(5, lab, in->shell);
    snprintf(lab, sizeof lab, "Resoluci%s: ", S_o());  KVLINE(6, lab, in->res);
    snprintf(lab, sizeof lab, "WM: ");                 KVLINE(7, lab, in->wm);
    snprintf(lab, sizeof lab, "Terminal: ");           KVLINE(8, lab, in->term);
    snprintf(lab, sizeof lab, "CPU: ");                KVLINE(9, lab, in->cpu);
    nright = 10;
    if (in->has_temp) {
        char tv[32];
        snprintf(tv, sizeof tv, "%d %sC", in->temp_c, S_deg());
        snprintf(lab, sizeof lab, "Temp. CPU: ");      KVLINE(10, lab, tv);
        nright = 11;
    }
#undef KVLINE

    for (int y = 0; y < total_rows; y++) {
        /* --- bloque izquierdo --- */
        if (y < L.frame_h) {
            for (int x = 0; x < FRAME_W; x++)
                once_frame_cell(y, x, &L, ly, lx);
        } else {
            /* paleta */
            int prow = y - L.frame_h;
            putchar(' ');
            for (int i = 0; i < 8; i++) {
                int idxp = prow * 8 + i;
                if (oplain) printf("###");
                else { o_bg(PAL256[idxp], PALBAS[idxp]); printf("   "); o_reset(); }
            }
            for (int x = 1 + 24; x < FRAME_W; x++) putchar(' ');
        }
        /* --- bloque derecho --- */
        int ry = y;
        if (ry < nright) {
            printf("%*s%s", COL_GAP, "", kvbuf[ry]);
        } else if (ry == L.live_row) {
            printf("%*s", COL_GAP, "");
            o_fg(208, 33); printf("-- "); o_reset();
            o_bold(); o_fg(220, 33); printf("EN VIVO"); o_reset();
            printf(" ");
            if (oplain) printf("*");
            else { o_bg(196, 41); printf(" "); o_reset(); }
            o_bold(); o_fg(196, 31); printf(" LIVE"); o_reset();
            o_fg(242, 37); printf(" %s se actualiza cada 1 s ", S_mid()); o_reset();
            o_fg(208, 33); printf("---"); o_reset();
        } else if (ry >= L.live_row + 1 && ry <= L.live_row + 3) {
            printf("%*s", COL_GAP, "");
            int which = ry - (L.live_row + 1);
            char det[80], ma[32], mb[32];
            if (which == 0) {
                snprintf(det, sizeof det, "%.0f%%", in->cpu_pct);
                once_meter("CPU", in->cpu_pct, det, L.bar_w);
            } else if (which == 1) {
                fmt_mem(in->mem_used, ma, sizeof ma); fmt_mem(in->mem_total, mb, sizeof mb);
                snprintf(det, sizeof det, "%s / %s", ma, mb);
                double rp = in->mem_total ? 100.0 * (double)in->mem_used / (double)in->mem_total : 0;
                once_meter("RAM", rp, det, L.bar_w);
            } else {
                if (in->swp_total > 0) {
                    fmt_mem(in->swp_used, ma, sizeof ma); fmt_mem(in->swp_total, mb, sizeof mb);
                    snprintf(det, sizeof det, "%s / %s", ma, mb);
                    double sp = 100.0 * (double)in->swp_used / (double)in->swp_total;
                    once_meter("Swap", sp, det, L.bar_w);
                } else {
                    once_meter("Swap", 0, "-", L.bar_w);
                }
            }
        } else if (ry == L.live_row + 5) {
            printf("%*s", COL_GAP, "");
            o_fg(242, 37);
            printf("%sq%s para salir %s %sr%s refrescar ahora",
                   S_lq(), S_rq(), S_mid(), S_lq(), S_rq());
            /* (en modo una sola vez es solo informativo) */
            o_reset();
        }
        (void)tmp;
        putchar('\n');
    }
    return 0;
}

/* ------------------------------------------------------------------ */
#define FLFETCH_VERSION "1.0"

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    const char *cs = nl_langinfo(CODESET);
    g_utf8 = (cs && (strstr(cs, "UTF-8") || strstr(cs, "utf8") || strstr(cs, "UTF8")));

    int once = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--once") == 0) once = 1;
        else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("flfetch %s\n", FLFETCH_VERSION);
            return 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("flfetch %s - el fetch de FLinux\n\n", FLFETCH_VERSION);
            printf("Uso: flfetch [--once]\n");
            printf("  Sin argumentos: ficha en vivo (CPU/RAM cada 1 s). q = salir, r = refrescar.\n");
            printf("  --once: imprime la ficha estatica y sale (estilo neofetch).\n");
            return 0;
        }
    }

    info_t in;
    memset(&in, 0, sizeof in);
    in.cpu_pct = -1;
    collect_static(&in);

    if (once || !isatty(STDOUT_FILENO) || !isatty(STDIN_FILENO))
        return print_once(&in);
    return run_live(&in);
}
