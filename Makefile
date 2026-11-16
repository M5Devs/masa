# Makefile for Masa - cross-compilation wrapper and Darwin environment shim
#
# Copyright (C) 2026 Masa contributors
# SPDX-License-Identifier: GPL-3.0-or-later

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -fsanitize=address
CFLAGS  += -Iinclude
LDFLAGS ?=
LDLIBS  ?= -fsanitize=address

BIN     := build/masa
OBJDIR  := build
TESTBIN := build/test_masa

# Host services backend: the POSIX one everywhere except native Windows,
# where the CreateProcess-based backend is used instead.
UNAME_S := $(shell uname -s 2>/dev/null)
ifneq (,$(findstring MINGW,$(UNAME_S)))
PLATFORM_SRC := src/platform_win.c
else ifneq (,$(findstring MSYS,$(UNAME_S)))
PLATFORM_SRC := src/platform_win.c
else ifneq (,$(findstring CYGWIN,$(UNAME_S)))
PLATFORM_SRC := src/platform_win.c
else
PLATFORM_SRC := src/platform_posix.c
endif

HEADERS := include/masa.h include/masa_compat.h include/masa_pty.h

SRC     := src/main.c src/cc_wrapper.c src/swift_wrapper.c src/sys_shims.c src/tool_shims.c src/sdk.c src/xcodeproj.c src/brew.c src/exec.c src/doctor.c src/pack.c $(PLATFORM_SRC)
OBJ     := $(SRC:src/%.c=$(OBJDIR)/%.o)

TEST_SRC := tests/test_masa.c $(SRC)
TEST_CFLAGS := -DMASA_NO_MAIN

# --- Optional GUI terminal (masa-term) --------------------------------
# Kept out of SRC/OBJ so the core tool and its tests stay dependency-free
# and keep building for Android and Windows.  Build with `make gui`.
GUI_BIN      := build/masa-term
# The GUI uses the POSIX pseudo-terminal backend on POSIX hosts and the
# ConPTY backend on Windows/MinGW.  masa_pty.h (SDL-free) and pty_common.c
# are shared by both, so the backend cross-compiles without SDL.
GUI_PTY_SRC  := $(if $(filter src/platform_win.c,$(PLATFORM_SRC)),src/pty_win.c,src/pty_posix.c)
GUI_SRC      := src/gui_term.c $(GUI_PTY_SRC) src/pty_common.c
GUI_DEPS     := $(GUI_SRC) $(PLATFORM_SRC)
GUI_CFLAGS  ?= $(shell pkg-config --cflags sdl2 SDL2_ttf vterm 2>/dev/null)
GUI_LIBS    ?= $(shell pkg-config --libs sdl2 SDL2_ttf vterm 2>/dev/null) -lutil
GUI_TESTBIN  := build/test_gui

.PHONY: all test clean run android gui test-gui dist

# Android NDK cross-compile: `make android` builds an aarch64-linux-android
# binary with the NDK toolchain (override with ANDROID_CC).  The NDK does
# not ship libasan by default, so AddressSanitizer is left off here; the
# plain `make`/`make test` targets keep it on for host runs.
ANDROID_CC     ?= aarch64-linux-android24-clang
ANDROID_CFLAGS := -std=c11 -Wall -Wextra -Werror -Iinclude
ANDROID_BIN    := build/android/masa

all: $(BIN)

android: $(SRC) $(HEADERS)
	mkdir -p $(dir $(ANDROID_BIN))
	$(ANDROID_CC) $(ANDROID_CFLAGS) $(SRC) -o $(ANDROID_BIN)

$(OBJDIR)/%.o: src/%.c $(HEADERS) | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) $(OBJ) -o $@ $(LDLIBS)

$(TESTBIN): $(TEST_SRC) $(HEADERS) | $(OBJDIR)
	$(CC) $(CFLAGS) $(TEST_CFLAGS) $(LDFLAGS) $(TEST_SRC) -o $@ $(LDLIBS)

$(OBJDIR):
	mkdir -p $(OBJDIR)

test: $(TESTBIN) $(BIN)
	./$(TESTBIN)

run: $(BIN)
	./$(BIN)

# Build the GUI terminal.  Needs libsdl2-dev, libsdl2-ttf-dev and
# libvterm-dev (pkg-config resolves the flags); override GUI_CFLAGS/
# GUI_LIBS when the headers are not on the default pkg-config path.  The
# core binary is built too because masa-term runs `masa shell` on the PTY.
gui: $(GUI_BIN) $(BIN)

$(GUI_BIN): $(GUI_DEPS) $(HEADERS) include/masa_gui.h | $(OBJDIR)
	$(CC) $(CFLAGS) $(GUI_CFLAGS) $(GUI_DEPS) -o $@ $(LDFLAGS) $(GUI_LIBS)

# The GUI tests reuse the core sources for the CLI/PTY plumbing and drive
# `masa shell`, so the core binary is built too.
test-gui: $(GUI_TESTBIN) $(BIN)
	SDL_VIDEODRIVER=dummy ./$(GUI_TESTBIN)

$(GUI_TESTBIN): tests/test_gui.c $(GUI_SRC) $(SRC) $(HEADERS) include/masa_gui.h | $(OBJDIR)
	$(CC) $(CFLAGS) -DMASA_NO_MAIN $(GUI_CFLAGS) $(GUI_SRC) $(SRC) \
	    tests/test_gui.c -o $@ $(LDFLAGS) $(GUI_LIBS)

# Local reproducible packaging: build the native binary and, when the
# cross toolchains are on PATH, the Windows (x86_64) and Android (aarch64)
# ones, then package each under dist/ with LICENSE and README.md and write
# dist/SHA256SUMS.  See scripts/dist.sh.
dist:
	sh scripts/dist.sh

clean:
	rm -rf $(OBJDIR)
