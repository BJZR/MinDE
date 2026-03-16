/* min-net.c — Panel WiFi de minDE
 *
 * Mismo estilo que min-launch.
 * Parsing robusto de nmcli: maneja SSIDs con ':' escapado como '\:'.
 *
 * Teclas: Arriba/Abajo navegar, Enter conectar, Esc cerrar, S escanear
 *
 * Compilar:
 *   gcc -O2 -o min-net min-net.c \
 *       $(pkg-config --cflags --libs x11 xft fontconfig)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>

#include "config.h"

/* ═══ DATOS ══════════════════════════════════════════════════ */

#define MAX_NETS  32
#define NET_ROWS  12
#define NET_W    420

typedef struct {
    char ssid    [128];
    char security[32];
    int  signal_pct;
    int  active;
} Net;

static Net  nets[MAX_NETS];
static int  nnets     = 0;
static char con_ssid[128] = "";
static int  selected  = 0;
static int  scroll    = 0;

static int  pw_mode = 0;
static char pw[128] = "";
static int  pw_len  = 0;

/* ── Parser de líneas nmcli -t (separador ':' escapado como '\:') ── */
static int split_t(const char *line, char out[][256], int maxf) {
    int f = 0, j = 0;
    for (int i = 0; line[i] && f < maxf; i++) {
        if (line[i] == '\\' && line[i+1] == ':') {
            /* colon escapado → incluir ':' literal */
            if (j < 255) out[f][j++] = ':';
            i++;
        } else if (line[i] == ':') {
            out[f][j] = '\0'; f++; j = 0;
        } else {
            if (j < 255) out[f][j++] = line[i];
        }
    }
    if (j > 0 || f > 0) { out[f][j] = '\0'; f++; }
    return f;
}

/* ── Caché ── */
static void cache_path(char *out, int len) {
    const char *h = getenv("HOME");
    char d[256]; snprintf(d, 255, "%s/.config/minde", h ? h : "/tmp");
    mkdir(d, 0755);
    snprintf(out, (size_t)len, "%s/networks.cache", d);
}

static void cache_save(void) {
    char p[512]; cache_path(p, sizeof p);
    FILE *f = fopen(p, "w"); if (!f) return;
    fprintf(f, "#con:%s\n", con_ssid);
    for (int i = 0; i < nnets; i++)
        fprintf(f, "%d|%s|%d|%s\n",
            nets[i].active, nets[i].ssid,
            nets[i].signal_pct, nets[i].security);
    fclose(f);
}

static int cache_load(void) {
    char p[512]; cache_path(p, sizeof p);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    nnets = 0; con_ssid[0] = '\0';
    char line[256];
    while (fgets(line, sizeof line, f) && nnets < MAX_NETS) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;
        if (!strncmp(line, "#con:", 5)) {
            snprintf(con_ssid, 128, "%.*s", 127, line+5); continue;
        }
        Net *n = &nets[nnets]; memset(n, 0, sizeof *n);
        int act; char ss[128]; int sig; char sec[32] = "";
        if (sscanf(line, "%d|%127[^|]|%d|%31[^\n]",
                   &act, ss, &sig, sec) >= 3) {
            n->active = act;
            snprintf(n->ssid, 128, "%s", ss);
            n->signal_pct = sig;
            snprintf(n->security, 32, "%s", sec);
            if (n->ssid[0]) nnets++;
        }
    }
    fclose(f);
    return nnets > 0;
}

