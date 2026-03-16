/* min-shortcut.c — Gestor de atajos de teclado de minDE
 *
 * Mismo estilo que min-launch.
 * Lee ~/.config/minde/shortcuts.conf
 * Permite buscar, añadir y borrar atajos.
 *
 * Formato shortcuts.conf:
 *   Super+Shift+B=firefox
 *   Super+Shift+F=thunar
 *
 * Teclas en lista: Arriba/Abajo=navegar Enter=ejecutar
 *                  A=añadir nuevo  Del/D=borrar  Esc=cerrar
 * En modo añadir:  Tab=cambiar campo  Enter=guardar  Esc=cancelar
 *
 * Compilar:
 *   gcc -O2 -o min-shortcut min-shortcut.c \
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

/* ═══ ATAJOS ═════════════════════════════════════════════════ */

#define MAX_SC   64
#define MAX_KEY  64
#define MAX_CMD  256

typedef struct {
    char key[MAX_KEY];
    char cmd[MAX_CMD];
} Shortcut;

static Shortcut scs[MAX_SC];
static int      nsc = 0;

static void sc_dir(char *out, int len) {
    const char *h = getenv("HOME");
    snprintf(out,(size_t)len,"%s/.config/minde",h?h:"/tmp");
}

static void sc_path(char *out, int len) {
    char d[256]; sc_dir(d,sizeof d);
    snprintf(out,(size_t)len,"%s/shortcuts.conf",d);
}

static void sc_load(void) {
    nsc = 0;
    char p[512]; sc_path(p,sizeof p);
    FILE *f = fopen(p,"r");
    if (!f) {
        char d[256]; sc_dir(d,sizeof d); mkdir(d,0755);
        f = fopen(p,"w");
        if (f) {
            fputs("# min-shortcut — atajos de minDE\n"
                  "# Formato: Modificador+Tecla=comando\n"
                  "# Ejemplo: Super+Shift+B=firefox\n", f);
            fclose(f);
        }
        return;
    }
    char line[512];
    while (fgets(line,sizeof line,f) && nsc < MAX_SC) {
        line[strcspn(line,"\r\n")] = '\0';
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line,'='); if (!eq) continue;
        *eq = '\0';
        if (!line[0] || !*(eq+1)) continue;
        snprintf(scs[nsc].key, MAX_KEY, "%.*s", MAX_KEY-1, line);
        snprintf(scs[nsc].cmd, MAX_CMD, "%.*s", MAX_CMD-1, eq+1);
        nsc++;
    }
    fclose(f);
}

static void sc_save(void) {
    char d[256]; sc_dir(d,sizeof d); mkdir(d,0755);
    char p[512]; sc_path(p,sizeof p);
    FILE *f = fopen(p,"w"); if (!f) return;
    fputs("# min-shortcut — atajos de minDE\n"
          "# Formato: Modificador+Tecla=comando\n\n", f);
    for (int i = 0; i < nsc; i++)
        fprintf(f,"%s=%s\n",scs[i].key,scs[i].cmd);
    fclose(f);
}

/* ── Filtrado ── */
static int   match_idx[MAX_SC];
static int   nmatches = 0;
static int   selected = 0;
static int   scroll   = 0;
static char  input[128] = "";
static int   input_len  = 0;

static void filter_list(void) {
    nmatches = 0; selected = 0; scroll = 0;
    for (int i = 0; i < nsc; i++) {
        if (!input_len) { match_idx[nmatches++] = i; continue; }
        if (strcasestr(scs[i].key, input) || strcasestr(scs[i].cmd, input))
            match_idx[nmatches++] = i;
    }
}

/* ── Modo edición ── */
#define MODE_LIST 0
#define MODE_EDIT 1
static int  mode = MODE_LIST;
static char edit_key[MAX_KEY] = "";
static int  edit_key_len = 0;
static char edit_cmd[MAX_CMD] = "";
static int  edit_cmd_len = 0;
static int  edit_field = 0;  /* 0=key 1=cmd */

