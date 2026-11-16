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

/* realpath() and friends. */
#ifndef _WIN32
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#endif

#include "masa.h"
#include "masa_compat.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -target T -fuse-ld=lld -Qunused-arguments [-isysroot S] */
#define MASA_CC_INJECTED_ARGS 6
/* -Wl,-platform_version,macos,X,X when linking */
#define MASA_CC_LINK_ARGS 1
/* Injected arguments plus the trailing NULL terminator. */
#define MASA_CC_REQUIRED_SLOTS (MASA_CC_INJECTED_ARGS + MASA_CC_LINK_ARGS + 1)

#define MASA_CC_MAX_ARGS 4096
#define MASA_PATH_MAX 4096

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

/* Directory part of a path, or "." when there is none. */
static void dir_of(const char *path, char *out, size_t outcap)
{
    const char *slash = strrchr(path, '/');

    if (slash == NULL) {
        snprintf(out, outcap, ".");
        return;
    }
    if (slash == path) {
        snprintf(out, outcap, "/");
        return;
    }
    {
        size_t len = (size_t)(slash - path);
        if (len >= outcap) {
            len = outcap - 1;
        }
        memcpy(out, path, len);
        out[len] = '\0';
    }
}

/* 1 when the directory holds a Masa shim (or the Masa binary) itself. */
static int dir_is_masa(const char *dir)
{
    char dir_real[MASA_PATH_MAX];
    char self[MASA_PATH_MAX];
    char self_real[MASA_PATH_MAX];
    char self_dir[MASA_PATH_MAX];
    char *self_alloc;
    const char *shim;

    if (dir == NULL || dir[0] == '\0') {
        return 0;
    }
    {
        char *real = masa_realpath(dir);

        if (real == NULL) {
            return 0;
        }
        snprintf(dir_real, sizeof(dir_real), "%s", real);
        free(real);
    }

    shim = getenv(MASA_SHIM_DIR_ENV);
    if (shim != NULL && shim[0] != '\0') {
        char *real = masa_realpath(shim);

        if (real != NULL) {
            int same = (strcmp(real, dir_real) == 0);

            free(real);
            if (same) {
                return 1;
            }
        }
    }

    self_alloc = masa_self_path();
    if (self_alloc == NULL) {
        return 0;
    }
    if (strlen(self_alloc) >= sizeof(self)) {
        free(self_alloc);
        return 0;
    }
    snprintf(self, sizeof(self), "%s", self_alloc);
    free(self_alloc);

    {
        char *real = masa_realpath(self);

        if (real != NULL) {
            snprintf(self_real, sizeof(self_real), "%s", real);
            free(real);
        } else {
            snprintf(self_real, sizeof(self_real), "%s", self);
        }
    }
    dir_of(self_real, self_dir, sizeof(self_dir));

    return strcmp(self_dir, dir_real) == 0;
}

static int candidate_is_usable(const char *path)
{
    struct stat st;

    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return 0;
    }
    if (access(path, X_OK) != 0) {
        return 0;
    }
    return 1;
}

/* An explicit MASA_REAL_CC / MASA_CC value is honoured as given. */
static char *try_explicit(const char *name)
{
    const char *value = getenv(name);

    if (value == NULL || value[0] == '\0') {
        return NULL;
    }
    if (strchr(value, '/') != NULL) {
        if (candidate_is_usable(value)) {
            return strdup(value);
        }
        return NULL;
    }
    return strdup(value);
}

