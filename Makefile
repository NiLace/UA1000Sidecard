# SPDX-License-Identifier: GPL-2.0-or-later
# UA-1000 Sidecard
#
#   make                 build ua1000ctl and ua1000gui
#   make install         install under $(DESTDIR)$(PREFIX) (default /usr/local)
#   make uninstall       remove what install put there
#   make clean           remove what make built

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man
APPDIR ?= $(PREFIX)/share/applications
DATADIR ?= $(PREFIX)/share

PKG_CONFIG ?= pkg-config

WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
	   -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Wundef \
	   -Wcast-qual -Wwrite-strings -Wvla
CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 $(WARNINGS)
override CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Isrc

# Third-party headers are system headers: our warnings are not about them.
system_includes = $(patsubst -I%,-isystem %,$(1))
ALSA_CFLAGS = $(call system_includes,$(shell $(PKG_CONFIG) --cflags alsa))
ALSA_LIBS = $(shell $(PKG_CONFIG) --libs alsa)
GTK_CFLAGS = $(call system_includes,$(shell $(PKG_CONFIG) --cflags gtk4))
GTK_LIBS = $(shell $(PKG_CONFIG) --libs gtk4)

CORE_SRC = src/ua1000_proto.c src/ua1000_state.c
CTL_SRC = $(CORE_SRC) src/ua1000_midi.c src/ua1000_statefile.c src/ua1000ctl.c
GUI_LIB_SRC = $(CORE_SRC) src/ua1000_midi.c src/ua1000_statefile.c \
	      $(filter-out src/gui/ua1000gui.c,$(wildcard src/gui/*.c))
GUI_SRC = $(GUI_LIB_SRC) src/gui/ua1000gui.c

# Where the desk panel's images are installed (src/gui/desk.c).
GUI_DEFS = -DSIDECARD_DATADIR='"$(DATADIR)/sidecard"'

# The desk's images, at 1x and 2x, and the application icon's sizes.
DESK_FILES = plate.jpg lit.jpg notebook.jpg cap_in.png cap_out.png \
	     cap_mon.png cap_in.json cap_out.json cap_mon.json desk.layout \
	     notebook.layout
DESK_SCALES = 1x 2x
ICON_SIZES = 16 32 48 64 128 256
ICON_NAME = org.nylarea.ua1000gui

# The icon theme's cache, refreshed on a real install only: into a DESTDIR (a
# package being built) the target system refreshes it itself.
update_icon_cache = -[ -n "$(DESTDIR)" ] || \
	gtk-update-icon-cache -q -t $(DATADIR)/icons/hicolor 2>/dev/null

.PHONY: all install uninstall clean

all: ua1000ctl ua1000gui

# A missing development package is reported by name, not by the compiler.
need-%:
	@$(PKG_CONFIG) --exists $* || { echo "$* development files not found" \
		"(pkg-config $*): install them first" >&2; exit 1; }

ua1000ctl: $(CTL_SRC) $(wildcard src/*.h) | need-alsa
	$(CC) $(CPPFLAGS) $(ALSA_CFLAGS) $(CFLAGS) -o $@ $(CTL_SRC) \
		$(LDFLAGS) $(ALSA_LIBS) -lm

ua1000gui: $(GUI_SRC) $(wildcard src/*.h src/gui/*.h) | need-alsa need-gtk4
	$(CC) $(CPPFLAGS) $(GUI_DEFS) $(ALSA_CFLAGS) $(GTK_CFLAGS) $(CFLAGS) \
		-o $@ $(GUI_SRC) $(LDFLAGS) $(GTK_LIBS) $(ALSA_LIBS) -lm -ldl

install: all
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)/man1 \
		$(DESTDIR)$(APPDIR)
	install -m 755 ua1000ctl ua1000gui $(DESTDIR)$(BINDIR)
	install -m 644 man/ua1000ctl.1 man/ua1000gui.1 $(DESTDIR)$(MANDIR)/man1
	install -m 644 data/$(ICON_NAME).desktop $(DESTDIR)$(APPDIR)
	for n in $(ICON_SIZES); do \
		install -d $(DESTDIR)$(DATADIR)/icons/hicolor/$${n}x$${n}/apps && \
		install -m 644 data/icons/$${n}x$${n}/$(ICON_NAME).png \
			$(DESTDIR)$(DATADIR)/icons/hicolor/$${n}x$${n}/apps || exit 1; done
	$(update_icon_cache)
	for s in $(DESK_SCALES); do \
		install -d $(DESTDIR)$(DATADIR)/sidecard/desk/$$s && \
		install -m 644 $(addprefix data/desk/$$s/,$(DESK_FILES)) \
			$(DESTDIR)$(DATADIR)/sidecard/desk/$$s || exit 1; done

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/ua1000ctl $(DESTDIR)$(BINDIR)/ua1000gui \
		$(DESTDIR)$(MANDIR)/man1/ua1000ctl.1 \
		$(DESTDIR)$(MANDIR)/man1/ua1000gui.1 \
		$(DESTDIR)$(APPDIR)/$(ICON_NAME).desktop
	rm -rf $(DESTDIR)$(DATADIR)/sidecard
	for n in $(ICON_SIZES); do \
		rm -f $(DESTDIR)$(DATADIR)/icons/hicolor/$${n}x$${n}/apps/$(ICON_NAME).png; done
	$(update_icon_cache)

clean:
	rm -f ua1000ctl ua1000gui