/* ── Escanear con parsing robusto ── */
static void net_scan(void) {
    nnets = 0; con_ssid[0] = '\0';

    /* Obtener SSID activo via conexión activa (más fiable que IN-USE) */
    FILE *fp = popen(
        "nmcli -t -f NAME,DEVICE con show --active 2>/dev/null"
        " | head -5", "r");
    if (fp) {
        /* Solo necesitamos que el popen cierre limpiamente */
        pclose(fp);
    }
    fp = popen(
        "nmcli -t -f ACTIVE,SSID dev wifi list 2>/dev/null", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof line, fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            char fields[4][256] = {{0}};
            int n = split_t(line, fields, 4);
            if (n >= 2 && !strcmp(fields[0], "yes") && fields[1][0]) {
                snprintf(con_ssid, 128, "%.*s", 127, fields[1]);
                break;
            }
        }
        pclose(fp);
    }

    /* Forzar escaneo ACTIVO — sin esto devuelve caché vacío bajo min-wm
     * porque no hay nm-applet escaneando en segundo plano.
     * -w 8 = esperar hasta 8s a que el hardware complete el escaneo. */
    system("nmcli -w 8 dev wifi rescan 2>/dev/null");

    /* Lista completa con escape yes (default de nmcli) */
    fp = popen(
        "nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY dev wifi list 2>/dev/null",
        "r");
    if (!fp) return;

    char line[512];
    while (fgets(line, sizeof line, fp) && nnets < MAX_NETS) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;

        /* Campos: IN-USE, SSID, SIGNAL, SECURITY */
        char f[4][256] = {{0}};
        int nf = split_t(line, f, 4);
        if (nf < 3) continue;

        /* f[0]=IN-USE ("*" o ""), f[1]=SSID, f[2]=SIGNAL, f[3]=SECURITY */
        if (!f[1][0]) continue;   /* SSID vacío → saltar */

        Net *n = &nets[nnets];
        memset(n, 0, sizeof *n);
        n->active = (f[0][0] == '*');
        snprintf(n->ssid, 128, "%.*s", 127, f[1]);
        n->signal_pct = atoi(f[2]);
        if (nf >= 4) {
            snprintf(n->security, 32, "%.*s", 31, f[3]);
            if (!strcmp(n->security, "--")) n->security[0] = '\0';
        }
        nnets++;
    }
    pclose(fp);

    /* Si no detectamos el activo por ACTIVE, buscarlo por nombre */
    if (!con_ssid[0]) {
        for (int i = 0; i < nnets; i++) {
            if (nets[i].active) {
                strncpy(con_ssid, nets[i].ssid, 127);
                break;
            }
        }
    }

    cache_save();
}

/* ── Señal ASCII ── */
static const char *sig_str(int p) {
    if (p >= 80) return "####";
    if (p >= 60) return "### ";
    if (p >= 40) return "##  ";
    if (p >= 20) return "#   ";
    return             "    ";
}

