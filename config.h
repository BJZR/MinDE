/* config.h — Configuración central de minDE
 *
 * Todo el escritorio (WM + barra + launcher) comparte este header.
 * Cambia aquí y recompila: los tres binarios se sincronizan solos.
 *
 * Integración con GTK: theme_load() sobreescribe los colores
 * en tiempo de ejecución leyendo ~/.config/gtk-3.0/settings.ini
 * y el gtk.css del tema activo.
 */

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <xcb/xcb.h>

/* ── Dimensiones ─────────────────────────────────────────── */

#define NUM_WS           5     /* workspaces totales                    */
#define BORDER_WIDTH     2     /* píxeles de borde de ventana           */
#define BAR_HEIGHT      24     /* altura de la barra en píxeles         */
#define LAUNCHER_W     480     /* ancho del launcher                    */
#define LAUNCHER_ROWS   12     /* filas visibles en el launcher         */
#define LAUNCHER_PAD    10     /* padding interno del launcher          */
#define LAUNCHER_IH     26     /* altura de cada ítem del launcher      */

/* ── Fuente (Xft fontconfig pattern) ────────────────────── */

#define FONT_NAME  "monospace:size=9:antialias=true"

/* ── Colores por defecto (se sobreescriben desde GTK) ────── */

/* Strings para Xft/Xlib */
#define COL_BG          "#1e2030"   /* fondo barra / launcher            */
#define COL_FG          "#c0caf5"   /* texto normal                      */
#define COL_ACCENT      "#5e81ac"   /* ws activo, ítem seleccionado      */
#define COL_DIM         "#565f89"   /* ws inactivo, texto secundario     */
#define COL_URGENT      "#f7768e"   /* ventana con urgencia              */
#define COL_SEL_BG      "#2d3f76"   /* fondo ítem seleccionado launcher  */
#define COL_BORDER_ACT  "#5e81ac"   /* borde ventana enfocada            */
#define COL_BORDER_INACT "#2e3440"  /* borde ventana desenfocada         */

/* uint32_t para XCB (0xRRGGBB) */
#define PIX_BORDER_ACT   0x5e81ac
#define PIX_BORDER_INACT 0x2e3440
#define PIX_URGENT       0xf7768e
#define PIX_BG           0x1e2030

/* ── Comandos ─────────────────────────────────────────────── */

#define TERMINAL  "xterm"           /* reemplaza con alacritty/kitty     */
#define LAUNCHER  "min-launch"          /* el launcher que compilamos nosotros*/

/* ── Modificador y teclas ─────────────────────────────────── */

/* Super (tecla Windows) = XCB_MOD_MASK_4 */
#define MOD XCB_MOD_MASK_4

/* ── Tema: estructura compartida en tiempo de ejecución ───── */

typedef struct {
    /* strings para Xft */
    char bg    [8];
    char fg    [8];
    char accent[8];
    char dim   [8];
    char urgent[8];
    char selbg [8];
    char bact  [8];
    char binact[8];
    /* pixels para XCB */
    uint32_t pix_bact;
    uint32_t pix_binact;
    uint32_t pix_urgent;
    uint32_t pix_bg;
} Theme;

/* ── Inicializa tema con valores por defecto ─────────────── */

static inline void theme_defaults(Theme *t) {
    memcpy(t->bg,     COL_BG,           8);
    memcpy(t->fg,     COL_FG,           8);
    memcpy(t->accent, COL_ACCENT,       8);
    memcpy(t->dim,    COL_DIM,          8);
    memcpy(t->urgent, COL_URGENT,       8);
    memcpy(t->selbg,  COL_SEL_BG,       8);
    memcpy(t->bact,   COL_BORDER_ACT,   8);
    memcpy(t->binact, COL_BORDER_INACT, 8);
    t->pix_bact   = PIX_BORDER_ACT;
    t->pix_binact = PIX_BORDER_INACT;
    t->pix_urgent = PIX_URGENT;
    t->pix_bg     = PIX_BG;
}

/* ── Convierte "#rrggbb" → uint32_t 0xRRGGBB ─────────────── */

static inline uint32_t hex_to_pix(const char *hex) {
    if (!hex || hex[0] != '#' || strlen(hex) < 7) return 0;
    return (uint32_t)strtol(hex + 1, NULL, 16);
}

/* ── Parsea una línea de gtk.css buscando una clave de color ─ */

static inline int parse_gtk_color(const char *line,
                                   const char *key,
                                   char out[8]) {
    const char *p = strstr(line, key);
    if (!p) return 0;
    p += strlen(key);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p == '#') {
        int i;
        char hex[8];
        for (i = 0; i < 7 && (isxdigit((unsigned char)p[i+1]) || p[i] == '#'); i++)
            hex[i] = p[i];
        hex[i] = '\0';
        if (strlen(hex) == 7) {
            memcpy(out, hex, 8);
            return 1;
        }
    }
    return 0;
}

