/* minde-settings.c — Configuración de minDE
 *
 * Aplicación de ajustes escrita desde cero en C puro + Xlib + Xft.
 * Sin GTK, sin Qt, sin dependencias extra.
 *
 * Permite cambiar:
 *   - Tema de color (presets + personalizado)
 *   - Fuente y tamaño
 *   - Grosor del borde de ventanas
 *   - Alto de la barra
 *
 * Guarda los cambios en ~/.config/minde/settings.conf
 * El formato es simple clave=valor, leído por config.h al arrancar.
 *
 * Compilar (ver Makefile):
 *   gcc -O2 -o minde-settings minde-settings.c \
 *       $(pkg-config --cflags --libs x11 xft fontconfig)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>

/* ── Temas predefinidos ──────────────────────────────────────────────── */

typedef struct {
    const char *name;
    const char *bg;
    const char *fg;
    const char *accent;
    const char *dim;
    const char *bact;
    const char *binact;
    const char *urgent;
} Preset;

static const Preset presets[] = {
    {
        "Breeze Dark",
        "#31363b", "#eff0f1", "#3daee9", "#7f8c8d",
        "#3daee9", "#31363b", "#da4453"
    },
    {
        "Nord",
        "#2e3440", "#eceff4", "#88c0d0", "#4c566a",
        "#88c0d0", "#3b4252", "#bf616a"
    },
    {
        "Gruvbox Dark",
        "#282828", "#ebdbb2", "#d79921", "#928374",
        "#d79921", "#3c3836", "#cc241d"
    },
    {
        "Tokyo Night",
        "#1a1b26", "#c0caf5", "#7aa2f7", "#565f89",
        "#7aa2f7", "#24283b", "#f7768e"
    },
    {
        "Catppuccin Mocha",
        "#1e1e2e", "#cdd6f4", "#cba6f7", "#6c7086",
        "#cba6f7", "#313244", "#f38ba8"
    },
    {
        "Solarized Dark",
        "#002b36", "#839496", "#268bd2", "#586e75",
        "#268bd2", "#073642", "#dc322f"
    },
    {
        "Dracula",
        "#282a36", "#f8f8f2", "#bd93f9", "#6272a4",
        "#bd93f9", "#44475a", "#ff5555"
    },
    {
        "Monochrome",
        "#111111", "#dddddd", "#888888", "#555555",
        "#888888", "#222222", "#ff4444"
    },
};
#define NPRESETS (int)(sizeof presets / sizeof *presets)

/* ── Configuración actual (cargada/guardada) ─────────────────────────── */

typedef struct {
    char bg    [8];
    char fg    [8];
    char accent[8];
    char dim   [8];
    char bact  [8];
    char binact[8];
    char urgent[8];
    char font  [128];
    int  font_size;
    int  border_width;
    int  bar_height;
} Config;

static Config cfg;

/* ── Ruta del archivo de configuración ──────────────────────────────── */

static void config_path(char *out, int len) {
    const char *home = getenv("HOME");
    snprintf(out, (size_t)len, "%s/.config/minde/settings.conf",
             home ? home : "/tmp");
}

/* ── Cargar configuración ────────────────────────────────────────────── */

static void config_defaults(void) {
    strncpy(cfg.bg,     "#1e2030", 7); cfg.bg[7]     = '\0';
    strncpy(cfg.fg,     "#c0caf5", 7); cfg.fg[7]     = '\0';
    strncpy(cfg.accent, "#7aa2f7", 7); cfg.accent[7] = '\0';
    strncpy(cfg.dim,    "#565f89", 7); cfg.dim[7]    = '\0';
    strncpy(cfg.bact,   "#7aa2f7", 7); cfg.bact[7]   = '\0';
    strncpy(cfg.binact, "#24283b", 7); cfg.binact[7] = '\0';
    strncpy(cfg.urgent, "#f7768e", 7); cfg.urgent[7] = '\0';
    strncpy(cfg.font, "monospace", sizeof cfg.font - 1);
    cfg.font_size    = 9;
    cfg.border_width = 2;
    cfg.bar_height   = 24;
}

static void config_load(void) {
    config_defaults();
    char path[512];
    config_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = line, *v = eq + 1;
        if (!strcmp(k, "bg"))           { strncpy(cfg.bg,     v, 7); cfg.bg[7]     = '\0'; }
        else if (!strcmp(k, "fg"))      { strncpy(cfg.fg,     v, 7); cfg.fg[7]     = '\0'; }
        else if (!strcmp(k, "accent"))  { strncpy(cfg.accent, v, 7); cfg.accent[7] = '\0'; }
        else if (!strcmp(k, "dim"))     { strncpy(cfg.dim,    v, 7); cfg.dim[7]    = '\0'; }
        else if (!strcmp(k, "bact"))    { strncpy(cfg.bact,   v, 7); cfg.bact[7]   = '\0'; }
        else if (!strcmp(k, "binact"))  { strncpy(cfg.binact, v, 7); cfg.binact[7] = '\0'; }
        else if (!strcmp(k, "urgent"))  { strncpy(cfg.urgent, v, 7); cfg.urgent[7] = '\0'; }
        else if (!strcmp(k, "font"))    { strncpy(cfg.font, v, sizeof cfg.font - 1); }
        else if (!strcmp(k, "font_size"))    cfg.font_size    = atoi(v);
        else if (!strcmp(k, "border_width")) cfg.border_width = atoi(v);
        else if (!strcmp(k, "bar_height"))   cfg.bar_height   = atoi(v);
    }
    fclose(f);
}

