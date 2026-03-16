/* min-clip.c — Portapapeles de minDE
 *
 * Mismo estilo que min-launch. Campo de búsqueda para filtrar.
 *
 * Modos:
 *   min-clip           → selector con búsqueda
 *   min-clip --daemon  → daemon (requiere xclip instalado)
 *
 * El daemon usa xclip como backend — es mucho más confiable que
 * implementar el protocolo X11 Selection a mano.
 *
 * Historial: ~/.config/minde/clipboard.txt
 * Instalar xclip: sudo xbps-install xclip
 *
 * Compilar:
 *   gcc -O2 -o min-clip min-clip.c \
 *       $(pkg-config --cflags --libs x11 xft fontconfig)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>

#include "config.h"

/* ═══ HISTORIAL ═════════════════════════════════════════════ */

#define MAX_CLIP  128
#define MAX_LEN   512

static char clips[MAX_CLIP][MAX_LEN];
static int  nclips = 0;

static void hist_dir(char *out, int len) {
    const char *h = getenv("HOME");
    snprintf(out, (size_t)len, "%s/.config/minde", h ? h : "/tmp");
}

static void hist_path(char *out, int len) {
    char d[256]; hist_dir(d, sizeof d);
    snprintf(out, (size_t)len, "%s/clipboard.txt", d);
}

static void hist_save(void) {
    char d[256]; hist_dir(d, sizeof d); mkdir(d, 0755);
    char p[512]; hist_path(p, sizeof p);
    FILE *f = fopen(p, "w"); if (!f) return;
    for (int i = 0; i < nclips; i++) {
        for (int j = 0; clips[i][j]; j++) {
            if (clips[i][j] == '\n') fputs("\\n", f);
            else                     fputc(clips[i][j], f);
        }
        fputc('\n', f);
    }
    fclose(f);
}

static void hist_load(void) {
    nclips = 0;
    char p[512]; hist_path(p, sizeof p);
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[MAX_LEN];
    while (fgets(line, sizeof line, f) && nclips < MAX_CLIP) {
        line[strcspn(line, "\n")] = '\0'; if (!line[0]) continue;
        char buf[MAX_LEN]; int bi = 0;
        for (int i = 0; line[i] && bi < MAX_LEN-1; i++) {
            if (line[i] == '\\' && line[i+1] == 'n') { buf[bi++] = '\n'; i++; }
            else buf[bi++] = line[i];
        }
        buf[bi] = '\0';
        strncpy(clips[nclips++], buf, MAX_LEN - 1);
    }
    fclose(f);
}

static void hist_add(const char *text) {
    if (!text || !text[0]) return;
    for (int i = 0; i < nclips; i++) {
        if (!strcmp(clips[i], text)) {
            char tmp[MAX_LEN]; strncpy(tmp, clips[i], MAX_LEN-1);
            memmove(&clips[1], &clips[0], (size_t)i * MAX_LEN);
            strncpy(clips[0], tmp, MAX_LEN-1); return;
        }
    }
    if (nclips < MAX_CLIP) nclips++;
    memmove(&clips[1], &clips[0], (size_t)(nclips-1) * MAX_LEN);
    strncpy(clips[0], text, MAX_LEN-1);
    clips[0][MAX_LEN-1] = '\0';
}

static void hist_delete(int idx) {
    if (idx < 0 || idx >= nclips) return;
    memmove(&clips[idx], &clips[idx+1], (size_t)(nclips-idx-1)*MAX_LEN);
    nclips--;
}

/* ═══ BACKEND xclip ══════════════════════════════════════════ */

/* Leer portapapeles usando xclip (fiable bajo cualquier WM) */
static char *clip_read_xclip(const char *sel) {
    char cmd[128];
    snprintf(cmd, sizeof cmd, "xclip -selection %s -o 2>/dev/null", sel);
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    char *buf = malloc(MAX_LEN);
    if (!buf) { pclose(fp); return NULL; }

    int n = 0;
    int c;
    while (n < MAX_LEN-1 && (c = fgetc(fp)) != EOF)
        buf[n++] = (char)c;
    buf[n] = '\0';
    pclose(fp);

    if (n == 0) { free(buf); return NULL; }
    return buf;
}

