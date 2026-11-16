/*
 * cc_wrapper.c - drive clang so that it emits Mach-O objects for macOS.
 *
 * The wrapper appends a target triple, selects the LLVM linker and, when
 * an SDK root is configured, points the compiler at it.  The command can
 * be inspected instead of executed with MASA_DRY_RUN=1.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _POSIX_C_SOURCE 200809L

#include "masa.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -target T -fuse-ld=lld [-isysroot S] */
#define MASA_CC_INJECTED_ARGS 5
/* Injected arguments plus the trailing NULL terminator. */
#define MASA_CC_REQUIRED_SLOTS (MASA_CC_INJECTED_ARGS + 1)

#define MASA_CC_MAX_ARGS 4096

static const char *env_or(const char *name, const char *fallback)
{
    const char *value = getenv(name);

    if (value != NULL && value[0] != '\0') {
        return value;
    }
    return fallback;
}

static int env_flag_enabled(const char *name)
{
    const char *value = getenv(name);

    if (value == NULL) {
        return 0;
    }
    return strcmp(value, "1") == 0 ||
           strcmp(value, "true") == 0 ||
           strcmp(value, "yes") == 0;
}

int masa_build_cc_args(const char *target, const char *sdk_path,
                       const char *args[], int *count, int max_args)
{
    int n;

    if (target == NULL || target[0] == '\0' || args == NULL || count == NULL) {
        return -1;
    }
    if (*count < 0 || max_args < MASA_CC_REQUIRED_SLOTS) {
        return -1;
    }
    if (*count > max_args - MASA_CC_REQUIRED_SLOTS) {
        return -1;
    }

    n = *count;
    args[n++] = "-target";
    args[n++] = target;
    args[n++] = "-fuse-ld=lld";

    if (sdk_path != NULL && sdk_path[0] != '\0') {
        args[n++] = "-isysroot";
        args[n++] = sdk_path;
    }

    args[n] = NULL;
    *count = n;
    return 0;
}

int masa_cc_main(int argc, char *const argv[])
{
    const char *args[MASA_CC_MAX_ARGS];
    const char *target;
    const char *sdk_path;
    const char *compiler;
    int count = 0;
    int i;

    target = env_or(MASA_TARGET_ENV, MASA_DEFAULT_TARGET);
    sdk_path = getenv(MASA_SDK_PATH_ENV);
    compiler = env_or(MASA_CC_ENV, "clang");

    args[count++] = compiler;
    for (i = 0; i < argc; i++) {
        if (count >= MASA_CC_MAX_ARGS - MASA_CC_REQUIRED_SLOTS) {
            fprintf(stderr, "masa: too many arguments\n");
            return 1;
        }
        args[count++] = argv[i];
    }

    if (masa_build_cc_args(target, sdk_path, args, &count, MASA_CC_MAX_ARGS) != 0) {
        fprintf(stderr, "masa: cannot build the cc command line\n");
        return 1;
    }

    if (env_flag_enabled(MASA_DRY_RUN_ENV)) {
        for (i = 0; i < count; i++) {
            printf("%s%s", (i == 0) ? "" : " ", args[i]);
        }
        printf("\n");
        return 0;
    }

    return masa_spawn_wait(args);
}