/* ═══ X11 ════════════════════════════════════════════════════ */

static Display  *dpy;
static int       scr;
static Window    root, win;
static XftDraw  *xd;
static XftFont  *font;
static Theme     theme;
static XftColor  c_bg, c_fg, c_accent, c_dim;

#define SC_ROWS  12
#define SC_W    560

static int W, H, row_h, hdr_h;

static void alloc_color(XftColor *c, const char *hex) {
    if (!XftColorAllocName(dpy, DefaultVisual(dpy,scr),
            DefaultColormap(dpy,scr), hex, c))
        XftColorAllocName(dpy, DefaultVisual(dpy,scr),
            DefaultColormap(dpy,scr), "#000000", c);
}

static int text_w(const char *s) {
    XGlyphInfo ext;
    XftTextExtentsUtf8(dpy,font,(const FcChar8*)s,strlen(s),&ext);
    return ext.xOff;
}

static void draw_text(int x, int y, const char *s, XftColor *col) {
    XftDrawStringUtf8(xd,col,font,x,y,(const FcChar8*)s,strlen(s));
}

static void draw(void) {
    GC gc = DefaultGC(dpy,scr);

    XSetForeground(dpy,gc,(unsigned long)theme.pix_bg);
    XFillRectangle(dpy,win,gc,0,0,(unsigned)W,(unsigned)H);

    /* Cabecera — igual que min-launch */
    XSetForeground(dpy,gc,(unsigned long)theme.pix_bact);
    XFillRectangle(dpy,win,gc,0,0,(unsigned)W,(unsigned)hdr_h);

    int baseline = LAUNCHER_PAD + font->ascent;

    if (mode == MODE_LIST) {
        draw_text(LAUNCHER_PAD, baseline, ">", &c_accent);
        draw_text(LAUNCHER_PAD + text_w("> ") + 4, baseline, input, &c_fg);
        int cx = LAUNCHER_PAD + text_w("> ") + 4 + text_w(input);
        draw_text(cx, baseline, "_", &c_fg);
    } else {
        /* Cabecera modo edición */
        const char *lbl = edit_field == 0 ? "Tecla:" : "Comando:";
        draw_text(LAUNCHER_PAD, baseline, lbl, &c_bg);
        const char *val = edit_field == 0 ? edit_key : edit_cmd;
        draw_text(LAUNCHER_PAD + text_w("Comando: "), baseline, val, &c_bg);
        int cx = LAUNCHER_PAD + text_w("Comando: ") + text_w(val);
        draw_text(cx, baseline, "_", &c_bg);
    }

    XSetForeground(dpy,gc,0x3a3f5c);
    XFillRectangle(dpy,win,gc,0,hdr_h,(unsigned)W,1);

    if (mode == MODE_LIST) {
        /* Lista */
        int vis = nmatches < SC_ROWS ? nmatches : SC_ROWS;

        if (nsc == 0) {
            int ty = hdr_h + 1 + row_h/2 + font->ascent;
            draw_text(LAUNCHER_PAD, ty,
                "Sin atajos  (pulsa A para añadir)", &c_dim);
        }

        for (int i = 0; i < vis; i++) {
            int mi = scroll + i;
            if (mi >= nmatches) break;
            int idx = match_idx[mi];

            int iy = hdr_h + 1 + i * row_h;
            int ty = iy + font->ascent + (row_h-font->ascent-font->descent)/2;

            if (mi == selected) {
                XSetForeground(dpy,gc,(unsigned long)theme.pix_bact);
                XFillRectangle(dpy,win,gc,0,iy,(unsigned)W,(unsigned)row_h);
            } else if (i % 2 != 0) {
                unsigned long alt=(theme.pix_bg&0xfefefe)+0x060608;
                XSetForeground(dpy,gc,alt);
                XFillRectangle(dpy,win,gc,0,iy,(unsigned)W,(unsigned)row_h);
            }

            /* Columna tecla (mitad izquierda) */
            XftColor *kc = (mi==selected) ? &c_bg : &c_accent;
            /* Truncar tecla si es muy larga */
            char kd[MAX_KEY]; snprintf(kd,MAX_KEY,"%.*s",MAX_KEY-1,scs[idx].key);
            while (text_w(kd) > W/2 - LAUNCHER_PAD*2 && strlen(kd) > 3) {
                int l = strlen(kd);
                kd[l-4]='.'; kd[l-3]='.'; kd[l-2]='.'; kd[l-1]='\0';
            }
            draw_text(LAUNCHER_PAD, ty, kd, kc);

            /* Separador "→" */
            int kw = W/2 - LAUNCHER_PAD;
            XftColor *ac = (mi==selected) ? &c_bg : &c_dim;
            draw_text(kw, ty, "\xe2\x86\x92", ac);

            /* Columna comando (mitad derecha) */
            char cd[MAX_CMD]; snprintf(cd,MAX_CMD,"%.*s",MAX_CMD-1,scs[idx].cmd);
            int max_cmd_w = W - kw - text_w("\xe2\x86\x92 ") - LAUNCHER_PAD*2;
            while (text_w(cd) > max_cmd_w && strlen(cd) > 3) {
                int l = strlen(cd);
                cd[l-4]='.'; cd[l-3]='.'; cd[l-2]='.'; cd[l-1]='\0';
            }
            XftColor *cc = (mi==selected) ? &c_bg : &c_fg;
            draw_text(kw + text_w("\xe2\x86\x92 "), ty, cd, cc);
        }

        /* Scrollbar */
        if (nmatches > SC_ROWS) {
            int sb_h = SC_ROWS * row_h;
            int th = sb_h * SC_ROWS / nmatches; if (th < 4) th = 4;
            int ty = hdr_h+1+(sb_h-th)*scroll/(nmatches-SC_ROWS);
            XSetForeground(dpy,gc,(unsigned long)theme.pix_bact);
            XFillRectangle(dpy,win,gc,W-3,ty,3,(unsigned)th);
        }

    } else {
        /* Modo edición: dos filas */
        /* Fila 1: tecla */
        int iy1 = hdr_h + 1;
        int ty1 = iy1 + font->ascent + (row_h-font->ascent-font->descent)/2;
        if (edit_field == 0) {
            XSetForeground(dpy,gc,(unsigned long)theme.pix_bact);
            XFillRectangle(dpy,win,gc,0,iy1,(unsigned)W,(unsigned)row_h);
        }
        XftColor *kc = (edit_field==0)?&c_bg:&c_dim;
        draw_text(LAUNCHER_PAD, ty1, "Tecla:   ", kc);
        XftColor *kv = (edit_field==0)?&c_bg:&c_fg;
        draw_text(LAUNCHER_PAD+text_w("Comando:  "), ty1, edit_key, kv);
        if (edit_field==0) {
            int cx = LAUNCHER_PAD+text_w("Comando:  ")+text_w(edit_key);
            draw_text(cx,ty1,"_",&c_bg);
        }

        /* Fila 2: comando */
        int iy2 = hdr_h + 1 + row_h + 2;
        int ty2 = iy2 + font->ascent + (row_h-font->ascent-font->descent)/2;
        if (edit_field == 1) {
            XSetForeground(dpy,gc,(unsigned long)theme.pix_bact);
            XFillRectangle(dpy,win,gc,0,iy2,(unsigned)W,(unsigned)row_h);
        }
        XftColor *cc = (edit_field==1)?&c_bg:&c_dim;
        draw_text(LAUNCHER_PAD, ty2, "Comando: ", cc);
        XftColor *cv = (edit_field==1)?&c_bg:&c_fg;
        draw_text(LAUNCHER_PAD+text_w("Comando:  "), ty2, edit_cmd, cv);
        if (edit_field==1) {
            int cx = LAUNCHER_PAD+text_w("Comando:  ")+text_w(edit_cmd);
            draw_text(cx,ty2,"_",&c_bg);
        }

        /* Ayuda */
        int hy = hdr_h + 1 + row_h*3 + 4;
        draw_text(LAUNCHER_PAD, hy+font->ascent,
            "Tab=siguiente campo   Enter=guardar   Esc=cancelar", &c_dim);
        draw_text(LAUNCHER_PAD, hy+font->ascent+row_h,
            "Ejemplo tecla: Super+Shift+B", &c_dim);
    }

    XFlush(dpy);
}