/* ── Guardar configuración ───────────────────────────────────────────── */

/* Envía SIGUSR1 a un proceso cuyo PID está en un archivo */
static void notify_pid_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    int pid = 0;
    fscanf(f, "%d", &pid);
    fclose(f);
    if (pid > 1) kill((pid_t)pid, SIGUSR1);
}

static void notify_reload(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    char path[512];
    snprintf(path, sizeof path,
             "%s/.config/minde/min-wm.pid", home);
    notify_pid_file(path);
    snprintf(path, sizeof path,
             "%s/.config/minde/min-bar.pid", home);
    notify_pid_file(path);
}

static int config_save(void) {
    char path[512];
    config_path(path, sizeof path);

    /* Crear directorio si no existe */
    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0755);
    }

    FILE *f = fopen(path, "w");
    if (!f) return 0;

    fprintf(f,
        "# minde-settings — generado automáticamente\n"
        "bg=%s\n"
        "fg=%s\n"
        "accent=%s\n"
        "dim=%s\n"
        "bact=%s\n"
        "binact=%s\n"
        "urgent=%s\n"
        "font=%s\n"
        "font_size=%d\n"
        "border_width=%d\n"
        "bar_height=%d\n",
        cfg.bg, cfg.fg, cfg.accent, cfg.dim,
        cfg.bact, cfg.binact, cfg.urgent,
        cfg.font, cfg.font_size,
        cfg.border_width, cfg.bar_height);

    fclose(f);
    return 1;
}

/* ── X11 / Xft globals ──────────────────────────────────────────────── */

static Display  *dpy;
static int       scr;
static Window    root, win;
static GC        gc;
static XftDraw  *xd;
static XftFont  *font_ui;
static XftFont  *font_sm;

/* Colores UI — se inicializan desde cfg (tema del usuario) */
static char UI_BG    [8] = "#1a1b26";
static char UI_PANEL [8] = "#24283b";
static char UI_FG    [8] = "#c0caf5";
static char UI_DIM   [8] = "#565f89";
static char UI_ACCENT[8] = "#7aa2f7";
static char UI_BTN   [8] = "#2d3f76";
static char UI_BTN_HOV[8]= "#3d4f86";
#define UI_SUCCESS "#9ece6a"
static char UI_BORDER[8] = "#414868";

static XftColor c_bg, c_panel, c_fg, c_dim, c_accent;
static XftColor c_btn, c_btn_hov, c_success, c_border;

static int W = 680, H = 580;

/* ── Secciones de la UI ─────────────────────────────────────────────── */

#define TAB_COLORS  0
#define TAB_FONT    1
#define TAB_LAYOUT  2
static int cur_tab = TAB_COLORS;

/* Preset seleccionado (-1 = personalizado) */
static int cur_preset = -1;

/* Campo de texto activo */
#define FIELD_NONE    -1
#define FIELD_BG       0
#define FIELD_FG       1
#define FIELD_ACCENT   2
#define FIELD_DIM      3
#define FIELD_BACT     4
#define FIELD_BINACT   5
#define FIELD_URGENT   6
#define FIELD_FONT     7
static int active_field = FIELD_NONE;
static char field_buf[128] = "";
static int  field_len = 0;

/* Mensaje de estado */
static char status_msg[128] = "";
static int  status_ok = 0;

/* ── Helpers Xft ─────────────────────────────────────────────────────── */

static void alloc_color(XftColor *c, const char *hex) {
    if (!XftColorAllocName(dpy, DefaultVisual(dpy, scr),
            DefaultColormap(dpy, scr), hex, c))
        XftColorAllocName(dpy, DefaultVisual(dpy, scr),
            DefaultColormap(dpy, scr), "#ffffff", c);
}

static int text_w(XftFont *fnt, const char *s) {
    XGlyphInfo e;
    XftTextExtentsUtf8(dpy, fnt, (const FcChar8 *)s, strlen(s), &e);
    return e.xOff;
}

static void draw_text(XftFont *fnt, int x, int y,
                      const char *s, XftColor *col) {
    XftDrawStringUtf8(xd, col, fnt, x, y,
        (const FcChar8 *)s, strlen(s));
}