/* ── Conectar ── */
static void net_connect(const char *ssid, const char *password) {
    char cmd[512];
    if (password && password[0])
        snprintf(cmd, sizeof cmd,
            "nmcli dev wifi connect '%s' password '%s' &",
            ssid, password);
    else
        snprintf(cmd, sizeof cmd,
            "nmcli dev wifi connect '%s' &", ssid);
    if (fork() == 0) {
        setsid();
        execlp("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(127);
    }
}

/* ═══ X11 / XFT — idéntico a min-launch ═════════════════════ */

static Display  *dpy;
static int       scr;
static Window    root, win;
static XftDraw  *xd;
static XftFont  *font;
static Theme     theme;

static XftColor  c_bg, c_fg, c_accent, c_dim;

static int W, H, row_h, hdr_h;

static void alloc_color(XftColor *c, const char *hex) {
    if (!XftColorAllocName(dpy, DefaultVisual(dpy, scr),
            DefaultColormap(dpy, scr), hex, c))
        XftColorAllocName(dpy, DefaultVisual(dpy, scr),
            DefaultColormap(dpy, scr), "#000000", c);
}

static int text_w(const char *s) {
    XGlyphInfo ext;
    XftTextExtentsUtf8(dpy, font, (const FcChar8 *)s, strlen(s), &ext);
    return ext.xOff;
}

static void draw_text(int x, int y, const char *s, XftColor *col) {
    XftDrawStringUtf8(xd, col, font, x, y, (const FcChar8 *)s, strlen(s));
}

/* ── Dibujo ── */
static void draw(void) {
    GC gc = DefaultGC(dpy, scr);

    XSetForeground(dpy, gc, (unsigned long)theme.pix_bg);
    XFillRectangle(dpy, win, gc, 0, 0, (unsigned)W, (unsigned)H);

    /* Cabecera */
    XSetForeground(dpy, gc, (unsigned long)theme.pix_bact);
    XFillRectangle(dpy, win, gc, 0, 0, (unsigned)W, (unsigned)hdr_h);

    int base = LAUNCHER_PAD + font->ascent;

    if (!pw_mode) {
        char hdr[256];
        if (con_ssid[0])
            snprintf(hdr, sizeof hdr, "Conectado: %s", con_ssid);
        else if (nnets > 0)
            snprintf(hdr, sizeof hdr, "%d redes", nnets);
        else
            snprintf(hdr, sizeof hdr, "Sin redes — pulsa S para escanear");
        draw_text(LAUNCHER_PAD, base, hdr, &c_bg);
    } else {
        char hdr[256];
        snprintf(hdr, sizeof hdr, "Contrasena: %s", nets[selected].ssid);
        draw_text(LAUNCHER_PAD, base, hdr, &c_bg);
    }

    XSetForeground(dpy, gc, 0x3a3f5c);
    XFillRectangle(dpy, win, gc, 0, hdr_h, (unsigned)W, 1);

    if (!pw_mode) {
        /* Lista */
        int vis = nnets < NET_ROWS ? nnets : NET_ROWS;
        for (int i = 0; i < vis; i++) {
            int mi = scroll + i;
            if (mi >= nnets) break;
            Net *n = &nets[mi];

            int iy     = hdr_h + 1 + i * row_h;
            int text_y = iy + font->ascent +
                         (row_h - font->ascent - font->descent) / 2;

            if (mi == selected) {
                XSetForeground(dpy, gc, (unsigned long)theme.pix_bact);
                XFillRectangle(dpy, win, gc, 0, iy, (unsigned)W, (unsigned)row_h);
            } else if (i % 2 != 0) {
                unsigned long alt = (theme.pix_bg & 0xfefefe) + 0x060608;
                XSetForeground(dpy, gc, alt);
                XFillRectangle(dpy, win, gc, 0, iy, (unsigned)W, (unsigned)row_h);
            }

            XftColor *nc = (mi == selected) ? &c_bg : &c_fg;
            if (n->active) {
                draw_text(LAUNCHER_PAD, text_y, "> ",
                          (mi == selected) ? &c_bg : &c_accent);
            }
            draw_text(LAUNCHER_PAD + text_w("> "), text_y, n->ssid, nc);

            /* Señal + seguridad a la derecha */
            char right[48];
            if (n->security[0])
                snprintf(right, sizeof right, "%s %s",
                         sig_str(n->signal_pct), n->security);
            else
                snprintf(right, sizeof right, "%s", sig_str(n->signal_pct));
            int rw = text_w(right);
            XftColor *rc = (mi == selected) ? &c_bg : &c_dim;
            draw_text(W - rw - LAUNCHER_PAD, text_y, right, rc);
        }

        /* Scrollbar */
        if (nnets > NET_ROWS) {
            int sb_h = NET_ROWS * row_h;
            int th   = sb_h * NET_ROWS / nnets; if (th < 4) th = 4;
            int ty   = hdr_h + 1 +
                       (sb_h - th) * scroll / (nnets - NET_ROWS);
            XSetForeground(dpy, gc, (unsigned long)theme.pix_bact);
            XFillRectangle(dpy, win, gc, W - 3, ty, 3, (unsigned)th);
        }

    } else {
        /* Campo contraseña — igual que el campo de búsqueda */
        int iy     = hdr_h + 1;
        int text_y = iy + font->ascent +
                     (row_h - font->ascent - font->descent) / 2;

        XSetForeground(dpy, gc, (unsigned long)theme.pix_bact);
        XFillRectangle(dpy, win, gc, 0, iy, (unsigned)W, (unsigned)row_h);

        draw_text(LAUNCHER_PAD, text_y, "> ", &c_accent);
        char stars[128] = "";
        for (int i = 0; i < pw_len && i < 60; i++) strcat(stars, "*");
        draw_text(LAUNCHER_PAD + text_w("> "), text_y, stars, &c_bg);

        int cx = LAUNCHER_PAD + text_w("> ") + text_w(stars);
        XSetForeground(dpy, gc, 0xc0caf5);
        XDrawLine(dpy, win, gc, cx, iy + 3, cx, iy + row_h - 3);
    }

    XFlush(dpy);
}

/* ── Eventos ── */
static void do_select(void) {
    if (selected < 0 || selected >= nnets) return;
    if (nets[selected].active) return;
    if (nets[selected].security[0]) {
        pw_mode = 1; pw_len = 0; pw[0] = '\0';
    } else {
        net_connect(nets[selected].ssid, "");
        XCloseDisplay(dpy); exit(0);
    }
}

static void on_key(XKeyEvent *e) {
    char buf[32] = ""; KeySym sym;
    XLookupString(e, buf, sizeof buf - 1, &sym, NULL);

    if (!pw_mode) {
        switch (sym) {
        case XK_Escape: case XK_q:
            XCloseDisplay(dpy); exit(0);
        case XK_Return: case XK_KP_Enter:
            do_select(); draw(); break;
        case XK_Up: case XK_KP_Up:
            if (selected > 0) {
                selected--;
                if (selected < scroll) scroll = selected;
            }
            draw(); break;
        case XK_Down: case XK_KP_Down:
            if (selected < nnets - 1) {
                selected++;
                if (selected >= scroll + NET_ROWS)
                    scroll = selected - NET_ROWS + 1;
            }
            draw(); break;
        case XK_s: case XK_F5:
            net_scan(); selected = 0; scroll = 0; draw(); break;
        default: break;
        }
    } else {
        switch (sym) {
        case XK_Escape:
            pw_mode = 0; pw_len = 0; pw[0] = '\0'; draw(); break;
        case XK_Return: case XK_KP_Enter:
            net_connect(nets[selected].ssid, pw);
            XCloseDisplay(dpy); exit(0);
        case XK_BackSpace:
            if (pw_len > 0) {
                do { pw_len--; }
                while (pw_len > 0 && (pw[pw_len] & 0xC0) == 0x80);
                pw[pw_len] = '\0'; draw();
            }
            break;
        default:
            if (buf[0] >= 0x20 && pw_len < 127) {
                int bl = strlen(buf);
                memcpy(pw + pw_len, buf, (size_t)bl);
                pw_len += bl; pw[pw_len] = '\0'; draw();
            }
        }
    }
}

static void on_click(int mx, int my) {
    (void)mx;
    if (pw_mode) return;
    for (int i = 0; i < NET_ROWS; i++) {
        int iy = hdr_h + 1 + i * row_h;
        if (my >= iy && my < iy + row_h) {
            int mi = scroll + i;
            if (mi >= nnets) break;
            selected = mi; draw();
            do_select(); draw(); return;
        }
    }
}

/* ═══ MAIN ═══════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    signal(SIGCHLD, SIG_IGN);

    if (argc > 1 && !strcmp(argv[1], "--daemon")) {
        for (;;) { net_scan(); sleep(30); }
        return 0;
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fputs("min-net: no X display\n", stderr); return 1; }
    scr  = DefaultScreen(dpy);
    root = DefaultRootWindow(dpy);

    theme_init(&theme);
    font = XftFontOpenName(dpy, scr, FONT_NAME);
    if (!font) font = XftFontOpenName(dpy, scr, "fixed:size=9");
    if (!font) { fputs("min-net: no font\n", stderr); return 1; }

    alloc_color(&c_bg,     theme.bg);
    alloc_color(&c_fg,     theme.fg);
    alloc_color(&c_accent, theme.accent);
    alloc_color(&c_dim,    theme.dim);

    row_h = font->ascent + font->descent + LAUNCHER_PAD;
    hdr_h = row_h + 2 * LAUNCHER_PAD;
    W = NET_W;
    H = hdr_h + 1 + NET_ROWS * row_h;

    int sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr);

    XSetWindowAttributes attr = {0};
    attr.background_pixel  = (unsigned long)theme.pix_bg;
    attr.override_redirect = True;
    attr.event_mask        = KeyPressMask | ButtonPressMask | ExposureMask;
    attr.border_pixel      = (unsigned long)theme.pix_bact;

    win = XCreateWindow(dpy, root, (sw - W) / 2, (sh - H) / 3,
        (unsigned)W, (unsigned)H, BORDER_WIDTH,
        DefaultDepth(dpy, scr), InputOutput, DefaultVisual(dpy, scr),
        CWBackPixel | CWOverrideRedirect | CWEventMask | CWBorderPixel,
        &attr);

    XStoreName(dpy, win, "min-net");
    xd = XftDrawCreate(dpy, win, DefaultVisual(dpy, scr),
                        DefaultColormap(dpy, scr));
    XMapRaised(dpy, win);
    XSetInputFocus(dpy, win, RevertToPointerRoot, CurrentTime);

    /* Mostrar caché inmediatamente, escanear en segundo plano */
    cache_load();
    draw();
    if (fork() == 0) { setsid(); net_scan(); _exit(0); }

    XEvent ev;
    while (XNextEvent(dpy, &ev) == 0) {
        switch (ev.type) {
        case Expose:      if (ev.xexpose.count == 0) draw(); break;
        case KeyPress:    on_key(&ev.xkey); break;
        case ButtonPress: on_click(ev.xbutton.x, ev.xbutton.y); break;
        }
    }
    XftFontClose(dpy, font);
    XftDrawDestroy(xd);
    XCloseDisplay(dpy);
    return 0;
}