/* ── Acciones ── */
static void do_run(void) {
    if (selected < 0 || selected >= nmatches) return;
    int idx = match_idx[selected];
    if (!scs[idx].cmd[0]) return;
    if (fork() == 0) {
        setsid();
        execlp("/bin/sh","sh","-c",scs[idx].cmd,NULL);
        _exit(127);
    }
    XCloseDisplay(dpy); exit(0);
}

static void do_delete(void) {
    if (selected < 0 || selected >= nmatches) return;
    int idx = match_idx[selected];
    memmove(&scs[idx],&scs[idx+1],(size_t)(nsc-idx-1)*sizeof(Shortcut));
    nsc--;
    sc_save(); filter_list();
    if (selected >= nmatches && selected > 0) selected--;
    draw();
}

/* ── Teclado ── */
static void edit_backspace(char *buf, int *len) {
    if (*len > 0) {
        do { (*len)--; }
        while (*len > 0 && (buf[*len] & 0xC0) == 0x80);
        buf[*len] = '\0';
    }
}

static void edit_append(char *buf, int *len, int max, const char *s) {
    int bl = strlen(s);
    if (*len + bl < max) {
        memcpy(buf + *len, s, (size_t)bl);
        *len += bl; buf[*len] = '\0';
    }
}

static void on_key(XKeyEvent *e) {
    char buf[32] = ""; KeySym sym;
    XLookupString(e, buf, sizeof buf-1, &sym, NULL);

    if (mode == MODE_LIST) {
        switch (sym) {
        case XK_Escape: XCloseDisplay(dpy); exit(0);
        case XK_Return: case XK_KP_Enter: do_run(); break;
        case XK_Up: case XK_KP_Up:
            if (selected > 0) { selected--;
                if (selected < scroll) scroll = selected; }
            draw(); break;
        case XK_Down: case XK_KP_Down:
            if (selected < nmatches-1) { selected++;
                if (selected >= scroll+SC_ROWS) scroll=selected-SC_ROWS+1; }
            draw(); break;
        case XK_Delete: case XK_d:
            if (!input_len) { do_delete(); break; }
            /* 'd' como búsqueda */
            if (input_len < 126) {
                input[input_len++]='d'; input[input_len]='\0';
                filter_list(); draw();
            }
            break;
        case XK_BackSpace:
            if (input_len > 0) {
                do { input_len--; }
                while (input_len>0 && (input[input_len]&0xC0)==0x80);
                input[input_len]='\0'; filter_list(); draw();
            }
            break;
        case XK_a:
            if (input_len == 0) {
                mode=MODE_EDIT;
                edit_key[0]='\0'; edit_key_len=0;
                edit_cmd[0]='\0'; edit_cmd_len=0;
                edit_field=0; draw();
            } else {
                if (input_len<126){
                    input[input_len++]='a'; input[input_len]='\0';
                    filter_list(); draw();
                }
            }
            break;
        default:
            if (buf[0] >= 0x20 && input_len < 126) {
                int bl=strlen(buf);
                if (input_len+bl<126) {
                    memcpy(input+input_len,buf,(size_t)bl);
                    input_len+=bl; input[input_len]='\0';
                    filter_list(); draw();
                }
            }
        }
    } else {
        /* Modo edición */
        switch (sym) {
        case XK_Escape: mode=MODE_LIST; draw(); break;
        case XK_Tab:    edit_field^=1; draw(); break;
        case XK_Return: case XK_KP_Enter:
            if (edit_field == 0 && edit_key[0]) {
                edit_field = 1; draw();
            } else if (edit_key[0] && edit_cmd[0] && nsc < MAX_SC) {
                snprintf(scs[nsc].key, MAX_KEY, "%.*s", MAX_KEY-1, edit_key);
                snprintf(scs[nsc].cmd, MAX_CMD, "%.*s", MAX_CMD-1, edit_cmd);
                nsc++;
                sc_save();
                input[0]='\0'; input_len=0;
                filter_list();
                mode=MODE_LIST; draw();
            }
            break;
        case XK_BackSpace:
            if (edit_field==0) edit_backspace(edit_key,&edit_key_len);
            else               edit_backspace(edit_cmd,&edit_cmd_len);
            draw(); break;
        default:
            if (buf[0] >= 0x20) {
                if (edit_field==0) edit_append(edit_key,&edit_key_len,MAX_KEY-1,buf);
                else               edit_append(edit_cmd,&edit_cmd_len,MAX_CMD-1,buf);
                draw();
            }
        }
    }
}

