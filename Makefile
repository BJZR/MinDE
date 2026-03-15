# Makefile — minDE (minwm + minibar + minrun)
#
# Dependencias (Debian/Ubuntu/Arch):
#   Debian/Ubuntu:
#     sudo apt install libxcb1-dev libxcb-keysyms1-dev \
#                      libxcb-ewmh-dev libxcb-icccm4-dev \
#                      libx11-dev libxft-dev libfontconfig1-dev
#   Arch:
#     sudo pacman -S libxcb xcb-util-keysyms xcb-util-wm \
#                    libx11 libxft fontconfig
#
# Uso:
#   make          → compilar todo
#   make install  → copiar binarios a ~/.local/bin/
#   make clean    → limpiar

CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c99 -pedantic
LDFLAGS =

# Flags por componente
WM_CFLAGS  = $(shell pkg-config --cflags xcb xcb-keysyms xcb-ewmh xcb-icccm)
WM_LIBS    = $(shell pkg-config --libs   xcb xcb-keysyms xcb-ewmh xcb-icccm)

BAR_CFLAGS = $(shell pkg-config --cflags x11 xft fontconfig)
BAR_LIBS   = $(shell pkg-config --libs   x11 xft fontconfig)

RUN_CFLAGS = $(shell pkg-config --cflags x11 xft fontconfig)
RUN_LIBS   = $(shell pkg-config --libs   x11 xft fontconfig)

BINS = min-wm min-bar min-launch min-theme

# ── Reglas ───────────────────────────────────────────────────

.PHONY: all install uninstall clean

all: $(BINS)

min-wm: min-wm.c config.h
	$(CC) $(CFLAGS) $(WM_CFLAGS) -o $@ $< $(WM_LIBS)

min-bar: min-bar.c config.h
	$(CC) $(CFLAGS) $(BAR_CFLAGS) -o $@ $< $(BAR_LIBS)

min-launch: min-launch.c config.h
	$(CC) $(CFLAGS) $(RUN_CFLAGS) -o $@ $< $(RUN_LIBS)

min-theme: min-theme.c
	$(CC) $(CFLAGS) $(BAR_CFLAGS) -o $@ $< $(BAR_LIBS)

# ── Instalación ──────────────────────────────────────────────

DESTDIR = $(HOME)/.local/bin

install: all
	@mkdir -p $(DESTDIR)
	cp -v $(BINS) $(DESTDIR)/
	@echo ""
	@echo "Instalado en $(DESTDIR)"
	@echo "Asegúrate de que $(DESTDIR) esté en tu PATH."
	@echo ""
	@echo "Para arrancar con startx, añade a ~/.xinitrc:"
	@echo "  exec minwm"

uninstall:
	$(foreach b,$(BINS),rm -fv $(DESTDIR)/$(b);)

clean:
	rm -f $(BINS)

# ── Ayuda ─────────────────────────────────────────────────────

help:
	@echo "minDE — Entorno de escritorio mínimo en C"
	@echo ""
	@echo "  make          Compilar min-wm, min-bar, min-launch, min-theme"
	@echo "  make install  Instalar en ~/.local/bin/"
	@echo "  make clean    Eliminar binarios compilados"
	@echo ""
	@echo "Atajos de teclado (Super = tecla Windows):"
	@echo "  Super + 1..5       Cambiar workspace"
	@echo "  Super + ←/→        Workspace anterior/siguiente"
	@echo "  Super + Enter      Terminal (configurable en config.h)"
	@echo "  Super + D          Launcher (minrun)"
	@echo "  Super + Q          Cerrar ventana activa"
	@echo "  Super + Shift + Q  Salir del WM"
