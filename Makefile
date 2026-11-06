# SPDX-License-Identifier: GPL-2.0-or-later
# glassine-home: the widget runtime and page packing (the drawing comes later).
# Needs a C11 compiler, Lua 5.4 (lua5.4-dev), pkg-config and the Linux UAPI headers.

CC ?= cc
PKG_CONFIG ?= pkg-config
LUA_PC ?= lua5.4
LUA_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(LUA_PC))
LUA_LIBS := $(shell $(PKG_CONFIG) --libs $(LUA_PC))
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration
LDLIBS += -lm

O := out
CORE := value toml
CORE_OBJ := $(CORE:%=$(O)/%.o)
TESTS := value

all: $(O)/glassine-home-run

$(O)/%.o: src/%.c src/*.h | $(O)
	$(CC) $(CFLAGS) $(LUA_CFLAGS) -c -o $@ $<

$(O)/test_%: test/test_%.c $(CORE_OBJ) test/t.h | $(O)
	$(CC) $(CFLAGS) $(LUA_CFLAGS) -o $@ $< $(CORE_OBJ) $(LUA_LIBS) $(LDLIBS)

$(O):
	mkdir -p $@

check: $(TESTS:%=$(O)/test_%)
	@set -e; for t in $(TESTS); do ./$(O)/test_$$t; done

clean:
	rm -rf $(O)

.PHONY: all check clean
