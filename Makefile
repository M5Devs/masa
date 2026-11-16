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

SRC     := src/main.c src/cc_wrapper.c src/sys_shims.c src/exec.c src/platform_linux.c
OBJ     := $(SRC:src/%.c=$(OBJDIR)/%.o)

TEST_SRC := tests/test_masa.c src/main.c src/cc_wrapper.c src/sys_shims.c src/exec.c src/platform_linux.c
TEST_CFLAGS := -DMASA_NO_MAIN

.PHONY: all test clean run

all: $(BIN)

$(OBJDIR)/%.o: src/%.c include/masa.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) $(OBJ) -o $@ $(LDLIBS)

$(TESTBIN): $(TEST_SRC) include/masa.h | $(OBJDIR)
	$(CC) $(CFLAGS) $(TEST_CFLAGS) $(LDFLAGS) $(TEST_SRC) -o $@ $(LDLIBS)

$(OBJDIR):
	mkdir -p $(OBJDIR)

test: $(TESTBIN) $(BIN)
	./$(TESTBIN)

run: $(BIN)
	./$(BIN)

clean:
	rm -rf $(OBJDIR)
