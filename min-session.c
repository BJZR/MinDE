/* min-session.c — Menú de sesión de minDE
 *
 * Mismo estilo que min-launch.
 * Aparece como overlay centrado sobre todo.
 * Captura teclado y ratón mientras está abierto.
 *
 * Opciones:
 *   Bloquear pantalla
 *   Cerrar sesión
 *   Reiniciar
 *   Apagar
 *
 * Los comandos se configuran en ~/.config/minde/session.conf
 * Esc o click fuera → cerrar sin hacer nada
 *
 * Compilar:
 *   gcc -O2 -o min-session min-session.c \
 *       $(pkg-config --cflags --libs x11 xft fontconfig)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>

#include "config.h"

/* ═══ OPCIONES DE SESIÓN ════════════════════════════════════ */

typedef struct {
    const char *label;
    char        cmd[256];
    const char *key;   /* tecla rápida */
} SesOpt;

static SesOpt opts[] = {
    { "Bloquear",        "xlock -mode blank",    "L" },
    { "Cerrar sesion",   "pkill -x min-wm",      "E" },
    { "Reiniciar",       "systemctl reboot",      "R" },
    { "Apagar",          "systemctl poweroff",    "P" },
};
#define NOPTS  4

/* Cargar comandos personalizados desde ~/.config/minde/session.conf */
static void conf_load(void) {
    const char *h = getenv("HOME"); if (!h) return;
    char path[512];
    snprintf(path, sizeof path, "%s/.config/minde/session.conf", h);
    FILE *f = fopen(path, "r");
    if (!f) {
        /* Crear con defaults */
        char dir[256]; snprintf(dir, sizeof dir, "%s/.config/minde", h);
        mkdir(dir, 0755);
        f = fopen(path, "w");
        if (f) {
            fputs(
                "# min-session — comandos de sesión\n"
                "lock=xlock -mode blank\n"
                "logout=pkill -x min-wm\n"
                "reboot=systemctl reboot\n"
                "poweroff=systemctl poweroff\n", f);
            fclose(f);
        }
        return;
    }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = '\0'; char *k = line, *v = eq + 1;
        if      (!strcmp(k, "lock"))     snprintf(opts[0].cmd, 256, "%.*s", 255, v);
        else if (!strcmp(k, "logout"))   snprintf(opts[1].cmd, 256, "%.*s", 255, v);
        else if (!strcmp(k, "reboot"))   snprintf(opts[2].cmd, 256, "%.*s", 255, v);
        else if (!strcmp(k, "poweroff")) snprintf(opts[3].cmd, 256, "%.*s", 255, v);
    }
    fclose(f);
}

/* ═══ X11 ════════════════════════════════════════════════════ */

static Display  *dpy;
static int       scr;
static Window    root, win;
static XftDraw  *xd;
static XftFont  *font_big, *font;
static Theme     theme;
static XftColor  c_bg, c_fg, c_accent, c_dim;

static int selected = 0;

/* Dimensiones del panel */
#define PANEL_W  320
static int panel_h, row_h, pad;
static int px, py;   /* posición del panel */

static void alloc_color(XftColor *c, const char *hex) {
    if (!XftColorAllocName(dpy, DefaultVisual(dpy, scr),
            DefaultColormap(dpy, scr), hex, c))
        XftColorAllocName(dpy, DefaultVisual(dpy, scr),
            DefaultColormap(dpy, scr), "#ffffff", c);
}

static int text_w(XftFont *f, const char *s) {
    XGlyphInfo e;
    XftTextExtentsUtf8(dpy, f, (const FcChar8 *)s, strlen(s), &e);
    return e.xOff;
}

static void draw_text(XftFont *f, int x, int y, const char *s, XftColor *c) {
    XftDrawStringUtf8(xd, c, f, x, y, (const FcChar8 *)s, strlen(s));
}

static void draw(void) {
    GC gc = DefaultGC(dpy, scr);
    int sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr);

    /* Fondo oscuro semitransparente (simulado con color sólido) */
    XSetForeground(dpy, gc, 0x0d0e18);
    XFillRectangle(dpy, win, gc, 0, 0, (unsigned)sw, (unsigned)sh);

    /* Panel central */
    XSetForeground(dpy, gc, (unsigned long)theme.pix_bg);
    XFillRectangle(dpy, win, gc, px, py,
                   (unsigned)PANEL_W, (unsigned)panel_h);
    /* Borde del panel */
    XSetForeground(dpy, gc, (unsigned long)theme.pix_bact);
    XDrawRectangle(dpy, win, gc, px, py,
                   (unsigned)(PANEL_W-1), (unsigned)(panel_h-1));

    /* Título */
    int hdr_h = row_h + pad;
    XSetForeground(dpy, gc, (unsigned long)theme.pix_bact);
    XFillRectangle(dpy, win, gc, px, py, (unsigned)PANEL_W, (unsigned)hdr_h);

    const char *title = "Sesion";
    int tw = text_w(font, title);
    draw_text(font, px + (PANEL_W - tw)/2,
              py + pad + font->ascent, title, &c_bg);

    /* Separador */
    XSetForeground(dpy, gc, 0x3a3f5c);
    XFillRectangle(dpy, win, gc, px, py + hdr_h,
                   (unsigned)PANEL_W, 1);

    /* Opciones */
    for (int i = 0; i < NOPTS; i++) {
        int iy = py + hdr_h + 1 + i * row_h;
        int ty = iy + font->ascent + (row_h - font->ascent - font->descent)/2;

        if (i == selected) {
            XSetForeground(dpy, gc, (unsigned long)theme.pix_bact);
            XFillRectangle(dpy, win, gc, px, iy,
                           (unsigned)PANEL_W, (unsigned)row_h);
        } else if (i % 2 != 0) {
            XSetForeground(dpy, gc, (theme.pix_bg & 0xfefefe) + 0x060608);
            XFillRectangle(dpy, win, gc, px, iy,
                           (unsigned)PANEL_W, (unsigned)row_h);
        }

        /* Tecla rápida */
        XftColor *kc = (i == selected) ? &c_bg : &c_accent;
        char kl[4]; snprintf(kl, sizeof kl, "[%s]", opts[i].key);
        draw_text(font, px + pad, ty, kl, kc);

        /* Label */
        XftColor *lc = (i == selected) ? &c_bg : &c_fg;
        draw_text(font, px + pad + text_w(font,"[L] ") + 4, ty, opts[i].label, lc);
    }

    /* Hint escape */
    int hint_y = py + panel_h - pad/2 - font->descent;
    const char *hint = "Esc — cancelar";
    int hw = text_w(font, hint);
    draw_text(font, px + (PANEL_W - hw)/2, hint_y, hint, &c_dim);

    XFlush(dpy);
}