/* Rect relleno */
static void fill_rect(int x, int y, int w, int h, const char *hex) {
    XColor xc; Colormap cm = DefaultColormap(dpy, scr);
    XParseColor(dpy, cm, hex, &xc);
    XAllocColor(dpy, cm, &xc);
    XSetForeground(dpy, gc, xc.pixel);
    XFillRectangle(dpy, win, gc, x, y, (unsigned)w, (unsigned)h);
}

/* Rect con borde */
static void draw_rect(int x, int y, int w, int h, const char *hex) {
    XColor xc; Colormap cm = DefaultColormap(dpy, scr);
    XParseColor(dpy, cm, hex, &xc);
    XAllocColor(dpy, cm, &xc);
    XSetForeground(dpy, gc, xc.pixel);
    XDrawRectangle(dpy, win, gc, x, y, (unsigned)(w-1), (unsigned)(h-1));
}

/* ── Layout constantes ───────────────────────────────────────────────── */

#define TAB_H    36
#define TAB_Y     8
#define CONTENT_Y (TAB_Y + TAB_H + 8)
#define PAD      20
#define ROW_H    38
#define BTN_H    32
#define BTN_W   120
#define SWATCH_S  18
#define FIELD_H   28
#define FIELD_W  160

/* ── Dibujar swatch de color ─────────────────────────────────────────── */

static void draw_swatch(int x, int y, const char *hex) {
    fill_rect(x, y, SWATCH_S, SWATCH_S, hex);
    draw_rect(x, y, SWATCH_S, SWATCH_S, UI_BORDER);
}

/* ── Dibujar campo de texto ──────────────────────────────────────────── */

typedef struct { int x, y, w, h; int field_id; } TextField;

static TextField tfields[8];
static int       ntfields = 0;

static void draw_textfield(int idx, int x, int y, int w,
                            const char *label, const char *value,
                            int field_id) {
    tfields[idx].x        = x;
    tfields[idx].y        = y;
    tfields[idx].w        = w;
    tfields[idx].h        = FIELD_H;
    tfields[idx].field_id = field_id;

    /* Label */
    draw_text(font_sm, x, y + 12, label, &c_dim);

    int fy = y + 16;
    int is_active = (active_field == field_id);

    fill_rect(x, fy, w, FIELD_H,
              is_active ? UI_BTN : UI_PANEL);
    draw_rect(x, fy, w, FIELD_H,
              is_active ? UI_ACCENT : UI_BORDER);

    const char *val = (is_active && field_buf[0]) ? field_buf : value;
    if (val && val[0]) {
        /* Mostrar swatch si es color */
        if (val[0] == '#' && strlen(val) == 7) {
            draw_swatch(x + 6, fy + 5, val);
            draw_text(font_sm, x + 30, fy + 18, val, &c_fg);
        } else {
            draw_text(font_sm, x + 8, fy + 18, val, &c_fg);
        }
    }
    if (is_active) {
        /* Cursor */
        int cx = (val && val[0] && val[0] == '#')
            ? x + 30 + text_w(font_sm, val) + 2
            : x + 8  + text_w(font_sm, val ? val : "") + 2;
        XSetForeground(dpy, gc, 0xc0caf5);
        XDrawLine(dpy, win, gc, cx, fy + 4, cx, fy + FIELD_H - 4);
    }
}

/* ── Dibujar botón ───────────────────────────────────────────────────── */

typedef struct { int x, y, w, h; int id; } Button;
#define BTN_APPLY   0
#define BTN_CANCEL  1
#define BTN_PRESET0 10   /* presets: 10..10+NPRESETS-1 */

static Button buttons[32];
static int    nbuttons = 0;

static void draw_button(int idx, int x, int y, int w, int h,
                         const char *label, const char *bg,
                         const char *fg_col, int id) {
    buttons[idx].x = x; buttons[idx].y = y;
    buttons[idx].w = w; buttons[idx].h = h;
    buttons[idx].id = id;

    fill_rect(x, y, w, h, bg);
    draw_rect(x, y, w, h, UI_BORDER);

    int tw = text_w(font_sm, label);
    int tx = x + (w - tw) / 2;
    int ty = y + h / 2 + font_sm->ascent / 2 - 1;

    XftColor col;
    alloc_color(&col, fg_col);
    draw_text(font_sm, tx, ty, label, &col);
    XftColorFree(dpy, DefaultVisual(dpy, scr),
                 DefaultColormap(dpy, scr), &col);
}

/* ── Spinbox entero ─────────────────────────────────────────────────── */

typedef struct { int x, y; int *val; int min, max; int id; } Spinbox;
static Spinbox spins[4];
static int     nspins = 0;

#define SPIN_FONTSIZE    100
#define SPIN_BORDER      101
#define SPIN_BARHEIGHT   102

