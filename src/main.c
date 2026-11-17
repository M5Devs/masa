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

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "masa.h"
#include "masa_compat.h"

#include <stdio.h>
#include <string.h>

static void print_usage(FILE *out, const char *prog)
{
    fprintf(out,
            "masa %s - cross-compilation wrapper and Darwin environment shim\n"
            "\n"
            "Usage:\n"
            "  %s cc [clang-options...]    Compile or link for macOS with clang\n"
            "  %s swiftc [swift-options...] Compile or link for macOS with swiftc\n"
            "  %s sys <command> [args...]  Run a shimmed Darwin command\n"
            "  %s exec <command> [args...] Run a command inside a shimmed environment\n"
            "  %s shell [-c <command>]     Interactive shell in the shim environment\n"
            "  %s init-shims <dir>         Create the argv[0] shim symlinks in <dir>\n"
            "  %s sdk info                 Report whether the SDK is usable\n"
            "  %s sdk init-stub <dir>      Write a clean-room stub SDK to <dir>\n"
            "  %s xcode build <App.xcodeproj> [-o <dir>] [--bundle]\n"
            "                              Build an Xcode project without Xcode\n"
            "  %s brew lint <formula.rb>   Lint a Homebrew formula\n"
            "  %s brew build <formula.rb> --source <dir> [--prefix <dir>]\n"
            "                              Build a Homebrew formula into a Cellar\n"
            "  %s swift build [dir]        Build a Swift package (or swiftc wrapper)\n"
            "  %s swift info [dir]         Report a Package.swift's name and layout\n"
            "  %s doctor                   Check local macOS cross-build readiness\n"
            "  %s pack --ipa <App.app> -o <out.ipa>\n"
            "  %s pack --dmg <App.app> -o <out.dmg>\n"
            "                              Package an .app as an .ipa or a .dmg\n"
            "  %s run [--root <dir>] <binary> [args...]\n"
            "                              Run a Mach-O executable on this host\n"
            "  %s danroot [--root <dir>] <binary> [args...]\n"
            "                              Alias for `run` (Masa Danroot loader)\n"
            "  %s version                  Print the Masa version\n"
            "  %s help                     Show this message\n"
            "\n"
            "Environment:\n"
            "  MASA_TARGET     target triple (default: %s)\n"
            "  MASA_SDK_PATH   SDK root passed to -isysroot and reported by shims\n"
            "  MASA_DRY_RUN    set to 1 to print the cc command instead of running it\n"
            "  MASA_REAL_CC    host compiler the cc shim must forward to\n"
            "  MASA_SWIFTC     host swift compiler the swift shims forward to\n"
            "  MASA_SHIM_DIR   directory holding the argv[0] shims\n"
            "  MASA_BREW_PREFIX  prefix for `masa brew build` (default: %s)\n"
            "  MASA_KEEP_PS1   set to 1 so `masa shell` keeps the caller's prompt\n"
            "  MASA_DANROOT_ROOT  virtual root for `masa run` path virtualization\n"
            "\n"
            "When invoked as one of the shim names below, Masa acts as that\n"
            "command directly (create the links with `masa init-shims <dir>`):\n"
            "  cc, clang, gcc, swift, swiftc, uname, sw_vers, xcode-select, xcrun,\n"
            "  ld, ar, ranlib, strip, nm, otool, codesign\n",
            MASA_VERSION,
            prog, prog, prog, prog, prog, prog, prog, prog,
            prog, prog, prog, prog, prog, prog, prog, prog,
            prog, prog, prog, prog,
            MASA_DEFAULT_TARGET, MASA_BREW_DEFAULT_PREFIX);
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
    if (strcmp(name, "swiftc") == 0 || strcmp(name, "swift") == 0) {
        return masa_swiftc_main(argc - 1, &argv[1]);
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
    if (strcmp(command, "swiftc") == 0) {
        return masa_swiftc_main(argc - 2, &argv[2]);
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
    if (strcmp(command, "shell") == 0) {
        return masa_shell_main(argc - 2, &argv[2]);
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
    if (strcmp(command, "xcode") == 0) {
        if (argc < 3 || argv[2] == NULL) {
            fprintf(stderr, "masa: xcode: missing subcommand (build)\n");
            return 2;
        }
        return masa_xcode_main(argc - 2, &argv[2]);
    }
    if (strcmp(command, "brew") == 0) {
        if (argc < 3 || argv[2] == NULL) {
            fprintf(stderr, "masa: brew: missing subcommand (lint | build)\n");
            return 2;
        }
        return masa_brew_main(argc - 2, &argv[2]);
    }
    if (strcmp(command, "swift") == 0) {
        return masa_swift_main(argc - 2, &argv[2]);
    }
    if (strcmp(command, "doctor") == 0) {
        return masa_doctor_main(argc - 2, &argv[2]);
    }
    if (strcmp(command, "pack") == 0) {
        return masa_pack_main(argc - 2, &argv[2]);
    }
    if (strcmp(command, "run") == 0 || strcmp(command, "danroot") == 0) {
        if (argc < 3 || argv[2] == NULL) {
            fprintf(stderr, "masa: %s: missing Mach-O executable\n", command);
            return 2;
        }
        return masa_danroot_main(argc - 2, &argv[2]);
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
