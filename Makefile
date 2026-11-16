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

HEADERS := include/masa.h include/masa_compat.h

SRC     := src/main.c src/cc_wrapper.c src/sys_shims.c src/tool_shims.c src/sdk.c src/xcodeproj.c src/brew.c src/exec.c $(PLATFORM_SRC)
OBJ     := $(SRC:src/%.c=$(OBJDIR)/%.o)

TEST_SRC := tests/test_masa.c $(SRC)
TEST_CFLAGS := -DMASA_NO_MAIN

.PHONY: all test clean run

all: $(BIN)

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

clean:
	rm -rf $(OBJDIR)
