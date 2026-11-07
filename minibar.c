/* minibar.c — Barra de estado de minDE
 *
 * Dibuja directamente en X11 con Xlib + Xft (sin lemonbar, sin polybar).
 * Lee el estado del WM vía EWMH:
 *   _NET_CURRENT_DESKTOP → workspace activo
 *   _NET_ACTIVE_WINDOW   → ventana enfocada
 *   _NET_WM_NAME         → título de la ventana activa
 *
 * Diseño:
 *   [  1  2 ●3  4  5  ]  [     título ventana     ]  [ HH:MM ]
 *
 * Compilar (ver Makefile):
 *   gcc -O2 -o minibar minibar.c \
 *       $(pkg-config --cflags --libs x11 xft fontconfig) -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>

#include "config.h"

/* ── Estado global ────────────────────────────────────────── */

static Display  *dpy;
static int       scr;
static Window    root;
static Window    bar;
static int       W, H;      /* ancho y alto de la barra */

static XftDraw  *xd;
static XftFont  *font;

static XftColor  c_bg, c_fg, c_accent, c_dim, c_urgent;

static Atom      a_cur_desk;   /* _NET_CURRENT_DESKTOP  */
static Atom      a_active;     /* _NET_ACTIVE_WINDOW    */
static Atom      a_wm_name;    /* _NET_WM_NAME          */
static Atom      a_utf8;       /* UTF8_STRING            */
static Atom      a_strut;      /* _NET_WM_STRUT_PARTIAL  */
static Atom      a_wtype;      /* _NET_WM_WINDOW_TYPE    */
static Atom      a_wtype_dock; /* _NET_WM_WINDOW_TYPE_DOCK */
static Atom      a_state;      /* _NET_WM_STATE          */
static Atom      a_state_above;/* _NET_WM_STATE_ABOVE    */

static Theme     theme;

static int       cur_ws   = 0;
static char      win_title[256] = "";

/* ── Xft helpers ─────────────────────────────────────────── */

static void alloc_color(XftColor *c, const char *hex) {
    if (!XftColorAllocName(dpy,
            DefaultVisual(dpy, scr),
            DefaultColormap(dpy, scr),
            hex, c)) {
        /* Fallback a negro */
        XftColorAllocName(dpy,
            DefaultVisual(dpy, scr),
            DefaultColormap(dpy, scr),
            "#000000", c);
    }
}

/* Devuelve el ancho en píxeles de una cadena con la fuente cargada */
static int text_w(const char *s) {
    XGlyphInfo ext;
    XftTextExtentsUtf8(dpy, font, (const FcChar8 *)s, strlen(s), &ext);
    return ext.xOff;
}

/* ── Lectura de propiedades EWMH ──────────────────────────── */

static long get_long_prop(Window w, Atom prop) {
    Atom type; int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    long val = -1;
    if (XGetWindowProperty(dpy, w, prop, 0, 1, False,
            XA_CARDINAL, &type, &fmt, &n, &after, &data) == Success
            && data) {
        val = *(long *)data;
        XFree(data);
    }
    return val;
}

static Window get_active_window(void) {
    Atom type; int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    Window active = None;
    if (XGetWindowProperty(dpy, root, a_active, 0, 1, False,
            XA_WINDOW, &type, &fmt, &n, &after, &data) == Success
            && data) {
        active = *(Window *)data;
        XFree(data);
    }
    return active;
}

static void get_win_title(Window w, char *out, int maxlen) {
    out[0] = '\0';
    if (!w || w == None) return;
    Atom type; int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;

    /* Intentar _NET_WM_NAME (UTF-8) */
    if (XGetWindowProperty(dpy, w, a_wm_name, 0, 256, False,
            a_utf8, &type, &fmt, &n, &after, &data) == Success
            && data && n > 0) {
        strncpy(out, (char *)data, (size_t)(maxlen - 1));
        out[maxlen - 1] = '\0';
        XFree(data);
        return;
    }
    if (data) XFree(data);

    /* Fallback: WM_NAME */
    XTextProperty tp;
    if (XGetWMName(dpy, w, &tp) && tp.value) {
        strncpy(out, (char *)tp.value, (size_t)(maxlen - 1));
        out[maxlen - 1] = '\0';
        XFree(tp.value);
    }
}