static void draw_spinbox(int idx, int x, int y,
                          const char *label, int *val,
                          int mn, int mx, int id) {
    spins[idx].x   = x;   spins[idx].y   = y;
    spins[idx].val = val; spins[idx].min  = mn;
    spins[idx].max = mx;  spins[idx].id   = id;

    draw_text(font_sm, x, y + 12, label, &c_dim);

    int fy = y + 16;
    fill_rect(x, fy, 100, FIELD_H, UI_PANEL);
    draw_rect(x, fy, 100, FIELD_H, UI_BORDER);

    char num[8]; snprintf(num, sizeof num, "%d", *val);
    draw_text(font_sm, x + 8, fy + 18, num, &c_fg);

    /* Botones - y + */
    fill_rect(x + 102, fy,     22, FIELD_H, UI_BTN);
    fill_rect(x + 126, fy,     22, FIELD_H, UI_BTN);
    draw_rect(x + 102, fy,     22, FIELD_H, UI_BORDER);
    draw_rect(x + 126, fy,     22, FIELD_H, UI_BORDER);

    XftColor col; alloc_color(&col, UI_FG);
    draw_text(font_sm, x + 108, fy + 18, "-", &col);
    draw_text(font_sm, x + 132, fy + 18, "+", &col);
    XftColorFree(dpy, DefaultVisual(dpy, scr),
                 DefaultColormap(dpy, scr), &col);
}

/* ── Pestañas ────────────────────────────────────────────────────────── */

static void draw_tabs(void) {
    const char *tabs[] = { "Colores", "Fuente", "Distribución" };
    int tx = PAD;
    for (int i = 0; i < 3; i++) {
        int tw = text_w(font_ui, tabs[i]) + 28;
        int is = (i == cur_tab);
        fill_rect(tx, TAB_Y, tw, TAB_H,
                  is ? UI_PANEL : UI_BG);
        if (is) draw_rect(tx, TAB_Y, tw, TAB_H + 1, UI_BORDER);

        XftColor col;
        alloc_color(&col, is ? UI_FG : UI_DIM);
        draw_text(font_ui, tx + 14,
                  TAB_Y + TAB_H / 2 + font_ui->ascent / 2 - 1,
                  tabs[i], &col);
        XftColorFree(dpy, DefaultVisual(dpy, scr),
                     DefaultColormap(dpy, scr), &col);
        tx += tw + 4;
    }
}

/* ── Vista: Colores ──────────────────────────────────────────────────── */

static void draw_tab_colors(void) {
    ntfields  = 0;
    nbuttons  = 2;   /* Apply + Cancel reservados */

    int y = CONTENT_Y + 8;

    /* Presets */
    draw_text(font_sm, PAD, y + 12, "Temas predefinidos", &c_dim);
    y += 20;

    int px = PAD;
    for (int i = 0; i < NPRESETS; i++) {
        int pw = text_w(font_sm, presets[i].name) + 20;
        int is = (cur_preset == i);
        fill_rect(px, y, pw, 26,
                  is ? UI_ACCENT : UI_BTN);
        draw_rect(px, y, pw, 26, UI_BORDER);

        XftColor col;
        alloc_color(&col, is ? UI_BG : UI_FG);
        draw_text(font_sm, px + 10, y + 17, presets[i].name, &col);
        XftColorFree(dpy, DefaultVisual(dpy, scr),
                     DefaultColormap(dpy, scr), &col);

        buttons[2 + i].x  = px; buttons[2 + i].y  = y;
        buttons[2 + i].w  = pw; buttons[2 + i].h  = 26;
        buttons[2 + i].id = BTN_PRESET0 + i;

        px += pw + 6;
        if (px > W - 160) { px = PAD; y += 32; }
    }
    nbuttons = 2 + NPRESETS;

    y += 38;

    /* Separador */
    fill_rect(PAD, y, W - PAD * 2, 1, UI_BORDER);
    y += 12;
    draw_text(font_sm, PAD, y + 12, "Personalizado", &c_dim);
    y += 20;

    /* Campos de color en dos columnas */
    struct { const char *label; char *val; int id; } cols[] = {
        { "Fondo (bg)",          cfg.bg,     FIELD_BG     },
        { "Texto (fg)",          cfg.fg,     FIELD_FG     },
        { "Acento",              cfg.accent, FIELD_ACCENT },
        { "Texto dim",           cfg.dim,    FIELD_DIM    },
        { "Borde activo",        cfg.bact,   FIELD_BACT   },
        { "Borde inactivo",      cfg.binact, FIELD_BINACT },
        { "Urgente",             cfg.urgent, FIELD_URGENT },
    };
    int ncols = (int)(sizeof cols / sizeof *cols);

    int col1x = PAD;
    int col2x = PAD + FIELD_W + 30;
    int fy = y;
    for (int i = 0; i < ncols; i++) {
        int cx = (i % 2 == 0) ? col1x : col2x;
        if (i > 0 && i % 2 == 0) fy += ROW_H + 6;
        draw_textfield(ntfields++, cx, fy, FIELD_W,
                       cols[i].label, cols[i].val, cols[i].id);
    }
    y = fy + ROW_H + 16;

    /* Preview de colores actuales */
    draw_text(font_sm, PAD, y + 12, "Previsualización", &c_dim);
    y += 18;
    struct { const char *n; char *c; } swatches[] = {
        {"bg", cfg.bg}, {"fg", cfg.fg}, {"acento", cfg.accent},
        {"dim", cfg.dim}, {"bact", cfg.bact},
        {"binact", cfg.binact}, {"urgente", cfg.urgent},
    };
    int sx = PAD;
    for (int i = 0; i < 7; i++) {
        draw_swatch(sx, y, swatches[i].c);
        draw_text(font_sm, sx, y + SWATCH_S + 12,
                  swatches[i].n, &c_dim);
        sx += 60;
    }
}

