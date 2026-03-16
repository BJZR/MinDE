/* min-polkit.c — Agente de autenticación Polkit para minDE
 *
 * Se registra con polkitd como agente de autenticación gráfico.
 * Cuando una app necesita privilegios muestra un diálogo
 * de contraseña en el mismo estilo que min-launch.
 *
 * Dependencias: libpolkit-agent-1, libpolkit-gobject-1, glib-2.0, x11, xft
 *
 * Compilar:
 *   gcc -O2 -o min-polkit min-polkit.c \
 *       $(pkg-config --cflags --libs polkit-agent-1 glib-2.0 x11 xft fontconfig)
 *
 * Instalar:
 *   Añadir a min-run antes del exec:
 *     min-polkit &
 *
 * Paquetes Void Linux:
 *   sudo xbps-install polkit-devel glib-devel
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Polkit + GLib */
#include <polkit/polkit.h>
#include <polkitagent/polkitagent.h>
#include <glib.h>
#include <glib-object.h>

/* X11 + Xft */
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>

#include "config.h"

/* ═══ DIÁLOGO X11 — mismo estilo que min-launch ═══════════ */

/* Muestra un diálogo modal de contraseña.
 * Devuelve la contraseña en `out` (máx max_len bytes) o "" si cancelado.
 * Retorna 1 = OK, 0 = cancelado. */
