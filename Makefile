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

SRC     := src/main.c src/cc_wrapper.c src/swift_wrapper.c src/sys_shims.c src/tool_shims.c src/sdk.c src/xcodeproj.c src/brew.c src/exec.c src/doctor.c src/pack.c src/danroot.c src/danroot_cf.c src/danroot_objc.c src/danroot_appkit.c $(PLATFORM_SRC)
OBJ     := $(SRC:src/%.c=$(OBJDIR)/%.o)

TEST_SRC := tests/test_masa.c $(SRC)
TEST_CFLAGS := -DMASA_NO_MAIN

# --- Optional GUI builds ----------------------------------------------
# Kept out of SRC/OBJ so the core tool and its tests stay dependency-free
# and keep building for Android and Windows.  Build with `make gui`.
#
#   build/masa-term  the SDL2 terminal emulator (needs SDL2_ttf + libvterm)
#   build/masa-gui   `masa` with the Danroot AppKit/CoreGraphics SDL2
#                    backend compiled in, so `masa run` can open a window
#                    for a Darwin GUI program (needs SDL2 only)
GUI_TERM_BIN := build/masa-term
GUI_BIN      := build/masa-gui
# The GUI terminal uses the POSIX pseudo-terminal backend on POSIX hosts and
# the ConPTY backend on Windows/MinGW.  masa_pty.h (SDL-free) and
# pty_common.c are shared by both, so the backend cross-compiles without SDL.
GUI_PTY_SRC  := $(if $(filter src/platform_win.c,$(PLATFORM_SRC)),src/pty_win.c,src/pty_posix.c)
GUI_SRC      := src/gui_term.c $(GUI_PTY_SRC) src/pty_common.c
GUI_DEPS     := $(GUI_SRC) $(PLATFORM_SRC)
GUI_CFLAGS  ?= $(shell pkg-config --cflags sdl2 SDL2_ttf vterm 2>/dev/null)
GUI_LIBS    ?= $(shell pkg-config --libs sdl2 SDL2_ttf vterm 2>/dev/null) -lutil
# The Danroot AppKit backend needs only SDL2 (no TTF/vterm).
GUI_APTK_CFLAGS ?= $(shell pkg-config --cflags sdl2 2>/dev/null)
GUI_APTK_LIBS   ?= $(shell pkg-config --libs sdl2 2>/dev/null)
GUI_TESTBIN  := build/test_gui

.PHONY: all test clean run android aarch64 gui test-gui dist

# Android NDK cross-compile: `make android` builds an aarch64-linux-android
# binary with the NDK toolchain (override with ANDROID_CC).  The NDK does
# not ship libasan by default, so AddressSanitizer is left off here; the
# plain `make`/`make test` targets keep it on for host runs.
ANDROID_CC     ?= aarch64-linux-android24-clang
ANDROID_CFLAGS := -std=c11 -Wall -Wextra -Werror -Iinclude
ANDROID_BIN    := build/android/masa

# Linux ARM64 build: `make aarch64` produces build/masa-aarch64 with the
# aarch64-linux-gnu toolchain (override with AARCH64_CC).  The cross
# toolchain ships no libasan, so AddressSanitizer is off; this binary is the
# ARM64 side of the Danroot cross-architecture bridge, run under qemu-aarch64
# to execute arm64-apple-darwin images on an x86_64 host.
AARCH64_CC     ?= aarch64-linux-gnu-gcc
AARCH64_CFLAGS := -std=c11 -Wall -Wextra -Werror -Iinclude
AARCH64_BIN    := build/masa-aarch64

all: $(BIN)

android: $(SRC) $(HEADERS)
	mkdir -p $(dir $(ANDROID_BIN))
	$(ANDROID_CC) $(ANDROID_CFLAGS) $(SRC) -o $(ANDROID_BIN)

aarch64: $(SRC) $(HEADERS)
	mkdir -p $(dir $(AARCH64_BIN))
	$(AARCH64_CC) $(AARCH64_CFLAGS) $(SRC) -o $(AARCH64_BIN)

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

# Build both GUI binaries.  masa-term needs libsdl2-dev, libsdl2-ttf-dev and
# libvterm-dev; masa-gui needs only libsdl2-dev.  pkg-config resolves the
# flags (override GUI_CFLAGS/GUI_LIBS/GUI_APTK_* when the headers are not on
# the default pkg-config path).  The core binary is built too because
# masa-term runs `masa shell` on the PTY.
gui: $(GUI_TERM_BIN) $(GUI_BIN) $(BIN)

$(GUI_TERM_BIN): $(GUI_DEPS) $(HEADERS) include/masa_gui.h | $(OBJDIR)
	$(CC) $(CFLAGS) $(GUI_CFLAGS) $(GUI_DEPS) -o $@ $(LDFLAGS) $(GUI_LIBS)

# masa-gui is `masa` itself with the Danroot SDL2 AppKit/CoreGraphics backend
# compiled in; every core source is built with -DMASA_DANROOT_SDL2.
$(GUI_BIN): $(SRC) $(HEADERS) | $(OBJDIR)
	$(CC) $(CFLAGS) -DMASA_DANROOT_SDL2 $(GUI_APTK_CFLAGS) $(SRC) \
	    -o $@ $(LDFLAGS) $(GUI_APTK_LIBS) $(LDLIBS)

# The GUI tests reuse the core sources for the CLI/PTY plumbing and drive
# `masa shell`, so the core binary is built too.  The Danroot AppKit test
# needs the SDL2 backend compiled in, hence -DMASA_DANROOT_SDL2 here.
# LSAN_OPTIONS suppresses the one third-party leak SDL2/libdbus leave
# behind (see scripts/lsan.supp); Masa's own allocations are still checked.
LSAN_SUPP ?= $(CURDIR)/scripts/lsan.supp

test-gui: $(GUI_TESTBIN) $(BIN)
	SDL_VIDEODRIVER=dummy LSAN_OPTIONS=suppressions=$(LSAN_SUPP) ./$(GUI_TESTBIN)

$(GUI_TESTBIN): tests/test_gui.c $(GUI_SRC) $(SRC) $(HEADERS) include/masa_gui.h | $(OBJDIR)
	$(CC) $(CFLAGS) -DMASA_NO_MAIN -DMASA_DANROOT_SDL2 $(GUI_CFLAGS) \
	    $(GUI_SRC) $(SRC) tests/test_gui.c -o $@ $(LDFLAGS) $(GUI_LIBS)

# Local reproducible packaging: build the native binary and, when the
# cross toolchains are on PATH, the Windows (x86_64) and Android (aarch64)
# ones, then package each under dist/ with LICENSE and README.md and write
# dist/SHA256SUMS.  See scripts/dist.sh.
dist:
	sh scripts/dist.sh

clean:
	rm -rf $(OBJDIR)