char *masa_resolve_cc(void)
{
    static const char *names[] = {"clang", "cc", "gcc"};
    char *explicit_path;
    const char *path_env;
    char *path_copy;
    char *dir;
    char candidate[MASA_PATH_MAX];
    char path_sep[2];
    size_t i;

    path_sep[0] = masa_path_separator();
    path_sep[1] = '\0';

    explicit_path = try_explicit(MASA_REAL_CC_ENV);
    if (explicit_path == NULL) {
        explicit_path = try_explicit(MASA_CC_ENV);
    }
    if (explicit_path != NULL) {
        return explicit_path;
    }

    path_env = getenv("PATH");
    if (path_env == NULL) {
        return NULL;
    }

    path_copy = strdup(path_env);
    if (path_copy == NULL) {
        return NULL;
    }

    for (dir = strtok(path_copy, path_sep); dir != NULL;
         dir = strtok(NULL, path_sep)) {
        const char *d = (dir[0] == '\0') ? "." : dir;

        if (dir_is_masa(d)) {
            continue;
        }

        for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            if (snprintf(candidate, sizeof(candidate), "%s/%s", d, names[i]) >=
                (int)sizeof(candidate)) {
                continue;
            }
            if (!candidate_is_usable(candidate)) {
                continue;
            }

            /* A symlink farm may point straight back at Masa: reject it. */
            {
                char *real = masa_realpath(candidate);

                if (real != NULL) {
                    char *self = masa_self_path();
                    int is_self = (self != NULL && strcmp(self, real) == 0);

                    free(self);
                    free(real);
                    if (is_self) {
                        continue;
                    }
                }
            }

            {
                char *result = strdup(candidate);
                free(path_copy);
                return result;
            }
        }
    }

    free(path_copy);
    return NULL;
}

/*
 * 1 when the invocation only produces object files.  Anything that might
 * link (no -c, and no -E/-S/-M-only output) needs a platform version.
 */
static int is_compile_only(int argc, char *const argv[])
{
    int i;

    for (i = 0; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "-c") == 0 || strcmp(a, "-S") == 0 ||
            strcmp(a, "-E") == 0 || strcmp(a, "-M") == 0 ||
            strcmp(a, "-MM") == 0 || strcmp(a, "-fsyntax-only") == 0) {
            return 1;
        }
        if (strcmp(a, "-o") == 0) {
            i++; /* skip the output file name */
        }
    }
    return 0;
}

/*
 * ld64.lld requires a platform version when linking.  clang only passes
 * one when it can infer it, so supply it explicitly through the linker
 * escape hatch when the caller has not.
 */
static int has_platform_version(int argc, char *const argv[])
{
    int i;

    for (i = 0; i < argc; i++) {
        if (strncmp(argv[i], "-Wl,-platform_version", 21) == 0 ||
            strcmp(argv[i], "-platform_version") == 0) {
            return 1;
        }
    }
    return 0;
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
    /* Linker-only flags are injected into every invocation, so tell
     * clang not to warn when a compile step cannot use them. */
    args[n++] = "-Qunused-arguments";

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
    char *compiler;
    char platform_arg[128];
    int count = 0;
    int i;
    int status;

    target = env_or(MASA_TARGET_ENV, MASA_DEFAULT_TARGET);
    sdk_path = getenv(MASA_SDK_PATH_ENV);

    compiler = masa_resolve_cc();
    if (compiler == NULL) {
        fprintf(stderr,
                "masa: cannot find a host compiler (set MASA_REAL_CC or "
                "MASA_CC, or install clang)\n");
        return 1;
    }

    args[count++] = compiler;
    for (i = 0; i < argc; i++) {
        if (count >= MASA_CC_MAX_ARGS - MASA_CC_REQUIRED_SLOTS) {
            fprintf(stderr, "masa: too many arguments\n");
            free(compiler);
            return 1;
        }
        args[count++] = argv[i];
    }

    if (masa_build_cc_args(target, sdk_path, args, &count, MASA_CC_MAX_ARGS) != 0) {
        fprintf(stderr, "masa: cannot build the cc command line\n");
        free(compiler);
        return 1;
    }

    if (!is_compile_only(argc, argv) && !has_platform_version(argc, argv)) {
        snprintf(platform_arg, sizeof(platform_arg),
                 "-Wl,-platform_version,%s,%s,%s",
                 MASA_MACOS_PLATFORM, MASA_MACOS_VERSION, MASA_MACOS_VERSION);
        if (count < MASA_CC_MAX_ARGS - 1) {
            args[count++] = platform_arg;
            args[count] = NULL;
        }
    }

    if (env_flag_enabled(MASA_DRY_RUN_ENV)) {
        for (i = 0; i < count; i++) {
            printf("%s%s", (i == 0) ? "" : " ", args[i]);
        }
        printf("\n");
        free(compiler);
        return 0;
    }

    status = masa_spawn_wait(args);
    free(compiler);
    return status;
}