/* ── Vista: Fuente ───────────────────────────────────────────────────── */

static void draw_tab_font(void) {
    ntfields = 0;
    nbuttons = 2;

    int y = CONTENT_Y + 20;

    draw_text(font_sm, PAD, y + 12, "Nombre de fuente (fontconfig)", &c_dim);
    y += 20;
    draw_textfield(ntfields++, PAD, y, 340,
                   "", cfg.font, FIELD_FONT);
    y += 56;

    /* Ejemplos de fuentes comunes */
    draw_text(font_sm, PAD, y, "Ejemplos:", &c_dim);
    y += 16;
    const char *examples[] = {
        "monospace",
        "JetBrains Mono",
        "Fira Code",
        "DejaVu Sans Mono",
        "Source Code Pro",
        "Hack",
        NULL
    };
    int ex = PAD;
    for (int i = 0; examples[i]; i++) {
        int ew = text_w(font_sm, examples[i]) + 16;
        fill_rect(ex, y, ew, 24, UI_BTN);
        draw_rect(ex, y, ew, 24, UI_BORDER);
        draw_text(font_sm, ex + 8, y + 16, examples[i], &c_fg);

        /* Registrar como botón clickeable para autocompletar */
        buttons[nbuttons].x  = ex; buttons[nbuttons].y  = y;
        buttons[nbuttons].w  = ew; buttons[nbuttons].h  = 24;
        buttons[nbuttons].id = 200 + i;
        nbuttons++;

        ex += ew + 8;
        if (ex > W - 100) { ex = PAD; y += 30; }
    }
    y += 36;

    /* Tamaño */
    nspins = 0;
    draw_spinbox(nspins++, PAD, y, "Tamaño de fuente (pt)",
                 &cfg.font_size, 6, 24, SPIN_FONTSIZE);
    y += 56;

    /* Preview */
    fill_rect(PAD, y, W - PAD * 2, 50, UI_PANEL);
    draw_rect(PAD, y, W - PAD * 2, 50, UI_BORDER);

    char preview[128];
    snprintf(preview, sizeof preview,
             "Muestra: %.*s %d  —  0123 AaBb",
             80, cfg.font, cfg.font_size);

    /* Cargar fuente de preview */
    char fpattern[200];
    snprintf(fpattern, sizeof fpattern,
             "%s:size=%d:antialias=true", cfg.font, cfg.font_size);
    XftFont *pf = XftFontOpenName(dpy, scr, fpattern);
    if (pf) {
        XftDrawStringUtf8(xd, &c_fg, pf,
            PAD + 10, y + 32,
            (const FcChar8 *)preview, strlen(preview));
        XftFontClose(dpy, pf);
    } else {
        draw_text(font_sm, PAD + 10, y + 28,
                  "(fuente no encontrada)", &c_dim);
    }
}

/* ── Vista: Distribución ─────────────────────────────────────────────── */

static void draw_tab_layout(void) {
    ntfields = 0;
    nspins   = 0;
    nbuttons = 2;

    int y = CONTENT_Y + 20;

    draw_spinbox(nspins++, PAD, y,
                 "Alto de la barra (px)",
                 &cfg.bar_height, 16, 48, SPIN_BARHEIGHT);
    y += 64;

    draw_spinbox(nspins++, PAD, y,
                 "Grosor del borde de ventanas (px)",
                 &cfg.border_width, 0, 8, SPIN_BORDER);
    y += 64;

    /* Preview mini del escritorio */
    int pw = W - PAD * 2, ph = 160;
    fill_rect(PAD, y, pw, ph, cfg.bg);
    draw_rect(PAD, y, pw, ph, UI_BORDER);

    /* Barra simulada */
    fill_rect(PAD, y, pw, cfg.bar_height, cfg.bg[0] ? cfg.bg : UI_PANEL);
    draw_rect(PAD, y, pw, cfg.bar_height, cfg.bact);

    /* Texto de la barra simulada */
    draw_text(font_sm, PAD + 10,
              y + cfg.bar_height / 2 + font_sm->ascent / 2,
              "1  2  ●3  4  5           Ventana de ejemplo           16:04",
              &c_fg);

    /* Ventana simulada con borde */
    int wx = PAD + 8;
    int wy = y + cfg.bar_height + 8;
    int ww = pw - 16;
    int wh = ph - cfg.bar_height - 16;
    if (wh > 0 && ww > 0) {
        fill_rect(wx, wy, ww, wh, "#252535");
        /* Borde activo simulado */
        for (int b = 0; b < cfg.border_width && b < 4; b++)
            draw_rect(wx - b, wy - b, ww + b * 2, wh + b * 2, cfg.bact);
    }

    draw_text(font_sm, PAD, y + ph + 12,
              "Vista previa del escritorio", &c_dim);
}