static void run_opt(int i) {
    if (i < 0 || i >= NOPTS || !opts[i].cmd[0]) return;
    if (fork() == 0) {
        setsid();
        execlp("/bin/sh", "sh", "-c", opts[i].cmd, NULL);
        _exit(127);
    }
    XCloseDisplay(dpy); exit(0);
}

static void on_key(XKeyEvent *e) {
    char buf[8] = ""; KeySym sym;
    XLookupString(e, buf, sizeof buf - 1, &sym, NULL);

    switch (sym) {
    case XK_Escape: case XK_q:
        XCloseDisplay(dpy); exit(0);
    case XK_Return: case XK_KP_Enter:
        run_opt(selected); break;
    case XK_Up: case XK_KP_Up:
        if (selected > 0) selected--;
        draw(); break;
    case XK_Down: case XK_KP_Down:
        if (selected < NOPTS - 1) selected++;
        draw(); break;
    /* Teclas rápidas */
    case XK_l: case XK_L: run_opt(0); break;
    case XK_e: case XK_E: run_opt(1); break;
    case XK_r: case XK_R: run_opt(2); break;
    case XK_p: case XK_P: run_opt(3); break;
    default: break;
    }
}

static void on_click(int mx, int my) {
    /* Click fuera del panel → cancelar */
    if (mx < px || mx >= px + PANEL_W || my < py || my >= py + panel_h) {
        XCloseDisplay(dpy); exit(0);
    }
    int hdr_h = row_h + pad;
    for (int i = 0; i < NOPTS; i++) {
        int iy = py + hdr_h + 1 + i * row_h;
        if (my >= iy && my < iy + row_h) {
            if (i == selected) { run_opt(i); return; }
            selected = i; draw(); return;
        }
    }
}

/* ═══ MAIN ═══════════════════════════════════════════════════ */

int main(void) {
    conf_load();
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fputs("min-session: no X\n", stderr); return 1; }
    scr  = DefaultScreen(dpy);
    root = DefaultRootWindow(dpy);

    theme_init(&theme);

    font_big = XftFontOpenName(dpy, scr,
        "monospace:size=11:weight=medium:antialias=true");
    font = XftFontOpenName(dpy, scr, FONT_NAME);
    if (!font_big) font_big = font;
    if (!font) { fputs("min-session: no font\n", stderr); return 1; }

    alloc_color(&c_bg,     theme.bg);
    alloc_color(&c_fg,     theme.fg);
    alloc_color(&c_accent, theme.accent);
    alloc_color(&c_dim,    theme.dim);

    pad   = LAUNCHER_PAD + 4;
    row_h = font->ascent + font->descent + LAUNCHER_PAD + 2;
    int hdr_h = row_h + pad;
    panel_h = hdr_h + 1 + NOPTS * row_h + pad;

    int sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr);
    px = (sw - PANEL_W) / 2;
    py = (sh - panel_h) / 3;

    /* Ventana pantalla completa con override_redirect */
    XSetWindowAttributes attr = {0};
    attr.background_pixel  = 0x0d0e18;
    attr.override_redirect = True;
    attr.event_mask = KeyPressMask | ButtonPressMask | ExposureMask;

    win = XCreateWindow(dpy, root, 0, 0,
        (unsigned)sw, (unsigned)sh, 0,
        DefaultDepth(dpy, scr), InputOutput, DefaultVisual(dpy, scr),
        CWBackPixel | CWOverrideRedirect | CWEventMask, &attr);

    XStoreName(dpy, win, "min-session");
    xd = XftDrawCreate(dpy, win, DefaultVisual(dpy, scr),
                        DefaultColormap(dpy, scr));
    XMapRaised(dpy, win);

    /* Capturar teclado y ratón */
    XGrabKeyboard(dpy, win, True,
        GrabModeAsync, GrabModeAsync, CurrentTime);
    XGrabPointer(dpy, win, True,
        ButtonPressMask, GrabModeAsync, GrabModeAsync,
        None, None, CurrentTime);
    XSetInputFocus(dpy, win, RevertToPointerRoot, CurrentTime);

    draw();

    XEvent ev;
    while (XNextEvent(dpy, &ev) == 0) {
        switch (ev.type) {
        case Expose:      if (ev.xexpose.count == 0) draw(); break;
        case KeyPress:    on_key(&ev.xkey); break;
        case ButtonPress: on_click(ev.xbutton.x, ev.xbutton.y); break;
        }
    }

    XUngrabKeyboard(dpy, CurrentTime);
    XUngrabPointer(dpy, CurrentTime);
    XftFontClose(dpy, font);
    if (font_big != font) XftFontClose(dpy, font_big);
    XftDrawDestroy(xd);
    XCloseDisplay(dpy);
    return 0;
}