/* Copiar al portapapeles usando xclip */
static void clip_write_xclip(const char *text) {
    /* Escribir en CLIPBOARD y PRIMARY */
    const char *sels[] = {"clipboard", "primary", NULL};
    for (int i = 0; sels[i]; i++) {
        char cmd[128];
        snprintf(cmd, sizeof cmd,
            "printf '%%s' \"$XCLIP_TEXT\" | xclip -selection %s 2>/dev/null",
            sels[i]);
        /* Usar variable de entorno para evitar problemas con comillas */
        setenv("XCLIP_TEXT", text, 1);
        if (fork() == 0) {
            setsid();
            execlp("/bin/sh", "sh", "-c", cmd, NULL);
            _exit(127);
        }
        wait(NULL);
    }
    unsetenv("XCLIP_TEXT");
}

/* ═══ DAEMON ════════════════════════════════════════════════ */

static volatile int d_run = 1;
static void on_sig(int s) { (void)s; d_run = 0; }

static void run_daemon(void) {
    signal(SIGTERM, on_sig);
    signal(SIGINT,  on_sig);
    signal(SIGCHLD, SIG_IGN);
    hist_load();
    char last[MAX_LEN] = "";
    fprintf(stderr, "[min-clip] daemon iniciado (backend: xclip)\n");

    /* Verificar que xclip está instalado */
    if (system("command -v xclip >/dev/null 2>&1") != 0) {
        fprintf(stderr, "[min-clip] ADVERTENCIA: xclip no encontrado.\n"
                        "           Instala: sudo xbps-install xclip\n");
    }

    while (d_run) {
        usleep(600000);  /* revisar cada 600ms */

        char *text = clip_read_xclip("clipboard");
        if (!text) text = clip_read_xclip("primary");
        if (!text) continue;

        /* Ignorar si es igual al último visto o si está vacío */
        if (!text[0] || !strcmp(text, last)) { free(text); continue; }

        /* Ignorar si es solo espacios/newlines */
        int has_content = 0;
        for (int i = 0; text[i]; i++) {
            if (text[i] > ' ') { has_content = 1; break; }
        }
        if (!has_content) { free(text); continue; }

        strncpy(last, text, MAX_LEN-1);
        hist_add(text);
        hist_save();
        free(text);
    }
    hist_save();
    fprintf(stderr, "[min-clip] daemon detenido\n");
}

/* ═══ VISOR — mismo estilo que min-launch ══════════════════ */

static Display  *dpy;
static int       scr;
static Window    root, win;
static XftDraw  *xd;
static XftFont  *font;
static Theme     theme;
static XftColor  c_bg, c_fg, c_accent, c_dim;

#define CLIP_ROWS  12
#define CLIP_W    500

static int W, H, row_h, input_h;

/* Filtrado */
static int   match_idx[MAX_CLIP];
static int   nmatches = 0;
static int   selected = 0;
static int   scroll   = 0;
static char  input[256] = "";
static int   input_len  = 0;

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

static void filter(void) {
    nmatches = 0; selected = 0; scroll = 0;
    for (int i = 0; i < nclips; i++) {
        if (!input_len) { match_idx[nmatches++] = i; continue; }
        char flat[MAX_LEN]; int j = 0;
        for (int k = 0; clips[i][k] && j < MAX_LEN-1; k++)
            flat[j++] = clips[i][k] == '\n' ? ' ' : clips[i][k];
        flat[j] = '\0';
        if (strcasestr(flat, input)) match_idx[nmatches++] = i;
    }
}

static void one_line(const char *src, char *dst, int maxw) {
    char tmp[MAX_LEN]; int j = 0;
    for (int i = 0; src[i] && j < MAX_LEN-1; i++)
        tmp[j++] = (src[i] == '\n' || src[i] == '\r') ? ' ' : src[i];
    tmp[j] = '\0';
    strncpy(dst, tmp, MAX_LEN-1); dst[MAX_LEN-1] = '\0';
    while (text_w(dst) > maxw && strlen(dst) > 3) {
        int l = strlen(dst);
        dst[l-4]='.'; dst[l-3]='.'; dst[l-2]='.'; dst[l-1]='\0';
    }
}

