# Afterburner Player — GTK4/libadwaita ProRes player + test tool.
#
#   make                 build ./abplayer-gtk
#   make check           parser tests + fuzz (needs ffmpeg for test clips)
#   make install         install to $(PREFIX) (default ~/.local) incl. desktop entry
#   make uninstall
#
# Arch:  sudo pacman -S --needed base-devel gtk4 libadwaita libepoxy

APP_ID  := io.github.boulder47.AfterburnerPlayer
PREFIX  ?= $(HOME)/.local
BINDIR  ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share

PKGS    := gtk4 libadwaita-1 epoxy
CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -pthread
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS   := $(shell pkg-config --libs $(PKGS))

SRCS := src/main.c src/view.c src/decoder.c src/card.c src/mov.c
OBJS := $(SRCS:.c=.o)
HDRS := $(wildcard src/*.h)

all: abplayer-gtk

abplayer-gtk: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(PKG_LIBS) -lm

src/%.o: src/%.c $(HDRS)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c -o $@ $<

# parser tests run sanitized, without any GTK dependency
tests/test_mov: tests/test_mov.c src/mov.c src/mov.h
	$(CC) -std=gnu11 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
		-fno-sanitize-recover=undefined -o $@ tests/test_mov.c src/mov.c -lm

check: tests/test_mov
	./tests/run-tests.sh

install: abplayer-gtk
	install -Dm755 abplayer-gtk $(DESTDIR)$(BINDIR)/abplayer-gtk
	install -Dm644 data/$(APP_ID).svg $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/$(APP_ID).svg
	sed 's|^Exec=abplayer-gtk|Exec=$(BINDIR)/abplayer-gtk|' data/$(APP_ID).desktop > $(APP_ID).desktop.tmp
	install -Dm644 $(APP_ID).desktop.tmp $(DESTDIR)$(DATADIR)/applications/$(APP_ID).desktop
	rm -f $(APP_ID).desktop.tmp
	@if command -v gtk4-update-icon-cache >/dev/null; then \
		gtk4-update-icon-cache -qtf $(DESTDIR)$(DATADIR)/icons/hicolor || true; fi
	@if command -v update-desktop-database >/dev/null; then \
		update-desktop-database -q $(DESTDIR)$(DATADIR)/applications || true; fi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/abplayer-gtk \
	      $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/$(APP_ID).svg \
	      $(DESTDIR)$(DATADIR)/applications/$(APP_ID).desktop

clean:
	rm -f abplayer-gtk $(OBJS) tests/test_mov
	rm -rf tests/out

.PHONY: all check install uninstall clean
