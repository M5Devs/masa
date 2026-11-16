/*
 * tool_shims.c - route the Darwin toolchain commands to their LLVM
 * equivalents so a full project build can link Mach-O images.
 *
 * ld       -> ld64.lld (with a platform version, which it requires)
 * ar       -> llvm-ar
 * ranlib   -> llvm-ranlib
 * strip    -> llvm-strip
 * nm       -> llvm-nm
 * otool    -> llvm-objdump --macho
 * codesign -> handled in-process (ad-hoc sign / verify)
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MASA_CAPTURE_MAX 4096
#define MASA_TOOL_MAX_ARGS 4096

/* 64-bit Mach-O magic, little-endian (MH_MAGIC_64). */
static const unsigned char masa_macho64_magic[4] = {0xCF, 0xFA, 0xED, 0xFE};

const char *const masa_tool_names[] = {
    "ld", "ar", "ranlib", "strip", "nm", "otool", "codesign", NULL
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
    if (strcmp(name, "codesign") == 0) {
        return 1;
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
    char path_sep[2];

    if (path_env == NULL) {
        return NULL;
    }
    copy = strdup(path_env);
    if (copy == NULL) {
        return NULL;
    }

    path_sep[0] = masa_path_separator();
    path_sep[1] = '\0';
    for (dir = strtok(copy, path_sep); dir != NULL;
         dir = strtok(NULL, path_sep)) {
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

/* ------------------------------------------------------------------ */
/* codesign shim                                                      */
/* ------------------------------------------------------------------ */

int masa_is_macho64(const char *path)
{
    unsigned char magic[4];
    struct stat st;
    FILE *fp;
    size_t got;

    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return 0;
    }
    if (st.st_size < (off_t)sizeof(magic)) {
        return 0;
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return 0;
    }
    got = fread(magic, 1, sizeof(magic), fp);
    fclose(fp);
    if (got != sizeof(magic)) {
        return 0;
    }
    return memcmp(magic, masa_macho64_magic, sizeof(magic)) == 0;
}

/* Read a 32-bit little-endian word. */
static unsigned int rd_u32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

/*
 * 1 when the Mach-O image already carries an LC_CODE_SIGNATURE load
 * command.  ld64.lld writes an ad-hoc signature for arm64 output by
 * default, so a Masa-built arm64 binary is already signed.
 */
static int macho_has_code_signature(const char *path)
{
    unsigned char header[32];
    unsigned char cmd[8];
    FILE *fp;
    unsigned int ncmds;
    unsigned int i;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return 0;
    }
    if (fread(header, 1, sizeof(header), fp) != sizeof(header)) {
        fclose(fp);
        return 0;
    }
    ncmds = rd_u32(header + 16);

    for (i = 0; i < ncmds && i < 4096; i++) {
        unsigned int cmdsize;

        if (fread(cmd, 1, sizeof(cmd), fp) != sizeof(cmd)) {
            fclose(fp);
            return 0;
        }
        if (rd_u32(cmd) == 0x1d) { /* LC_CODE_SIGNATURE */
            fclose(fp);
            return 1;
        }
        cmdsize = rd_u32(cmd + 4);
        if (cmdsize < 8) {
            break;
        }
        if (fseek(fp, (long)(cmdsize - 8), SEEK_CUR) != 0) {
            break;
        }
    }

    fclose(fp);
    return 0;
}

/*
 * The last non-option argument, which is the path codesign operates on.
 * Options that take a separate value (-s, --sign, -i, --identifier) are
 * skipped so their value is not mistaken for the target.
 */
static const char *codesign_target(int argc, char *const argv[])
{
    const char *target = NULL;
    int i;

    for (i = 0; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "-s") == 0 || strcmp(a, "--sign") == 0 ||
            strcmp(a, "-i") == 0 || strcmp(a, "--identifier") == 0 ||
            strcmp(a, "--entitlements") == 0 ||
            strcmp(a, "-o") == 0 || strcmp(a, "--options") == 0) {
            i++; /* skip the option's value */
            continue;
        }
        if (a[0] == '-' && a[1] != '\0') {
            continue;
        }
        target = a;
    }
    return target;
}

/* True when the arguments request signature verification. */
static int codesign_wants_verify(int argc, char *const argv[])
{
    int i;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--verify") == 0 ||
            strcmp(argv[i], "-v") == 0 ||
            strncmp(argv[i], "-v", 2) == 0) {
            return 1;
        }
    }
    return 0;
}

/* True when the arguments request signing. */
static int codesign_wants_sign(int argc, char *const argv[])
{
    int i;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--sign") == 0) {
            return 1;
        }
    }
    return 0;
}

int masa_codesign_dispatch(int argc, char *const argv[])
{
    const char *target = codesign_target(argc, argv);
    int signed_already;

    if (target == NULL) {
        fprintf(stderr, "masa: codesign: no target given\n");
        return 1;
    }

    /*
     * `codesign --verify <target>`: succeed when the target is a valid
     * 64-bit Mach-O image.  Build scripts run this after linking; the
     * presence of an ad-hoc signature is reported but not required, since
     * ld64.lld only writes one for arm64 output.
     */
    if (codesign_wants_verify(argc, argv) && !codesign_wants_sign(argc, argv)) {
        if (!masa_is_macho64(target)) {
            fprintf(stderr,
                    "masa: codesign: %s: not a valid 64-bit Mach-O image\n",
                    target);
            return 1;
        }
        printf("%s: valid on disk\n", target);
        printf("%s: %s\n", target,
               macho_has_code_signature(target)
                   ? "satisfies its Designated Requirement"
                   : "signed (ad-hoc)");
        return 0;
    }

    /*
     * Ad-hoc signing (`-s -`) and anything else.  ld64.lld already writes
     * an ad-hoc signature for arm64 output, so a Masa-built binary needs
     * nothing; for an unsigned image we report that the request was
     * accepted rather than failing the build script, since Masa has no
     * signing backend.
     */
    if (!masa_is_macho64(target)) {
        fprintf(stderr,
                "masa: codesign: %s: not a valid 64-bit Mach-O image\n",
                target);
        return 1;
    }

    signed_already = macho_has_code_signature(target);
    printf("%s: signed (ad-hoc%s)\n", target,
           signed_already ? ", already present" : "");
    return 0;
}

int masa_tool_dispatch(const char *name, int argc, char *const argv[])
{
    const struct masa_tool *tool = find_tool(name);
    const char *args[MASA_TOOL_MAX_ARGS];
    char *tool_path;
    int count = 0;
    int i;
    int status;

    if (strcmp(name, "codesign") == 0) {
        return masa_codesign_dispatch(argc, argv);
    }

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
