# SPDX-License-Identifier: GPL-2.0-or-later
# glassine-home: the Home screen and its widget runtime.
# Needs a C11 compiler, Lua 5.4 (lua5.4-dev), pkg-config and the Linux UAPI headers for the
# runtime; cairo, pango, librsvg, glib, gdk-pixbuf and GlassineOS/ui for the drawing; and
# wayland-client, wayland-protocols and xkbcommon for the program itself.
#
# GlassineOS/ui: test/ui.sh finds a checkout ($GLASSINE_UI, ../ui, else a clone); it is
# built and installed into out/ui-stage. UI_SYSTEM=1 uses an installed glassine-ui instead.

CC ?= cc
PKG_CONFIG ?= pkg-config
LUA_PC ?= lua5.4
LUA_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(LUA_PC))
LUA_LIBS := $(shell $(PKG_CONFIG) --libs $(LUA_PC))
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration
LDLIBS += -lm
PREFIX ?= /usr
DESTDIR ?=

O := out
CORE := value toml pack manifest desc broker sched conf catalog places
CORE_OBJ := $(CORE:%=$(O)/%.o)
# the sandbox; tests link a build of the child with test-only payloads
SANDBOX := zygote seccomp run
SANDBOX_OBJ := $(SANDBOX:%=$(O)/%.o) $(O)/child.o
SANDBOX_TEST_OBJ := $(SANDBOX:%=$(O)/%.o) $(O)/child-hooks.o
TESTS := value pack manifest desc sched conf catalog places sandbox

# the screen: model, scene, images (drawn with GlassineOS/ui), and the frames wl.c commits
SCREEN := apps core scene images frames
SCREEN_OBJ := $(SCREEN:%=$(O)/%.o)

ifeq ($(UI_SYSTEM),1)
UI_STAGE :=
UI_DEP :=
UI_ENV :=
UI_RPATH :=
else
UI_DIR = $(shell sh test/ui.sh)
UI_STAGE := $(CURDIR)/$(O)/ui-stage
UI_DEP := $(UI_STAGE)/lib/pkgconfig/glassine-ui.pc
UI_ENV := PKG_CONFIG_PATH=$(UI_STAGE)/lib/pkgconfig
UI_RPATH := -Wl,-rpath,$(UI_STAGE)/lib
endif
SCREEN_PKGS := glassine-ui pangocairo gdk-pixbuf-2.0 glib-2.0
SCREEN_CFLAGS = $(shell $(UI_ENV) $(PKG_CONFIG) --cflags $(SCREEN_PKGS))
SCREEN_LIBS = $(shell $(UI_ENV) $(PKG_CONFIG) --libs $(SCREEN_PKGS)) -lpthread

WL_PROTO := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols 2>/dev/null)
WL_PKGS := wayland-client xkbcommon
ifneq ($(WL_PROTO),)
HOME_BIN := $(O)/glassine-home
endif

all: $(O)/glassine-home-run $(HOME_BIN)

$(O)/%.o: src/%.c src/*.h | $(O)
	$(CC) $(CFLAGS) $(LUA_CFLAGS) -c -o $@ $<

$(SCREEN_OBJ): $(O)/%.o: src/%.c src/*.h $(UI_DEP) | $(O)
	$(CC) $(CFLAGS) $(LUA_CFLAGS) $(SCREEN_CFLAGS) -c -o $@ $<

$(O)/child-hooks.o: src/child.c src/*.h | $(O)
	$(CC) $(CFLAGS) $(LUA_CFLAGS) -DGH_TEST_HOOKS -c -o $@ $<

$(O)/glassine-home-run: src/cli/run.c $(CORE_OBJ) $(SANDBOX_OBJ) | $(O)
	$(CC) $(CFLAGS) -o $@ $< $(CORE_OBJ) $(SANDBOX_OBJ) $(LUA_LIBS) $(LDLIBS)

# GlassineOS/ui, staged
$(CURDIR)/$(O)/ui-stage/lib/pkgconfig/glassine-ui.pc:
	$(MAKE) -s -C "$(UI_DIR)" install O=$(CURDIR)/$(O)/ui-build PREFIX=$(CURDIR)/$(O)/ui-stage >/dev/null

# the program: xdg-shell from wayland-protocols
$(O)/proto/xdg-shell-client-protocol.h $(O)/proto/xdg-shell-protocol.c: | $(O)
	mkdir -p $(O)/proto
	wayland-scanner client-header $(WL_PROTO)/stable/xdg-shell/xdg-shell.xml $(O)/proto/xdg-shell-client-protocol.h
	wayland-scanner private-code $(WL_PROTO)/stable/xdg-shell/xdg-shell.xml $(O)/proto/xdg-shell-protocol.c

$(O)/wl.o: src/wl.c src/*.h $(O)/proto/xdg-shell-client-protocol.h $(UI_DEP) | $(O)
	$(CC) $(CFLAGS) $(LUA_CFLAGS) $(SCREEN_CFLAGS) -I$(O)/proto $(shell $(PKG_CONFIG) --cflags $(WL_PKGS)) -c -o $@ $<

$(O)/glassine-home: src/cli/home.c $(O)/wl.o $(O)/proto/xdg-shell-protocol.c $(SCREEN_OBJ) $(CORE_OBJ) $(SANDBOX_OBJ) | $(O)
	$(CC) $(CFLAGS) -I$(O)/proto -o $@ src/cli/home.c $(O)/wl.o $(O)/proto/xdg-shell-protocol.c $(SCREEN_OBJ) \
	    $(CORE_OBJ) $(SANDBOX_OBJ) $(SCREEN_LIBS) $(shell $(PKG_CONFIG) --libs $(WL_PKGS)) $(LUA_LIBS) $(LDLIBS) $(UI_RPATH)

$(O)/test_home: test/test_home.c $(SCREEN_OBJ) $(CORE_OBJ) $(SANDBOX_OBJ) test/t.h | $(O)
	$(CC) $(CFLAGS) $(LUA_CFLAGS) $(SCREEN_CFLAGS) -o $@ $< $(SCREEN_OBJ) $(CORE_OBJ) $(SANDBOX_OBJ) \
	    $(SCREEN_LIBS) $(LUA_LIBS) $(LDLIBS) $(UI_RPATH)

$(O)/test_%: test/test_%.c $(CORE_OBJ) $(SANDBOX_TEST_OBJ) test/t.h | $(O)
	$(CC) $(CFLAGS) $(LUA_CFLAGS) -o $@ $< $(CORE_OBJ) $(SANDBOX_TEST_OBJ) $(LUA_LIBS) $(LDLIBS)

$(O):
	mkdir -p $@

# test_home renders every Home board into out/renders (see test/test_home.c)
check: $(TESTS:%=$(O)/test_%) $(O)/test_home all
	@set -e; for t in $(TESTS); do ./$(O)/test_$$t; done; sh test/reading.sh; sh test/resize.sh
	@FONTCONFIG_FILE=$$(sh test/fonts.sh) ./$(O)/test_home

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/glassine-home/widgets \
	    $(DESTDIR)$(PREFIX)/share/glassine-home/native
	install -m 755 $(O)/glassine-home-run $(DESTDIR)$(PREFIX)/bin/
	$(if $(HOME_BIN),install -m 755 $(HOME_BIN) $(DESTDIR)$(PREFIX)/bin/)
	cp -R widgets/. $(DESTDIR)$(PREFIX)/share/glassine-home/widgets/
	cp -R native/. $(DESTDIR)$(PREFIX)/share/glassine-home/native/

clean:
	rm -rf $(O)

.PHONY: all check install clean
