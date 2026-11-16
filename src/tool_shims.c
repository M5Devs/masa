/*
 * tool_shims.c - route the Darwin toolchain commands to their LLVM
 * equivalents so a full project build can link Mach-O images.
 *
 * ld      -> ld64.lld (with a platform version, which it requires)
 * ar      -> llvm-ar
 * ranlib  -> llvm-ranlib
 * strip   -> llvm-strip
 * nm      -> llvm-nm
 * otool   -> llvm-objdump --macho
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* realpath() and friends. */
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "masa.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MASA_PATH_MAX 4096
#define MASA_CAPTURE_MAX 4096
#define MASA_TOOL_MAX_ARGS 4096

const char *const masa_tool_names[] = {
    "ld", "ar", "ranlib", "strip", "nm", "otool", NULL
};

/* Description of one toolchain shim. */
struct masa_tool {
    const char *name;          /* shim name, e.g. "ld" */
    const char *prog_name;     /* clang -print-prog-name=<this> */
    const char *fallbacks[4];  /* bare names tried on PATH, then NULL */
    const char *prefix[3];     /* leading args for the real tool */
};

static const struct masa_tool masa_tools[] = {
    { "ld",     "ld64.lld",     { "ld64.lld", "lld", NULL },     { NULL } },
    { "ar",     "llvm-ar",      { "llvm-ar", NULL },             { NULL } },
    { "ranlib", "llvm-ranlib",  { "llvm-ranlib", NULL },         { NULL } },
    { "strip",  "llvm-strip",   { "llvm-strip", NULL },          { NULL } },
    { "nm",     "llvm-nm",      { "llvm-nm", NULL },             { NULL } },
    { "otool",  "llvm-objdump", { "llvm-objdump", "llvm-otool", NULL },
                                 { "--macho", NULL } }
};

int masa_tool_is_shim(const char *name)
{
    size_t i;

    if (name == NULL) {
        return 0;
    }
    for (i = 0; i < sizeof(masa_tools) / sizeof(masa_tools[0]); i++) {
        if (strcmp(name, masa_tools[i].name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_executable_file(const char *path)
{
    struct stat st;

    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return 0;
    }
    return access(path, X_OK) == 0;
}

/* Trim trailing whitespace from a captured string in place. */
static void trim_trailing(char *s)
{
    size_t len = strlen(s);

    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                       s[len - 1] == ' ' || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
}

/*
 * Ask the host compiler where it keeps a tool.  The answer is only
 * accepted when it is an executable path that differs from the queried
 * name, which rejects clang's habit of echoing the name back.
 */
static char *prog_name_lookup(const char *cc, const char *prog_name)
{
    const char *argv[3];
    char out[MASA_CAPTURE_MAX];
    char candidate[MASA_PATH_MAX];

    argv[0] = cc;
    argv[1] = candidate;
    argv[2] = NULL;

    if (snprintf(candidate, sizeof(candidate), "-print-prog-name=%s",
                 prog_name) >= (int)sizeof(candidate)) {
        return NULL;
    }

    if (masa_capture(argv, out, sizeof(out)) != 0) {
        return NULL;
    }
    trim_trailing(out);

    if (out[0] == '\0' || strcmp(out, prog_name) == 0 ||
        strchr(out, '/') == NULL) {
        return NULL;
    }
    if (!is_executable_file(out)) {
        return NULL;
    }
    return strdup(out);
}

/* Look for a bare name in the directories of PATH. */
static char *path_lookup(const char *name)
{
    const char *path_env = getenv("PATH");
    char *copy;
    char *dir;
    char candidate[MASA_PATH_MAX];

    if (path_env == NULL) {
        return NULL;
    }
    copy = strdup(path_env);
    if (copy == NULL) {
        return NULL;
    }

    for (dir = strtok(copy, ":"); dir != NULL; dir = strtok(NULL, ":")) {
        const char *d = (dir[0] == '\0') ? "." : dir;

        if (snprintf(candidate, sizeof(candidate), "%s/%s", d, name) >=
            (int)sizeof(candidate)) {
            continue;
        }
        if (is_executable_file(candidate)) {
            char *result = strdup(candidate);
            free(copy);
            return result;
        }
    }
    free(copy);
    return NULL;
}

char *masa_locate_tool(const char *prog_name, const char *const fallbacks[])
{
    char *cc;
    char *found;
    char next_to[MASA_PATH_MAX];
    const char *slash;
    size_t i;

    cc = masa_resolve_cc();
    if (cc != NULL) {
        found = prog_name_lookup(cc, prog_name);
        if (found != NULL) {
            free(cc);
            return found;
        }

        /* The LLVM tools usually sit next to the compiler. */
        slash = strrchr(cc, '/');
        if (slash != NULL) {
            size_t dirlen = (size_t)(slash - cc);

            if (dirlen < sizeof(next_to)) {
                memcpy(next_to, cc, dirlen);
                next_to[dirlen] = '\0';
                for (i = 0; fallbacks[i] != NULL; i++) {
                    char probe[MASA_PATH_MAX];

                    if (snprintf(probe, sizeof(probe), "%s/%s", next_to,
                                 fallbacks[i]) >= (int)sizeof(probe)) {
                        continue;
                    }
                    if (is_executable_file(probe)) {
                        char *result = strdup(probe);
                        free(cc);
                        return result;
                    }
                }
            }
        }
        free(cc);
    }

    for (i = 0; fallbacks[i] != NULL; i++) {
        found = path_lookup(fallbacks[i]);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

static int has_platform_version(int argc, char *const argv[])
{
    int i;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-platform_version") == 0) {
            return 1;
        }
    }
    return 0;
}

static const struct masa_tool *find_tool(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof(masa_tools) / sizeof(masa_tools[0]); i++) {
        if (strcmp(name, masa_tools[i].name) == 0) {
            return &masa_tools[i];
        }
    }
    return NULL;
}

int masa_tool_dispatch(const char *name, int argc, char *const argv[])
{
    const struct masa_tool *tool = find_tool(name);
    const char *args[MASA_TOOL_MAX_ARGS];
    char *tool_path;
    int count = 0;
    int i;
    int status;

    if (tool == NULL) {
        return -1;
    }

    tool_path = masa_locate_tool(tool->prog_name, tool->fallbacks);
    if (tool_path == NULL) {
        fprintf(stderr, "masa: %s: cannot find %s (install LLVM)\n",
                name, tool->prog_name);
        return 1;
    }

    args[count++] = tool_path;

    for (i = 0; tool->prefix[i] != NULL; i++) {
        args[count++] = tool->prefix[i];
    }
    for (i = 0; i < argc; i++) {
        if (count >= MASA_TOOL_MAX_ARGS - 5) {
            fprintf(stderr, "masa: %s: too many arguments\n", name);
            free(tool_path);
            return 1;
        }
        args[count++] = argv[i];
    }

    /*
     * ld64.lld refuses to link without a platform version.  Supply one
     * unless the caller (usually clang) already did.
     */
    if (strcmp(name, "ld") == 0 && !has_platform_version(argc, argv)) {
        args[count++] = "-platform_version";
        args[count++] = MASA_MACOS_PLATFORM;
        args[count++] = MASA_MACOS_VERSION;
        args[count++] = MASA_MACOS_VERSION;
    }

    args[count] = NULL;

    status = masa_spawn_wait(args);
    free(tool_path);
    return status;
}