static void on_click(int mx, int my) {
    (void)mx;
    if (mode != MODE_LIST || my < hdr_h) return;
    for (int i = 0; i < SC_ROWS; i++) {
        int iy = hdr_h + 1 + i * row_h;
        if (my >= iy && my < iy + row_h) {
            int mi = scroll + i;
            if (mi >= nmatches) break;
            if (mi == selected) { do_run(); return; }
            selected = mi; draw(); return;
        }
    }
}

/* ═══ MAIN ═══════════════════════════════════════════════════ */

int main(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fputs("min-shortcut: no X\n",stderr); return 1; }
    scr  = DefaultScreen(dpy);
    root = DefaultRootWindow(dpy);

    sc_load();
    theme_init(&theme);

    font = XftFontOpenName(dpy,scr,FONT_NAME);
    if (!font) font = XftFontOpenName(dpy,scr,"fixed:size=9");
    if (!font) { fputs("min-shortcut: no font\n",stderr); return 1; }

    alloc_color(&c_bg,    theme.bg);
    alloc_color(&c_fg,    theme.fg);
    alloc_color(&c_accent,theme.accent);
    alloc_color(&c_dim,   theme.dim);

    row_h = font->ascent + font->descent + LAUNCHER_PAD;
    hdr_h = row_h + 2 * LAUNCHER_PAD;
    W = SC_W;
    H = hdr_h + 1 + SC_ROWS * row_h;

    filter_list();

    int sw=DisplayWidth(dpy,scr), sh=DisplayHeight(dpy,scr);

    XSetWindowAttributes attr={0};
    attr.background_pixel  =(unsigned long)theme.pix_bg;
    attr.override_redirect =True;
    attr.event_mask        =KeyPressMask|ButtonPressMask|ExposureMask;
    attr.border_pixel      =(unsigned long)theme.pix_bact;

    win=XCreateWindow(dpy,root,(sw-W)/2,(sh-H)/3,
        (unsigned)W,(unsigned)H,BORDER_WIDTH,
        DefaultDepth(dpy,scr),InputOutput,DefaultVisual(dpy,scr),
        CWBackPixel|CWOverrideRedirect|CWEventMask|CWBorderPixel,&attr);

    XStoreName(dpy,win,"min-shortcut");
    xd=XftDrawCreate(dpy,win,DefaultVisual(dpy,scr),DefaultColormap(dpy,scr));
    XMapRaised(dpy,win);
    XSetInputFocus(dpy,win,RevertToPointerRoot,CurrentTime);
    draw();

    XEvent ev;
    while (XNextEvent(dpy,&ev)==0) {
        switch(ev.type) {
        case Expose:      if(ev.xexpose.count==0)draw(); break;
        case KeyPress:    on_key(&ev.xkey); break;
        case ButtonPress: on_click(ev.xbutton.x,ev.xbutton.y); break;
        }
    }
    XftFontClose(dpy,font); XftDrawDestroy(xd); XCloseDisplay(dpy);
    return 0;
}
