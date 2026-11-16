/*
 * swift_wrapper.c - drive swiftc so that it emits Mach-O objects for
 * macOS, and read a minimal Swift Package manifest.
 *
 * The wrapper appends a target triple and, when an SDK root is
 * configured, points swiftc at it through -sdk.  The command can be
 * inspected instead of executed with MASA_DRY_RUN=1.  `masa swift` also
 * understands enough of Package.swift to report the package name and
 * whether a conventional Sources/ tree exists.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "masa.h"
#include "masa_compat.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -target T [-sdk S], plus the NULL terminator. */
#define MASA_SWIFT_REQUIRED_SLOTS 4
#define MASA_SWIFT_MAX_ARGS 4096

/* Relative path of the Swift Package manifest. */
#define MASA_PACKAGE_RELPATH "Package.swift"

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

int masa_build_swift_args(const char *target, const char *sdk_path,
                          const char *args[], int *count, int max_args)
{
    int n;

    if (target == NULL || target[0] == '\0' || args == NULL || count == NULL) {
        return -1;
    }
    if (*count < 0 || max_args < MASA_SWIFT_REQUIRED_SLOTS) {
        return -1;
    }
    if (*count > max_args - MASA_SWIFT_REQUIRED_SLOTS) {
        return -1;
    }

    n = *count;
    args[n++] = "-target";
    args[n++] = target;

    if (sdk_path != NULL && sdk_path[0] != '\0') {
        args[n++] = "-sdk";
        args[n++] = sdk_path;
    }

    args[n] = NULL;
    *count = n;
    return 0;
}

char *masa_resolve_swiftc(void)
{
    const char *explicit_path = getenv(MASA_SWIFTC_ENV);

    if (explicit_path != NULL && explicit_path[0] != '\0') {
        /* An explicit path is honoured only when it is a real program. */
        if (strchr(explicit_path, '/') != NULL) {
            struct stat st;

            if (stat(explicit_path, &st) == 0 && S_ISREG(st.st_mode) &&
                access(explicit_path, X_OK) == 0) {
                return strdup(explicit_path);
            }
            return NULL;
        }
        return strdup(explicit_path);
    }
    return masa_which("swiftc");
}

int masa_swiftc_main(int argc, char *const argv[])
{
    const char *args[MASA_SWIFT_MAX_ARGS];
    const char *target;
    const char *sdk_path;
    char *swiftc;
    int count = 0;
    int i;
    int status;

    target = env_or(MASA_TARGET_ENV, MASA_DEFAULT_SWIFT_TARGET);
    sdk_path = getenv(MASA_SDK_PATH_ENV);

    swiftc = masa_resolve_swiftc();
    if (swiftc == NULL) {
        /*
         * A dry run only needs the command line; fall back to the bare
         * name so the invocation can be inspected without a toolchain.
         */
        if (env_flag_enabled(MASA_DRY_RUN_ENV)) {
            swiftc = strdup("swiftc");
        }
        if (swiftc == NULL) {
            fprintf(stderr,
                    "masa: cannot find swiftc (set %s or install the Swift "
                    "toolchain)\n", MASA_SWIFTC_ENV);
            return 1;
        }
    }

    args[count++] = swiftc;
    for (i = 0; i < argc; i++) {
        if (count >= MASA_SWIFT_MAX_ARGS - MASA_SWIFT_REQUIRED_SLOTS) {
            fprintf(stderr, "masa: too many arguments\n");
            free(swiftc);
            return 1;
        }
        args[count++] = argv[i];
    }

    if (masa_build_swift_args(target, sdk_path, args, &count,
                              MASA_SWIFT_MAX_ARGS) != 0) {
        fprintf(stderr, "masa: cannot build the swiftc command line\n");
        free(swiftc);
        return 1;
    }

    if (env_flag_enabled(MASA_DRY_RUN_ENV)) {
        for (i = 0; i < count; i++) {
            printf("%s%s", (i == 0) ? "" : " ", args[i]);
        }
        printf("\n");
        free(swiftc);
        return 0;
    }

    status = masa_spawn_wait(args);
    free(swiftc);
    return status;
}

/* ------------------------------------------------------------------ */
/* Package.swift                                                      */
/* ------------------------------------------------------------------ */

/*
 * Extract a `name: "Foo"` value from the manifest text.  This is a
 * deliberately small reader: Swift Package Manager writes the package
 * name as the first `name:` string literal in the Package(...) call, so
 * a scan for the first quoted value after `name` is enough for the
 * common case, and a miss simply leaves the name unknown.
 */
static char *extract_package_name(const char *text)
{
    const char *p = text;

    while ((p = strstr(p, "name")) != NULL) {
        const char *q = p + 4;

        /* The token must be a whole word: `name` not `filename`. */
        if (p > text && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) {
            p += 4;
            continue;
        }
        while (*q == ' ' || *q == '\t') {
            q++;
        }
        if (*q != ':') {
            p += 4;
            continue;
        }
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') {
            q++;
        }
        if (*q != '"') {
            p += 4;
            continue;
        }
        q++;
        {
            const char *end = strchr(q, '"');

            if (end != NULL && end > q) {
                size_t len = (size_t)(end - q);
                char *name = malloc(len + 1);

                if (name == NULL) {
                    return NULL;
                }
                memcpy(name, q, len);
                name[len] = '\0';
                return name;
            }
        }
        p += 4;
    }
    return NULL;
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