static int password_dialog(const char *message, const char *user,
                            char *out, int max_len) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 0;
    int scr  = DefaultScreen(dpy);
    Window root = DefaultRootWindow(dpy);

    Theme theme; theme_init(&theme);

    XftFont *font = XftFontOpenName(dpy, scr, FONT_NAME);
    if (!font) { XCloseDisplay(dpy); return 0; }

    int row_h  = font->ascent + font->descent + LAUNCHER_PAD;
    int input_h = row_h + 2 * LAUNCHER_PAD;
    int W = 420;
    int H = input_h * 3 + 1 + row_h;  /* mensaje + usuario + contraseña */

    int sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr);

    XSetWindowAttributes attr = {0};
    attr.background_pixel  = (unsigned long)theme.pix_bg;
    attr.override_redirect = True;
    attr.event_mask = KeyPressMask | ExposureMask;
    attr.border_pixel = (unsigned long)theme.pix_bact;

    Window win = XCreateWindow(dpy, root,
        (sw - W)/2, (sh - H)/3, (unsigned)W, (unsigned)H,
        BORDER_WIDTH, DefaultDepth(dpy, scr), InputOutput,
        DefaultVisual(dpy, scr),
        CWBackPixel | CWOverrideRedirect | CWEventMask | CWBorderPixel,
        &attr);

    XftDraw *xd = XftDrawCreate(dpy, win,
        DefaultVisual(dpy, scr), DefaultColormap(dpy, scr));

    XftColor c_bg, c_fg, c_accent, c_dim;
    #define AC(c,h) if(!XftColorAllocName(dpy,DefaultVisual(dpy,scr),\
        DefaultColormap(dpy,scr),h,&c)) \
        XftColorAllocName(dpy,DefaultVisual(dpy,scr),\
        DefaultColormap(dpy,scr),"#888",&c)
    AC(c_bg,    theme.bg);
    AC(c_fg,    theme.fg);
    AC(c_accent,theme.accent);
    AC(c_dim,   theme.dim);
    #undef AC

    XMapRaised(dpy, win);
    XGrabKeyboard(dpy, win, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    XSetInputFocus(dpy, win, RevertToPointerRoot, CurrentTime);

    char pw[256] = ""; int pw_len = 0;
    int result = 0;

    /* Función de dibujo interna */
    #define TW(s) ({ XGlyphInfo _e; \
        XftTextExtentsUtf8(dpy,font,(const FcChar8*)(s),strlen(s),&_e); \
        _e.xOff; })
    #define DT(x,y,s,c) XftDrawStringUtf8(xd,c,font,x,y,(const FcChar8*)(s),strlen(s))

    for (;;) {
        GC gc = DefaultGC(dpy, scr);

        /* Fondo */
        XSetForeground(dpy, gc, (unsigned long)theme.pix_bg);
        XFillRectangle(dpy, win, gc, 0, 0, (unsigned)W, (unsigned)H);

        /* Fila 1: mensaje (fondo acento) */
        XSetForeground(dpy, gc, (unsigned long)theme.pix_bact);
        XFillRectangle(dpy, win, gc, 0, 0, (unsigned)W, (unsigned)input_h);
        char msg_short[256]; strncpy(msg_short, message, 255);
        while (TW(msg_short) > W - LAUNCHER_PAD*2 && strlen(msg_short) > 3) {
            int l = strlen(msg_short);
            msg_short[l-4]='.'; msg_short[l-3]='.';
            msg_short[l-2]='.'; msg_short[l-1]='\0';
        }
        DT(LAUNCHER_PAD, LAUNCHER_PAD + font->ascent, msg_short, &c_bg);

        XSetForeground(dpy, gc, 0x3a3f5c);
        XFillRectangle(dpy, win, gc, 0, input_h, (unsigned)W, 1);

        /* Fila 2: usuario */
        int y2 = input_h + 1;
        int base2 = y2 + LAUNCHER_PAD + font->ascent;
        char ustr[128]; snprintf(ustr, sizeof ustr, "Usuario: %s", user ? user : "root");
        DT(LAUNCHER_PAD, base2, ustr, &c_dim);

        XSetForeground(dpy, gc, 0x3a3f5c);
        XFillRectangle(dpy, win, gc, 0, y2 + input_h, (unsigned)W, 1);

        /* Fila 3: contraseña */
        int y3 = y2 + input_h + 1;
        XSetForeground(dpy, gc, (unsigned long)theme.pix_bact);
        XFillRectangle(dpy, win, gc, 0, y3, (unsigned)W, (unsigned)input_h);
        int base3 = y3 + LAUNCHER_PAD + font->ascent;
        DT(LAUNCHER_PAD, base3, "> ", &c_accent);
        char stars[128] = "";
        for (int i = 0; i < pw_len && i < 60; i++) strcat(stars, "*");
        DT(LAUNCHER_PAD + TW("> "), base3, stars, &c_bg);
        int cx = LAUNCHER_PAD + TW("> ") + TW(stars);
        DT(cx, base3, "_", &c_bg);

        XFlush(dpy);

        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type != KeyPress) continue;

        char buf[32] = ""; KeySym sym;
        XLookupString(&ev.xkey, buf, sizeof buf - 1, &sym, NULL);

        if (sym == XK_Escape) { result = 0; break; }
        if (sym == XK_Return || sym == XK_KP_Enter) {
            strncpy(out, pw, max_len - 1);
            out[max_len - 1] = '\0';
            result = 1; break;
        }
        if (sym == XK_BackSpace) {
            if (pw_len > 0) {
                do { pw_len--; }
                while (pw_len > 0 && (pw[pw_len] & 0xC0) == 0x80);
                pw[pw_len] = '\0';
            }
        } else if (buf[0] >= 0x20 && pw_len < 253) {
            int bl = strlen(buf);
            memcpy(pw + pw_len, buf, (size_t)bl);
            pw_len += bl; pw[pw_len] = '\0';
        }
    }
    #undef TW
    #undef DT

    XUngrabKeyboard(dpy, CurrentTime);
    XftFontClose(dpy, font);
    XftDrawDestroy(xd);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return result;
}

/* ═══ AGENTE POLKIT ══════════════════════════════════════════ */

/* Subclase de PolkitAgentListener */
#define MINDE_TYPE_AGENT (minde_agent_get_type())
G_DECLARE_FINAL_TYPE(MindeAgent, minde_agent, MINDE, AGENT, PolkitAgentListener)

struct _MindeAgent { PolkitAgentListener parent; };
G_DEFINE_TYPE(MindeAgent, minde_agent, POLKIT_AGENT_TYPE_LISTENER)

static void minde_agent_init(MindeAgent *self) { (void)self; }