/* ── Dibujo principal ────────────────────────────────────────────────── */

static void draw(void) {
    /* Fondo */
    fill_rect(0, 0, W, H, UI_BG);

    /* Panel central */
    fill_rect(0, CONTENT_Y - 2, W, H - CONTENT_Y, UI_PANEL);
    fill_rect(0, CONTENT_Y - 2, W, 1, UI_BORDER);

    draw_tabs();

    switch (cur_tab) {
    case TAB_COLORS: draw_tab_colors(); break;
    case TAB_FONT:   draw_tab_font();   break;
    case TAB_LAYOUT: draw_tab_layout(); break;
    }

    /* Barra inferior: Apply + Cancel + status */
    fill_rect(0, H - 50, W, 50, UI_BG);
    fill_rect(0, H - 50, W, 1, UI_BORDER);

    draw_button(0, W - PAD - BTN_W - BTN_W - 12, H - 38,
                BTN_W, BTN_H, "Aplicar y guardar",
                UI_ACCENT, UI_BG, BTN_APPLY);
    draw_button(1, W - PAD - BTN_W, H - 38,
                BTN_W, BTN_H, "Cancelar",
                UI_BTN, UI_FG, BTN_CANCEL);

    if (status_msg[0]) {
        XftColor col;
        alloc_color(&col, status_ok ? UI_SUCCESS : "#f7768e");
        draw_text(font_sm, PAD, H - 22, status_msg, &col);
        XftColorFree(dpy, DefaultVisual(dpy, scr),
                     DefaultColormap(dpy, scr), &col);
    }

    XFlush(dpy);
}

/* ── Aplicar preset ──────────────────────────────────────────────────── */

static void apply_preset(int idx) {
    if (idx < 0 || idx >= NPRESETS) return;
    cur_preset = idx;
    const Preset *p = &presets[idx];
    strncpy(cfg.bg,     p->bg,     7); cfg.bg[7]     = '\0';
    strncpy(cfg.fg,     p->fg,     7); cfg.fg[7]     = '\0';
    strncpy(cfg.accent, p->accent, 7); cfg.accent[7] = '\0';
    strncpy(cfg.dim,    p->dim,    7); cfg.dim[7]    = '\0';
    strncpy(cfg.bact,   p->bact,   7); cfg.bact[7]   = '\0';
    strncpy(cfg.binact, p->binact, 7); cfg.binact[7] = '\0';
    strncpy(cfg.urgent, p->urgent, 7); cfg.urgent[7] = '\0';
    active_field = FIELD_NONE;
    field_buf[0] = '\0'; field_len = 0;
}

/* ── Confirmar campo activo ──────────────────────────────────────────── */

static void commit_field(void) {
    if (active_field == FIELD_NONE || !field_buf[0]) return;

    /* Validar color */
    if (active_field <= FIELD_URGENT) {
        if (field_buf[0] != '#' || strlen(field_buf) != 7) {
            snprintf(status_msg, sizeof status_msg,
                     "Color inválido: usa formato #rrggbb");
            status_ok = 0;
            return;
        }
        for (int i = 1; i < 7; i++) {
            char c = field_buf[i];
            if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) {
                snprintf(status_msg, sizeof status_msg,
                         "Color inválido: solo hex 0-9 a-f");
                status_ok = 0;
                return;
            }
        }
    }

    char *dst = NULL;
    switch (active_field) {
    case FIELD_BG:     dst = cfg.bg;     break;
    case FIELD_FG:     dst = cfg.fg;     break;
    case FIELD_ACCENT: dst = cfg.accent; break;
    case FIELD_DIM:    dst = cfg.dim;    break;
    case FIELD_BACT:   dst = cfg.bact;   break;
    case FIELD_BINACT: dst = cfg.binact; break;
    case FIELD_URGENT: dst = cfg.urgent; break;
    case FIELD_FONT:
        snprintf(cfg.font, sizeof cfg.font, "%s", field_buf);
        break;
    }
    if (dst) {
        strncpy(dst, field_buf, 7); dst[7] = '\0';
    }
    cur_preset   = -1;
    active_field = FIELD_NONE;
    field_buf[0] = '\0'; field_len = 0;
    status_msg[0] = '\0';
}

