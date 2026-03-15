/* minrun.c — Launcher de aplicaciones de minDE
 *
 * Launcher escrito desde cero en C puro + Xlib + Xft.
 * Sin dmenu, sin rofi, sin dependencias extra.
 *
 * Funcionamiento:
 *   1. Escanea todos los directorios del PATH en busca de ejecutables
 *   2. Muestra una ventana flotante centrada con campo de búsqueda
 *   3. Filtra la lista al escribir (búsqueda por prefijo + substring)
 *   4. Navegar con ↑/↓, ejecutar con Enter, cerrar con Escape
 *
 * Layout:
 *   ╔════════════════════════════╗
 *   ║ > firefox_                 ║  ← campo de entrada
 *   ╟────────────────────────────╢
 *   ║ firefox                    ║  ← ítem (seleccionado con fondo)
 *   ║ firefox-esr                ║
 *   ║ ...                        ║
 *   ╚════════════════════════════╝
 *
 * Compilar (ver Makefile):
 *   gcc -O2 -o minrun minrun.c \
 *       $(pkg-config --cflags --libs x11 xft fontconfig)
 */

#define _GNU_SOURCE   /* strdup, strncasecmp, strcasestr */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>

#include "config.h"

/* ── Catálogo de ejecutables ──────────────────────────────── */

#define MAX_ENTRIES  8192
#define MAX_NAMELEN  256

static char  entries[MAX_ENTRIES][MAX_NAMELEN];
static int   nentries = 0;

/* ── Estado del launcher ─────────────────────────────────── */

static Display  *dpy;
static int       scr;
static Window    root;
static Window    win;

static XftDraw  *xd;
static XftFont  *font;
static XftColor  c_bg, c_fg, c_accent, c_dim, c_selbg, c_border;

static Theme     theme;

/* Entrada de texto */
static char  input[MAX_NAMELEN] = "";
static int   input_len = 0;

/* Lista filtrada */
static int   match_idx[MAX_ENTRIES];  /* índices en entries[] */
static int   nmatches = 0;
static int   selected = 0;            /* ítem seleccionado    */
static int   scroll   = 0;            /* primera fila visible */

/* Dimensiones */
static int W, H;
static int row_h;

/* ── Helpers Xft ─────────────────────────────────────────── */

static void alloc_color(XftColor *c, const char *hex) {
    if (!XftColorAllocName(dpy,
            DefaultVisual(dpy, scr),
            DefaultColormap(dpy, scr), hex, c))
        XftColorAllocName(dpy,
            DefaultVisual(dpy, scr),
            DefaultColormap(dpy, scr), "#000000", c);
}

static int text_w(const char *s) {
    XGlyphInfo ext;
    XftTextExtentsUtf8(dpy, font, (const FcChar8 *)s, strlen(s), &ext);
    return ext.xOff;
}

static void draw_text(int x, int y, const char *s, XftColor *col) {
    XftDrawStringUtf8(xd, col, font, x, y,
        (const FcChar8 *)s, strlen(s));
}

/* ── Escaneo del PATH ─────────────────────────────────────── */

