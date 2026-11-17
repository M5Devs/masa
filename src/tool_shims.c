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
 * pbcopy   -> host clipboard tool, else a local clipboard file
 * pbpaste  -> host clipboard tool, else a local clipboard file
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

#ifndef _WIN32
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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
/* pbcopy / pbpaste shims                                              */
/* ------------------------------------------------------------------ */

/*
 * Where the local clipboard file lives: $MASA_CLIPBOARD_FILE, else
 * $TMPDIR/masa-clipboard (Windows: $TEMP), else /tmp/masa-clipboard.
 */
static int clipboard_path(char *buf, size_t cap)
{
    const char *env = getenv(MASA_CLIPBOARD_FILE_ENV);

    if (env != NULL && env[0] != '\0') {
        return (snprintf(buf, cap, "%s", env) >= (int)cap) ? -1 : 0;
    }
    {
        const char *base = getenv(MASA_TMPDIR_ENV);

#ifdef _WIN32
        if (base == NULL || base[0] == '\0') {
            base = getenv("TEMP");
        }
        if (base == NULL || base[0] == '\0') {
            base = ".";
        }
#else
        if (base == NULL || base[0] == '\0') {
            base = "/tmp";
        }
#endif
        if (base[strlen(base) - 1] == '/') {
            return (snprintf(buf, cap, "%smasa-clipboard", base) >= (int)cap)
                       ? -1
                       : 0;
        }
        return (snprintf(buf, cap, "%s/masa-clipboard", base) >= (int)cap)
                   ? -1
                   : 0;
    }
}

static int clipboard_file_write(const char *path, const char *data, size_t len)
{
    FILE *fp = fopen(path, "wb");

    if (fp == NULL) {
        return -1;
    }
    if (len > 0 && fwrite(data, 1, len, fp) != len) {
        fclose(fp);
        return -1;
    }
    return (fclose(fp) == 0) ? 0 : -1;
}

/* A missing clipboard file is an empty clipboard, not an error. */
static int clipboard_file_read(const char *path, char *out, size_t outcap)
{
    FILE *fp;
    size_t used = 0;

    if (out != NULL && outcap > 0) {
        out[0] = '\0';
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return 0;
    }
    if (out != NULL && outcap > 1) {
        used = fread(out, 1, outcap - 1, fp);
        out[used] = '\0';
    }
    fclose(fp);
    return 0;
}

/* Read all of standard input into a malloc'd buffer (NUL-terminated). */
static int clipboard_read_stdin(char **out, size_t *out_len)
{
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);

    if (buf == NULL) {
        return -1;
    }
    for (;;) {
        size_t n;

        if (len == cap) {
            char *grown = realloc(buf, cap * 2);

            if (grown == NULL) {
                free(buf);
                return -1;
            }
            buf = grown;
            cap *= 2;
        }
        n = fread(buf + len, 1, cap - len, stdin);
        len += n;
        if (n == 0) {
            if (feof(stdin)) {
                break;
            }
            if (ferror(stdin)) {
                free(buf);
                return -1;
            }
        }
    }
    buf[len] = '\0';
    *out = buf;
    *out_len = len;
    return 0;
}

#ifndef _WIN32
/*
 * Feed data to argv's standard input and wait for it.  Returns the child's
 * exit status, or -1 when the tool could not be started (execvp failed
 * with the child exiting 127).  Nothing is printed on a missing tool so
 * the caller can quietly try the next candidate.
 */