/* ── Dibujo ───────────────────────────────────────────────── */

/* Dibuja texto centrado verticalmente en (x, ancho disponible) */
static void draw_text_at(int x, int max_w, const char *s, XftColor *col) {
    if (!s || !s[0]) return;
    char buf[256];
    strncpy(buf, s, 255); buf[255] = '\0';
    /* Truncar si no cabe */
    while (text_w(buf) > max_w && strlen(buf) > 3) {
        buf[strlen(buf) - 4] = '.';
        buf[strlen(buf) - 3] = '.';
        buf[strlen(buf) - 2] = '.';
        buf[strlen(buf) - 1] = '\0';
    }
    XftDrawStringUtf8(xd, col, font, x,
        (H / 2) + (font->ascent - font->descent) / 2,
        (const FcChar8 *)buf, strlen(buf));
}

static void draw_bar(void) {
    /* Fondo */
    XSetForeground(dpy, DefaultGC(dpy, scr),
        (unsigned long)theme.pix_bg);
    XFillRectangle(dpy, bar, DefaultGC(dpy, scr), 0, 0, (unsigned)W, (unsigned)H);

    /* ── Sección izquierda: workspaces ── */
    int pad = 14;
    int x = 8;

    for (int i = 0; i < NUM_WS; i++) {
        char label[4];
        snprintf(label, sizeof label, "%d", i + 1);

        if (i == cur_ws) {
            /* Workspace activo: fondo acento */
            int lw = text_w(label);
            int bx = x - 5, bw = lw + 10;
            XSetForeground(dpy, DefaultGC(dpy, scr),
                (unsigned long)theme.pix_bact);
            XFillRectangle(dpy, bar, DefaultGC(dpy, scr),
                bx, 3, (unsigned)bw, (unsigned)(H - 6));
            draw_text_at(x, bw, label, &c_fg);
            x += bw + 4;
        } else {
            draw_text_at(x, pad, label, &c_dim);
            x += pad;
        }
    }

    /* ── Sección derecha: reloj ── */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char clock_str[16];
    strftime(clock_str, sizeof clock_str, "%H:%M", tm);

    int cw = text_w(clock_str);
    draw_text_at(W - cw - 12, cw + 12, clock_str, &c_dim);

    /* ── Centro: título ventana activa ── */
    if (win_title[0]) {
        int title_area_x = x + 20;
        int title_area_w = W - cw - 30 - title_area_x;
        if (title_area_w > 40) {
            /* Centrar el título en el área disponible */
            int tw = text_w(win_title);
            int tx = title_area_x + (title_area_w - tw) / 2;
            if (tx < title_area_x) tx = title_area_x;
            draw_text_at(tx, title_area_w, win_title, &c_fg);
        }
    }

    XFlush(dpy);
}

/* ── Actualizar estado desde EWMH ────────────────────────── */

static void update_state(void) {
    /* Solo leer estado EWMH — NO recargar tema (causaría loop) */
    long d = get_long_prop(root, a_cur_desk);
    if (d >= 0 && d < NUM_WS) cur_ws = (int)d;

    Window active = get_active_window();
    get_win_title(active, win_title, sizeof win_title);
}

/* ── Configurar ventana de barra ─────────────────────────── */