static void draw(void) {
    GC gc = DefaultGC(dpy, scr);

    XSetForeground(dpy, gc, (unsigned long)theme.pix_bg);
    XFillRectangle(dpy, win, gc, 0, 0, (unsigned)W, (unsigned)H);

    /* Campo búsqueda — igual que min-launch */
    XSetForeground(dpy, gc, (unsigned long)theme.pix_bact);
    XFillRectangle(dpy, win, gc, 0, 0, (unsigned)W, (unsigned)input_h);

    int baseline = LAUNCHER_PAD + font->ascent;
    draw_text(LAUNCHER_PAD, baseline, ">", &c_accent);
    draw_text(LAUNCHER_PAD + text_w("> ") + 4, baseline, input, &c_fg);

    int cx = LAUNCHER_PAD + text_w("> ") + 4 + text_w(input);
    draw_text(cx, baseline, "_", &c_fg);

    XSetForeground(dpy, gc, 0x3a3f5c);
    XFillRectangle(dpy, win, gc, 0, input_h, (unsigned)W, 1);

    /* Lista */
    int vis = nmatches < CLIP_ROWS ? nmatches : CLIP_ROWS;
    for (int i = 0; i < vis; i++) {
        int mi = scroll + i;
        if (mi >= nmatches) break;
        int idx = match_idx[mi];

        int iy     = input_h + 1 + i * row_h;
        int ty     = iy + font->ascent + (row_h - font->ascent - font->descent)/2;

        if (mi == selected) {
            XSetForeground(dpy, gc, (unsigned long)theme.pix_bact);
            XFillRectangle(dpy, win, gc, 0, iy, (unsigned)W, (unsigned)row_h);
        } else if (i % 2 != 0) {
            unsigned long alt = (theme.pix_bg & 0xfefefe) + 0x060608;
            XSetForeground(dpy, gc, alt);
            XFillRectangle(dpy, win, gc, 0, iy, (unsigned)W, (unsigned)row_h);
        }

        /* Número */
        char num[6]; snprintf(num, sizeof num, "%2d ", idx + 1);
        XftColor *dc = (mi == selected) ? &c_bg : &c_dim;
        draw_text(LAUNCHER_PAD, ty, num, dc);

        /* Texto truncado en una línea */
        char disp[MAX_LEN];
        one_line(clips[idx], disp,
                 W - LAUNCHER_PAD*2 - text_w("00 ") - 4);
        XftColor *tc = (mi == selected) ? &c_bg : &c_fg;
        draw_text(LAUNCHER_PAD + text_w("00 ") + 2, ty, disp, tc);
    }

    if (nmatches == 0) {
        int ty = input_h + 1 + row_h/2 + font->ascent;
        const char *msg = nclips == 0
            ? "Portapapeles vacio"
            : "Sin resultados";
        draw_text(LAUNCHER_PAD, ty, msg, &c_dim);
    }

    /* Scrollbar */
    if (nmatches > CLIP_ROWS) {
        int sb_h = CLIP_ROWS * row_h;
        int th = sb_h * CLIP_ROWS / nmatches; if (th < 4) th = 4;
        int ty = input_h + 1 + (sb_h - th) * scroll / (nmatches - CLIP_ROWS);
        XSetForeground(dpy, gc, (unsigned long)theme.pix_bact);
        XFillRectangle(dpy, win, gc, W-3, ty, 3, (unsigned)th);
    }

    XFlush(dpy);
}

static void do_paste(void) {
    if (selected < 0 || selected >= nmatches) return;
    int idx = match_idx[selected];
    /* Usar xclip para poner en portapapeles */
    clip_write_xclip(clips[idx]);
    XCloseDisplay(dpy); exit(0);
}

