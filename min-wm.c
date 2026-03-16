/* minwm.c — Window manager de minDE
 *
 * Filosofía:
 *   - 5 workspaces, exactamente 1 ventana por workspace
 *   - Sin decoraciones: solo borde coloreado (activo/inactivo)
 *   - Ventana siempre a pantalla completa menos BAR_HEIGHT
 *   - Colores leídos del tema GTK activo
 *   - EWMH básico para que barra y apps GTK funcionen bien
 *   - Focus sigue al ratón (focus-follows-mouse)
 *
 * Compilar (ver Makefile):
 *   gcc -O2 -o minwm minwm.c \
 *       $(pkg-config --cflags --libs xcb xcb-keysyms xcb-ewmh xcb-icccm)
 *
 * Atajos (Super = tecla Windows):
 *   Super + 1..5      → cambiar workspace
 *   Super + →/←       → workspace siguiente/anterior
 *   Super + Enter     → terminal (TERMINAL en config.h)
 *   Super + D         → launcher (minrun)
 *   Super + Q         → cerrar ventana activa
 *   Super + Shift + Q → salir del WM
 *   Super + Shift + R → recargar tema en vivo
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>
#include <X11/keysym.h>

#include "config.h"

/* ── Estado global ────────────────────────────────────────── */

static xcb_connection_t      *conn;
static xcb_screen_t          *scr;
static xcb_ewmh_connection_t  ewmh;
static xcb_key_symbols_t     *ksyms;
static Theme                  theme;

/* Átomos ICCCM (no forman parte de xcb_ewmh_connection_t) */
static xcb_atom_t   a_wm_protocols;
static xcb_atom_t   a_wm_delete_window;

/* Cada workspace guarda su ventana (XCB_WINDOW_NONE = vacío) */
static xcb_window_t ws_win[NUM_WS];
static int          ws_cur   = 0;
static int          running  = 1;
/* Cuántos UNMAP_NOTIFY debemos ignorar (los que generamos nosotros) */
static int          ignore_unmap = 0;

/* ── Prototipos ───────────────────────────────────────────── */

static void ws_go(int n);
static void ws_next(void);
static void ws_prev(void);
static void win_close(void);
static void do_terminal(void);
static void do_launcher(void);
static void do_quit(void);
static void do_reload(void);
static void do_wifi(void);
static void do_theme(void);
static void do_clip(void);
static void do_shortcut(void);
static void do_session(void);
static void do_launcher_kill(void);

/* ── Tabla de keybinds ────────────────────────────────────── */

typedef struct { xcb_keysym_t sym; uint16_t mod; void (*fn)(void); } Bind;

/* ws0..ws4 se asignan dinámicamente en main() */
static void ws0(void){ws_go(0);} static void ws1(void){ws_go(1);}
static void ws2(void){ws_go(2);} static void ws3(void){ws_go(3);}
static void ws4(void){ws_go(4);}

static Bind binds[] = {
    { XK_1,     MOD,              ws0          },
    { XK_2,     MOD,              ws1          },
    { XK_3,     MOD,              ws2          },
    { XK_4,     MOD,              ws3          },
    { XK_5,     MOD,              ws4          },
    { XK_Right, MOD,              ws_next      },
    { XK_Left,  MOD,              ws_prev      },
    { XK_Return,MOD,              do_terminal  },
    { XK_d,     MOD,              do_launcher  },
    { XK_q,     MOD,              win_close    },
    { XK_q,     MOD|XCB_MOD_MASK_SHIFT, do_quit  },
    { XK_r,     MOD|XCB_MOD_MASK_SHIFT, do_reload },
    { XK_w,     MOD|XCB_MOD_MASK_SHIFT, do_wifi     },
    { XK_t,     MOD|XCB_MOD_MASK_SHIFT, do_theme    },
    { XK_p,     MOD|XCB_MOD_MASK_SHIFT, do_clip     },
    { XK_o,     MOD|XCB_MOD_MASK_SHIFT, do_shortcut },
    { XK_Escape,MOD,                    do_session  },
};

/* ── Utilidades ───────────────────────────────────────────── */

