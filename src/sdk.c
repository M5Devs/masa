/*
 * sdk.c - inspect an SDK root and generate a clean-room stub SDK.
 *
 * The stub SDK is written from scratch here: a TAPI v4 text-based
 * definition (.tbd) that names the symbols a Mach-O image needs, plus a
 * few standard headers.  It contains no Apple code, so it is safe to
 * ship and enough for ld64.lld to produce real MH_EXECUTE images.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _POSIX_C_SOURCE 200809L

#include "masa.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MASA_PATH_MAX 4096

static const char *sdk_root(void)
{
    const char *value = getenv(MASA_SDK_PATH_ENV);

    if (value != NULL && value[0] != '\0') {
        return value;
    }
    return MASA_DEFAULT_SDK_PATH;
}

/* Create every directory in path, ignoring those that already exist. */
static int mkdir_p(const char *path)
{
    char tmp[MASA_PATH_MAX];
    char *p;
    size_t len = strlen(path);

    if (len == 0 || len >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, path, len + 1);

    for (p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int write_text_file(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");

    if (fp == NULL) {
        fprintf(stderr, "masa: cannot write '%s': %s\n", path, strerror(errno));
        return -1;
    }
    if (fputs(text, fp) == EOF || fclose(fp) != 0) {
        fprintf(stderr, "masa: cannot write '%s': %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Stub contents                                                      */
/* ------------------------------------------------------------------ */

/*
 * The .tbd files below are clean-room stubs: the symbol lists are the
 * names a normal C program references, written out here rather than
 * copied from any SDK.  install-name follows the convention a real
 * libSystem advertises so linked images record a plausible load command.
 */
static const char *libsystem_tbd(void)
{
    return
        "--- !tapi-tbd\n"
        "tbd-version:     4\n"
        "targets:         [ arm64-macos, x86_64-macos ]\n"
        "install-name:    '/usr/lib/libSystem.B.dylib'\n"
        "current-version: 1311.100.3\n"
        "compatibility-version: 1.0\n"
        "exports:\n"
        "  - targets:         [ arm64-macos, x86_64-macos ]\n"
        "    symbols:         [ _exit, _abort, _malloc, _free, _calloc,"
        " _realloc, _memcpy, _memset, _memmove, _strlen, _strcmp, _strncmp,"
        " _printf, _fprintf, _puts, _putchar, _write, _read, _open, _close,"
        " dyld_stub_binder ]\n"
        "...\n";
}

static const char *crt1_tbd(void)
{
    return
        "--- !tapi-tbd\n"
        "tbd-version:     4\n"
        "targets:         [ arm64-macos, x86_64-macos ]\n"
        "install-name:    '/usr/lib/crt1.o'\n"
        "current-version: 1.0\n"
        "compatibility-version: 1.0\n"
        "exports:\n"
        "  - targets:         [ arm64-macos, x86_64-macos ]\n"
        "    symbols:         [ _main ]\n"
        "...\n";
}

static const char *stdlib_h(void)
{
    return
        "#ifndef MASA_STUB_STDLIB_H\n"
        "#define MASA_STUB_STDLIB_H\n"
        "\n"
        "/* Clean-room stub of the standard declarations used by tests. */\n"
        "#define EXIT_SUCCESS 0\n"
        "#define EXIT_FAILURE 1\n"
        "#define NULL ((void *)0)\n"
        "\n"
        "typedef unsigned long size_t;\n"
        "\n"
        "void exit(int status);\n"
        "void abort(void);\n"
        "void *malloc(size_t size);\n"
        "void *calloc(size_t count, size_t size);\n"
        "void *realloc(void *ptr, size_t size);\n"
        "void free(void *ptr);\n"
        "\n"
        "#endif /* MASA_STUB_STDLIB_H */\n";
}

static const char *string_h(void)
{
    return
        "#ifndef MASA_STUB_STRING_H\n"
        "#define MASA_STUB_STRING_H\n"
        "\n"
        "typedef unsigned long size_t;\n"
        "\n"
        "void *memcpy(void *dst, const void *src, size_t n);\n"
        "void *memset(void *dst, int c, size_t n);\n"
        "void *memmove(void *dst, const void *src, size_t n);\n"
        "size_t strlen(const char *s);\n"
        "int strcmp(const char *a, const char *b);\n"
        "int strncmp(const char *a, const char *b, size_t n);\n"
        "\n"
        "#endif /* MASA_STUB_STRING_H */\n";
}

static const char *stdio_h(void)
{
    return
        "#ifndef MASA_STUB_STDIO_H\n"
        "#define MASA_STUB_STDIO_H\n"
        "\n"
        "typedef unsigned long size_t;\n"
        "\n"
        "int printf(const char *format, ...);\n"
        "int fprintf(void *stream, const char *format, ...);\n"
        "int puts(const char *s);\n"
        "int putchar(int c);\n"
        "\n"
        "#endif /* MASA_STUB_STDIO_H */\n";
}

/* ------------------------------------------------------------------ */
/* sdk init-stub                                                      */
/* ------------------------------------------------------------------ */

struct stub_file {
    const char *relpath;
    const char *(*contents)(void);
};

static const struct stub_file stub_files[] = {
    { "usr/lib/libSystem.tbd", libsystem_tbd },
    { "usr/lib/libSystem.B.tbd", libsystem_tbd },
    { "usr/lib/crt1.o", crt1_tbd },
    { "usr/include/stdlib.h", stdlib_h },
    { "usr/include/string.h", string_h },
    { "usr/include/stdio.h", stdio_h }
};

static int sdk_init_stub(const char *dir)
{
    size_t i;

    if (dir == NULL || dir[0] == '\0') {
        fprintf(stderr, "masa: sdk init-stub: missing directory\n");
        return 2;
    }

    for (i = 0; i < sizeof(stub_files) / sizeof(stub_files[0]); i++) {
        char path[MASA_PATH_MAX];
        char parent[MASA_PATH_MAX];
        const char *slash;

        if (snprintf(path, sizeof(path), "%s/%s", dir,
                     stub_files[i].relpath) >= (int)sizeof(path)) {
            fprintf(stderr, "masa: sdk init-stub: path too long\n");
            return 1;
        }

        /* Create the containing directory before writing the file. */
        slash = strrchr(path, '/');
        if (slash != NULL) {
            size_t len = (size_t)(slash - path);
            memcpy(parent, path, len);
            parent[len] = '\0';
            if (mkdir_p(parent) != 0) {
                fprintf(stderr, "masa: sdk init-stub: cannot create '%s': %s\n",
                        parent, strerror(errno));
                return 1;
            }
        }

        if (write_text_file(path, stub_files[i].contents()) != 0) {
            return 1;
        }
    }

    printf("masa: wrote stub SDK to %s\n", dir);
    printf("  %s/%s\n", dir, MASA_SDK_LIBSYSTEM_RELPATH);
    printf("Use it with: MASA_SDK_PATH=%s masa exec make\n", dir);
    return 0;
}

/* ------------------------------------------------------------------ */
/* sdk info                                                           */
/* ------------------------------------------------------------------ */

static int sdk_info(void)
{
    const char *root = sdk_root();
    char libpath[MASA_PATH_MAX];
    struct stat st;
    int ready;

    if (snprintf(libpath, sizeof(libpath), "%s/%s", root,
                 MASA_SDK_LIBSYSTEM_RELPATH) >= (int)sizeof(libpath)) {
        fprintf(stderr, "masa: sdk info: path too long\n");
        return 1;
    }

    ready = (stat(libpath, &st) == 0 && S_ISREG(st.st_mode));

    printf("SDK root:      %s\n", root);
    printf("libSystem.tbd: %s\n", libpath);
    printf("status:        %s\n", ready ? "ready" : "missing libSystem.tbd");
    if (!ready) {
        printf("hint:          run `masa sdk init-stub %s` to create a stub\n",
               root);
    }
    return ready ? 0 : 1;
}

int masa_sdk_main(int argc, char *const argv[])
{
    if (argc < 1 || argv[0] == NULL || argv[0][0] == '\0') {
        fprintf(stderr, "masa: sdk: missing subcommand (info | init-stub)\n");
        return 2;
    }

    if (strcmp(argv[0], "info") == 0) {
        return sdk_info();
    }
    if (strcmp(argv[0], "init-stub") == 0) {
        return sdk_init_stub(argc > 1 ? argv[1] : NULL);
    }

    fprintf(stderr, "masa: sdk: unknown subcommand '%s'\n", argv[0]);
    return 2;
}