static int entry_cmp(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

static int entry_exists(const char *name) {
    /* Búsqueda binaria porque ya están ordenados en inserción
     * lineal es suficiente durante el llenado inicial */
    for (int i = 0; i < nentries; i++)
        if (strcmp(entries[i], name) == 0) return 1;
    return 0;
}

static void scan_path(void) {
    char *pathenv = getenv("PATH");
    if (!pathenv) return;

    char *pathcopy = strdup(pathenv);
    if (!pathcopy) return;

    char *dir = strtok(pathcopy, ":");
    while (dir && nentries < MAX_ENTRIES) {
        DIR *d = opendir(dir);
        if (!d) { dir = strtok(NULL, ":"); continue; }

        struct dirent *ent;
        while ((ent = readdir(d)) && nentries < MAX_ENTRIES) {
            if (ent->d_name[0] == '.') continue;

            char full[512];
            snprintf(full, sizeof full, "%s/%s", dir, ent->d_name);

            struct stat st;
            if (stat(full, &st) != 0) continue;
            if (!S_ISREG(st.st_mode)) continue;
            if (!(st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) continue;
            if (strlen(ent->d_name) >= MAX_NAMELEN) continue;

            if (!entry_exists(ent->d_name)) {
                strncpy(entries[nentries], ent->d_name, MAX_NAMELEN - 1);
                entries[nentries][MAX_NAMELEN - 1] = '\0';
                nentries++;
            }
        }
        closedir(d);
        dir = strtok(NULL, ":");
    }
    free(pathcopy);

    /* Ordenar alfabéticamente */
    qsort(entries, (size_t)nentries, MAX_NAMELEN, entry_cmp);
}

/* ── Filtrado ─────────────────────────────────────────────── */

static void filter(void) {
    nmatches = 0;
    selected = 0;
    scroll   = 0;

    if (input_len == 0) {
        /* Sin input: mostrar todos */
        for (int i = 0; i < nentries && nmatches < MAX_ENTRIES; i++)
            match_idx[nmatches++] = i;
        return;
    }

    /* Primero: coincidencias por prefijo (prioridad más alta) */
    for (int i = 0; i < nentries && nmatches < MAX_ENTRIES; i++) {
        if (strncasecmp(entries[i], input, (size_t)input_len) == 0)
            match_idx[nmatches++] = i;
    }

    /* Después: coincidencias por substring (no duplicar) */
    for (int i = 0; i < nentries && nmatches < MAX_ENTRIES; i++) {
        if (strncasecmp(entries[i], input, (size_t)input_len) == 0)
            continue;  /* ya está */
        if (strcasestr(entries[i], input))
            match_idx[nmatches++] = i;
    }
}

/* ── Dibujo ───────────────────────────────────────────────── */

static void draw(void) {
    GC gc = DefaultGC(dpy, scr);

    /* Fondo general */
    XSetForeground(dpy, gc, (unsigned long)theme.pix_bg);
    XFillRectangle(dpy, win, gc, 0, 0, (unsigned)W, (unsigned)H);

    /* Línea divisoria fondo campo de entrada */
    int input_h = row_h + 2 * LAUNCHER_PAD;
    XSetForeground(dpy, gc, (unsigned long)theme.pix_bact);
    XFillRectangle(dpy, win, gc, 0, 0, (unsigned)W, (unsigned)input_h);

    /* Prompt ">" */
    int baseline = LAUNCHER_PAD + font->ascent;
    draw_text(LAUNCHER_PAD, baseline, ">", &c_accent);

    /* Texto de entrada */
    char cur[MAX_NAMELEN + 2];
    snprintf(cur, sizeof cur, "%s", input);
    draw_text(LAUNCHER_PAD + text_w("> ") + 4, baseline, cur, &c_fg);

    /* Cursor de texto (línea vertical) */
    int cursor_x = LAUNCHER_PAD + text_w("> ") + 4 + text_w(input);
    XSetForeground(dpy, gc, ((unsigned long)theme.pix_bact & 0x00ffffffUL) | 0xc0c0c0UL);
    /* Simplificado: dibujar cursor con Xft */
    draw_text(cursor_x, baseline, "_", &c_fg);

    /* Separador */
    XSetForeground(dpy, gc, 0x3a3f5c);
    XFillRectangle(dpy, win, gc, 0, input_h, (unsigned)W, 1);

    /* Lista de ítems */
    int visible = LAUNCHER_ROWS;
    if (nmatches < visible) visible = nmatches;

    for (int i = 0; i < visible; i++) {
        int midx = scroll + i;
        if (midx >= nmatches) break;

        int iy = input_h + 1 + i * row_h;
        int text_y = iy + font->ascent + (row_h - font->ascent - font->descent) / 2;

        if (midx == selected) {
            /* Fondo ítem seleccionado */
            XSetForeground(dpy, gc, (unsigned long)theme.pix_bact);
            XFillRectangle(dpy, win, gc, 0, iy, (unsigned)W, (unsigned)row_h);
            draw_text(LAUNCHER_PAD, text_y,
                entries[match_idx[midx]], &c_fg);
        } else {
            XftColor *col = (i % 2 == 0) ? &c_fg : &c_dim;
            draw_text(LAUNCHER_PAD, text_y,
                entries[match_idx[midx]], col);
        }
    }

    /* Scrollbar mínima si hay más ítems de los visibles */
    if (nmatches > LAUNCHER_ROWS) {
        int sb_h = H - (row_h + 2 * LAUNCHER_PAD) - 1;
        int th   = sb_h * LAUNCHER_ROWS / nmatches;
        if (th < 4) th = 4;
        int ty = (row_h + 2 * LAUNCHER_PAD) + 1
               + (sb_h - th) * scroll / (nmatches - LAUNCHER_ROWS);
        XSetForeground(dpy, gc, (unsigned long)theme.pix_bact);
        XFillRectangle(dpy, win, gc, W - 3, ty, 3, (unsigned)th);
    }

    XFlush(dpy);
}

/* ── Navegación ───────────────────────────────────────────── */

static void sel_up(void) {
    if (selected > 0) selected--;
    if (selected < scroll) scroll = selected;
    draw();
}

static void sel_down(void) {
    if (selected < nmatches - 1) selected++;
    if (selected >= scroll + LAUNCHER_ROWS)
        scroll = selected - LAUNCHER_ROWS + 1;
    draw();
}

/* ── Ejecución ────────────────────────────────────────────── */

static void execute(const char *cmd) {
    if (!cmd || !cmd[0]) return;
    XUnmapWindow(dpy, win);
    XFlush(dpy);
    XCloseDisplay(dpy);
    execlp(cmd, cmd, NULL);
    /* Si falla, salir */
    _exit(127);
}

/* ── Manejo de teclado ────────────────────────────────────── */

static void on_key(XKeyEvent *e) {
    char buf[32] = "";
    KeySym sym;
    XLookupString(e, buf, sizeof buf - 1, &sym, NULL);

    switch (sym) {
    case XK_Escape:
        XCloseDisplay(dpy);
        exit(0);

    case XK_Return:
    case XK_KP_Enter:
        if (nmatches > 0 && selected < nmatches)
            execute(entries[match_idx[selected]]);
        else if (input_len > 0)
            execute(input);
        break;

    case XK_BackSpace:
        if (input_len > 0) {
            /* Borrar último byte UTF-8 */
            do { input_len--; }
            while (input_len > 0 && (input[input_len] & 0xC0) == 0x80);
            input[input_len] = '\0';
            filter();
            draw();
        }
        break;

    case XK_Up:
    case XK_KP_Up:
        sel_up();
        break;

    case XK_Down:
    case XK_KP_Down:
        sel_down();
        break;

    case XK_Tab:
        /* Completar con el primer resultado */
        if (nmatches > 0) {
            strncpy(input, entries[match_idx[0]], MAX_NAMELEN - 1);
            input_len = strlen(input);
            filter();
            draw();
        }
        break;

    default:
        /* Caracter imprimible */
        if (buf[0] >= 0x20 && input_len < MAX_NAMELEN - 1) {
            int blen = strlen(buf);
            if (input_len + blen < MAX_NAMELEN) {
                memcpy(input + input_len, buf, (size_t)blen);
                input_len += blen;
                input[input_len] = '\0';
                filter();
                draw();
            }
        }
        break;
    }
}

/* ── Crear ventana del launcher ───────────────────────────── */

static void setup_window(void) {
    int sw = DisplayWidth(dpy, scr);
    int sh = DisplayHeight(dpy, scr);

    row_h = font->ascent + font->descent + LAUNCHER_PAD;
    int input_h = row_h + 2 * LAUNCHER_PAD;
    W = LAUNCHER_W;
    H = input_h + 1 + LAUNCHER_ROWS * row_h;

    int wx = (sw - W) / 2;
    int wy = (sh - H) / 3;  /* Ligeramente hacia arriba */

    XSetWindowAttributes attr = {0};
    attr.background_pixel  = (unsigned long)theme.pix_bg;
    attr.override_redirect = True;
    attr.event_mask        = KeyPressMask | ExposureMask;
    attr.border_pixel      = (unsigned long)theme.pix_bact;

    win = XCreateWindow(dpy, root,
        wx, wy, (unsigned)W, (unsigned)H,
        BORDER_WIDTH,
        DefaultDepth(dpy, scr),
        InputOutput,
        DefaultVisual(dpy, scr),
        CWBackPixel | CWOverrideRedirect | CWEventMask | CWBorderPixel,
        &attr);

    /* Nombre de la ventana */
    XStoreName(dpy, win, "min-launch");

    /* Xft draw context */
    xd = XftDrawCreate(dpy, win,
        DefaultVisual(dpy, scr),
        DefaultColormap(dpy, scr));

    XMapRaised(dpy, win);

    /* Tomar el foco del teclado */
    XSetInputFocus(dpy, win, RevertToPointerRoot, CurrentTime);
    XFlush(dpy);
}

/* ── main ─────────────────────────────────────────────────── */

int main(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fputs("minrun: no se pudo abrir X display\n", stderr);
        return 1;
    }
    scr  = DefaultScreen(dpy);
    root = DefaultRootWindow(dpy);

    /* Tema y fuente */
    theme_init(&theme);
    font = XftFontOpenName(dpy, scr, FONT_NAME);
    if (!font) {
        font = XftFontOpenName(dpy, scr, "fixed:size=10");
        if (!font) {
            fputs("minrun: no se pudo cargar fuente\n", stderr);
            XCloseDisplay(dpy);
            return 1;
        }
    }

    alloc_color(&c_bg,     theme.bg);
    alloc_color(&c_fg,     theme.fg);
    alloc_color(&c_accent, theme.accent);
    alloc_color(&c_dim,    theme.dim);
    alloc_color(&c_selbg,  theme.selbg);
    alloc_color(&c_border, theme.bact);

    /* Escanear ejecutables */
    scan_path();
    filter();   /* lista inicial: todos */

    /* Crear ventana y arrancar */
    setup_window();
    draw();

    /* ── Bucle de eventos ── */
    XEvent ev;
    while (XNextEvent(dpy, &ev) == 0) {
        switch (ev.type) {
        case KeyPress:
            on_key(&ev.xkey);
            break;
        case Expose:
            if (ev.xexpose.count == 0) draw();
            break;
        }
    }

    XftFontClose(dpy, font);
    XftDrawDestroy(xd);
    XCloseDisplay(dpy);
    return 0;
}