static void setup_bar_window(void) {
    W = DisplayWidth(dpy, scr);
    H = BAR_HEIGHT;

    XSetWindowAttributes attr = {0};
    attr.override_redirect = True;
    attr.background_pixel  = (unsigned long)theme.pix_bg;
    attr.event_mask        = ExposureMask;

    bar = XCreateWindow(dpy, root,
        0, 0, (unsigned)W, (unsigned)H, 0,
        DefaultDepth(dpy, scr),
        InputOutput,
        DefaultVisual(dpy, scr),
        CWOverrideRedirect | CWBackPixel | CWEventMask,
        &attr);

    /* Tipo dock: el WM no lo gestiona, siempre encima */
    XChangeProperty(dpy, bar, a_wtype, XA_ATOM, 32,
        PropModeReplace, (unsigned char *)&a_wtype_dock, 1);

    /* Estado: siempre encima */
    XChangeProperty(dpy, bar, a_state, XA_ATOM, 32,
        PropModeReplace, (unsigned char *)&a_state_above, 1);

    /* Strut: reservar espacio en la parte superior de la pantalla */
    long strut[12] = {0};
    strut[2] = H;          /* top strut                 */
    strut[8] = 0;          /* top_start_x               */
    strut[9] = W - 1;      /* top_end_x                 */
    XChangeProperty(dpy, bar, a_strut, XA_CARDINAL, 32,
        PropModeReplace, (unsigned char *)strut, 12);

    /* WM_CLASS para identificación */
    XClassHint *ch = XAllocClassHint();
    if (ch) {
        ch->res_name  = (char *)"minibar";
        ch->res_class = (char *)"Minibar";
        XSetClassHint(dpy, bar, ch);
        XFree(ch);
    }

    /* Xft draw context */
    xd = XftDrawCreate(dpy, bar,
        DefaultVisual(dpy, scr),
        DefaultColormap(dpy, scr));

    XMapWindow(dpy, bar);
    XFlush(dpy);
}

/* ── Suscribirse a cambios de propiedades en la raíz ─────── */

static void subscribe_root(void) {
    /* Solo escuchar cambios de propiedades EWMH del WM */
    XSelectInput(dpy, root, PropertyChangeMask);
}

/* ── main ─────────────────────────────────────────────────── */

int main(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fputs("minibar: no se pudo abrir X display\n", stderr);
        return 1;
    }
    scr  = DefaultScreen(dpy);
    root = DefaultRootWindow(dpy);

    /* Internalized atoms */
    a_cur_desk   = XInternAtom(dpy, "_NET_CURRENT_DESKTOP",  False);
    a_active     = XInternAtom(dpy, "_NET_ACTIVE_WINDOW",    False);
    a_wm_name    = XInternAtom(dpy, "_NET_WM_NAME",          False);
    a_utf8       = XInternAtom(dpy, "UTF8_STRING",           False);
    a_strut      = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);
    a_wtype      = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE",   False);
    a_wtype_dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    a_state      = XInternAtom(dpy, "_NET_WM_STATE",         False);
    a_state_above= XInternAtom(dpy, "_NET_WM_STATE_ABOVE",   False);

    /* Tema y fuente */
    theme_init(&theme);
    font = XftFontOpenName(dpy, scr, FONT_NAME);
    if (!font) {
        /* Fallback a fixed si no hay la fuente pedida */
        font = XftFontOpenName(dpy, scr, "fixed:size=9");
        if (!font) {
            fputs("minibar: no se pudo cargar fuente\n", stderr);
            XCloseDisplay(dpy);
            return 1;
        }
    }

    alloc_color(&c_bg,     theme.bg);
    alloc_color(&c_fg,     theme.fg);
    alloc_color(&c_accent, theme.accent);
    alloc_color(&c_dim,    theme.dim);
    alloc_color(&c_urgent, theme.urgent);

    subscribe_root();
    setup_bar_window();
    update_state();
    draw_bar();

    int xfd = ConnectionNumber(dpy);

    /* ── Bucle de eventos con timeout de 1s para el reloj ── */
    for (;;) {
        /* Procesar todos los eventos pendientes */
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);

            if (ev.type == Expose && ev.xexpose.count == 0) {
                draw_bar();
            } else if (ev.type == PropertyNotify) {
                Atom a = ev.xproperty.atom;
    /* Solo actuar si el evento viene de la raíz y es un átomo que nos importa */
            if (ev.xproperty.window == root &&
                (a == a_cur_desk || a == a_active || a == a_wm_name)) {
                    update_state();
                    draw_bar();
                }
            }
        }

        /* Esperar 1 segundo o hasta que llegue un evento X */
        fd_set fds;
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        if (select(xfd + 1, &fds, NULL, NULL, &tv) == 0) {
            /* Timeout: actualizar reloj */
            draw_bar();
        }
    }

    /* No se llega aquí normalmente, pero por limpieza: */
    XftFontClose(dpy, font);
    XftDrawDestroy(xd);
    XCloseDisplay(dpy);
    return 0;
}