static void spawn(const char *cmd) {
    if (fork() == 0) {
        setsid();
        execlp("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(127);
    }
}

static void do_terminal(void)    { spawn(TERMINAL); }
static void do_launcher(void)    { spawn(LAUNCHER); }
static void do_quit(void)        { running = 0; }
static void do_reload(void)      { kill(getpid(), SIGUSR1); }

/* Lanza min-net (panel wifi de texto) */
static void do_wifi(void) {
    const char *home = getenv("HOME");
    char cmd[512];
    if (home) snprintf(cmd, sizeof cmd, "%s/.local/bin/min-net &", home);
    else       snprintf(cmd, sizeof cmd, "min-net &");
    spawn(cmd);
}

static void do_theme(void) {
    const char *home = getenv("HOME");
    char cmd[512];
    if (home) snprintf(cmd, sizeof cmd, "%s/.local/bin/min-theme &", home);
    else       snprintf(cmd, sizeof cmd, "min-theme &");
    spawn(cmd);
}

static void do_clip(void) {
    const char *home = getenv("HOME");
    char cmd[512];
    if (home) snprintf(cmd, sizeof cmd, "%s/.local/bin/min-clip &", home);
    else       snprintf(cmd, sizeof cmd, "min-clip &");
    spawn(cmd);
}

static void do_shortcut(void) {
    const char *home = getenv("HOME");
    char cmd[512];
    if (home) snprintf(cmd, sizeof cmd, "%s/.local/bin/min-shortcut &", home);
    else       snprintf(cmd, sizeof cmd, "min-shortcut &");
    spawn(cmd);
}

static void do_session(void) {
    const char *home = getenv("HOME");
    char cmd[512];
    if (home) snprintf(cmd, sizeof cmd, "%s/.local/bin/min-session &", home);
    else       snprintf(cmd, sizeof cmd, "min-session &");
    spawn(cmd);
}

/* Cierra min-launch si está abierto (al mapear una ventana real) */
static void do_launcher_kill(void) {
    spawn("pkill -x min-launch 2>/dev/null");
}

/* ── Borde ────────────────────────────────────────────────── */

static void border_set(xcb_window_t w, uint32_t pix) {
    xcb_change_window_attributes(conn, w, XCB_CW_BORDER_PIXEL, &pix);
}

/* ── Geometría ────────────────────────────────────────────── */

/* Ventana principal:
 *   - Lee WM_NORMAL_HINTS para saber el tamaño preferido
 *   - Si cabe en pantalla → usar ese tamaño y centrar
 *   - Si no cabe → ocupar toda la pantalla menos la barra       */
static void tile(xcb_window_t w) {
    int bh  = theme.bar_height;
    int bw  = theme.border_width;
    int sw  = (int)scr->width_in_pixels;
    int sh  = (int)scr->height_in_pixels - bh;

    /* Leer size hints de la ventana */
    xcb_size_hints_t hints;
    int has_hints = 0;
    xcb_get_property_reply_t *rp = xcb_get_property_reply(conn,
        xcb_get_property(conn, 0, w,
            XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 0, 18), NULL);
    if (rp && xcb_get_property_value_length(rp) > 0) {
        memcpy(&hints, xcb_get_property_value(rp), sizeof hints);
        has_hints = 1;
    }
    if (rp) free(rp);

    int wx = 0, wy = bh, ww = sw, wh = sh;

    if (has_hints) {
        /* PSize: tamaño preferido explícito del programa */
        int pw = 0, ph = 0;
        if (hints.flags & (1 << 3)) { pw = hints.width;  ph = hints.height; }
        /* USSize: tamaño indicado por el usuario al lanzar la app */
        if (hints.flags & (1 << 1)) { pw = hints.width;  ph = hints.height; }

        if (pw > 32 && ph > 32 && pw < sw && ph < sh) {
            /* La ventana cabe: centrarla con su tamaño natural */
            ww = pw;
            wh = ph;
            wx = (sw - ww) / 2;
            wy = bh + (sh - wh) / 2;
        }
        /* Si no cabe o no hay hints válidos: ocupar toda la pantalla */
    }

    uint32_t v[5] = {
        (uint32_t)wx, (uint32_t)wy,
        (uint32_t)(ww - bw * 2),
        (uint32_t)(wh - bw * 2),
        (uint32_t)bw
    };
    xcb_configure_window(conn, w,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
        XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
        XCB_CONFIG_WINDOW_BORDER_WIDTH, v);
}

/* Ventana flotante (dialog/transient):
 *   - Respeta el tamaño natural si cabe al 60%
 *   - Siempre centrada sobre la pantalla                        */
static void float_center(xcb_window_t w) {
    int bh  = theme.bar_height;
    int bw  = theme.border_width;
    int sw  = (int)scr->width_in_pixels;
    int sh  = (int)scr->height_in_pixels - bh;

    int fw = sw * 60 / 100;
    int fh = sh * 60 / 100;

    /* Intentar usar tamaño natural de la ventana */
    xcb_get_property_reply_t *rp = xcb_get_property_reply(conn,
        xcb_get_property(conn, 0, w,
            XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 0, 18), NULL);
    if (rp && xcb_get_property_value_length(rp) > 0) {
        xcb_size_hints_t hints;
        memcpy(&hints, xcb_get_property_value(rp), sizeof hints);
        int pw = 0, ph = 0;
        if (hints.flags & (1 << 3)) { pw = hints.width; ph = hints.height; }
        if (hints.flags & (1 << 1)) { pw = hints.width; ph = hints.height; }
        if (pw > 32 && ph > 32 && pw <= fw && ph <= fh) {
            fw = pw; fh = ph;
        }
    }
    if (rp) free(rp);

    int fx = (sw - fw) / 2;
    int fy = bh + (sh - fh) / 2;
    uint32_t v[5] = {
        (uint32_t)fx, (uint32_t)fy,
        (uint32_t)(fw - bw * 2),
        (uint32_t)(fh - bw * 2),
        (uint32_t)bw
    };
    xcb_configure_window(conn, w,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
        XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
        XCB_CONFIG_WINDOW_BORDER_WIDTH, v);
}

/* Devuelve 1 si la ventana tiene WM_TRANSIENT_FOR (es un dialog/popup) */
static int is_transient(xcb_window_t w) {
    xcb_get_property_reply_t *r = xcb_get_property_reply(conn,
        xcb_get_property(conn, 0, w,
            XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 0, 1), NULL);
    int transient = 0;
    if (r) {
        transient = (xcb_get_property_value_length(r) > 0);
        free(r);
    }
    return transient;
}

/* ── Focus ────────────────────────────────────────────────── */

static void focus(xcb_window_t w) {
    if (!w || w == XCB_WINDOW_NONE) {
        xcb_ewmh_set_active_window(&ewmh, 0, XCB_WINDOW_NONE);
        xcb_flush(conn);
        return;
    }
    border_set(w, theme.pix_bact);
    xcb_set_input_focus(conn,
        XCB_INPUT_FOCUS_POINTER_ROOT, w, XCB_CURRENT_TIME);
    xcb_ewmh_set_active_window(&ewmh, 0, w);
    /* Subir al frente (hay una sola ventana por ws, pero por si acaso) */
    uint32_t stack = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn, w, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
    xcb_flush(conn);
}

/* ── Workspaces ───────────────────────────────────────────── */

static void ws_go(int n) {
    if (n < 0 || n >= NUM_WS || n == ws_cur) return;

    /* Ocultar ventana actual — marcar que el UNMAP_NOTIFY es nuestro */
    if (ws_win[ws_cur] != XCB_WINDOW_NONE) {
        border_set(ws_win[ws_cur], theme.pix_binact);
        ignore_unmap++;
        xcb_unmap_window(conn, ws_win[ws_cur]);
    }

    ws_cur = n;
    xcb_ewmh_set_current_desktop(&ewmh, 0, (uint32_t)ws_cur);

    /* Mostrar ventana del workspace destino */
    if (ws_win[ws_cur] != XCB_WINDOW_NONE) {
        xcb_map_window(conn, ws_win[ws_cur]);
        tile(ws_win[ws_cur]);
        focus(ws_win[ws_cur]);
    } else {
        focus(XCB_WINDOW_NONE);
    }
    xcb_flush(conn);
}

static void ws_next(void) { ws_go((ws_cur + 1) % NUM_WS); }
static void ws_prev(void) { ws_go((ws_cur + NUM_WS - 1) % NUM_WS); }

/* Devuelve en qué workspace está una ventana (-1 si ninguno) */
static int ws_of(xcb_window_t w) {
    for (int i = 0; i < NUM_WS; i++)
        if (ws_win[i] == w) return i;
    return -1;
}

/* ── Gestión de ventanas ──────────────────────────────────── */

static void win_manage(xcb_window_t w) {
    /* Ignorar si ya la gestionamos */
    if (ws_of(w) >= 0) return;

    /* ── Detectar transient PRIMERO, antes de tocar nada ── */
    if (is_transient(w)) {
        /* Ventana flotante: dialogs, popups, ventanas hijo.
         * Reglas:
         *   - NO ocupa ningún slot de workspace
         *   - NO desplaza ni oculta la ventana principal
         *   - Se centra al 60% de pantalla
         *   - Borde de acento para distinguirla
         *   - Al cerrarse, la ventana principal sigue intacta */
        uint32_t mask = XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK;
        uint32_t vals[2] = {
            theme.pix_bact,
            XCB_EVENT_MASK_ENTER_WINDOW
        };
        xcb_change_window_attributes(conn, w, mask, vals);
        float_center(w);
        xcb_map_window(conn, w);
        focus(w);
        xcb_flush(conn);
        return;   /* <-- salir aquí, no tocar ws_win[] */
    }

    /* ── Ventana normal ── */

    /* Si el workspace ya tiene una ventana principal, la nueva
     * ventana flota centrada sobre ella (no desplaza a la original) */
    if (ws_win[ws_cur] != XCB_WINDOW_NONE) {
        uint32_t fmask = XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK;
        uint32_t fvals[2] = {
            theme.pix_bact,
            XCB_EVENT_MASK_ENTER_WINDOW
        };
        xcb_change_window_attributes(conn, w, fmask, fvals);
        float_center(w);
        xcb_map_window(conn, w);
        focus(w);
        xcb_flush(conn);
        return;   /* no tocar ws_win[] — ventana original intacta */
    }

    /* Workspace vacío: asignar al slot */
    ws_win[ws_cur] = w;

    /* Configurar eventos y borde */
    uint32_t mask = XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t vals[2] = {
        theme.pix_binact,
        XCB_EVENT_MASK_ENTER_WINDOW |
        XCB_EVENT_MASK_PROPERTY_CHANGE
    };
    xcb_change_window_attributes(conn, w, mask, vals);

    xcb_grab_button(conn, 0, w,
        XCB_EVENT_MASK_BUTTON_PRESS,
        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
        XCB_NONE, XCB_NONE,
        XCB_BUTTON_INDEX_ANY, MOD);

    xcb_ewmh_set_wm_desktop(&ewmh, w, (uint32_t)ws_cur);

    tile(w);
    xcb_map_window(conn, w);
    focus(w);
    xcb_flush(conn);
}

static void win_forget(xcb_window_t w) {
    int i = ws_of(w);
    if (i < 0) return;
    ws_win[i] = XCB_WINDOW_NONE;
    if (i == ws_cur)
        focus(XCB_WINDOW_NONE);
}

static void win_close(void) {
    xcb_window_t w = ws_win[ws_cur];
    if (!w || w == XCB_WINDOW_NONE) return;

    /* Intentar cierre limpio con WM_DELETE_WINDOW (átomo ICCCM) */
    xcb_get_property_reply_t *rp = xcb_get_property_reply(conn,
        xcb_get_property(conn, 0, w,
            a_wm_protocols, XCB_ATOM_ATOM, 0, 32), NULL);

    int del = 0;
    if (rp) {
        xcb_atom_t *atoms = xcb_get_property_value(rp);
        int n = xcb_get_property_value_length(rp) / sizeof(xcb_atom_t);
        for (int i = 0; i < n; i++)
            if (atoms[i] == a_wm_delete_window) { del = 1; break; }
        free(rp);
    }

    if (del) {
        xcb_client_message_event_t e = {0};
        e.response_type  = XCB_CLIENT_MESSAGE;
        e.window         = w;
        e.format         = 32;
        e.type           = a_wm_protocols;
        e.data.data32[0] = a_wm_delete_window;
        e.data.data32[1] = XCB_CURRENT_TIME;
        xcb_send_event(conn, 0, w, XCB_EVENT_MASK_NO_EVENT, (char *)&e);
    } else {
        xcb_kill_client(conn, w);
    }
    xcb_flush(conn);
}

/* ── Registro de teclas ───────────────────────────────────── */

static void keys_grab(void) {
    xcb_ungrab_key(conn, XCB_GRAB_ANY, scr->root, XCB_MOD_MASK_ANY);
    int n = (int)(sizeof binds / sizeof *binds);
    for (int i = 0; i < n; i++) {
        xcb_keycode_t *kcs = xcb_key_symbols_get_keycode(ksyms, binds[i].sym);
        if (!kcs) continue;
        for (xcb_keycode_t *kc = kcs; *kc; kc++) {
            /* Grabar con y sin NumLock (MOD_MASK_2) */
            xcb_grab_key(conn, 1, scr->root, binds[i].mod, *kc,
                XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
            xcb_grab_key(conn, 1, scr->root,
                binds[i].mod | XCB_MOD_MASK_2, *kc,
                XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        }
        free(kcs);
    }
    xcb_flush(conn);
}

/* ── EWMH ─────────────────────────────────────────────────── */

static void ewmh_setup(void) {
    xcb_intern_atom_cookie_t *ck = xcb_ewmh_init_atoms(conn, &ewmh);
    xcb_ewmh_init_atoms_replies(&ewmh, ck, NULL);

    /* Internamos los átomos ICCCM manualmente (no están en xcb_ewmh) */
    {
        xcb_intern_atom_reply_t *r;
        r = xcb_intern_atom_reply(conn,
            xcb_intern_atom(conn, 0, strlen("WM_PROTOCOLS"), "WM_PROTOCOLS"), NULL);
        a_wm_protocols = r ? r->atom : XCB_ATOM_NONE;
        free(r);

        r = xcb_intern_atom_reply(conn,
            xcb_intern_atom(conn, 0, strlen("WM_DELETE_WINDOW"), "WM_DELETE_WINDOW"), NULL);
        a_wm_delete_window = r ? r->atom : XCB_ATOM_NONE;
        free(r);
    }

    xcb_atom_t supported[] = {
        ewmh._NET_SUPPORTED,
        ewmh._NET_ACTIVE_WINDOW,
        ewmh._NET_NUMBER_OF_DESKTOPS,
        ewmh._NET_CURRENT_DESKTOP,
        ewmh._NET_WM_DESKTOP,
        ewmh._NET_WM_STATE,
        ewmh._NET_WM_STATE_FULLSCREEN,
        ewmh._NET_WM_NAME,
        ewmh._NET_WM_WINDOW_TYPE,
        ewmh._NET_WM_WINDOW_TYPE_DOCK,
        ewmh._NET_WM_STRUT_PARTIAL,
        a_wm_protocols,
        a_wm_delete_window,
    };
    xcb_ewmh_set_supported(&ewmh, 0,
        sizeof supported / sizeof *supported, supported);

    xcb_ewmh_set_number_of_desktops(&ewmh, 0, NUM_WS);
    xcb_ewmh_set_current_desktop(&ewmh, 0, 0);

    /* Ventana de soporte del WM (requerida por EWMH) */
    xcb_window_t sup = xcb_generate_id(conn);
    xcb_create_window(conn, 0, sup, scr->root,
        -1, -1, 1, 1, 0,
        XCB_WINDOW_CLASS_INPUT_ONLY,
        scr->root_visual, 0, NULL);
    xcb_ewmh_set_supporting_wm_check(&ewmh, scr->root, sup);
    xcb_ewmh_set_supporting_wm_check(&ewmh, sup, sup);
    xcb_ewmh_set_wm_name(&ewmh, sup, strlen("min-wm"), "min-wm");
    xcb_flush(conn);
}

/* ── Manejadores de eventos ───────────────────────────────── */

static void on_key_press(xcb_key_press_event_t *e) {
    xcb_keysym_t sym = xcb_key_press_lookup_keysym(ksyms, e, 0);
    /* Limpiar NumLock y CapsLock del estado */
    uint16_t mod = e->state & ~(XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2);
    int n = (int)(sizeof binds / sizeof *binds);
    for (int i = 0; i < n; i++)
        if (binds[i].sym == sym && binds[i].mod == mod && binds[i].fn)
            binds[i].fn();
}

static void on_map_request(xcb_map_request_event_t *e) {
    /* Verificar si es una ventana de tipo dock (barra) */
    xcb_ewmh_get_atoms_reply_t wtype = {0};
    if (xcb_ewmh_get_wm_window_type_reply(&ewmh,
            xcb_ewmh_get_wm_window_type(&ewmh, e->window), &wtype, NULL)) {
        for (uint32_t i = 0; i < wtype.atoms_len; i++) {
            if (wtype.atoms[i] == ewmh._NET_WM_WINDOW_TYPE_DOCK) {
                /* Dock: mapear sin gestionar */
                xcb_map_window(conn, e->window);
                xcb_ewmh_get_atoms_reply_wipe(&wtype);
                xcb_flush(conn);
                return;
            }
        }
        xcb_ewmh_get_atoms_reply_wipe(&wtype);
    }
    /* Cerrar min-launch si está abierto */
    do_launcher_kill();
    win_manage(e->window);
}

static void on_unmap(xcb_unmap_notify_event_t *e) {
    /* Si nosotros provocamos este unmap (cambio de workspace), ignorarlo */
    if (ignore_unmap > 0) { ignore_unmap--; return; }
    win_forget(e->window);
}

static void on_destroy(xcb_destroy_notify_event_t *e) {
    win_forget(e->window);
}

static void on_configure_request(xcb_configure_request_event_t *e) {
    /* Ventanas que gestionamos: imponerles nuestra geometría */
    if (ws_of(e->window) >= 0) {
        tile(e->window);
        return;
    }
    /* Ventanas no gestionadas (popups, dialogs, dock): respetar su pedido */
    uint32_t v[7]; int n = 0;
    uint16_t m = e->value_mask;
    if (m & XCB_CONFIG_WINDOW_X)            v[n++] = (uint32_t)e->x;
    if (m & XCB_CONFIG_WINDOW_Y)            v[n++] = (uint32_t)e->y;
    if (m & XCB_CONFIG_WINDOW_WIDTH)        v[n++] = e->width;
    if (m & XCB_CONFIG_WINDOW_HEIGHT)       v[n++] = e->height;
    if (m & XCB_CONFIG_WINDOW_BORDER_WIDTH) v[n++] = e->border_width;
    if (m & XCB_CONFIG_WINDOW_SIBLING)      v[n++] = e->sibling;
    if (m & XCB_CONFIG_WINDOW_STACK_MODE)   v[n++] = e->stack_mode;
    xcb_configure_window(conn, e->window, m, v);
    xcb_flush(conn);
}

static void on_enter(xcb_enter_notify_event_t *e) {
    if (e->mode != XCB_NOTIFY_MODE_NORMAL) return;
    /* Focus-follows-mouse */
    if (ws_of(e->event) >= 0 && e->event == ws_win[ws_cur])
        focus(e->event);
}

/* ── main ─────────────────────────────────────────────────── */

/* Pipe para despertar el bucle de eventos en SIGUSR1 */
int g_reload_pipe_w = -1;

static void reload_signal_handler(int sig) {
    (void)sig;
    if (g_reload_pipe_w >= 0) {
        char b = 1;
        write(g_reload_pipe_w, &b, 1);
    }
}

/* Aplica el tema recargado a todas las ventanas abiertas */
static void reload_theme(void) {
    theme_init(&theme);
    fprintf(stderr, "min-wm: tema recargado borde=%s\n", theme.bact);
    /* Re-aplicar bordes a todas las ventanas */
    for (int i = 0; i < NUM_WS; i++) {
        if (ws_win[i] == XCB_WINDOW_NONE) continue;
        uint32_t pix = (i == ws_cur) ? theme.pix_bact : theme.pix_binact;
        border_set(ws_win[i], pix);
        /* Re-aplicar grosor de borde */
        uint32_t bw = (uint32_t)theme.border_width;
        xcb_configure_window(conn, ws_win[i],
            XCB_CONFIG_WINDOW_BORDER_WIDTH, &bw);
    }
    xcb_flush(conn);
}

int main(void) {
    /* Conectar a X */
    int snum;
    conn = xcb_connect(NULL, &snum);
    if (xcb_connection_has_error(conn)) {
        fputs("min-wm: fallo al conectar con X\n", stderr);
        return 1;
    }

    /* Pantalla */
    const xcb_setup_t *setup = xcb_get_setup(conn);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < snum; i++) xcb_screen_next(&it);
    scr = it.data;

    /* Intentar ser el WM (SubstructureRedirect) */
    uint32_t evmask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT
                    | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
                    | XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_void_cookie_t ck = xcb_change_window_attributes_checked(conn,
        scr->root, XCB_CW_EVENT_MASK, &evmask);
    xcb_generic_error_t *err = xcb_request_check(conn, ck);
    if (err) {
        fputs("min-wm: ya hay un WM corriendo\n", stderr);
        free(err);
        xcb_disconnect(conn);
        return 1;
    }

    /* Inicializar workspaces */
    for (int i = 0; i < NUM_WS; i++) ws_win[i] = XCB_WINDOW_NONE;

    /* Tema GTK */
    theme_init(&theme);
    fprintf(stderr, "min-wm: borde activo=%s inactivo=%s\n",
            theme.bact, theme.binact);

    /* EWMH, teclas */
    ewmh_setup();
    ksyms = xcb_key_symbols_alloc(conn);
    keys_grab();

    /* Hijos zombie: ignorarlos */
    signal(SIGCHLD, SIG_IGN);

    /* Arrancar la barra */
    /* Lanzar minibar con ruta absoluta para compatibilidad con SDDM/LightDM
     * que no cargan el PATH del usuario */
    {
        const char *home = getenv("HOME");
        char bar_cmd[512];
        if (home)
            snprintf(bar_cmd, sizeof bar_cmd,
                "%s/.local/bin/min-bar &", home);
        else
            snprintf(bar_cmd, sizeof bar_cmd,
                "/usr/local/bin/min-bar &");
        spawn(bar_cmd);
    }

    fprintf(stderr,
        "min-wm: listo | Super+1-5=ws | Super+Enter=term | "
        "Super+D=launcher | Super+Q=cerrar | Super+Shift+Q=salir\n");

    /* ── PID file para que min-theme pueda enviarnos señales ── */
    {
        const char *home = getenv("HOME");
        if (home) {
            char piddir[512], pidpath[512];
            snprintf(piddir,  sizeof piddir,  "%s/.config/minde", home);
            snprintf(pidpath, sizeof pidpath, "%s/.config/minde/min-wm.pid", home);
            mkdir(piddir, 0755);
            FILE *pf = fopen(pidpath, "w");
            if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }
        }
    }

    /* ── SIGUSR1: recargar tema en vivo ── */
    /* Al recibir SIGUSR1 simplemente seteamos una bandera que el
     * bucle de eventos comprueba. No llamamos a theme_init() desde
     * el handler porque no es async-signal-safe.                  */
    /* Usamos pipe para despertar xcb_wait_for_event sin bloquear  */
    int reload_pipe[2];
    if (pipe(reload_pipe) < 0) { reload_pipe[0] = reload_pipe[1] = -1; }

    /* Guardamos el write-end en global para el handler */
    extern int g_reload_pipe_w;
    g_reload_pipe_w = reload_pipe[1];

    signal(SIGUSR1, reload_signal_handler);

    /* ── Bucle de eventos ── */
    int xfd = xcb_get_file_descriptor(conn);
    xcb_generic_event_t *ev;
    while (running) {
        /* Usar select() para vigilar tanto X como el pipe de reload */
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        int maxfd = xfd;
        if (reload_pipe[0] >= 0) {
            FD_SET(reload_pipe[0], &fds);
            if (reload_pipe[0] > maxfd) maxfd = reload_pipe[0];
        }
        if (select(maxfd + 1, &fds, NULL, NULL, NULL) < 0) break;

        /* ¿Llegó señal de recarga? */
        if (reload_pipe[0] >= 0 && FD_ISSET(reload_pipe[0], &fds)) {
            char buf[16];
            read(reload_pipe[0], buf, sizeof buf);  /* vaciar pipe */
            reload_theme();
        }

        /* Procesar todos los eventos X pendientes */
        while ((ev = xcb_poll_for_event(conn))) {
            switch (ev->response_type & ~0x80) {
            case XCB_KEY_PRESS:
                on_key_press((xcb_key_press_event_t *)ev);        break;
            case XCB_MAP_REQUEST:
                on_map_request((xcb_map_request_event_t *)ev);    break;
            case XCB_UNMAP_NOTIFY:
                on_unmap((xcb_unmap_notify_event_t *)ev);         break;
            case XCB_DESTROY_NOTIFY:
                on_destroy((xcb_destroy_notify_event_t *)ev);     break;
            case XCB_CONFIGURE_REQUEST:
                on_configure_request((xcb_configure_request_event_t *)ev); break;
            case XCB_ENTER_NOTIFY:
                on_enter((xcb_enter_notify_event_t *)ev);         break;
            }
            free(ev);
        }
        if (xcb_connection_has_error(conn)) break;
    }

    /* Limpieza */
    xcb_key_symbols_free(ksyms);
    xcb_ewmh_connection_wipe(&ewmh);
    xcb_disconnect(conn);
    fputs("min-wm: saliendo\n", stderr);
    return 0;
}