static int is_directory(const char *path)
{
    struct stat st;

    return (path != NULL && path[0] != '\0' && stat(path, &st) == 0 &&
            S_ISDIR(st.st_mode));
}

void masa_package_free(struct masa_package *pkg)
{
    if (pkg == NULL) {
        return;
    }
    free(pkg->name);
    free(pkg->root);
    pkg->name = NULL;
    pkg->root = NULL;
    pkg->has_sources = 0;
}

int masa_package_parse(const char *path, struct masa_package *out,
                       char *err, size_t errcap)
{
    FILE *fp;
    char *buf;
    long size;
    size_t got;
    char root[MASA_PATH_MAX];
    char sources[MASA_PATH_MAX];
    struct stat st;

    if (err != NULL && errcap > 0) {
        err[0] = '\0';
    }
    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    if (path == NULL || path[0] == '\0') {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "no Package.swift path given");
        }
        return -1;
    }
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "cannot find '%s'", path);
        }
        return -1;
    }

    fp = fopen(path, "rb");
    if (fp == NULL) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "cannot open '%s': %s", path,
                     strerror(errno));
        }
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "cannot size '%s'", path);
        }
        return -1;
    }
    buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(fp);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "out of memory");
        }
        return -1;
    }
    got = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[got] = '\0';

    /* A manifest must at least mention Package; otherwise it is not one. */
    if (strstr(buf, "Package") == NULL) {
        free(buf);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "'%s' is not a Package.swift manifest",
                     path);
        }
        return -1;
    }

    out->name = extract_package_name(buf);
    free(buf);

    dir_of(path, root, sizeof(root));
    out->root = strdup(root);
    if (out->root == NULL) {
        masa_package_free(out);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "out of memory");
        }
        return -1;
    }

    if (snprintf(sources, sizeof(sources), "%s/Sources", root) <
        (int)sizeof(sources)) {
        out->has_sources = is_directory(sources);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* masa swift                                                         */
/* ------------------------------------------------------------------ */

static const char *package_path_in(const char *dir, char *buf, size_t cap)
{
    if (dir == NULL || dir[0] == '\0') {
        dir = ".";
    }
    if (snprintf(buf, cap, "%s/%s", dir, MASA_PACKAGE_RELPATH) >= (int)cap) {
        return NULL;
    }
    return buf;
}

static int swift_info(const char *dir)
{
    char pbpath[MASA_PATH_MAX];
    char err[256];
    struct masa_package pkg;

    if (package_path_in(dir, pbpath, sizeof(pbpath)) == NULL) {
        fprintf(stderr, "masa: swift info: package path too long\n");
        return 1;
    }
    if (masa_package_parse(pbpath, &pkg, err, sizeof(err)) != 0) {
        fprintf(stderr, "masa: swift info: %s\n", err);
        return 1;
    }

    printf("Package.swift: %s\n", pbpath);
    printf("name:          %s\n", (pkg.name != NULL) ? pkg.name : "(unknown)");
    printf("root:          %s\n", pkg.root);
    printf("sources:       %s\n", pkg.has_sources ? "yes (Sources/)" : "no");
    masa_package_free(&pkg);
    return 0;
}

static int swift_build(const char *dir)
{
    char pbpath[MASA_PATH_MAX];
    char err[256];
    struct masa_package pkg;
    char out[MASA_PATH_MAX];
    char *args[4];

    if (package_path_in(dir, pbpath, sizeof(pbpath)) == NULL) {
        fprintf(stderr, "masa: swift build: package path too long\n");
        return 1;
    }
    if (masa_package_parse(pbpath, &pkg, err, sizeof(err)) != 0) {
        fprintf(stderr, "masa: swift build: %s\n", err);
        return 1;
    }

    if (snprintf(out, sizeof(out), "%s/%s", pkg.root,
                 (pkg.name != NULL) ? pkg.name : "app") >= (int)sizeof(out)) {
        masa_package_free(&pkg);
        fprintf(stderr, "masa: swift build: output path too long\n");
        return 1;
    }

    /*
     * Hand the manifest to swiftc, which links a single-file package into
     * a Mach-O executable.  The wrapper adds the target triple and SDK.
     */
    args[0] = pbpath;
    args[1] = "-o";
    args[2] = out;
    args[3] = NULL;

    printf("masa: swift: building '%s'\n",
           (pkg.name != NULL) ? pkg.name : "app");
    masa_package_free(&pkg);
    return masa_swiftc_main(3, args);
}

int masa_swift_main(int argc, char *const argv[])
{
    const char *sub;

    if (argc < 1 || argv[0] == NULL || argv[0][0] == '\0') {
        fprintf(stderr, "masa: swift: missing subcommand (info | build)\n");
        return 2;
    }
    sub = argv[0];

    if (strcmp(sub, "info") == 0) {
        return swift_info((argc >= 2) ? argv[1] : NULL);
    }
    if (strcmp(sub, "build") == 0) {
        return swift_build((argc >= 2) ? argv[1] : NULL);
    }

    fprintf(stderr, "masa: swift: unknown subcommand '%s'\n", sub);
    return 2;
}
