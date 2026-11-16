/*
 * main.c - command line router for Masa.
 *
 * Masa is a BusyBox-style multiplexer: when argv[0] is a command it
 * emulates (cc, clang, gcc, uname, sw_vers, xcode-select, xcrun) it
 * behaves as that command, otherwise argv[1] selects a Masa subcommand.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _POSIX_C_SOURCE 200809L

#include "masa.h"

#include <stdio.h>
#include <string.h>

static void print_usage(FILE *out, const char *prog)
{
    fprintf(out,
            "masa %s - cross-compilation wrapper and Darwin environment shim\n"
            "\n"
            "Usage:\n"
            "  %s cc [clang-options...]    Compile or link for macOS with clang\n"
            "  %s sys <command> [args...]  Run a shimmed Darwin command\n"
            "  %s exec <command> [args...] Run a command inside a shimmed environment\n"
            "  %s init-shims <dir>         Create the argv[0] shim symlinks in <dir>\n"
            "  %s sdk info                 Report whether the SDK is usable\n"
            "  %s sdk init-stub <dir>      Write a clean-room stub SDK to <dir>\n"
            "  %s version                  Print the Masa version\n"
            "  %s help                     Show this message\n"
            "\n"
            "Environment:\n"
            "  MASA_TARGET     target triple (default: %s)\n"
            "  MASA_SDK_PATH   SDK root passed to -isysroot and reported by shims\n"
            "  MASA_DRY_RUN    set to 1 to print the cc command instead of running it\n"
            "  MASA_REAL_CC    host compiler the cc shim must forward to\n"
            "  MASA_SHIM_DIR   directory holding the argv[0] shims\n"
            "\n"
            "When invoked as one of the shim names below, Masa acts as that\n"
            "command directly (create the links with `masa init-shims <dir>`):\n"
            "  cc, clang, gcc, uname, sw_vers, xcode-select, xcrun,\n"
            "  ld, ar, ranlib, strip, nm, otool\n",
            MASA_VERSION, prog, prog, prog, prog, prog, prog, prog, prog,
            MASA_DEFAULT_TARGET);
}

static int cmd_version(void)
{
    printf("masa %s\n", MASA_VERSION);
    printf("default target: %s\n", MASA_DEFAULT_TARGET);
    printf("default SDK path: %s\n", MASA_DEFAULT_SDK_PATH);
    return 0;
}

/* Basename of a path, without any leading directory components. */
static const char *base_name(const char *path)
{
    const char *slash;

    if (path == NULL) {
        return "";
    }
    slash = strrchr(path, '/');
    return (slash != NULL) ? slash + 1 : path;
}

/*
 * Dispatch on the program name.  argv is the original argument vector, so
 * argv[0] is the name itself; the cc wrapper wants the arguments after it
 * while the sys shims want argv[0] to remain the command name.
 *
 * Returns -1 when the name is not a shim.
 */
static int dispatch_argv0(const char *name, int argc, char *const argv[])
{
    if (name == NULL || name[0] == '\0' || argc < 1) {
        return -1;
    }

    if (strcmp(name, "cc") == 0 ||
        strcmp(name, "clang") == 0 ||
        strcmp(name, "gcc") == 0) {
        return masa_cc_main(argc - 1, &argv[1]);
    }
    if (strcmp(name, "uname") == 0 ||
        strcmp(name, "sw_vers") == 0 ||
        strcmp(name, "xcode-select") == 0 ||
        strcmp(name, "xcrun") == 0) {
        return masa_sys_dispatch(argc, argv);
    }
    if (masa_tool_is_shim(name)) {
        return masa_tool_dispatch(name, argc - 1, &argv[1]);
    }
    return -1;
}

int masa_main(int argc, char *const argv[])
{
    const char *prog = "masa";
    const char *command;
    int dispatched;

    if (argc > 0 && argv[0] != NULL) {
        prog = argv[0];
    }

    /* BusyBox-style: the program name wins over the subcommand. */
    dispatched = dispatch_argv0(base_name(prog), argc, argv);
    if (dispatched >= 0) {
        return dispatched;
    }

    if (argc < 2 || argv[1] == NULL) {
        print_usage(stderr, prog);
        return 2;
    }

    command = argv[1];

    if (strcmp(command, "cc") == 0) {
        return masa_cc_main(argc - 2, &argv[2]);
    }
    if (strcmp(command, "sys") == 0) {
        if (argc < 3 || argv[2] == NULL) {
            fprintf(stderr, "masa: sys: missing command\n");
            return 2;
        }
        return masa_sys_dispatch(argc - 2, &argv[2]);
    }
    if (strcmp(command, "exec") == 0) {
        if (argc < 3 || argv[2] == NULL) {
            fprintf(stderr, "masa: exec: missing command\n");
            return 2;
        }
        return masa_exec_main(argc - 2, &argv[2]);
    }
    if (strcmp(command, "init-shims") == 0) {
        if (argc < 3 || argv[2] == NULL) {
            fprintf(stderr, "masa: init-shims: missing directory\n");
            return 2;
        }
        return masa_init_shims_main(argc - 2, &argv[2]);
    }
    if (strcmp(command, "sdk") == 0) {
        if (argc < 3 || argv[2] == NULL) {
            fprintf(stderr, "masa: sdk: missing subcommand (info | init-stub)\n");
            return 2;
        }
        return masa_sdk_main(argc - 2, &argv[2]);
    }
    if (strcmp(command, "version") == 0 || strcmp(command, "--version") == 0) {
        return cmd_version();
    }
    if (strcmp(command, "help") == 0 ||
        strcmp(command, "-h") == 0 ||
        strcmp(command, "--help") == 0) {
        print_usage(stdout, prog);
        return 0;
    }

    fprintf(stderr, "masa: unknown subcommand '%s'\n\n", command);
    print_usage(stderr, prog);
    return 2;
}

#ifndef MASA_NO_MAIN
int main(int argc, char *argv[])
{
    return masa_main(argc, argv);
}
#endif