/* ── Click en botón ──────────────────────────────────────────────────── */

static void on_button(int id) {
    if (id == BTN_APPLY) {
        commit_field();
        if (config_save()) {
            notify_reload();
            snprintf(status_msg, sizeof status_msg,
                "Guardado. Cambios aplicados en vivo.");
            status_ok = 1;
        } else {
            snprintf(status_msg, sizeof status_msg,
                "Error al guardar ~/.config/minde/settings.conf");
            status_ok = 0;
        }
    } else if (id == BTN_CANCEL) {
        config_load();
        cur_preset   = -1;
        active_field = FIELD_NONE;
        field_buf[0] = '\0'; field_len = 0;
        snprintf(status_msg, sizeof status_msg, "Cambios descartados.");
        status_ok = 1;
    } else if (id >= BTN_PRESET0 && id < BTN_PRESET0 + NPRESETS) {
        apply_preset(id - BTN_PRESET0);
        snprintf(status_msg, sizeof status_msg,
                 "Tema '%s' seleccionado. Pulsa Aplicar para guardar.",
                 presets[id - BTN_PRESET0].name);
        status_ok = 1;
    } else if (id >= 200) {
        /* Click en ejemplo de fuente */
        const char *examples[] = {
            "monospace", "JetBrains Mono", "Fira Code",
            "DejaVu Sans Mono", "Source Code Pro", "Hack", NULL
        };
        int idx = id - 200;
        if (examples[idx]) {
            strncpy(cfg.font, examples[idx], sizeof cfg.font - 1);
            active_field = FIELD_NONE;
        }
    }
}

/* ── Eventos de ratón ────────────────────────────────────────────────── */

static void on_click(int mx, int my) {
    /* Pestañas */
    const char *tabs[] = { "Colores", "Fuente", "Distribución" };
    int tx = PAD;
    for (int i = 0; i < 3; i++) {
        int tw = text_w(font_ui, tabs[i]) + 28;
        if (mx >= tx && mx < tx + tw &&
            my >= TAB_Y && my < TAB_Y + TAB_H) {
            cur_tab      = i;
            active_field = FIELD_NONE;
            field_buf[0] = '\0'; field_len = 0;
            return;
        }
        tx += tw + 4;
    }

    /* Spinboxes */
    for (int i = 0; i < nspins; i++) {
        Spinbox *s = &spins[i];
        int fy = s->y + 16;
        /* Botón menos */
        if (mx >= s->x + 102 && mx < s->x + 124 &&
            my >= fy && my < fy + FIELD_H) {
            if (*s->val > s->min) (*s->val)--;
            return;
        }
        /* Botón más */
        if (mx >= s->x + 126 && mx < s->x + 148 &&
            my >= fy && my < fy + FIELD_H) {
            if (*s->val < s->max) (*s->val)++;
            return;
        }
    }

    /* Campos de texto */
    for (int i = 0; i < ntfields; i++) {
        TextField *f = &tfields[i];
        if (mx >= f->x && mx < f->x + f->w &&
            my >= f->y + 16 && my < f->y + 16 + f->h) {
            /* Confirmar campo anterior */
            commit_field();
            active_field = f->field_id;
            /* Precargar valor actual en el buffer */
            const char *cur = NULL;
            switch (active_field) {
            case FIELD_BG:     cur = cfg.bg;     break;
            case FIELD_FG:     cur = cfg.fg;     break;
            case FIELD_ACCENT: cur = cfg.accent; break;
            case FIELD_DIM:    cur = cfg.dim;    break;
            case FIELD_BACT:   cur = cfg.bact;   break;
            case FIELD_BINACT: cur = cfg.binact; break;
            case FIELD_URGENT: cur = cfg.urgent; break;
            case FIELD_FONT:   cur = cfg.font;   break;
            }
            if (cur) {
                snprintf(field_buf, sizeof field_buf, "%s", cur);
                field_len = strlen(field_buf);
            }
            return;
        }
    }

    /* Botones */
    for (int i = 0; i < nbuttons; i++) {
        Button *b = &buttons[i];
        if (mx >= b->x && mx < b->x + b->w &&
            my >= b->y && my < b->y + b->h) {
            on_button(b->id);
            return;
        }
    }

    /* Click fuera: confirmar campo activo */
    if (active_field != FIELD_NONE) {
        commit_field();
    }
}

/* ── Eventos de teclado ──────────────────────────────────────────────── */