/* ── Carga colores del tema GTK activo ───────────────────── */

static inline void theme_load_gtk(Theme *t) {
    const char *home = getenv("HOME");
    if (!home) return;

    /* 1. Leer nombre del tema desde settings.ini */
    char ini[512];
    snprintf(ini, sizeof ini, "%s/.config/gtk-3.0/settings.ini", home);
    FILE *f = fopen(ini, "r");
    if (!f) return;

    char theme_name[256] = "";
    char line[512];
    while (fgets(line, sizeof line, f)) {
        const char *p = strstr(line, "gtk-theme-name");
        if (p) {
            p = strchr(p, '=');
            if (p++) {
                while (*p == ' ') p++;
                size_t n = strcspn(p, " \n\r\t");
                if (n && n < sizeof theme_name) {
                    strncpy(theme_name, p, n);
                    theme_name[n] = '\0';
                }
            }
        }
    }
    fclose(f);
    if (!theme_name[0]) return;

    /* 2. Buscar gtk.css del tema */
    const char *paths[] = {
        "%s/.themes/%s/gtk-3.0/gtk.css",
        "/usr/share/themes/%s/gtk-3.0/gtk.css",
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        char css[512];
        if (i == 0)
            snprintf(css, sizeof css, paths[i], home, theme_name);
        else
            snprintf(css, sizeof css, paths[i], theme_name);

        f = fopen(css, "r");
        if (!f) continue;

        char tmp[8];
        while (fgets(line, sizeof line, f)) {
            /* Buscar posibles variables de color */
            if (parse_gtk_color(line, "accent_bg_color", tmp)) {
                memcpy(t->accent,  tmp, 8);
                memcpy(t->bact,  tmp, 8);
                t->pix_bact = hex_to_pix(tmp);
            }
            if (parse_gtk_color(line, "accent_color", tmp)) {
                memcpy(t->accent,  tmp, 8);
            }
            if (parse_gtk_color(line, "window_bg_color", tmp)) {
                memcpy(t->bg,  tmp, 8);
                t->pix_bg = hex_to_pix(tmp);
            }
            if (parse_gtk_color(line, "window_fg_color", tmp)) {
                memcpy(t->fg,  tmp, 8);
            }
            if (parse_gtk_color(line, "headerbar_bg_color", tmp)) {
                memcpy(t->bg,  tmp, 8);
                t->pix_bg = hex_to_pix(tmp);
            }
            if (parse_gtk_color(line, "unfocused_border_color", tmp)) {
                memcpy(t->binact,  tmp, 8);
                t->pix_binact = hex_to_pix(tmp);
            }
            if (parse_gtk_color(line, "error_color", tmp)) {
                memcpy(t->urgent,  tmp, 8);
                t->pix_urgent = hex_to_pix(tmp);
            }
        }
        fclose(f);
        fprintf(stderr, "[theme] Cargado: %s\n", theme_name);
        return;
    }
}

/* ── Inicializa y carga tema completo ────────────────────── */


/* ── Lee ~/.config/minde/settings.conf (generado por minde-settings) ─ */
static inline void theme_load_settings(Theme *t) {
    const char *home = getenv("HOME");
    if (!home) return;
    char path[512];
    snprintf(path, sizeof path, "%s/.config/minde/settings.conf", home);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = line, *v = eq + 1;
        char hex[8];
        if (!strcmp(k, "bg"))     { strncpy(t->bg,     v, 7); t->bg[7]='\0';
                                    t->pix_bg     = hex_to_pix(t->bg);     }
        else if (!strcmp(k,"fg")) { strncpy(t->fg,     v, 7); t->fg[7]='\0'; }
        else if (!strcmp(k,"accent")){ strncpy(t->accent,v,7); t->accent[7]='\0'; }
        else if (!strcmp(k,"dim"))   { strncpy(t->dim,  v, 7); t->dim[7]='\0';   }
        else if (!strcmp(k,"bact"))  { strncpy(t->bact, v, 7); t->bact[7]='\0';
                                       t->pix_bact = hex_to_pix(t->bact);  }
        else if (!strcmp(k,"binact")){ strncpy(t->binact,v,7); t->binact[7]='\0';
                                       t->pix_binact=hex_to_pix(t->binact);}
        else if (!strcmp(k,"urgent")){ strncpy(t->urgent,v,7); t->urgent[7]='\0';
                                       t->pix_urgent=hex_to_pix(t->urgent);}
        (void)hex;
    }
    fclose(f);
    fprintf(stderr, "[theme] settings.conf cargado\n");
}

static inline void theme_init(Theme *t) {
    theme_defaults(t);
    theme_load_gtk(t);      /* GTK si hay tema GTK instalado   */
    theme_load_settings(t); /* settings.conf gana sobre todo   */
}
