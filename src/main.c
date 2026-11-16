/*
 * main.c - command line router for Masa.
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
            "  %s version                  Print the Masa version\n"
            "  %s help                     Show this message\n"
            "\n"
            "Environment:\n"
            "  MASA_TARGET    target triple (default: %s)\n"
            "  MASA_SDK_PATH  SDK root passed to -isysroot and reported by shims\n"
            "  MASA_DRY_RUN   set to 1 to print the cc command instead of running it\n"
            "  MASA_CC        compiler used by the cc wrapper (default: clang)\n"
            "\n"
            "Shimmed commands: uname, sw_vers, xcode-select, xcrun\n",
            MASA_VERSION, prog, prog, prog, prog, MASA_DEFAULT_TARGET);
}

static int cmd_version(void)
{
    printf("masa %s\n", MASA_VERSION);
    printf("default target: %s\n", MASA_DEFAULT_TARGET);
    printf("default SDK path: %s\n", MASA_DEFAULT_SDK_PATH);
    return 0;
}

int masa_main(int argc, char *const argv[])
{
    const char *prog = "masa";
    const char *command;

    if (argc > 0 && argv[0] != NULL) {
        prog = argv[0];
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