static void on_key(XKeyEvent *e) {
    char buf[32] = ""; KeySym sym;
    XLookupString(e, buf, sizeof buf-1, &sym, NULL);
    switch (sym) {
    case XK_Escape:
        XCloseDisplay(dpy); exit(0);
    case XK_Return: case XK_KP_Enter:
        do_paste(); break;
    case XK_Up: case XK_KP_Up:
        if (selected > 0) { selected--;
            if (selected < scroll) scroll = selected; }
        draw(); break;
    case XK_Down: case XK_KP_Down:
        if (selected < nmatches-1) { selected++;
            if (selected >= scroll+CLIP_ROWS) scroll = selected-CLIP_ROWS+1; }
        draw(); break;
    case XK_BackSpace:
        if (input_len > 0) {
            do { input_len--; }
            while (input_len > 0 && (input[input_len] & 0xC0) == 0x80);
            input[input_len] = '\0'; filter(); draw();
        }
        break;
    case XK_Delete: case XK_d:
        if (selected >= 0 && selected < nmatches) {
            hist_delete(match_idx[selected]); hist_save();
            filter();
            if (selected >= nmatches && selected > 0) selected--;
            draw();
        }
        break;
    case XK_c:
        if (input_len == 0) {
            nclips = 0; hist_save();
            filter(); selected = 0; scroll = 0; draw();
        } else {
            /* 'c' como búsqueda */
            if (input_len < 253) {
                input[input_len++] = 'c'; input[input_len] = '\0';
                filter(); draw();
            }
        }
        break;
    default:
        if (buf[0] >= 0x20 && input_len < 253) {
            int bl = strlen(buf);
            if (input_len + bl < 253) {
                memcpy(input + input_len, buf, (size_t)bl);
                input_len += bl; input[input_len] = '\0';
                filter(); draw();
            }
        }
    }
}

static void on_click(int mx, int my) {
    (void)mx;
    if (my < input_h) return;
    for (int i = 0; i < CLIP_ROWS; i++) {
        int iy = input_h + 1 + i * row_h;
        if (my >= iy && my < iy + row_h) {
            int mi = scroll + i;
            if (mi >= nmatches) break;
            if (mi == selected) { do_paste(); return; }
            selected = mi; draw(); return;
        }
    }
}

/* ═══ MAIN ═══════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    signal(SIGCHLD, SIG_IGN);

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fputs("min-clip: no X display\n", stderr); return 1; }
    scr  = DefaultScreen(dpy);
    root = DefaultRootWindow(dpy);

    if (argc > 1 && !strcmp(argv[1], "--daemon")) {
        run_daemon();
        XCloseDisplay(dpy); return 0;
    }

    hist_load();
    theme_init(&theme);

    font = XftFontOpenName(dpy, scr, FONT_NAME);
    if (!font) font = XftFontOpenName(dpy, scr, "fixed:size=9");
    if (!font) { fputs("min-clip: no font\n", stderr); return 1; }

    alloc_color(&c_bg,     theme.bg);
    alloc_color(&c_fg,     theme.fg);
    alloc_color(&c_accent, theme.accent);
    alloc_color(&c_dim,    theme.dim);

    row_h   = font->ascent + font->descent + LAUNCHER_PAD;
    input_h = row_h + 2 * LAUNCHER_PAD;
    W = CLIP_W;
    H = input_h + 1 + CLIP_ROWS * row_h;

    filter();

    int sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr);

    XSetWindowAttributes attr = {0};
    attr.background_pixel  = (unsigned long)theme.pix_bg;
    attr.override_redirect = True;
    attr.event_mask = KeyPressMask | ButtonPressMask | ExposureMask;
    attr.border_pixel = (unsigned long)theme.pix_bact;

    win = XCreateWindow(dpy, root, (sw-W)/2, (sh-H)/3,
        (unsigned)W, (unsigned)H, BORDER_WIDTH,
        DefaultDepth(dpy, scr), InputOutput, DefaultVisual(dpy, scr),
        CWBackPixel|CWOverrideRedirect|CWEventMask|CWBorderPixel, &attr);

    XStoreName(dpy, win, "min-clip");
    xd = XftDrawCreate(dpy, win, DefaultVisual(dpy, scr),
                        DefaultColormap(dpy, scr));
    XMapRaised(dpy, win);
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

    XftFontClose(dpy, font);
    XftDrawDestroy(xd);
    XCloseDisplay(dpy);
    return 0;
}