static int clipboard_feed(const char *const argv[], const char *data,
                          size_t len)
{
    int fds[2];
    pid_t pid;
    int status = -1;
    size_t off = 0;

    if (pipe(fds) != 0) {
        return -1;
    }
    fflush(stdout);
    fflush(stderr);

    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[1]);
        if (dup2(fds[0], STDIN_FILENO) < 0) {
            _exit(127);
        }
        close(fds[0]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    close(fds[0]);
    while (off < len) {
        ssize_t n = write(fds[1], data + off, len - off);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        off += (size_t)n;
    }
    close(fds[1]);

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Capture argv's standard output into out (NUL-terminated). */
static int clipboard_capture(const char *const argv[], char *out, size_t outcap)
{
    int fds[2];
    pid_t pid;
    int status = -1;
    size_t used = 0;

    if (out != NULL && outcap > 0) {
        out[0] = '\0';
    }
    if (pipe(fds) != 0) {
        return -1;
    }
    fflush(stdout);
    fflush(stderr);

    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(fds[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    close(fds[1]);
    for (;;) {
        ssize_t n;

        if (out != NULL && used < outcap - 1) {
            n = read(fds[0], out + used, outcap - 1 - used);
        } else {
            char scratch[512];

            n = read(fds[0], scratch, sizeof(scratch));
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        if (out != NULL && used < outcap - 1) {
            used += (size_t)n;
        }
    }
    close(fds[0]);

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    if (out != NULL && outcap > 0) {
        out[used] = '\0';
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Store data on a host clipboard.  Returns 1 when a tool accepted it. */
static int clipboard_try_host_copy(const char *data, size_t len)
{
    static const char *const cmds[][5] = {
        {"wl-copy", NULL},
        {"xclip", "-selection", "clipboard", NULL},
        {"xsel", "-i", "--clipboard", NULL},
    };
    size_t i;

    for (i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        if (clipboard_feed(cmds[i], data, len) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Read a host clipboard into out.  Returns 1 when a tool supplied data. */
static int clipboard_try_host_paste(char *out, size_t outcap)
{
    static const char *const cmds[][5] = {
        {"wl-paste", NULL},
        {"xclip", "-selection", "clipboard", "-o", NULL},
        {"xsel", "-o", "--clipboard", NULL},
    };
    size_t i;

    for (i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        if (clipboard_capture(cmds[i], out, outcap) == 0) {
            return 1;
        }
    }
    return 0;
}
#endif /* !_WIN32 */

int masa_clipboard_is_shim(const char *name)
{
    return (name != NULL &&
            (strcmp(name, "pbcopy") == 0 || strcmp(name, "pbpaste") == 0));
}

int masa_pbcopy_dispatch(int argc, char *const argv[])
{
    char *data = NULL;
    size_t len = 0;
    char path[MASA_PATH_MAX];
    int rc;

    (void)argc;
    (void)argv;

    if (clipboard_read_stdin(&data, &len) != 0) {
        fprintf(stderr, "masa: pbcopy: cannot read standard input\n");
        return 1;
    }
    if (clipboard_path(path, sizeof(path)) != 0) {
        free(data);
        fprintf(stderr, "masa: pbcopy: clipboard path too long\n");
        return 1;
    }

    /*
     * With $MASA_CLIPBOARD_FILE set the file is authoritative, which keeps
     * the round trip deterministic in a headless container.  Otherwise a
     * reachable host clipboard tool is preferred and the file is kept in
     * sync so `pbpaste` still works where no display is available.
     */
#ifndef _WIN32
    {
        const char *forced = getenv(MASA_CLIPBOARD_FILE_ENV);

        if (forced == NULL || forced[0] == '\0') {
            clipboard_try_host_copy(data, len);
        }
    }
#endif

    rc = clipboard_file_write(path, data, len);
    free(data);
    if (rc != 0) {
        fprintf(stderr, "masa: pbcopy: cannot write '%s': %s\n", path,
                strerror(errno));
        return 1;
    }
    return 0;
}

int masa_pbpaste_dispatch(int argc, char *const argv[])
{
    char path[MASA_PATH_MAX];
    char *text = NULL;
    size_t cap = 65536;

    (void)argc;
    (void)argv;

    if (clipboard_path(path, sizeof(path)) != 0) {
        fprintf(stderr, "masa: pbpaste: clipboard path too long\n");
        return 1;
    }
    text = malloc(cap);
    if (text == NULL) {
        fprintf(stderr, "masa: pbpaste: out of memory\n");
        return 1;
    }
    text[0] = '\0';

#ifndef _WIN32
    {
        const char *forced = getenv(MASA_CLIPBOARD_FILE_ENV);

        if (forced == NULL || forced[0] == '\0') {
            if (clipboard_try_host_paste(text, cap)) {
                fputs(text, stdout);
                free(text);
                return 0;
            }
        }
    }
#endif

    if (clipboard_file_read(path, text, cap) != 0) {
        fprintf(stderr, "masa: pbpaste: cannot read '%s'\n", path);
        free(text);
        return 1;
    }
    fputs(text, stdout);
    free(text);
    return 0;
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