/* Se llama cuando polkitd pide autenticación */
static void minde_agent_initiate_authentication(
    PolkitAgentListener    *listener,
    const gchar            *action_id,
    const gchar            *message,
    const gchar            *icon_name,
    PolkitDetails          *details,
    const gchar            *cookie,
    GList                  *identities,
    GCancellable           *cancellable,
    GAsyncReadyCallback     callback,
    gpointer                user_data)
{
    (void)listener; (void)icon_name; (void)details; (void)cancellable;

    fprintf(stderr, "[min-polkit] auth requerida: %s\n", action_id);

    /* Determinar usuario a autenticar (primer identity de la lista) */
    const char *user_name = "root";
    if (identities) {
        PolkitIdentity *id = POLKIT_IDENTITY(identities->data);
        if (POLKIT_IS_UNIX_USER(id)) {
            PolkitUnixUser *uu = POLKIT_UNIX_USER(id);
            user_name = polkit_unix_user_get_name(uu);
        }
    }

    /* Mostrar diálogo de contraseña */
    char pw[256] = "";
    int ok = password_dialog(message, user_name, pw, sizeof pw);

    GSimpleAsyncResult *res = g_simple_async_result_new(
        G_OBJECT(listener), callback, user_data,
        (gpointer)minde_agent_initiate_authentication);

    if (!ok) {
        /* Cancelado */
        g_simple_async_result_set_error(res, POLKIT_ERROR,
            POLKIT_ERROR_CANCELLED, "Cancelado por el usuario");
        g_simple_async_result_complete(res);
        g_object_unref(res);
        return;
    }

    /* Autenticar con polkit */
    GError *err = NULL;
    PolkitAgentSession *session = NULL;

    if (identities) {
        session = polkit_agent_session_new(
            POLKIT_IDENTITY(identities->data), cookie);
    }

    if (session) {
        polkit_agent_session_response(session, pw);
        GMainContext *ctx = g_main_context_default();
        /* Iterar el bucle brevemente para que polkit procese */
        for (int i = 0; i < 10; i++) {
            g_main_context_iteration(ctx, FALSE);
            g_usleep(50000);
        }
        g_object_unref(session);
    }

    /* Limpiar contraseña de memoria */
    memset(pw, 0, sizeof pw);

    g_simple_async_result_complete(res);
    g_object_unref(res);
    (void)err;
}

static gboolean minde_agent_initiate_authentication_finish(
    PolkitAgentListener *listener,
    GAsyncResult        *res,
    GError             **error)
{
    (void)listener;
    return !g_simple_async_result_propagate_error(
        G_SIMPLE_ASYNC_RESULT(res), error);
}

static void minde_agent_class_init(MindeAgentClass *klass) {
    PolkitAgentListenerClass *lc = POLKIT_AGENT_LISTENER_CLASS(klass);
    lc->initiate_authentication        = minde_agent_initiate_authentication;
    lc->initiate_authentication_finish = minde_agent_initiate_authentication_finish;
}

/* ═══ MAIN ═══════════════════════════════════════════════════ */

int main(void) {
    /* Inicializar GLib/GObject */
    g_type_init();

    /* Crear agente */
    MindeAgent *agent = g_object_new(MINDE_TYPE_AGENT, NULL);

    /* Registrar con polkitd para esta sesión */
    GError *err = NULL;
    PolkitSubject *subject = polkit_unix_session_new_for_process_sync(
        getpid(), NULL, &err);
    if (!subject) {
        fprintf(stderr, "[min-polkit] no se pudo obtener sesión: %s\n",
                err ? err->message : "desconocido");
        /* Fallback: registrar para el proceso actual */
        subject = polkit_unix_process_new_for_owner(getpid(), 0, getuid());
    }

    gpointer handle = polkit_agent_listener_register(
        POLKIT_AGENT_LISTENER(agent),
        POLKIT_AGENT_REGISTER_FLAGS_NONE,
        subject, NULL, NULL, &err);

    if (!handle) {
        fprintf(stderr, "[min-polkit] no se pudo registrar: %s\n",
                err ? err->message : "desconocido");
        /* No terminar — puede que otro agente ya esté corriendo */
    } else {
        fprintf(stderr, "[min-polkit] agente registrado\n");
    }

    g_object_unref(subject);

    /* Bucle principal GLib */
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    if (handle) polkit_agent_listener_unregister(handle);
    g_main_loop_unref(loop);
    g_object_unref(agent);
    return 0;
}