static void on_key(XKeyEvent *e) {
    char buf[32] = "";
    KeySym sym;
    XLookupString(e, buf, sizeof buf - 1, &sym, NULL);

    if (sym == XK_Escape) {
        if (active_field != FIELD_NONE) {
            active_field = FIELD_NONE;
            field_buf[0] = '\0'; field_len = 0;
        } else {
            XCloseDisplay(dpy);
            exit(0);
        }
        return;
    }
    if (sym == XK_Return || sym == XK_Tab) {
        commit_field();
        return;
    }
    if (sym == XK_BackSpace) {
        if (active_field != FIELD_NONE && field_len > 0) {
            do { field_len--; }
            while (field_len > 0 && (field_buf[field_len] & 0xC0) == 0x80);
            field_buf[field_len] = '\0';
        }
        return;
    }

    /* Caracter imprimible */
    if (active_field != FIELD_NONE && buf[0] >= 0x20) {
        int blen = strlen(buf);
        int maxl = (active_field <= FIELD_URGENT) ? 7 : 127;
        if (field_len + blen < maxl) {
            memcpy(field_buf + field_len, buf, (size_t)blen);
            field_len += blen;
            field_buf[field_len] = '\0';
        }
    }
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(void) {
    config_load();

    /* Sincronizar colores UI con el tema cargado */
    memcpy(UI_BG,     cfg.bg,     8);
    memcpy(UI_PANEL,  cfg.binact, 8);   /* panel = borde inactivo (oscuro) */
    memcpy(UI_FG,     cfg.fg,     8);
    memcpy(UI_DIM,    cfg.dim,    8);
    memcpy(UI_ACCENT, cfg.accent, 8);
    /* BTN: mezcla entre bg y accent — usar borde inactivo */
    memcpy(UI_BTN,    cfg.binact, 8);
    /* BTN_HOV: borde activo */
    memcpy(UI_BTN_HOV,cfg.bact,   8);
    /* BORDER: dim */
    memcpy(UI_BORDER, cfg.dim,    8);

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fputs("minde-settings: no se pudo abrir X display\n", stderr);
        return 1;
    }
    scr  = DefaultScreen(dpy);
    root = DefaultRootWindow(dpy);

    /* Fuentes */
    font_ui = XftFontOpenName(dpy, scr,
                 "monospace:size=10:weight=medium:antialias=true");
    font_sm = XftFontOpenName(dpy, scr,
                 "monospace:size=9:antialias=true");
    if (!font_ui) font_ui = XftFontOpenName(dpy, scr, "fixed:size=10");
    if (!font_sm) font_sm = XftFontOpenName(dpy, scr, "fixed:size=9");
    if (!font_ui || !font_sm) {
        fputs("minde-settings: no se pudo cargar fuente\n", stderr);
        return 1;
    }

    /* Crear ventana */
    XSetWindowAttributes attr = {0};
    attr.background_pixel = BlackPixel(dpy, scr);
    attr.event_mask = ExposureMask | KeyPressMask |
                      ButtonPressMask | StructureNotifyMask;

    win = XCreateWindow(dpy, root,
        (DisplayWidth(dpy, scr)  - W) / 2,
        (DisplayHeight(dpy, scr) - H) / 2,
        (unsigned)W, (unsigned)H, 0,
        DefaultDepth(dpy, scr), InputOutput,
        DefaultVisual(dpy, scr),
        CWBackPixel | CWEventMask, &attr);

    XStoreName(dpy, win, "min-theme");

    /* WM_CLASS para que el launcher lo muestre bien */
    XClassHint *ch = XAllocClassHint();
    if (ch) {
        ch->res_name  = (char *)"min-theme";
        ch->res_class = (char *)"MinTheme";
        XSetClassHint(dpy, win, ch);
        XFree(ch);
    }

    gc = XCreateGC(dpy, win, 0, NULL);
    xd = XftDrawCreate(dpy, win, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr));

    /* Colores UI */
    alloc_color(&c_bg,      UI_BG);
    alloc_color(&c_panel,   UI_PANEL);
    alloc_color(&c_fg,      UI_FG);
    alloc_color(&c_dim,     UI_DIM);
    alloc_color(&c_accent,  UI_ACCENT);
    alloc_color(&c_btn,     UI_BTN);
    alloc_color(&c_btn_hov, UI_BTN_HOV);
    alloc_color(&c_success, UI_SUCCESS);
    alloc_color(&c_border,  UI_BORDER);

    XMapWindow(dpy, win);
    XFlush(dpy);

    /* Bucle de eventos */
    XEvent ev;
    while (XNextEvent(dpy, &ev) == 0) {
        switch (ev.type) {
        case Expose:
            if (ev.xexpose.count == 0) draw();
            break;
        case KeyPress:
            on_key(&ev.xkey);
            draw();
            break;
        case ButtonPress:
            on_click(ev.xbutton.x, ev.xbutton.y);
            draw();
            break;
        case ConfigureNotify:
            W = ev.xconfigure.width;
            H = ev.xconfigure.height;
            draw();
            break;
        }
    }

    XftFontClose(dpy, font_ui);
    XftFontClose(dpy, font_sm);
    XftDrawDestroy(xd);
    XFreeGC(dpy, gc);
    XCloseDisplay(dpy);
    return 0;
}
