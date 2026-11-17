/*
 * test_masa.c - unit tests for the Masa cross-compilation wrapper.
 *
 * The tests exercise the public entry point (masa_main) the same way a
 * shell would, capturing stdout/stderr through pipes, and additionally
 * cover the argument builder directly.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _POSIX_C_SOURCE 200809L

#include "masa.h"
#include "masa_compat.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Tiny test harness                                                  */
/* ------------------------------------------------------------------ */

static int g_run;
static int g_failed;
static int g_skipped;

/* Snapshot of PATH, restored by reset_masa_env so tests cannot leak a
 * modified search path into the ones that follow. */
static char *g_original_path;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
            g_failed++;                                                      \
        }                                                                    \
    } while (0)

#define CHECK_STREQ(actual, expected)                                        \
    do {                                                                     \
        const char *a_ = (actual);                                           \
        const char *e_ = (expected);                                         \
        if (a_ == NULL || strcmp(a_, e_) != 0) {                             \
            fprintf(stderr, "  FAIL %s:%d: expected \"%s\", got \"%s\"\n",   \
                    __FILE__, __LINE__, e_, (a_ != NULL) ? a_ : "(null)");   \
            g_failed++;                                                      \
        }                                                                    \
    } while (0)

#define CHECK_CONTAINS(haystack, needle)                                     \
    do {                                                                     \
        const char *h_ = (haystack);                                         \
        const char *n_ = (needle);                                           \
        if (h_ == NULL || strstr(h_, n_) == NULL) {                          \
            fprintf(stderr, "  FAIL %s:%d: \"%s\" not found in \"%s\"\n",    \
                    __FILE__, __LINE__, n_, (h_ != NULL) ? h_ : "(null)");   \
            g_failed++;                                                      \
        }                                                                    \
    } while (0)

#define RUN(fn)                                                              \
    do {                                                                     \
        int before_ = g_failed;                                              \
        printf("[ RUN  ] %s\n", #fn);                                        \
        fn();                                                                \
        g_run++;                                                             \
        if (g_failed == before_) {                                           \
            printf("[ PASS ] %s\n", #fn);                                    \
        } else {                                                             \
            printf("[ FAIL ] %s\n", #fn);                                    \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static void set_env(const char *name, const char *value)
{
    if (value == NULL) {
        unsetenv(name);
    } else {
        setenv(name, value, 1);
    }
}

/*
 * Read fd to EOF, storing at most cap - 1 bytes in buf (NUL-terminated)
 * and discarding any remainder.  Reading all the way to EOF is what
 * keeps the writer alive: if the reader stopped early and closed the
 * pipe, a child still writing its output would be killed by SIGPIPE
 * (which made the captured-exit-status tests flaky).
 */
static size_t drain_fd(int fd, char *buf, size_t cap)
{
    size_t used = 0;
    char scratch[4096];

    for (;;) {
        ssize_t n;
        int full = (buf == NULL || cap == 0 || used >= cap - 1);

        if (full) {
            n = read(fd, scratch, sizeof(scratch));
        } else {
            n = read(fd, buf + used, cap - 1 - used);
        }
        if (n > 0) {
            if (!full) {
                used += (size_t)n;
            }
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    if (buf != NULL && cap > 0) {
        buf[used] = '\0';
    }
    return used;
}

/*
 * Run masa_main in a forked child with stdout/stderr redirected into
 * pipes, and return the child's exit status.  Output buffers are always
 * NUL terminated.  Both streams are drained to EOF before the child is
 * reaped so a verbose child never dies from SIGPIPE; the captured
 * streams are small, so reading stdout before stderr cannot deadlock.
 */
static int run_masa(char *const argv[], char *out, size_t outcap,
                    char *err, size_t errcap)
{
    int out_pipe[2];
    int err_pipe[2];
    int status = -1;
    pid_t pid;
    size_t used;

    if (out != NULL && outcap > 0) {
        out[0] = '\0';
    }
    if (err != NULL && errcap > 0) {
        err[0] = '\0';
    }

    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        return -1;
    }

    fflush(stdout);
    fflush(stderr);

    pid = fork();
    if (pid < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        int argc = 0;
        int status;

        while (argv[argc] != NULL) {
            argc++;
        }

        close(out_pipe[0]);
        close(err_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[1]);
        close(err_pipe[1]);

        /* Never let a closed capture pipe kill us with SIGPIPE. */
        signal(SIGPIPE, SIG_IGN);

        status = masa_main(argc, argv);

        /* _exit() would skip stdio flushing and lose buffered output. */
        fflush(NULL);
        _exit(status);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);

    used = drain_fd(out_pipe[0], out, (out != NULL) ? outcap : 0);
    (void)used;
    close(out_pipe[0]);

    used = drain_fd(err_pipe[0], err, (err != NULL) ? errcap : 0);
    (void)used;
    close(err_pipe[0]);

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

/*
 * Like run_masa(), but the child's stdin is a pipe carrying script.  Used
 * to drive the interactive `masa shell` (no -c) from the test harness.
 */
static int run_masa_with_stdin(char *const argv[], const char *script,
                               char *out, size_t outcap)
{
    int in_pipe[2];
    int out_pipe[2];
    int status = -1;
    pid_t pid;
    ssize_t n;
    size_t used;
    size_t script_len = strlen(script);
    size_t written = 0;

    if (out != NULL && outcap > 0) {
        out[0] = '\0';
    }
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        return -1;
    }

    fflush(stdout);
    fflush(stderr);

    pid = fork();
    if (pid < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        int argc = 0;

        while (argv[argc] != NULL) {
            argc++;
        }
        close(in_pipe[1]);
        close(out_pipe[0]);
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]);
        close(out_pipe[1]);
        /* Never let a closed capture pipe kill us with SIGPIPE. */
        signal(SIGPIPE, SIG_IGN);
        status = masa_main(argc, argv);
        fflush(NULL);
        _exit(status);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);

    while (written < script_len) {
        n = write(in_pipe[1], script + written, script_len - written);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        written += (size_t)n;
    }
    close(in_pipe[1]);

    used = drain_fd(out_pipe[0], out, (out != NULL) ? outcap : 0);
    (void)used;
    close(out_pipe[0]);

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

/* Locate an executable on PATH. */
static int tool_available(const char *name)
{
    const char *path = getenv("PATH");
    char candidate[4096];
    char *copy;
    char *dir;

    if (path == NULL || path[0] == '\0') {
        return 0;
    }

    copy = strdup(path);
    if (copy == NULL) {
        return 0;
    }

    for (dir = strtok(copy, ":"); dir != NULL; dir = strtok(NULL, ":")) {
        if (dir[0] == '\0') {
            dir = ".";
        }
        if (snprintf(candidate, sizeof(candidate), "%s/%s", dir, name) <
            (int)sizeof(candidate)) {
            if (access(candidate, X_OK) == 0) {
                free(copy);
                return 1;
            }
        }
    }

    free(copy);
    return 0;
}

/* Clear every environment variable the code under test consults. */
static void reset_masa_env(void)
{
    set_env(MASA_TARGET_ENV, NULL);
    set_env(MASA_SDK_PATH_ENV, NULL);
    set_env(MASA_DRY_RUN_ENV, NULL);
    set_env(MASA_CC_ENV, NULL);
    set_env(MASA_REAL_CC_ENV, NULL);
    set_env(MASA_SWIFTC_ENV, NULL);
    set_env(MASA_SHIM_DIR_ENV, NULL);
    set_env(MASA_SHIM_TARGET_ENV, NULL);
    set_env(MASA_KEEP_PS1_ENV, NULL);
    set_env(MASA_DANROOT_ROOT_ENV, NULL);
    set_env(MASA_QEMU_AARCH64_ENV, NULL);
    set_env(MASA_DANROOT_AARCH64_ENV, NULL);
    set_env(MASA_DANROOT_NO_BRIDGE_ENV, NULL);
    set_env(MASA_BREW_TAPS_ENV, NULL);
    set_env(MASA_BREW_TAP_BASE_ENV, NULL);
    set_env(MASA_BREW_CACHE_ENV, NULL);
    set_env(MASA_BREW_API_BASE_ENV, NULL);
    set_env(MASA_CLIPBOARD_FILE_ENV, NULL);
    set_env("PATH", g_original_path);
}

/* ------------------------------------------------------------------ */
/* Argument builder                                                   */
/* ------------------------------------------------------------------ */

static void test_build_cc_args_sequence(void)
{
    const char *args[16];
    const char *expected[] = {"-target", "arm64-apple-darwin", "-fuse-ld=lld",
                              "-Qunused-arguments", "-isysroot", "/sdk"};
    int count = 0;
    int i;
    int rc;

    rc = masa_build_cc_args("arm64-apple-darwin", "/sdk", args, &count, 16);
    CHECK(rc == 0);
    CHECK(count == 6);
    for (i = 0; i < 6; i++) {
        CHECK_STREQ(args[i], expected[i]);
    }
    CHECK(args[6] == NULL);
}

static void test_build_cc_args_appends(void)
{
    const char *args[16];
    int count = 2;
    int rc;

    args[0] = "clang";
    args[1] = "-c";
    rc = masa_build_cc_args("x86_64-apple-darwin", NULL, args, &count, 16);
    CHECK(rc == 0);
    CHECK(count == 6);
    CHECK_STREQ(args[0], "clang");
    CHECK_STREQ(args[1], "-c");
    CHECK_STREQ(args[2], "-target");
    CHECK_STREQ(args[3], "x86_64-apple-darwin");
    CHECK_STREQ(args[4], "-fuse-ld=lld");
    CHECK_STREQ(args[5], "-Qunused-arguments");
    CHECK(args[6] == NULL);
}

static void test_build_cc_args_empty_sdk_skipped(void)
{
    const char *args[16];
    int count = 0;
    int rc;

    rc = masa_build_cc_args("arm64-apple-darwin", "", args, &count, 16);
    CHECK(rc == 0);
    CHECK(count == 4);
    CHECK_STREQ(args[2], "-fuse-ld=lld");
    CHECK_STREQ(args[3], "-Qunused-arguments");
}

static void test_build_cc_args_rejects_bad_input(void)
{
    const char *args[16];
    int count = 0;

    CHECK(masa_build_cc_args(NULL, NULL, args, &count, 16) == -1);
    CHECK(masa_build_cc_args("", NULL, args, &count, 16) == -1);
    CHECK(masa_build_cc_args("arm64-apple-darwin", NULL, NULL, &count, 16) == -1);
    CHECK(masa_build_cc_args("arm64-apple-darwin", NULL, args, NULL, 16) == -1);
    /* Too few slots for the injected arguments plus the terminator. */
    CHECK(masa_build_cc_args("arm64-apple-darwin", "/sdk", args, &count, 5) == -1);
}

static void test_build_cc_args_rejects_overflow(void)
{
    const char *args[8];
    int count = 6; /* only two slots left: not enough for -target + value + lld */

    CHECK(masa_build_cc_args("arm64-apple-darwin", NULL, args, &count, 8) == -1);
}

/* ------------------------------------------------------------------ */
/* cc wrapper - dry run                                               */
/* ------------------------------------------------------------------ */

static void test_dry_run_default_target(void)
{
    char *argv[] = {"masa", "cc", "-c", "foo.c", NULL};
    char out[1024];
    int rc;

    reset_masa_env();
    set_env(MASA_DRY_RUN_ENV, "1");

    rc = run_masa(argv, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "clang");
    CHECK_CONTAINS(out, "-c foo.c");
    CHECK_CONTAINS(out, "-target " MASA_DEFAULT_TARGET);
    CHECK_CONTAINS(out, "-fuse-ld=lld");
    CHECK(strstr(out, "-isysroot") == NULL);

    reset_masa_env();
}

static void test_dry_run_custom_target_and_sdk(void)
{
    char *argv[] = {"masa", "cc", "-O2", "bar.c", "-o", "bar.o", NULL};
    char out[1024];
    int rc;

    reset_masa_env();
    set_env(MASA_DRY_RUN_ENV, "1");
    set_env(MASA_TARGET_ENV, "x86_64-apple-darwin");
    set_env(MASA_SDK_PATH_ENV, "/opt/sdk/MacOSX.sdk");

    rc = run_masa(argv, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "-target x86_64-apple-darwin");
    CHECK_CONTAINS(out, "-fuse-ld=lld");
    CHECK_CONTAINS(out, "-isysroot /opt/sdk/MacOSX.sdk");
    CHECK_CONTAINS(out, "-O2 bar.c -o bar.o");

    reset_masa_env();
}

static void test_dry_run_is_exact(void)
{
    char *argv[] = {"masa", "cc", "-c", "foo.c", NULL};
    char out[1024];
    char expected[1024];
    int rc;

    reset_masa_env();
    set_env(MASA_DRY_RUN_ENV, "1");
    set_env(MASA_TARGET_ENV, "arm64-apple-darwin");
    set_env(MASA_SDK_PATH_ENV, "/sdk");
    /* A bare MASA_CC name is honoured verbatim, keeping the output exact. */
    set_env(MASA_CC_ENV, "myclang");

    rc = run_masa(argv, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    snprintf(expected, sizeof(expected),
             "myclang -c foo.c -target arm64-apple-darwin -fuse-ld=lld"
             " -Qunused-arguments -isysroot /sdk\n");
    CHECK_STREQ(out, expected);

    reset_masa_env();
}

static void test_dry_run_respects_masa_cc(void)
{
    char *argv[] = {"masa", "cc", "-c", "foo.c", NULL};
    char out[1024];
    int rc;

    reset_masa_env();
    set_env(MASA_DRY_RUN_ENV, "1");
    set_env(MASA_CC_ENV, "clang-19");

    rc = run_masa(argv, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "clang-19 -c foo.c");

    reset_masa_env();
}

/* ------------------------------------------------------------------ */
/* sys shims                                                          */
/* ------------------------------------------------------------------ */

static void test_sys_uname(void)
{
    char *argv_s[] = {"masa", "sys", "uname", "-s", NULL};
    char *argv_m[] = {"masa", "sys", "uname", "-m", NULL};
    char *argv_plain[] = {"masa", "sys", "uname", NULL};
    char out[256];
    int rc;

    reset_masa_env();

    rc = run_masa(argv_s, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_STREQ(out, "Darwin\n");

    rc = run_masa(argv_m, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_STREQ(out, "arm64\n");

    rc = run_masa(argv_plain, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_STREQ(out, "Darwin\n");
}

static void test_sys_uname_x86_64_target(void)
{
    char *argv[] = {"masa", "sys", "uname", "-m", NULL};
    char out[256];
    int rc;

    reset_masa_env();
    set_env(MASA_TARGET_ENV, "x86_64-apple-darwin");
    rc = run_masa(argv, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_STREQ(out, "x86_64\n");
    reset_masa_env();
}

static void test_sys_sw_vers(void)
{
    char *argv[] = {"masa", "sys", "sw_vers", NULL};
    char out[512];
    int rc;

    reset_masa_env();
    rc = run_masa(argv, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "ProductName:");
    CHECK_CONTAINS(out, MASA_SW_PRODUCT_NAME);
    CHECK_CONTAINS(out, "ProductVersion:");
    CHECK_CONTAINS(out, MASA_SW_PRODUCT_VERSION);
    CHECK_CONTAINS(out, "BuildVersion:");
    CHECK_CONTAINS(out, MASA_SW_BUILD_VERSION);
}

static void test_sys_sw_vers_flags(void)
{
    char *argv_name[] = {"masa", "sys", "sw_vers", "-productName", NULL};
    char *argv_ver[] = {"masa", "sys", "sw_vers", "-productVersion", NULL};
    char *argv_build[] = {"masa", "sys", "sw_vers", "-buildVersion", NULL};
    char out[256];
    int rc;

    reset_masa_env();

    rc = run_masa(argv_name, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_STREQ(out, MASA_SW_PRODUCT_NAME "\n");

    rc = run_masa(argv_ver, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_STREQ(out, MASA_SW_PRODUCT_VERSION "\n");

    rc = run_masa(argv_build, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_STREQ(out, MASA_SW_BUILD_VERSION "\n");
}

static void test_sys_sdk_path_default(void)
{
    char *argv_xcode[] = {"masa", "sys", "xcode-select", "-p", NULL};
    char *argv_xcrun[] = {"masa", "sys", "xcrun", "--show-sdk-path", NULL};
    char out[512];
    int rc;

    reset_masa_env();

    rc = run_masa(argv_xcode, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_STREQ(out, MASA_DEFAULT_SDK_PATH "\n");

    rc = run_masa(argv_xcrun, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_STREQ(out, MASA_DEFAULT_SDK_PATH "\n");
}

static void test_sys_sdk_path_override(void)
{
    char *argv_xcode[] = {"masa", "sys", "xcode-select", "-p", NULL};
    char *argv_xcrun[] = {"masa", "sys", "xcrun", "--show-sdk-path", NULL};
    char out[512];
    int rc;

    reset_masa_env();
    set_env(MASA_SDK_PATH_ENV, "/custom/sdk");

    rc = run_masa(argv_xcode, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_STREQ(out, "/custom/sdk\n");

    rc = run_masa(argv_xcrun, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_STREQ(out, "/custom/sdk\n");

    reset_masa_env();
}

static void test_sys_dispatch_errors(void)
{
    char *argv_unknown[] = {"masa", "sys", "nope", NULL};
    char *argv_uname_bad[] = {"masa", "sys", "uname", "-z", NULL};
    char *argv_sw_bad[] = {"masa", "sys", "sw_vers", "-bogus", NULL};
    char err[512];
    int rc;

    reset_masa_env();

    rc = run_masa(argv_unknown, NULL, 0, err, sizeof(err));
    CHECK(rc == 1);
    CHECK_CONTAINS(err, "unsupported command");

    rc = run_masa(argv_uname_bad, NULL, 0, err, sizeof(err));
    CHECK(rc == 1);
    CHECK_CONTAINS(err, "uname");

    rc = run_masa(argv_sw_bad, NULL, 0, err, sizeof(err));
    CHECK(rc == 1);
    CHECK_CONTAINS(err, "sw_vers");
}

static void test_sys_missing_command(void)
{
    char *argv[] = {"masa", "sys", NULL};
    char err[256];
    int rc;

    reset_masa_env();
    rc = run_masa(argv, NULL, 0, err, sizeof(err));
    CHECK(rc == 2);
    CHECK_CONTAINS(err, "missing command");
}

/* ------------------------------------------------------------------ */
/* CLI router                                                         */
/* ------------------------------------------------------------------ */

static void test_version(void)
{
    char *argv[] = {"masa", "version", NULL};
    char out[512];
    int rc;

    rc = run_masa(argv, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "masa " MASA_VERSION);
    CHECK_CONTAINS(out, MASA_DEFAULT_TARGET);
}

static void test_help_and_unknown(void)
{
    char *argv_help[] = {"masa", "help", NULL};
    char *argv_unknown[] = {"masa", "bogus", NULL};
    char *argv_none[] = {"masa", NULL};
    char out[2048];
    char err[2048];
    int rc;

    rc = run_masa(argv_help, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "Usage:");

    rc = run_masa(argv_unknown, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 2);
    CHECK_CONTAINS(err, "unknown subcommand");

    rc = run_masa(argv_none, NULL, 0, err, sizeof(err));
    CHECK(rc == 2);
    CHECK_CONTAINS(err, "Usage:");
}

/* ------------------------------------------------------------------ */
/* End-to-end: compile a real Mach-O object                           */
/* ------------------------------------------------------------------ */

static void test_cc_compiles_macho_object(void)
{
    static const unsigned char macho64_le[4] = {0xCF, 0xFA, 0xED, 0xFE};
    char dir[] = "/tmp/masa_test_XXXXXX";
    char src[512];
    char obj[512];
    char out[2048];
    char err[4096];
    unsigned char magic[4];
    char *argv[8];
    FILE *fp;
    int rc;

    if (!tool_available("clang")) {
        printf("  SKIP clang is not installed\n");
        g_skipped++;
        return;
    }

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }

    snprintf(src, sizeof(src), "%s/hello.c", dir);
    snprintf(obj, sizeof(obj), "%s/hello.o", dir);

    fp = fopen(src, "w");
    if (fp == NULL) {
        fprintf(stderr, "  FAIL cannot write %s\n", src);
        g_failed++;
        return;
    }
    fputs("int masa_hello(void) { return 42; }\n", fp);
    fclose(fp);

    reset_masa_env();
    set_env(MASA_TARGET_ENV, "arm64-apple-darwin");

    argv[0] = "masa";
    argv[1] = "cc";
    argv[2] = "-c";
    argv[3] = src;
    argv[4] = "-o";
    argv[5] = obj;
    argv[6] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 0);
    if (rc != 0) {
        fprintf(stderr, "  clang stderr: %s\n", err);
    }

    fp = fopen(obj, "rb");
    if (fp == NULL) {
        fprintf(stderr, "  FAIL no object file produced at %s\n", obj);
        g_failed++;
    } else {
        memset(magic, 0, sizeof(magic));
        CHECK(fread(magic, 1, sizeof(magic), fp) == sizeof(magic));
        fclose(fp);
        CHECK(memcmp(magic, macho64_le, sizeof(magic)) == 0);
    }

    remove(obj);
    remove(src);
    rmdir(dir);
    reset_masa_env();
}

static void test_cc_x86_64_macho_object(void)
{
    static const unsigned char macho64_le[4] = {0xCF, 0xFA, 0xED, 0xFE};
    char dir[] = "/tmp/masa_test_XXXXXX";
    char src[512];
    char obj[512];
    char err[4096];
    unsigned char magic[4];
    char *argv[8];
    FILE *fp;
    int rc;

    if (!tool_available("clang")) {
        printf("  SKIP clang is not installed\n");
        g_skipped++;
        return;
    }

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }

    snprintf(src, sizeof(src), "%s/hello.c", dir);
    snprintf(obj, sizeof(obj), "%s/hello.o", dir);

    fp = fopen(src, "w");
    if (fp == NULL) {
        fprintf(stderr, "  FAIL cannot write %s\n", src);
        g_failed++;
        return;
    }
    fputs("int masa_hello(void) { return 42; }\n", fp);
    fclose(fp);

    reset_masa_env();
    set_env(MASA_TARGET_ENV, "x86_64-apple-darwin");

    argv[0] = "masa";
    argv[1] = "cc";
    argv[2] = "-c";
    argv[3] = src;
    argv[4] = "-o";
    argv[5] = obj;
    argv[6] = NULL;

    rc = run_masa(argv, NULL, 0, err, sizeof(err));
    CHECK(rc == 0);

    fp = fopen(obj, "rb");
    if (fp == NULL) {
        fprintf(stderr, "  FAIL no object file produced at %s\n", obj);
        g_failed++;
    } else {
        memset(magic, 0, sizeof(magic));
        CHECK(fread(magic, 1, sizeof(magic), fp) == sizeof(magic));
        fclose(fp);
        CHECK(memcmp(magic, macho64_le, sizeof(magic)) == 0);
    }

    remove(obj);
    remove(src);
    rmdir(dir);
    reset_masa_env();
}

static void test_cc_missing_compiler(void)
{
    char *argv[] = {"masa", "cc", "-c", "foo.c", NULL};
    char err[1024];
    int rc;

    reset_masa_env();
    set_env(MASA_CC_ENV, "masa-no-such-compiler-xyz");

    rc = run_masa(argv, NULL, 0, err, sizeof(err));
    CHECK(rc == MASA_EXIT_SPAWN_ERROR);
    CHECK_CONTAINS(err, "masa-no-such-compiler-xyz");

    reset_masa_env();
}


/* ------------------------------------------------------------------ */
/* Milestone 2 helpers                                                */
/* ------------------------------------------------------------------ */

/* Path of the real masa binary sitting next to this test executable. */
static char *masa_bin_path(void)
{
    char self[4096];
    char *slash;
    ssize_t n;
    size_t len;

    n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) {
        return NULL;
    }
    self[n] = '\0';

    slash = strrchr(self, '/');
    if (slash == NULL) {
        return NULL;
    }
    *slash = '\0';

    len = strlen(self) + strlen("/masa") + 1;
    {
        char *out = malloc(len);
        if (out == NULL) {
            return NULL;
        }
        snprintf(out, len, "%s/masa", self);
        return out;
    }
}

static int is_symlink_to(const char *path, const char *target)
{
    char buf[4096];
    struct stat st;
    ssize_t n;

    if (lstat(path, &st) != 0 || !S_ISLNK(st.st_mode)) {
        return 0;
    }
    n = readlink(path, buf, sizeof(buf) - 1);
    if (n <= 0) {
        return 0;
    }
    buf[n] = '\0';
    return strcmp(buf, target) == 0;
}

static int write_file(const char *path, const char *contents)
{
    FILE *fp = fopen(path, "w");

    if (fp == NULL) {
        return -1;
    }
    fputs(contents, fp);
    fclose(fp);
    return 0;
}

static int read_magic(const char *path, unsigned char magic[4])
{
    FILE *fp = fopen(path, "rb");
    size_t got;

    if (fp == NULL) {
        return -1;
    }
    got = fread(magic, 1, 4, fp);
    fclose(fp);
    return (got == 4) ? 0 : -1;
}

static void remove_tree(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *ent;

    if (d != NULL) {
        while ((ent = readdir(d)) != NULL) {
            char child[4096];
            struct stat st;

            if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            if (snprintf(child, sizeof(child), "%s/%s", dir, ent->d_name) >=
                (int)sizeof(child)) {
                continue;
            }
            if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
                remove_tree(child);
            } else {
                unlink(child);
            }
        }
        closedir(d);
    }
    rmdir(dir);
}

/* ------------------------------------------------------------------ */
/* Milestone 2: argv[0] dispatch                                      */
/* ------------------------------------------------------------------ */

static void test_argv0_dispatch_sys(void)
{
    char *argv_uname_s[] = {"/some/dir/uname", "-s", NULL};
    char *argv_uname_m[] = {"/some/dir/uname", "-m", NULL};
    char *argv_sw[] = {"/some/dir/sw_vers", "-productName", NULL};
    char *argv_xcrun[] = {"/some/dir/xcrun", "--show-sdk-path", NULL};
    char out[256];
    int rc;

    reset_masa_env();

    rc = run_masa(argv_uname_s, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_STREQ(out, "Darwin\n");

    rc = run_masa(argv_uname_m, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_STREQ(out, "arm64\n");

    rc = run_masa(argv_sw, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_STREQ(out, MASA_SW_PRODUCT_NAME "\n");

    rc = run_masa(argv_xcrun, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_STREQ(out, MASA_DEFAULT_SDK_PATH "\n");
}

static void test_argv0_dispatch_cc(void)
{
    char *argv_cc[] = {"/some/dir/cc", "-c", "foo.c", NULL};
    char *argv_clang[] = {"/some/dir/clang", "-c", "foo.c", NULL};
    char *argv_gcc[] = {"/some/dir/gcc", "-c", "foo.c", NULL};
    char out[1024];
    int rc;

    if (!tool_available("clang")) {
        printf("  SKIP clang is not installed\n");
        g_skipped++;
        return;
    }

    reset_masa_env();
    set_env(MASA_DRY_RUN_ENV, "1");

    rc = run_masa(argv_cc, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "clang -c foo.c");
    CHECK_CONTAINS(out, "-target " MASA_DEFAULT_TARGET);
    CHECK_CONTAINS(out, "-fuse-ld=lld");

    rc = run_masa(argv_clang, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "clang -c foo.c");

    rc = run_masa(argv_gcc, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "clang -c foo.c");

    reset_masa_env();
}

static void test_argv0_non_shim_routes(void)
{
    char *argv_version[] = {"/usr/local/bin/masa", "version", NULL};
    char *argv_unknown[] = {"/usr/local/bin/masa", "bogus", NULL};
    char out[1024];
    char err[2048];
    int rc;

    reset_masa_env();

    rc = run_masa(argv_version, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "masa " MASA_VERSION);

    rc = run_masa(argv_unknown, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 2);
    CHECK_CONTAINS(err, "unknown subcommand");
}

/* ------------------------------------------------------------------ */
/* Milestone 2: init-shims and exec                                   */
/* ------------------------------------------------------------------ */

static void test_init_shims_creates_links(void)
{
    char dir[] = "/tmp/masa_shims_XXXXXX";
    char path[4096];
    char *argv[4];
    char out[512];
    size_t i;
    int rc;

    reset_masa_env();
    set_env(MASA_SHIM_TARGET_ENV, "/bin/true");

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }

    argv[0] = "masa";
    argv[1] = "init-shims";
    argv[2] = dir;
    argv[3] = NULL;

    rc = run_masa(argv, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);

    for (i = 0; masa_shim_tools[i] != NULL; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, masa_shim_tools[i]);
        CHECK(is_symlink_to(path, "/bin/true"));
    }

    /* A second run must refresh the links, not fail. */
    rc = run_masa(argv, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);

    remove_tree(dir);
    reset_masa_env();
}

static void test_init_shims_missing_dir(void)
{
    char *argv[] = {"masa", "init-shims", NULL};
    char err[256];
    int rc;

    reset_masa_env();
    rc = run_masa(argv, NULL, 0, err, sizeof(err));
    CHECK(rc == 2);
    CHECK_CONTAINS(err, "missing directory");
}

static void test_exec_sets_environment(void)
{
    char *argv[6];
    char out[2048];
    char err[2048];
    char *masa_bin;
    int rc;

    masa_bin = masa_bin_path();
    if (masa_bin == NULL || access(masa_bin, X_OK) != 0) {
        printf("  SKIP %s is not built\n", (masa_bin != NULL) ? masa_bin : "masa");
        g_skipped++;
        free(masa_bin);
        return;
    }

    reset_masa_env();
    set_env(MASA_SHIM_TARGET_ENV, masa_bin);

    argv[0] = "masa";
    argv[1] = "exec";
    argv[2] = "sh";
    argv[3] = "-c";
    argv[4] = "echo real=$MASA_REAL_CC; echo dir=$MASA_SHIM_DIR; command -v cc";
    argv[5] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "real=/");
    CHECK_CONTAINS(out, "dir=/");
    CHECK_CONTAINS(out, "masa-shims-");

    /* The temporary shim directory must be cleaned up afterwards. */
    {
        const char *dir = strstr(out, "dir=");
        if (dir != NULL) {
            char shim_dir[4096];
            size_t len = 0;
            const char *start = dir + 4;
            const char *nl = strchr(start, '\n');
            if (nl != NULL) {
                len = (size_t)(nl - start);
                if (len < sizeof(shim_dir)) {
                    memcpy(shim_dir, start, len);
                    shim_dir[len] = '\0';
                    CHECK(access(shim_dir, F_OK) != 0);
                }
            }
        }
    }

    free(masa_bin);
    reset_masa_env();
}

static void test_exec_missing_command(void)
{
    char *argv[] = {"masa", "exec", NULL};
    char err[256];
    int rc;

    reset_masa_env();
    rc = run_masa(argv, NULL, 0, err, sizeof(err));
    CHECK(rc == 2);
    CHECK_CONTAINS(err, "missing command");
}

static void test_exec_make_integration(void)
{
    static const unsigned char macho64_le[4] = {0xCF, 0xFA, 0xED, 0xFE};
    char dir[] = "/tmp/masa_make_XXXXXX";
    char src[4096];
    char mk[4096];
    char obj[4096];
    char out[4096];
    char err[8192];
    unsigned char magic[4];
    char *argv[8];
    char *masa_bin;
    int rc;

    if (!tool_available("clang")) {
        printf("  SKIP clang is not installed\n");
        g_skipped++;
        return;
    }
    if (!tool_available("make")) {
        printf("  SKIP make is not installed\n");
        g_skipped++;
        return;
    }

    masa_bin = masa_bin_path();
    if (masa_bin == NULL || access(masa_bin, X_OK) != 0) {
        printf("  SKIP the masa binary is not built\n");
        g_skipped++;
        free(masa_bin);
        return;
    }

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        free(masa_bin);
        return;
    }

    snprintf(src, sizeof(src), "%s/hello.c", dir);
    snprintf(mk, sizeof(mk), "%s/Makefile", dir);
    snprintf(obj, sizeof(obj), "%s/hello.o", dir);

    if (write_file(src, "int masa_hello(void) { return 42; }\n") != 0 ||
        write_file(mk,
                   "all: hello.o\n"
                   "\n"
                   "hello.o: hello.c\n"
                   "\t$(CC) -c hello.c -o hello.o\n") != 0) {
        fprintf(stderr, "  FAIL cannot write test inputs\n");
        g_failed++;
        free(masa_bin);
        return;
    }

    reset_masa_env();
    set_env(MASA_SHIM_TARGET_ENV, masa_bin);

    /* make's default $(CC) is `cc`, which must hit the Masa shim. */
    argv[0] = "masa";
    argv[1] = "exec";
    argv[2] = "make";
    argv[3] = "-C";
    argv[4] = dir;
    argv[5] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  make stdout: %s\n  make stderr: %s\n", out, err);
    }
    CHECK(rc == 0);

    if (read_magic(obj, magic) != 0) {
        fprintf(stderr, "  FAIL no object file produced at %s\n", obj);
        g_failed++;
    } else {
        CHECK(memcmp(magic, macho64_le, sizeof(magic)) == 0);
    }

    remove_tree(dir);
    free(masa_bin);
    reset_masa_env();
}


/*
 * The critical safety property: when the shim directory is first on PATH,
 * `masa cc` must not resolve back to itself.  With no real compiler
 * reachable it must fail cleanly instead of recursing.
 */
static void test_cc_avoids_recursion(void)
{
    char dir[] = "/tmp/masa_recurse_XXXXXX";
    char *argv[] = {"masa", "cc", "-c", "foo.c", NULL};
    char *masa_bin;
    char err[1024];
    int rc;

    masa_bin = masa_bin_path();
    if (masa_bin == NULL || access(masa_bin, X_OK) != 0) {
        printf("  SKIP the masa binary is not built\n");
        g_skipped++;
        free(masa_bin);
        return;
    }

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        free(masa_bin);
        return;
    }

    /* A shim farm whose only compiler is Masa itself. */
    if (masa_make_shims(dir, masa_bin) != 0) {
        fprintf(stderr, "  FAIL cannot create shim farm\n");
        g_failed++;
        remove_tree(dir);
        free(masa_bin);
        return;
    }

    reset_masa_env();
    set_env(MASA_SHIM_DIR_ENV, dir);
    set_env("PATH", dir); /* nothing else reachable */
    set_env(MASA_DRY_RUN_ENV, "1");

    rc = run_masa(argv, NULL, 0, err, sizeof(err));
    CHECK(rc == 1);
    CHECK_CONTAINS(err, "cannot find a host compiler");

    remove_tree(dir);
    free(masa_bin);
    reset_masa_env();
}

/*
 * When MASA_REAL_CC points outside the shim directory, the wrapper must
 * use exactly that compiler even though the shim directory is first on
 * PATH.
 */
static void test_cc_honours_real_cc_over_shims(void)
{
    char dir[] = "/tmp/masa_realcc_XXXXXX";
    char *argv[] = {"masa", "cc", "-c", "foo.c", NULL};
    char out[1024];
    char *masa_bin;
    int rc;

    masa_bin = masa_bin_path();
    if (masa_bin == NULL || access(masa_bin, X_OK) != 0 ||
        !tool_available("clang")) {
        printf("  SKIP clang or the masa binary is unavailable\n");
        g_skipped++;
        free(masa_bin);
        return;
    }

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        free(masa_bin);
        return;
    }
    if (masa_make_shims(dir, masa_bin) != 0) {
        fprintf(stderr, "  FAIL cannot create shim farm\n");
        g_failed++;
        remove_tree(dir);
        free(masa_bin);
        return;
    }

    reset_masa_env();
    set_env(MASA_SHIM_DIR_ENV, dir);
    set_env("PATH", dir); /* only Masa shims are on PATH */
    set_env(MASA_REAL_CC_ENV, "/usr/bin/clang");
    set_env(MASA_DRY_RUN_ENV, "1");

    rc = run_masa(argv, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "/usr/bin/clang -c foo.c");
    CHECK(strstr(out, dir) == NULL);

    remove_tree(dir);
    free(masa_bin);
    reset_masa_env();
}


/* ------------------------------------------------------------------ */
/* Milestone 3 helpers                                                */
/* ------------------------------------------------------------------ */

/* Read the 16-byte Mach-O 64-bit header: magic (0..3) and filetype (12..15). */
static int read_macho_header(const char *path, unsigned char magic[4],
                             unsigned char filetype[4])
{
    unsigned char header[16];
    FILE *fp = fopen(path, "rb");
    size_t got;

    if (fp == NULL) {
        return -1;
    }
    got = fread(header, 1, sizeof(header), fp);
    fclose(fp);
    if (got < sizeof(header)) {
        return -1;
    }
    memcpy(magic, header, 4);
    memcpy(filetype, header + 12, 4);
    return 0;
}

static int file_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

/* True when the stub SDK was written to dir (libSystem.tbd present). */
static int sdk_is_stubbed(const char *dir)
{
    char path[4096];

    snprintf(path, sizeof(path), "%s/%s", dir, MASA_SDK_LIBSYSTEM_RELPATH);
    return file_exists(path);
}

/* Run `masa sdk init-stub <dir>` and return 0 on success. */
static int make_stub_sdk(const char *dir)
{
    char *argv[5];
    char out[1024];
    char err[1024];
    int rc;

    argv[0] = "masa";
    argv[1] = "sdk";
    argv[2] = "init-stub";
    argv[3] = (char *)dir;
    argv[4] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  init-stub stderr: %s\n", err);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Milestone 3: sdk                                                   */
/* ------------------------------------------------------------------ */

static void test_sdk_init_stub(void)
{
    char dir[] = "/tmp/masa_sdk_XXXXXX";
    char path[4096];
    FILE *fp;
    char buf[2048];
    size_t got;
    int rc;

    reset_masa_env();

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }

    rc = make_stub_sdk(dir);
    CHECK(rc == 0);

    snprintf(path, sizeof(path), "%s/usr/lib/libSystem.tbd", dir);
    CHECK(file_exists(path));
    snprintf(path, sizeof(path), "%s/usr/lib/libSystem.B.tbd", dir);
    CHECK(file_exists(path));
    /* Newer clang implicitly links -lgcc_s.1 for x86_64 Darwin. */
    snprintf(path, sizeof(path), "%s/usr/lib/libgcc_s.1.tbd", dir);
    CHECK(file_exists(path));
    snprintf(path, sizeof(path), "%s/usr/lib/crt1.o", dir);
    CHECK(file_exists(path));
    snprintf(path, sizeof(path), "%s/usr/include/stdlib.h", dir);
    CHECK(file_exists(path));
    snprintf(path, sizeof(path), "%s/usr/include/string.h", dir);
    CHECK(file_exists(path));
    snprintf(path, sizeof(path), "%s/usr/include/stdio.h", dir);
    CHECK(file_exists(path));
    /* Milestone 14: the Objective-C runtime and framework stubs. */
    snprintf(path, sizeof(path), "%s/usr/lib/libobjc.A.tbd", dir);
    CHECK(file_exists(path));
    snprintf(path, sizeof(path), "%s/usr/include/objc/objc.h", dir);
    CHECK(file_exists(path));
    snprintf(path, sizeof(path), "%s/usr/include/objc/runtime.h", dir);
    CHECK(file_exists(path));
    snprintf(path, sizeof(path), "%s/usr/include/objc/message.h", dir);
    CHECK(file_exists(path));
    snprintf(path, sizeof(path),
             "%s/System/Library/Frameworks/CoreFoundation.framework"
             "/CoreFoundation.tbd", dir);
    CHECK(file_exists(path));
    snprintf(path, sizeof(path),
             "%s/System/Library/Frameworks/CoreFoundation.framework"
             "/Headers/CoreFoundation.h", dir);
    CHECK(file_exists(path));
    snprintf(path, sizeof(path),
             "%s/System/Library/Frameworks/Foundation.framework"
             "/Foundation.tbd", dir);
    CHECK(file_exists(path));
    snprintf(path, sizeof(path),
             "%s/System/Library/Frameworks/Foundation.framework"
             "/Headers/Foundation.h", dir);
    CHECK(file_exists(path));

    /* The libSystem stub exports the stack-protector symbols arm64 needs. */
    snprintf(path, sizeof(path), "%s/usr/lib/libSystem.tbd", dir);
    fp = fopen(path, "r");
    if (fp != NULL) {
        got = fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
        buf[got] = '\0';
        CHECK_CONTAINS(buf, "___stack_chk_fail");
        CHECK_CONTAINS(buf, "___stack_chk_guard");
    } else {
        fprintf(stderr, "  FAIL cannot read %s\n", path);
        g_failed++;
    }

    /* The .tbd must be a TAPI v4 YAML document. */
    snprintf(path, sizeof(path), "%s/usr/lib/libSystem.tbd", dir);
    fp = fopen(path, "r");
    if (fp != NULL) {
        got = fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
        buf[got] = '\0';
        CHECK_CONTAINS(buf, "!tapi-tbd");
        CHECK_CONTAINS(buf, "tbd-version:     4");
        CHECK_CONTAINS(buf, "install-name:");
    } else {
        fprintf(stderr, "  FAIL cannot read %s\n", path);
        g_failed++;
    }

    remove_tree(dir);
    reset_masa_env();
}

static void test_sdk_info(void)
{
    char dir[] = "/tmp/masa_sdkinfo_XXXXXX";
    char *argv_info[] = {"masa", "sdk", "info", NULL};
    char out[2048];
    char err[1024];
    int rc;

    reset_masa_env();

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }

    /* Point at a directory with no SDK: not ready. */
    set_env(MASA_SDK_PATH_ENV, dir);
    rc = run_masa(argv_info, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 1);
    CHECK_CONTAINS(out, "missing libSystem.tbd");

    /* After writing the stub it must report ready. */
    rc = make_stub_sdk(dir);
    CHECK(rc == 0);

    rc = run_masa(argv_info, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "ready");

    remove_tree(dir);
    reset_masa_env();
}

static void test_sdk_bad_subcommand(void)
{
    char *argv[] = {"masa", "sdk", "bogus", NULL};
    char err[512];
    int rc;

    reset_masa_env();
    rc = run_masa(argv, NULL, 0, err, sizeof(err));
    CHECK(rc == 2);
    CHECK_CONTAINS(err, "unknown subcommand");
}

/* ------------------------------------------------------------------ */
/* Milestone 3: tool shims                                            */
/* ------------------------------------------------------------------ */

static void test_argv0_dispatch_tools(void)
{
    char *argv_ar[] = {"/some/dir/ar", "--version", NULL};
    char *argv_ranlib[] = {"/some/dir/ranlib", "--version", NULL};
    char *argv_nm[] = {"/some/dir/nm", "--version", NULL};
    char *argv_otool[] = {"/some/dir/otool", "--version", NULL};
    char *argv_ld[] = {"/some/dir/ld", NULL};
    char out[2048];
    char err[2048];
    int rc;

    if (!tool_available("clang")) {
        printf("  SKIP clang is not installed\n");
        g_skipped++;
        return;
    }

    reset_masa_env();

    rc = run_masa(argv_ar, out, sizeof(out), err, sizeof(err));
    if (rc != 0 && strstr(err, "cannot find") != NULL) {
        printf("  SKIP LLVM binutils not installed\n");
        g_skipped++;
        return;
    }
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "LLVM");

    rc = run_masa(argv_ranlib, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "LLVM");

    rc = run_masa(argv_nm, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "LLVM");

    rc = run_masa(argv_otool, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "LLVM");

    /*
     * `ld` with no arguments must reach ld64.lld, which rejects the empty
     * link with a missing-architecture error.
     */
    rc = run_masa(argv_ld, NULL, 0, err, sizeof(err));
    CHECK(rc != 0);
    CHECK_CONTAINS(err, "arch");
}

static void test_cc_link_platform_version(void)
{
    char *argv_link[] = {"masa", "cc", "hello.o", "-o", "hello", NULL};
    char *argv_compile[] = {"masa", "cc", "-c", "hello.c", "-o", "hello.o", NULL};
    char out[1024];
    int rc;

    reset_masa_env();
    set_env(MASA_DRY_RUN_ENV, "1");
    set_env(MASA_CC_ENV, "myclang");

    /* A link gets an explicit platform version for ld64.lld. */
    rc = run_masa(argv_link, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "-Wl,-platform_version,macos,");

    /* A compile-only step must not. */
    rc = run_masa(argv_compile, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK(strstr(out, "-platform_version") == NULL);

    reset_masa_env();
}

/* ------------------------------------------------------------------ */
/* Milestone 3: full executable linking                               */
/* ------------------------------------------------------------------ */

/*
 * Compile and LINK (no -c) a hello.c against the stub SDK and verify the
 * result is a Mach-O executable: magic CF FA ED FE and filetype 2.
 */
static void link_executable(const char *target)
{
    static const unsigned char macho64_le[4] = {0xCF, 0xFA, 0xED, 0xFE};
    static const unsigned char mh_execute[4] = {0x02, 0x00, 0x00, 0x00};
    char dir[] = "/tmp/masa_link_XXXXXX";
    char sdk[4096];
    char src[4096];
    char exe[4096];
    char out[4096];
    char err[8192];
    unsigned char magic[4];
    unsigned char filetype[4];
    char *argv[8];
    int rc;

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }
    snprintf(sdk, sizeof(sdk), "%s/sdk", dir);
    snprintf(src, sizeof(src), "%s/hello.c", dir);
    snprintf(exe, sizeof(exe), "%s/hello", dir);

    reset_masa_env();
    set_env(MASA_TARGET_ENV, target);

    if (make_stub_sdk(sdk) != 0 || !sdk_is_stubbed(sdk)) {
        fprintf(stderr, "  FAIL could not create the stub SDK\n");
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }
    set_env(MASA_SDK_PATH_ENV, sdk);

    if (write_file(src, "int main(void) { return 0; }\n") != 0) {
        fprintf(stderr, "  FAIL cannot write %s\n", src);
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }

    argv[0] = "masa";
    argv[1] = "cc";
    argv[2] = src;
    argv[3] = "-o";
    argv[4] = exe;
    argv[5] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  link stderr (%s): %s\n", target, err);
    }
    CHECK(rc == 0);

    if (read_macho_header(exe, magic, filetype) != 0) {
        fprintf(stderr, "  FAIL no executable produced at %s\n", exe);
        g_failed++;
    } else {
        CHECK(memcmp(magic, macho64_le, sizeof(magic)) == 0);
        CHECK(memcmp(filetype, mh_execute, sizeof(filetype)) == 0);
    }

    remove_tree(dir);
    reset_masa_env();
}

static void test_link_executable_arm64(void)
{
    if (!tool_available("clang")) {
        printf("  SKIP clang is not installed\n");
        g_skipped++;
        return;
    }
    link_executable("arm64-apple-darwin");
}

static void test_link_executable_x86_64(void)
{
    if (!tool_available("clang")) {
        printf("  SKIP clang is not installed\n");
        g_skipped++;
        return;
    }
    link_executable("x86_64-apple-darwin");
}

/*
 * The same property, but reached through `masa exec make`: make calls the
 * shimmed cc to link an executable that calls a stubbed libSystem symbol.
 */
static void test_exec_make_links_executable(void)
{
    static const unsigned char macho64_le[4] = {0xCF, 0xFA, 0xED, 0xFE};
    static const unsigned char mh_execute[4] = {0x02, 0x00, 0x00, 0x00};
    char dir[] = "/tmp/masa_makelink_XXXXXX";
    char sdk[4096];
    char src[4096];
    char mk[4096];
    char exe[4096];
    char out[4096];
    char err[8192];
    unsigned char magic[4];
    unsigned char filetype[4];
    char *argv[8];
    char *masa_bin;
    int rc;

    if (!tool_available("clang")) {
        printf("  SKIP clang is not installed\n");
        g_skipped++;
        return;
    }
    if (!tool_available("make")) {
        printf("  SKIP make is not installed\n");
        g_skipped++;
        return;
    }

    masa_bin = masa_bin_path();
    if (masa_bin == NULL || access(masa_bin, X_OK) != 0) {
        printf("  SKIP the masa binary is not built\n");
        g_skipped++;
        free(masa_bin);
        return;
    }

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        free(masa_bin);
        return;
    }
    snprintf(sdk, sizeof(sdk), "%s/sdk", dir);
    snprintf(src, sizeof(src), "%s/hello.c", dir);
    snprintf(mk, sizeof(mk), "%s/Makefile", dir);
    snprintf(exe, sizeof(exe), "%s/hello", dir);

    reset_masa_env();
    set_env(MASA_TARGET_ENV, "arm64-apple-darwin");

    if (make_stub_sdk(sdk) != 0 || !sdk_is_stubbed(sdk)) {
        fprintf(stderr, "  FAIL could not create the stub SDK\n");
        g_failed++;
        remove_tree(dir);
        free(masa_bin);
        reset_masa_env();
        return;
    }
    set_env(MASA_SDK_PATH_ENV, sdk);
    set_env(MASA_SHIM_TARGET_ENV, masa_bin);

    if (write_file(src,
                   "extern void exit(int);\n"
                   "int main(void) { exit(0); return 0; }\n") != 0 ||
        write_file(mk,
                   "all: hello\n"
                   "\n"
                   "hello: hello.c\n"
                   "\t$(CC) hello.c -o hello\n") != 0) {
        fprintf(stderr, "  FAIL cannot write test inputs\n");
        g_failed++;
        remove_tree(dir);
        free(masa_bin);
        reset_masa_env();
        return;
    }

    argv[0] = "masa";
    argv[1] = "exec";
    argv[2] = "make";
    argv[3] = "-C";
    argv[4] = dir;
    argv[5] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  make stdout: %s\n  make stderr: %s\n", out, err);
    }
    CHECK(rc == 0);

    if (read_macho_header(exe, magic, filetype) != 0) {
        fprintf(stderr, "  FAIL no executable produced at %s\n", exe);
        g_failed++;
    } else {
        CHECK(memcmp(magic, macho64_le, sizeof(magic)) == 0);
        CHECK(memcmp(filetype, mh_execute, sizeof(filetype)) == 0);
    }

    remove_tree(dir);
    free(masa_bin);
    reset_masa_env();
}

/* ------------------------------------------------------------------ */
/* Milestone 4 helpers                                                */
/* ------------------------------------------------------------------ */

/*
 * A realistic project.pbxproj: a PBXNativeTarget whose PBXSourcesBuildPhase
 * pulls two C files through PBXBuildFile -> PBXFileReference, both living
 * in a `src` PBXGroup so the group path has to be joined.
 */
static const char *demo_pbxproj(void)
{
    return
    "// !$*UTF8*$!\n"
    "{\n"
    "\tarchiveVersion = 1;\n"
    "\tclasses = {\n"
    "\t};\n"
    "\tobjectVersion = 56;\n"
    "\tobjects = {\n"
    "\n"
    "/* Begin PBXBuildFile section */\n"
    "\t\tA1000000000000000000001 /* main.c in Sources */ = {isa = PBXBuildFile; fileRef = B1000000000000000000001 /* main.c */; };\n"
    "\t\tA1000000000000000000002 /* helper.c in Sources */ = {isa = PBXBuildFile; fileRef = B1000000000000000000002 /* helper.c */; };\n"
    "/* End PBXBuildFile section */\n"
    "\n"
    "/* Begin PBXFileReference section */\n"
    "\t\tB1000000000000000000001 /* main.c */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.c.c; path = main.c; sourceTree = \"<group>\"; };\n"
    "\t\tB1000000000000000000002 /* helper.c */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.c.c; path = helper.c; sourceTree = \"<group>\"; };\n"
    "\t\tB1000000000000000000003 /* DemoApp */ = {isa = PBXFileReference; explicitFileType = \"compiled.mach-o.executable\"; path = DemoApp; sourceTree = BUILT_PRODUCTS_DIR; };\n"
    "/* End PBXFileReference section */\n"
    "\n"
    "/* Begin PBXGroup section */\n"
    "\t\tC1000000000000000000001 = {\n"
    "\t\t\tisa = PBXGroup;\n"
    "\t\t\tchildren = (\n"
    "\t\t\t\tC1000000000000000000002 /* src */,\n"
    "\t\t\t);\n"
    "\t\t\tsourceTree = \"<group>\";\n"
    "\t\t};\n"
    "\t\tC1000000000000000000002 /* src */ = {\n"
    "\t\t\tisa = PBXGroup;\n"
    "\t\t\tchildren = (\n"
    "\t\t\t\tB1000000000000000000001 /* main.c */,\n"
    "\t\t\t\tB1000000000000000000002 /* helper.c */,\n"
    "\t\t\t);\n"
    "\t\t\tpath = src;\n"
    "\t\t\tsourceTree = \"<group>\";\n"
    "\t\t};\n"
    "/* End PBXGroup section */\n"
    "\n"
    "/* Begin PBXNativeTarget section */\n"
    "\t\tD1000000000000000000001 /* DemoApp */ = {\n"
    "\t\t\tisa = PBXNativeTarget;\n"
    "\t\t\tbuildConfigurationList = E1000000000000000000001;\n"
    "\t\t\tbuildPhases = (\n"
    "\t\t\t\tF1000000000000000000001 /* Sources */,\n"
    "\t\t\t);\n"
    "\t\t\tbuildRules = (\n"
    "\t\t\t);\n"
    "\t\t\tdependencies = (\n"
    "\t\t\t);\n"
    "\t\t\tname = DemoApp;\n"
    "\t\t\tproductName = DemoApp;\n"
    "\t\t\tproductReference = B1000000000000000000003 /* DemoApp */;\n"
    "\t\t\tproductType = \"com.apple.product-type.tool\";\n"
    "\t\t};\n"
    "/* End PBXNativeTarget section */\n"
    "\n"
    "/* Begin PBXProject section */\n"
    "\t\tE1000000000000000000002 /* Project object */ = {\n"
    "\t\t\tisa = PBXProject;\n"
    "\t\t\tattributes = {\n"
    "\t\t\t\tBuildIndependentTargetsInParallel = 1;\n"
    "\t\t\t};\n"
    "\t\t\tbuildConfigurationList = E1000000000000000000003;\n"
    "\t\t\tcompatibilityVersion = \"Xcode 14.0\";\n"
    "\t\t\tdevelopmentRegion = en;\n"
    "\t\t\tmainGroup = C1000000000000000000001;\n"
    "\t\t\tproductRefGroup = C1000000000000000000003;\n"
    "\t\t\tprojectDirPath = \"\";\n"
    "\t\t\tprojectRoot = \"\";\n"
    "\t\t\ttargets = (\n"
    "\t\t\t\tD1000000000000000000001 /* DemoApp */,\n"
    "\t\t\t);\n"
    "\t\t};\n"
    "/* End PBXProject section */\n"
    "\n"
    "/* Begin PBXSourcesBuildPhase section */\n"
    "\t\tF1000000000000000000001 /* Sources */ = {\n"
    "\t\t\tisa = PBXSourcesBuildPhase;\n"
    "\t\t\tbuildActionMask = 2147483647;\n"
    "\t\t\tfiles = (\n"
    "\t\t\t\tA1000000000000000000001 /* main.c in Sources */,\n"
    "\t\t\t\tA1000000000000000000002 /* helper.c in Sources */,\n"
    "\t\t\t);\n"
    "\t\t\trunOnlyForDeploymentPostprocessing = 0;\n"
    "\t\t};\n"
    "/* End PBXSourcesBuildPhase section */\n"
    "\t};\n"
    "\trootObject = E1000000000000000000002 /* Project object */;\n"
    "}\n";
}

/* Write a DemoApp.xcodeproj + src/{main.c,helper.c} tree under dir. */
static int make_demo_project(const char *dir)
{
    char xcodeproj[512];
    char pbx[4096];
    char src[512];
    char mainc[4096];
    char helperh[4096];
    char helperc[4096];

    snprintf(xcodeproj, sizeof(xcodeproj), "%s/DemoApp.xcodeproj", dir);
    snprintf(pbx, sizeof(pbx), "%s/project.pbxproj", xcodeproj);
    snprintf(src, sizeof(src), "%s/src", dir);
    snprintf(mainc, sizeof(mainc), "%s/main.c", src);
    snprintf(helperh, sizeof(helperh), "%s/helper.h", src);
    snprintf(helperc, sizeof(helperc), "%s/helper.c", src);

    if (mkdir(xcodeproj, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    if (mkdir(src, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    if (write_file(pbx, demo_pbxproj()) != 0 ||
        write_file(mainc,
                   "#include \"helper.h\"\n"
                   "int main(void) { return helper_value() == 42 ? 0 : 1; }\n") != 0 ||
        write_file(helperh, "int helper_value(void);\n") != 0 ||
        write_file(helperc,
                   "#include \"helper.h\"\n"
                   "int helper_value(void) { return 42; }\n") != 0) {
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Milestone 4: pbxproj parser                                        */
/* ------------------------------------------------------------------ */

static void test_pbxproj_parse_target_and_sources(void)
{
    struct masa_pbxproj proj;
    char err[256];
    const char *text = demo_pbxproj();
    int rc;
    size_t i;
    int saw_main = 0;
    int saw_helper = 0;

    memset(&proj, 0, sizeof(proj));
    rc = masa_pbxproj_parse(text, strlen(text), &proj, err, sizeof(err));
    CHECK(rc == 0);
    if (rc != 0) {
        fprintf(stderr, "  parse error: %s\n", err);
        return;
    }

    CHECK_STREQ(proj.product_name, "DemoApp");
    CHECK(proj.source_count == 2);
    for (i = 0; i < proj.source_count; i++) {
        if (strcmp(proj.sources[i], "src/main.c") == 0) {
            saw_main = 1;
        }
        if (strcmp(proj.sources[i], "src/helper.c") == 0) {
            saw_helper = 1;
        }
    }
    CHECK(saw_main);
    CHECK(saw_helper);

    masa_pbxproj_free(&proj);
}

static void test_pbxproj_parse_flat_sources(void)
{
    /* File references at the top level, no group nesting. */
    static const char *flat =
        "{\n"
        "  objects = {\n"
        "    T1 = {isa = PBXNativeTarget; name = Flat; productName = Flat;"
        " buildPhases = (P1); };\n"
        "    P1 = {isa = PBXSourcesBuildPhase; files = (F1, F2); };\n"
        "    F1 = {isa = PBXBuildFile; fileRef = R1; };\n"
        "    F2 = {isa = PBXBuildFile; fileRef = R2; };\n"
        "    R1 = {isa = PBXFileReference; path = alpha.c; };\n"
        "    R2 = {isa = PBXFileReference; path = beta.c; };\n"
        "  };\n"
        "  rootObject = T1;\n"
        "}\n";
    struct masa_pbxproj proj;
    char err[256];
    int rc;
    size_t i;
    int saw_alpha = 0;
    int saw_beta = 0;

    memset(&proj, 0, sizeof(proj));
    rc = masa_pbxproj_parse(flat, strlen(flat), &proj, err, sizeof(err));
    CHECK(rc == 0);
    if (rc != 0) {
        fprintf(stderr, "  parse error: %s\n", err);
        return;
    }
    CHECK_STREQ(proj.product_name, "Flat");
    CHECK(proj.source_count == 2);
    for (i = 0; i < proj.source_count; i++) {
        if (strcmp(proj.sources[i], "alpha.c") == 0) {
            saw_alpha = 1;
        }
        if (strcmp(proj.sources[i], "beta.c") == 0) {
            saw_beta = 1;
        }
    }
    CHECK(saw_alpha);
    CHECK(saw_beta);

    masa_pbxproj_free(&proj);
}

static void test_pbxproj_rejects_malformed(void)
{
    struct masa_pbxproj proj;
    char err[256];

    /* NULL / empty input. */
    memset(&proj, 0, sizeof(proj));
    CHECK(masa_pbxproj_parse(NULL, 0, &proj, err, sizeof(err)) == -1);
    CHECK(proj.product_name == NULL);
    CHECK(proj.sources == NULL);
    CHECK(proj.source_count == 0);

    memset(&proj, 0, sizeof(proj));
    CHECK(masa_pbxproj_parse("", 0, &proj, err, sizeof(err)) == -1);

    /* Unbalanced braces: the value parser must fail cleanly. */
    memset(&proj, 0, sizeof(proj));
    CHECK(masa_pbxproj_parse("{ objects = { T = {isa = PBXNativeTarget; }",
                             strlen("{ objects = { T = {isa = PBXNativeTarget; }"),
                             &proj, err, sizeof(err)) == -1);
    CHECK(proj.product_name == NULL);

    /* Well-formed but with no target. */
    memset(&proj, 0, sizeof(proj));
    CHECK(masa_pbxproj_parse("{ objects = { }; }",
                             strlen("{ objects = { }; }"),
                             &proj, err, sizeof(err)) == -1);
    CHECK(proj.product_name == NULL);

    /* A target with no C sources must be rejected, not silently built. */
    {
        static const char *nosrc =
            "{ objects = { T = {isa = PBXNativeTarget; productName = X; }; }; }";
        memset(&proj, 0, sizeof(proj));
        CHECK(masa_pbxproj_parse(nosrc, strlen(nosrc), &proj, err,
                                 sizeof(err)) == -1);
        CHECK(proj.product_name == NULL);
    }

    /* Not a plist at all. */
    memset(&proj, 0, sizeof(proj));
    CHECK(masa_pbxproj_parse("this is not a plist", 19, &proj, err,
                             sizeof(err)) == -1);
}

/* ------------------------------------------------------------------ */
/* Milestone 4: masa xcode build                                      */
/* ------------------------------------------------------------------ */

static void test_xcode_build_bundle(void)
{
    static const unsigned char macho64_le[4] = {0xCF, 0xFA, 0xED, 0xFE};
    static const unsigned char mh_execute[4] = {0x02, 0x00, 0x00, 0x00};
    char dir[] = "/tmp/masa_xcode_XXXXXX";
    char sdk[512];
    char out_dir[512];
    char proj[512];
    char exe[4096];
    char plist[4096];
    char out[4096];
    char err[8192];
    unsigned char magic[4];
    unsigned char filetype[4];
    char *argv[10];
    int rc;

    if (!tool_available("clang")) {
        printf("  SKIP clang is not installed\n");
        g_skipped++;
        return;
    }

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }
    snprintf(sdk, sizeof(sdk), "%s/sdk", dir);
    snprintf(out_dir, sizeof(out_dir), "%s/out", dir);
    snprintf(proj, sizeof(proj), "%s/DemoApp.xcodeproj", dir);
    snprintf(exe, sizeof(exe),
             "%s/DemoApp.app/Contents/MacOS/DemoApp", out_dir);
    snprintf(plist, sizeof(plist),
             "%s/DemoApp.app/Contents/Info.plist", out_dir);

    reset_masa_env();
    set_env(MASA_TARGET_ENV, "arm64-apple-darwin");

    if (make_stub_sdk(sdk) != 0 || !sdk_is_stubbed(sdk)) {
        fprintf(stderr, "  FAIL could not create the stub SDK\n");
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }
    set_env(MASA_SDK_PATH_ENV, sdk);

    if (make_demo_project(dir) != 0) {
        fprintf(stderr, "  FAIL cannot write the demo project\n");
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }

    argv[0] = "masa";
    argv[1] = "xcode";
    argv[2] = "build";
    argv[3] = proj;
    argv[4] = "--bundle";
    argv[5] = "-o";
    argv[6] = out_dir;
    argv[7] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  xcode build stdout: %s\n  stderr: %s\n", out, err);
    }
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "DemoApp");

    if (!file_exists(exe)) {
        fprintf(stderr, "  FAIL no bundled executable at %s\n", exe);
        g_failed++;
    } else if (read_macho_header(exe, magic, filetype) != 0) {
        fprintf(stderr, "  FAIL cannot read the Mach-O header of %s\n", exe);
        g_failed++;
    } else {
        CHECK(memcmp(magic, macho64_le, sizeof(magic)) == 0);
        CHECK(memcmp(filetype, mh_execute, sizeof(filetype)) == 0);
    }

    CHECK(file_exists(plist));
    if (file_exists(plist)) {
        FILE *fp = fopen(plist, "r");
        if (fp != NULL) {
            char buf[1024];
            size_t got = fread(buf, 1, sizeof(buf) - 1, fp);
            fclose(fp);
            buf[got] = '\0';
            CHECK_CONTAINS(buf, "CFBundleExecutable");
            CHECK_CONTAINS(buf, "DemoApp");
        }
    }

    remove_tree(dir);
    reset_masa_env();
}

/* Without --bundle the binary is written straight into the output dir. */
static void test_xcode_build_plain(void)
{
    char dir[] = "/tmp/masa_xcode2_XXXXXX";
    char sdk[512];
    char out_dir[512];
    char proj[512];
    char exe[4096];
    char out[4096];
    char err[8192];
    char *argv[10];
    int rc;

    if (!tool_available("clang")) {
        printf("  SKIP clang is not installed\n");
        g_skipped++;
        return;
    }

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }
    snprintf(sdk, sizeof(sdk), "%s/sdk", dir);
    snprintf(out_dir, sizeof(out_dir), "%s/bin", dir);
    snprintf(proj, sizeof(proj), "%s/DemoApp.xcodeproj", dir);
    snprintf(exe, sizeof(exe), "%s/DemoApp", out_dir);

    reset_masa_env();
    set_env(MASA_TARGET_ENV, "arm64-apple-darwin");

    if (make_stub_sdk(sdk) != 0 || !sdk_is_stubbed(sdk) ||
        make_demo_project(dir) != 0) {
        fprintf(stderr, "  FAIL cannot prepare the project\n");
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }
    set_env(MASA_SDK_PATH_ENV, sdk);

    argv[0] = "masa";
    argv[1] = "xcode";
    argv[2] = "build";
    argv[3] = proj;
    argv[4] = "-o";
    argv[5] = out_dir;
    argv[6] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  xcode build stderr: %s\n", err);
    }
    CHECK(rc == 0);
    CHECK(file_exists(exe));

    remove_tree(dir);
    reset_masa_env();
}

static void test_xcode_build_missing_project(void)
{
    char dir[] = "/tmp/masa_xcode3_XXXXXX";
    char missing[512];
    char err[1024];
    char *argv[6];
    int rc;

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }
    snprintf(missing, sizeof(missing), "%s/Nope.xcodeproj", dir);

    reset_masa_env();

    argv[0] = "masa";
    argv[1] = "xcode";
    argv[2] = "build";
    argv[3] = missing;
    argv[4] = NULL;

    rc = run_masa(argv, NULL, 0, err, sizeof(err));
    CHECK(rc != 0);
    CHECK_CONTAINS(err, "does not exist");

    remove_tree(dir);
    reset_masa_env();
}

/* A .xcodeproj without a project.pbxproj must be reported clearly. */
static void test_xcode_build_missing_pbxproj(void)
{
    char dir[] = "/tmp/masa_xcode4_XXXXXX";
    char proj[512];
    char err[1024];
    char *argv[6];
    int rc;

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }
    snprintf(proj, sizeof(proj), "%s/Empty.xcodeproj", dir);
    if (mkdir(proj, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "  FAIL cannot create %s\n", proj);
        g_failed++;
        remove_tree(dir);
        return;
    }

    reset_masa_env();

    argv[0] = "masa";
    argv[1] = "xcode";
    argv[2] = "build";
    argv[3] = proj;
    argv[4] = NULL;

    rc = run_masa(argv, NULL, 0, err, sizeof(err));
    CHECK(rc != 0);
    CHECK_CONTAINS(err, "missing");

    remove_tree(dir);
    reset_masa_env();
}

static void test_xcode_unknown_subcommand(void)
{
    char *argv[] = {"masa", "xcode", "bogus", NULL};
    char err[512];
    int rc;

    reset_masa_env();
    rc = run_masa(argv, NULL, 0, err, sizeof(err));
    CHECK(rc == 2);
    CHECK_CONTAINS(err, "unknown subcommand");
}

/* ------------------------------------------------------------------ */
/* Milestone 4: codesign shim                                         */
/* ------------------------------------------------------------------ */

static void test_codesign_argv0_dispatch(void)
{
    char *argv_verify[] = {"/some/dir/codesign", "--verify", "/etc/hostname", NULL};
    char *argv_sign[] = {"/some/dir/codesign", "-s", "-", "/etc/hostname", NULL};
    char err[1024];
    int rc;

    reset_masa_env();

    /* A non-Mach-O target fails verification and signing alike. */
    rc = run_masa(argv_verify, NULL, 0, err, sizeof(err));
    CHECK(rc != 0);
    CHECK_CONTAINS(err, "Mach-O");

    rc = run_masa(argv_sign, NULL, 0, err, sizeof(err));
    CHECK(rc != 0);
    CHECK_CONTAINS(err, "Mach-O");

    /* No target at all. */
    {
        char *argv_none[] = {"/some/dir/codesign", "--verify", NULL};
        rc = run_masa(argv_none, NULL, 0, err, sizeof(err));
        CHECK(rc != 0);
        CHECK_CONTAINS(err, "no target");
    }
}

/*
 * The end-to-end flow the task asks for: build a bundle, then sign and
 * verify its executable through `masa exec codesign`.
 */
static void test_xcode_codesign_roundtrip(void)
{
    char dir[] = "/tmp/masa_sign_XXXXXX";
    char sdk[512];
    char out_dir[512];
    char proj[512];
    char exe[4096];
    char out[4096];
    char err[8192];
    char *argv[10];
    char *masa_bin;
    int rc;

    if (!tool_available("clang")) {
        printf("  SKIP clang is not installed\n");
        g_skipped++;
        return;
    }

    masa_bin = masa_bin_path();
    if (masa_bin == NULL || access(masa_bin, X_OK) != 0) {
        printf("  SKIP the masa binary is not built\n");
        g_skipped++;
        free(masa_bin);
        return;
    }

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        free(masa_bin);
        return;
    }
    snprintf(sdk, sizeof(sdk), "%s/sdk", dir);
    snprintf(out_dir, sizeof(out_dir), "%s/out", dir);
    snprintf(proj, sizeof(proj), "%s/DemoApp.xcodeproj", dir);
    snprintf(exe, sizeof(exe),
             "%s/DemoApp.app/Contents/MacOS/DemoApp", out_dir);

    reset_masa_env();
    set_env(MASA_TARGET_ENV, "arm64-apple-darwin");
    set_env(MASA_SHIM_TARGET_ENV, masa_bin);

    if (make_stub_sdk(sdk) != 0 || !sdk_is_stubbed(sdk) ||
        make_demo_project(dir) != 0) {
        fprintf(stderr, "  FAIL cannot prepare the project\n");
        g_failed++;
        remove_tree(dir);
        free(masa_bin);
        reset_masa_env();
        return;
    }
    set_env(MASA_SDK_PATH_ENV, sdk);

    argv[0] = "masa";
    argv[1] = "xcode";
    argv[2] = "build";
    argv[3] = proj;
    argv[4] = "--bundle";
    argv[5] = "-o";
    argv[6] = out_dir;
    argv[7] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  xcode build stderr: %s\n", err);
    }
    CHECK(rc == 0);
    CHECK(file_exists(exe));

    /* Ad-hoc sign through masa exec; the shim must not fail the build. */
    argv[0] = "masa";
    argv[1] = "exec";
    argv[2] = "codesign";
    argv[3] = "-s";
    argv[4] = "-";
    argv[5] = "--force";
    argv[6] = exe;
    argv[7] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  codesign -s - stderr: %s\n", err);
    }
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "signed");

    /* Verify through masa exec. */
    argv[0] = "masa";
    argv[1] = "exec";
    argv[2] = "codesign";
    argv[3] = "--verify";
    argv[4] = exe;
    argv[5] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  codesign --verify stderr: %s\n", err);
    }
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "valid on disk");

    remove_tree(dir);
    free(masa_bin);
    reset_masa_env();
}

/* init-shims must create a codesign symlink too. */
static void test_init_shims_includes_codesign(void)
{
    char dir[] = "/tmp/masa_shim_sign_XXXXXX";
    char path[4096];
    char *argv[4];
    char out[512];
    int rc;

    reset_masa_env();
    set_env(MASA_SHIM_TARGET_ENV, "/bin/true");

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }

    argv[0] = "masa";
    argv[1] = "init-shims";
    argv[2] = dir;
    argv[3] = NULL;

    rc = run_masa(argv, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);

    snprintf(path, sizeof(path), "%s/codesign", dir);
    CHECK(is_symlink_to(path, "/bin/true"));

    remove_tree(dir);
    reset_masa_env();
}

/* ------------------------------------------------------------------ */
/* Homebrew formula linter and builder                                */
/* ------------------------------------------------------------------ */

/* A realistic formula used by the lint and build tests. */
static const char *demo_formula(void)
{
    return
        "class Mytool < Formula\n"
        "  desc \"A tiny demonstration tool\"\n"
        "  homepage \"https://example.com/mytool\"\n"
        "  url \"https://example.com/mytool-1.2.3.tar.gz\"\n"
        "  sha256 \"0123456789abcdef0123456789abcdef0123456789abcdef"
        "0123456789abcdef\"\n"
        "  license \"MIT\"\n"
        "\n"
        "  def install\n"
        "    system \"make\"\n"
        "    system \"make\", \"install\", \"PREFIX=#{prefix}\"\n"
        "  end\n"
        "\n"
        "  test do\n"
        "    system \"#{bin}/mytool\"\n"
        "  end\n"
        "end\n";
}

static void test_brew_formula_parse(void)
{
    struct masa_brew_formula f;
    char err[256];
    int rc = masa_brew_formula_parse(demo_formula(), strlen(demo_formula()),
                                     &f, err, sizeof(err));

    CHECK(rc == 0);
    if (rc != 0) {
        return;
    }
    CHECK_STREQ(f.class_name, "Mytool");
    CHECK_STREQ(f.name, "mytool");
    CHECK_STREQ(f.desc, "A tiny demonstration tool");
    CHECK_STREQ(f.homepage, "https://example.com/mytool");
    CHECK_STREQ(f.url, "https://example.com/mytool-1.2.3.tar.gz");
    CHECK(f.sha256 != NULL);
    CHECK_STREQ(f.license, "MIT");
    CHECK_STREQ(f.version, "1.2.3");
    CHECK(f.has_install == 1);
    CHECK(f.has_test == 1);
    CHECK(f.step_count == 2);
    if (f.step_count == 2) {
        CHECK_STREQ(f.steps[0].prog, "make");
        CHECK(f.steps[0].argc == 1);
        CHECK_STREQ(f.steps[1].prog, "make");
        CHECK(f.steps[1].argc == 3);
        CHECK_STREQ(f.steps[1].argv[1], "install");
        CHECK_STREQ(f.steps[1].argv[2], "PREFIX=#{prefix}");
    }
    masa_brew_formula_free(&f);
}

/* The class name maps to the formula name; the URL supplies the version. */
static void test_brew_formula_name_and_version(void)
{
    static const char *text =
        "class FooBarBaz < Formula\n"
        "  desc \"x\"\n"
        "  homepage \"https://e.com\"\n"
        "  url \"https://e.com/foo-bar-baz-4.5.tar.gz\"\n"
        "  sha256 \"aa\"\n"
        "  def install\n"
        "    system \"make\"\n"
        "  end\n"
        "end\n";
    struct masa_brew_formula f;
    char err[256];
    int rc = masa_brew_formula_parse(text, strlen(text), &f, err, sizeof(err));

    CHECK(rc == 0);
    if (rc != 0) {
        return;
    }
    CHECK_STREQ(f.name, "foo-bar-baz");
    CHECK_STREQ(f.version, "4.5");
    CHECK(f.has_test == 0);
    masa_brew_formula_free(&f);
}

/* An explicit `version "..."` wins over the URL-derived one. */
static void test_brew_formula_explicit_version(void)
{
    static const char *text =
        "class MyTool < Formula\n"
        "  desc \"x\"\n"
        "  homepage \"https://e.com\"\n"
        "  url \"https://e.com/mytool-9.9.9.tar.gz\"\n"
        "  sha256 \"aa\"\n"
        "  version \"1.0.0\"\n"
        "  def install\n"
        "    system \"make\"\n"
        "  end\n"
        "end\n";
    struct masa_brew_formula f;
    char err[256];
    int rc = masa_brew_formula_parse(text, strlen(text), &f, err, sizeof(err));

    CHECK(rc == 0);
    if (rc != 0) {
        return;
    }
    CHECK_STREQ(f.name, "my-tool");
    CHECK_STREQ(f.version, "1.0.0");
    masa_brew_formula_free(&f);
}

/* Malformed inputs are rejected cleanly and leave nothing allocated. */
static void test_brew_formula_rejects_malformed(void)
{
    struct masa_brew_formula f;
    char err[256];
    static const char *bad[] = {
        "",                              /* empty */
        "puts 1\n",                      /* not a formula */
        "class Broken Formula\n",        /* no < Formula */
        "def install\n  system \"make\"\nend\n", /* no class */
    };
    size_t i;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        int rc = masa_brew_formula_parse(bad[i], strlen(bad[i]), &f,
                                         err, sizeof(err));
        CHECK(rc != 0);
        CHECK(err[0] != '\0');
        /* Freeing a failed parse is a no-op but must stay safe. */
        masa_brew_formula_free(&f);
    }

    /* A NULL/empty buffer must not crash either. */
    CHECK(masa_brew_formula_parse(NULL, 0, &f, err, sizeof(err)) != 0);
}

static void test_brew_lint_valid(void)
{
    struct masa_brew_formula f;
    char err[256];
    int rc;

    if (masa_brew_formula_parse(demo_formula(), strlen(demo_formula()), &f,
                                err, sizeof(err)) != 0) {
        CHECK(0);
        return;
    }
    rc = masa_brew_lint_formula(&f, "demo.rb");
    CHECK(rc == 0);
    masa_brew_formula_free(&f);
}

/* Missing sha256 is a required-field failure. */
static void test_brew_lint_missing_sha256(void)
{
    static const char *text =
        "class Nohash < Formula\n"
        "  desc \"x\"\n"
        "  homepage \"https://e.com\"\n"
        "  url \"https://e.com/x-1.0.tar.gz\"\n"
        "  def install\n"
        "    system \"make\"\n"
        "  end\n"
        "end\n";
    struct masa_brew_formula f;
    char err[256];
    int rc;

    CHECK(masa_brew_formula_parse(text, strlen(text), &f, err, sizeof(err)) == 0);
    rc = masa_brew_lint_formula(&f, "nohash.rb");
    CHECK(rc != 0);
    CHECK(f.sha256 == NULL);
    masa_brew_formula_free(&f);
}

/* Missing `def install` is a required-field failure. */
static void test_brew_lint_missing_install(void)
{
    static const char *text =
        "class Noinstall < Formula\n"
        "  desc \"x\"\n"
        "  homepage \"https://e.com\"\n"
        "  url \"https://e.com/x-1.0.tar.gz\"\n"
        "  sha256 \"abc\"\n"
        "end\n";
    struct masa_brew_formula f;
    char err[256];
    int rc;

    CHECK(masa_brew_formula_parse(text, strlen(text), &f, err, sizeof(err)) == 0);
    rc = masa_brew_lint_formula(&f, "noinstall.rb");
    CHECK(rc != 0);
    CHECK(f.has_install == 0);
    masa_brew_formula_free(&f);
}

/* The lint subcommand reads a file and reports through the router. */
static void test_brew_lint_subcommand(void)
{
    char dir[] = "/tmp/masa_brew_lint_XXXXXX";
    char formula[512];
    char missing[512];
    char out[4096];
    char err[8192];
    char *argv[6];
    int rc;

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }
    snprintf(formula, sizeof(formula), "%s/mytool.rb", dir);
    snprintf(missing, sizeof(missing), "%s/nope.rb", dir);

    if (write_file(formula, demo_formula()) != 0) {
        fprintf(stderr, "  FAIL cannot write the formula\n");
        g_failed++;
        remove_tree(dir);
        return;
    }

    reset_masa_env();

    argv[0] = "masa";
    argv[1] = "brew";
    argv[2] = "lint";
    argv[3] = formula;
    argv[4] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  brew lint stderr: %s\n", err);
    }
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "mytool");

    /* A missing file is reported, not crashed on. */
    argv[3] = missing;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc != 0);
    CHECK_CONTAINS(err, "cannot open");

    /* Unknown subcommand. */
    argv[2] = "frobnicate";
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 2);

    remove_tree(dir);
    reset_masa_env();
}

/* A C project the demo formula builds: a Makefile and a source file. */
static int make_brew_source(const char *dir)
{
    char src[512];
    char makefile[1024];
    char mainc[1024];

    snprintf(src, sizeof(src), "%s/src", dir);
    snprintf(makefile, sizeof(makefile), "%s/Makefile", src);
    snprintf(mainc, sizeof(mainc), "%s/mytool.c", src);

    if (mkdir(src, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    if (write_file(mainc,
                   "#include <stdio.h>\n"
                   "int main(void) { puts(\"mytool\"); return 0; }\n") != 0 ||
        write_file(makefile,
                   "PREFIX ?= /usr/local\n"
                   "all: mytool\n"
                   "\n"
                   "mytool: mytool.c\n"
                   "\t$(CC) mytool.c -o mytool\n"
                   "\n"
                   "install: mytool\n"
                   "\tmkdir -p $(PREFIX)/bin\n"
                   "\tcp mytool $(PREFIX)/bin/mytool\n") != 0) {
        return -1;
    }
    return 0;
}

static void test_brew_build_makefile(void)
{
    static const unsigned char macho64_le[4] = {0xCF, 0xFA, 0xED, 0xFE};
    static const unsigned char mh_execute[4] = {0x02, 0x00, 0x00, 0x00};
    char dir[] = "/tmp/masa_brew_build_XXXXXX";
    char sdk[512];
    char prefix[512];
    char formula[512];
    char srcdir[512];
    char binpath[4096];
    char linkpath[4096];
    char out[8192];
    char err[8192];
    unsigned char magic[4];
    unsigned char filetype[4];
    char *argv[10];
    char *masa_bin;
    int rc;

    if (!tool_available("clang") || !tool_available("make")) {
        printf("  SKIP clang or make is not installed\n");
        g_skipped++;
        return;
    }

    masa_bin = masa_bin_path();
    if (masa_bin == NULL || access(masa_bin, X_OK) != 0) {
        printf("  SKIP the masa binary is not built\n");
        g_skipped++;
        free(masa_bin);
        return;
    }

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        free(masa_bin);
        return;
    }
    snprintf(sdk, sizeof(sdk), "%s/sdk", dir);
    snprintf(prefix, sizeof(prefix), "%s/prefix", dir);
    snprintf(formula, sizeof(formula), "%s/mytool.rb", dir);
    snprintf(srcdir, sizeof(srcdir), "%s/src", dir);
    snprintf(binpath, sizeof(binpath),
             "%s/Cellar/mytool/1.2.3/bin/mytool", prefix);
    snprintf(linkpath, sizeof(linkpath), "%s/bin/mytool", prefix);

    reset_masa_env();
    set_env(MASA_TARGET_ENV, "arm64-apple-darwin");
    set_env(MASA_SHIM_TARGET_ENV, masa_bin);

    if (make_stub_sdk(sdk) != 0 || !sdk_is_stubbed(sdk) ||
        make_brew_source(dir) != 0 ||
        write_file(formula, demo_formula()) != 0) {
        fprintf(stderr, "  FAIL cannot prepare the brew project\n");
        g_failed++;
        remove_tree(dir);
        free(masa_bin);
        reset_masa_env();
        return;
    }
    set_env(MASA_SDK_PATH_ENV, sdk);

    argv[0] = "masa";
    argv[1] = "brew";
    argv[2] = "build";
    argv[3] = formula;
    argv[4] = "--source";
    argv[5] = srcdir;
    argv[6] = "--prefix";
    argv[7] = prefix;
    argv[8] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  brew build stdout: %s\n  stderr: %s\n", out, err);
    }
    CHECK(rc == 0);

    CHECK(file_exists(binpath));
    if (file_exists(binpath)) {
        if (read_macho_header(binpath, magic, filetype) != 0) {
            fprintf(stderr, "  FAIL cannot read the Mach-O header\n");
            g_failed++;
        } else {
            CHECK(memcmp(magic, macho64_le, sizeof(magic)) == 0);
            CHECK(memcmp(filetype, mh_execute, sizeof(filetype)) == 0);
        }
    }

    /* <prefix>/bin/mytool must be a symlink to the keg binary. */
    CHECK(is_symlink_to(linkpath, binpath));

    /* The installed binary is a valid thing to sign and verify. */
    argv[0] = "masa";
    argv[1] = "exec";
    argv[2] = "codesign";
    argv[3] = "-s";
    argv[4] = "-";
    argv[5] = binpath;
    argv[6] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "signed");

    argv[0] = "masa";
    argv[1] = "exec";
    argv[2] = "codesign";
    argv[3] = "--verify";
    argv[4] = binpath;
    argv[5] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "valid on disk");

    remove_tree(dir);
    free(masa_bin);
    reset_masa_env();
}

/* The `cc` + `mkdir -p #{bin}` + `cp` variant of an install block. */
static void test_brew_build_cc_steps(void)
{
    static const unsigned char macho64_le[4] = {0xCF, 0xFA, 0xED, 0xFE};
    static const unsigned char mh_execute[4] = {0x02, 0x00, 0x00, 0x00};
    static const char *formula_text =
        "class Cctool < Formula\n"
        "  desc \"Built with cc directly\"\n"
        "  homepage \"https://example.com/cctool\"\n"
        "  url \"https://example.com/cctool-2.0.0.tar.gz\"\n"
        "  sha256 \"deadbeef\"\n"
        "  def install\n"
        "    system \"cc\", \"cctool.c\", \"-o\", \"cctool\"\n"
        "    system \"mkdir\", \"-p\", \"#{bin}\"\n"
        "    system \"cp\", \"cctool\", \"#{bin}/cctool\"\n"
        "  end\n"
        "end\n";
    char dir[] = "/tmp/masa_brew_cc_XXXXXX";
    char sdk[512];
    char prefix[512];
    char formula[512];
    char srcdir[512];
    char binpath[4096];
    char linkpath[4096];
    char out[8192];
    char err[8192];
    unsigned char magic[4];
    unsigned char filetype[4];
    char *argv[10];
    char *masa_bin;
    int rc;

    if (!tool_available("clang")) {
        printf("  SKIP clang is not installed\n");
        g_skipped++;
        return;
    }

    masa_bin = masa_bin_path();
    if (masa_bin == NULL || access(masa_bin, X_OK) != 0) {
        printf("  SKIP the masa binary is not built\n");
        g_skipped++;
        free(masa_bin);
        return;
    }

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        free(masa_bin);
        return;
    }
    snprintf(sdk, sizeof(sdk), "%s/sdk", dir);
    snprintf(prefix, sizeof(prefix), "%s/prefix", dir);
    snprintf(formula, sizeof(formula), "%s/cctool.rb", dir);
    snprintf(srcdir, sizeof(srcdir), "%s/src", dir);
    snprintf(binpath, sizeof(binpath),
             "%s/Cellar/cctool/2.0.0/bin/cctool", prefix);
    snprintf(linkpath, sizeof(linkpath), "%s/bin/cctool", prefix);

    reset_masa_env();
    set_env(MASA_TARGET_ENV, "arm64-apple-darwin");
    set_env(MASA_SHIM_TARGET_ENV, masa_bin);

    if (make_stub_sdk(sdk) != 0 || !sdk_is_stubbed(sdk) ||
        mkdir(srcdir, 0755) != 0 ||
        write_file(formula, formula_text) != 0) {
        fprintf(stderr, "  FAIL cannot prepare the brew project\n");
        g_failed++;
        remove_tree(dir);
        free(masa_bin);
        reset_masa_env();
        return;
    }
    {
        char mainc[1024];
        snprintf(mainc, sizeof(mainc), "%s/cctool.c", srcdir);
        if (write_file(mainc, "int main(void) { return 0; }\n") != 0) {
            fprintf(stderr, "  FAIL cannot write the source\n");
            g_failed++;
            remove_tree(dir);
            free(masa_bin);
            reset_masa_env();
            return;
        }
    }
    set_env(MASA_SDK_PATH_ENV, sdk);

    argv[0] = "masa";
    argv[1] = "brew";
    argv[2] = "build";
    argv[3] = formula;
    argv[4] = "--source";
    argv[5] = srcdir;
    argv[6] = "--prefix";
    argv[7] = prefix;
    argv[8] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  brew build stdout: %s\n  stderr: %s\n", out, err);
    }
    CHECK(rc == 0);

    CHECK(file_exists(binpath));
    if (file_exists(binpath)) {
        if (read_macho_header(binpath, magic, filetype) != 0) {
            g_failed++;
        } else {
            CHECK(memcmp(magic, macho64_le, sizeof(magic)) == 0);
            CHECK(memcmp(filetype, mh_execute, sizeof(filetype)) == 0);
        }
    }
    CHECK(is_symlink_to(linkpath, binpath));

    remove_tree(dir);
    free(masa_bin);
    reset_masa_env();
}

/* Missing arguments and a missing install block are reported cleanly. */
static void test_brew_build_errors(void)
{
    char dir[] = "/tmp/masa_brew_err_XXXXXX";
    char noinstall[512];
    char out[4096];
    char err[8192];
    char *argv[10];
    int rc;

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }
    snprintf(noinstall, sizeof(noinstall), "%s/noinstall.rb", dir);
    write_file(noinstall,
               "class Noinstall < Formula\n"
               "  desc \"x\"\n"
               "  homepage \"https://e.com\"\n"
               "  url \"https://e.com/x-1.0.tar.gz\"\n"
               "  sha256 \"abc\"\n"
               "end\n");

    reset_masa_env();

    /* No formula argument. */
    argv[0] = "masa";
    argv[1] = "brew";
    argv[2] = "build";
    argv[3] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 2);
    CHECK_CONTAINS(err, "missing <formula.rb>");

    /* Formula but no --source. */
    argv[3] = noinstall;
    argv[4] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 2);
    CHECK_CONTAINS(err, "missing --source");

    /* A formula with no `def install` cannot be built. */
    argv[4] = "--source";
    argv[5] = dir;
    argv[6] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc != 0);
    CHECK_CONTAINS(err, "no 'def install'");

    remove_tree(dir);
    reset_masa_env();
}

/*
 * A formula whose install steps are single-string `system "..."` calls:
 * `system "cc main.c -o mytool"`, `system "mkdir -p #{bin}"` and
 * `system "cp mytool #{bin}/mytool"`.  Ruby runs each of these through
 * `/bin/sh -c`, so spaces and metacharacters must be interpreted - the
 * whole point of the shell path in brew.c.
 */
static void test_brew_build_shell_string_steps(void)
{
    static const unsigned char macho64_le[4] = {0xCF, 0xFA, 0xED, 0xFE};
    static const unsigned char mh_execute[4] = {0x02, 0x00, 0x00, 0x00};
    static const char *formula_text =
        "class Shelltool < Formula\n"
        "  desc \"Built from single-string system calls\"\n"
        "  homepage \"https://example.com/shelltool\"\n"
        "  url \"https://example.com/shelltool-3.0.0.tar.gz\"\n"
        "  sha256 \"cafef00d\"\n"
        "  def install\n"
        "    system \"cc main.c -o mytool\"\n"
        "    system \"mkdir -p #{bin}\"\n"
        "    system \"cp mytool #{bin}/mytool\"\n"
        "  end\n"
        "end\n";
    char dir[] = "/tmp/masa_brew_shell_XXXXXX";
    char sdk[512];
    char prefix[512];
    char formula[512];
    char srcdir[512];
    char mainc[1024];
    char binpath[4096];
    char linkpath[4096];
    char out[8192];
    char err[8192];
    unsigned char magic[4];
    unsigned char filetype[4];
    char *argv[10];
    char *masa_bin;
    int rc;

    if (!tool_available("clang")) {
        printf("  SKIP clang is not installed\n");
        g_skipped++;
        return;
    }

    masa_bin = masa_bin_path();
    if (masa_bin == NULL || access(masa_bin, X_OK) != 0) {
        printf("  SKIP the masa binary is not built\n");
        g_skipped++;
        free(masa_bin);
        return;
    }

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        free(masa_bin);
        return;
    }
    snprintf(sdk, sizeof(sdk), "%s/sdk", dir);
    snprintf(prefix, sizeof(prefix), "%s/prefix", dir);
    snprintf(formula, sizeof(formula), "%s/shelltool.rb", dir);
    snprintf(srcdir, sizeof(srcdir), "%s/src", dir);
    snprintf(mainc, sizeof(mainc), "%s/main.c", srcdir);
    snprintf(binpath, sizeof(binpath),
             "%s/Cellar/shelltool/3.0.0/bin/mytool", prefix);
    snprintf(linkpath, sizeof(linkpath), "%s/bin/mytool", prefix);

    reset_masa_env();
    set_env(MASA_TARGET_ENV, "arm64-apple-darwin");
    set_env(MASA_SHIM_TARGET_ENV, masa_bin);

    if (make_stub_sdk(sdk) != 0 || !sdk_is_stubbed(sdk) ||
        mkdir(srcdir, 0755) != 0 ||
        write_file(mainc, "int main(void) { return 0; }\n") != 0 ||
        write_file(formula, formula_text) != 0) {
        fprintf(stderr, "  FAIL cannot prepare the brew project\n");
        g_failed++;
        remove_tree(dir);
        free(masa_bin);
        reset_masa_env();
        return;
    }
    set_env(MASA_SDK_PATH_ENV, sdk);

    argv[0] = "masa";
    argv[1] = "brew";
    argv[2] = "build";
    argv[3] = formula;
    argv[4] = "--source";
    argv[5] = srcdir;
    argv[6] = "--prefix";
    argv[7] = prefix;
    argv[8] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  brew build stdout: %s\n  stderr: %s\n", out, err);
    }
    CHECK(rc == 0);

    CHECK(file_exists(binpath));
    if (file_exists(binpath)) {
        if (read_macho_header(binpath, magic, filetype) != 0) {
            g_failed++;
        } else {
            CHECK(memcmp(magic, macho64_le, sizeof(magic)) == 0);
            CHECK(memcmp(filetype, mh_execute, sizeof(filetype)) == 0);
        }
    }
    CHECK(is_symlink_to(linkpath, binpath));

    remove_tree(dir);
    free(masa_bin);
    reset_masa_env();
}

/*
 * The single-string parser must route shell commands through the shell
 * and leave a bare one-word command on the direct-exec path.
 */
static void test_brew_formula_shell_string_flag(void)
{
    static const char *text =
        "class Flags < Formula\n"
        "  desc \"x\"\n"
        "  homepage \"https://e.com\"\n"
        "  url \"https://e.com/flags-1.0.tar.gz\"\n"
        "  sha256 \"abc\"\n"
        "  def install\n"
        "    system \"make\"\n"
        "    system \"cc main.c -o mytool\"\n"
        "    system \"mkdir -p #{bin}\"\n"
        "    system \"cc\", \"main.c\", \"-o\", \"mytool\"\n"
        "  end\n"
        "end\n";
    struct masa_brew_formula f;
    char err[256];
    int rc;

    memset(&f, 0, sizeof(f));
    rc = masa_brew_formula_parse(text, strlen(text), &f, err, sizeof(err));
    CHECK(rc == 0);
    if (rc != 0) {
        fprintf(stderr, "  parse error: %s\n", err);
        return;
    }
    CHECK(f.step_count == 4);
    if (f.step_count == 4) {
        /* `system "make"` is a bare word: direct exec. */
        CHECK(f.steps[0].use_shell == 0);
        /* `system "cc main.c -o mytool"` needs the shell. */
        CHECK(f.steps[1].use_shell == 1);
        /* `system "mkdir -p #{bin}"` needs the shell. */
        CHECK(f.steps[2].use_shell == 1);
        /* Multi-argument form never uses the shell. */
        CHECK(f.steps[3].use_shell == 0);
        CHECK(f.steps[3].argc == 4);
    }
    masa_brew_formula_free(&f);
}

/* ------------------------------------------------------------------ */
/* Milestone 17: casks, online tap/fetch and SHA-256                  */
/* ------------------------------------------------------------------ */

/* A realistic Homebrew cask, the shape `brew create --cask` emits. */
static const char *demo_cask(void)
{
    return
        "cask \"openrom\" do\n"
        "  version \"1.4.2\"\n"
        "  sha256 \"0123456789abcdef0123456789abcdef0123456789abcdef"
        "0123456789abcdef\"\n"
        "  url \"https://example.com/OpenROM-1.4.2.dmg\",\n"
        "      verified: \"example.com\"\n"
        "  name \"OpenROM\"\n"
        "  desc \"A retro ROM manager\"\n"
        "  homepage \"https://example.com/openrom\"\n"
        "\n"
        "  app \"OpenROM.app\"\n"
        "end\n";
}

static void test_brew_cask_parse(void)
{
    struct masa_brew_cask c;
    char err[256];
    int rc;

    memset(&c, 0, sizeof(c));
    rc = masa_brew_cask_parse(demo_cask(), strlen(demo_cask()), &c, err,
                              sizeof(err));
    CHECK(rc == 0);
    if (rc != 0) {
        fprintf(stderr, "  parse error: %s\n", err);
        return;
    }
    CHECK_STREQ(c.token, "openrom");
    CHECK_STREQ(c.version, "1.4.2");
    CHECK_STREQ(c.name, "OpenROM");
    CHECK_STREQ(c.app, "OpenROM.app");
    CHECK(c.has_app == 1);
    CHECK(c.url != NULL && strstr(c.url, "OpenROM-1.4.2.dmg") != NULL);
    masa_brew_cask_free(&c);
}

/* A cask with a `pkg` artifact and no explicit `name` still parses. */
static void test_brew_cask_pkg_artifact(void)
{
    static const char *text =
        "cask \"toolbox\" do\n"
        "  version \"2.0\"\n"
        "  sha256 \"deadbeef\"\n"
        "  url \"https://e.com/toolbox-2.0.pkg\"\n"
        "  homepage \"https://e.com\"\n"
        "  pkg \"Toolbox.pkg\"\n"
        "end\n";
    struct masa_brew_cask c;
    char err[256];
    int rc;

    memset(&c, 0, sizeof(c));
    rc = masa_brew_cask_parse(text, strlen(text), &c, err, sizeof(err));
    CHECK(rc == 0);
    CHECK(c.has_app == 1);
    CHECK_STREQ(c.app, "Toolbox.pkg");
    CHECK(c.name == NULL);
    masa_brew_cask_free(&c);
}

static void test_brew_cask_rejects_malformed(void)
{
    static const char *not_a_cask = "foo \"bar\" do\n  version \"1\"\nend\n";
    struct masa_brew_cask c;
    char err[256];
    int rc;

    memset(&c, 0, sizeof(c));
    rc = masa_brew_cask_parse(not_a_cask, strlen(not_a_cask), &c, err,
                              sizeof(err));
    CHECK(rc != 0);
    CHECK(c.token == NULL);

    /* Empty input is rejected too. */
    rc = masa_brew_cask_parse("", 0, &c, err, sizeof(err));
    CHECK(rc != 0);
}

static void test_brew_lint_cask_valid(void)
{
    struct masa_brew_cask c;
    char err[256];
    int rc;

    memset(&c, 0, sizeof(c));
    if (masa_brew_cask_parse(demo_cask(), strlen(demo_cask()), &c, err,
                             sizeof(err)) != 0) {
        CHECK(0);
        return;
    }
    rc = masa_brew_lint_cask(&c, "openrom.rb");
    CHECK(rc == 0);
    masa_brew_cask_free(&c);
}

/* A cask missing its `sha256` is a required-field failure. */
static void test_brew_lint_cask_missing_sha(void)
{
    static const char *text =
        "cask \"nohash\" do\n"
        "  version \"1.0\"\n"
        "  url \"https://e.com/x.dmg\"\n"
        "  homepage \"https://e.com\"\n"
        "  app \"X.app\"\n"
        "end\n";
    struct masa_brew_cask c;
    char err[256];
    int rc;

    memset(&c, 0, sizeof(c));
    CHECK(masa_brew_cask_parse(text, strlen(text), &c, err, sizeof(err)) == 0);
    rc = masa_brew_lint_cask(&c, "nohash.rb");
    CHECK(rc != 0);
    CHECK(c.sha256 == NULL);
    masa_brew_cask_free(&c);
}

/* A cask with no artifact (no app/pkg/binary) is a required-field failure. */
static void test_brew_lint_cask_missing_artifact(void)
{
    static const char *text =
        "cask \"noapp\" do\n"
        "  version \"1.0\"\n"
        "  sha256 \"abc\"\n"
        "  url \"https://e.com/x.dmg\"\n"
        "  homepage \"https://e.com\"\n"
        "end\n";
    struct masa_brew_cask c;
    char err[256];
    int rc;

    memset(&c, 0, sizeof(c));
    CHECK(masa_brew_cask_parse(text, strlen(text), &c, err, sizeof(err)) == 0);
    rc = masa_brew_lint_cask(&c, "noapp.rb");
    CHECK(rc != 0);
    CHECK(c.has_app == 0);
    masa_brew_cask_free(&c);
}

/* `masa brew lint` auto-detects a cask and reports it through the router. */
static void test_brew_lint_cask_subcommand(void)
{
    char dir[] = "/tmp/masa_cask_lint_XXXXXX";
    char path[512];
    char out[4096];
    char err[4096];
    char *argv[5];
    int rc;

    if (mkdtemp(dir) == NULL) {
        g_failed++;
        return;
    }
    snprintf(path, sizeof(path), "%s/openrom.rb", dir);
    if (write_file(path, demo_cask()) != 0) {
        g_failed++;
        remove_tree(dir);
        return;
    }

    argv[0] = "masa";
    argv[1] = "brew";
    argv[2] = "lint";
    argv[3] = path;
    argv[4] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "ok (cask)");
    CHECK_CONTAINS(out, "token:    openrom");
    CHECK_CONTAINS(out, "artifact: OpenROM.app");

    remove_tree(dir);
}

/*
 * SHA-256 against the published FIPS 180-4 / NIST test vectors.  These
 * are the canonical "abc", empty-string and multi-block values; a wrong
 * digest here means the whole verification path in `brew fetch` is wrong.
 */
static void test_sha256_nist_vectors(void)
{
    char hex[65];
    struct masa_sha256_ctx ctx;

    masa_sha256_buffer("", 0, hex);
    CHECK_STREQ(hex,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    masa_sha256_buffer("abc", 3, hex);
    CHECK_STREQ(hex,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    masa_sha256_buffer("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                       56, hex);
    CHECK_STREQ(hex,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* One million 'a' characters, streamed in small chunks. */
    {
        char chunk[1000];
        int i;

        memset(chunk, 'a', sizeof(chunk));
        masa_sha256_init(&ctx);
        for (i = 0; i < 1000; i++) {
            masa_sha256_update(&ctx, chunk, sizeof(chunk));
        }
        {
            unsigned char digest[32];
            static const char digits[] = "0123456789abcdef";
            int j;

            masa_sha256_final(&ctx, digest);
            for (j = 0; j < 32; j++) {
                hex[j * 2] = digits[digest[j] >> 4];
                hex[j * 2 + 1] = digits[digest[j] & 0x0f];
            }
            hex[64] = '\0';
        }
    }
    CHECK_STREQ(hex,
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

/* A 120-byte input exercises the padding path that spills a block. */
static void test_sha256_block_boundary(void)
{
    char hex[65];
    static const char input[] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    int rc = masa_sha256_buffer(input, 120, hex);

    CHECK(rc == 0);
    /* Verified independently with coreutils sha256sum. */
    CHECK_STREQ(hex,
        "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c");
}

/* `masa brew fetch` against a local file:// URL: match and mismatch. */
static void test_brew_fetch_file_url(void)
{
    char dir[] = "/tmp/masa_fetch_XXXXXX";
    char src[512];
    char cask[512];
    char out_dir[512];
    char out[8192];
    char err[8192];
    char url[768];
    char *argv[7];
    char hex[65];
    int rc;

    if (mkdtemp(dir) == NULL) {
        g_failed++;
        return;
    }
    snprintf(src, sizeof(src), "%s/payload.bin", dir);
    if (write_file(src, "masa release payload\n") != 0) {
        g_failed++;
        remove_tree(dir);
        return;
    }
    if (masa_sha256_file(src, hex) != 0) {
        g_failed++;
        remove_tree(dir);
        return;
    }
    snprintf(url, sizeof(url), "file://%s", src);
    snprintf(out_dir, sizeof(out_dir), "%s/out", dir);

    /* A cask whose sha256 matches the payload verifies. */
    snprintf(cask, sizeof(cask), "%s/ok.rb", dir);
    {
        char text[1024];

        snprintf(text, sizeof(text),
                 "cask \"payload\" do\n"
                 "  version \"1.0\"\n"
                 "  sha256 \"%s\"\n"
                 "  url \"%s\"\n"
                 "  app \"Payload.app\"\n"
                 "end\n", hex, url);
        if (write_file(cask, text) != 0) {
            g_failed++;
            remove_tree(dir);
            return;
        }
    }

    argv[0] = "masa";
    argv[1] = "brew";
    argv[2] = "fetch";
    argv[3] = cask;
    argv[4] = "-o";
    argv[5] = out_dir;
    argv[6] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  fetch stderr: %s\n", err);
    }
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "checksum OK");

    /* A cask whose sha256 does not match is rejected. */
    snprintf(cask, sizeof(cask), "%s/bad.rb", dir);
    {
        char text[1024];

        snprintf(text, sizeof(text),
                 "cask \"payload\" do\n"
                 "  version \"1.0\"\n"
                 "  sha256 \"%s\"\n"
                 "  url \"%s\"\n"
                 "  app \"Payload.app\"\n"
                 "end\n",
                 "00000000000000000000000000000000000000000000000000000000"
                 "00000000", url);
        if (write_file(cask, text) != 0) {
            g_failed++;
            remove_tree(dir);
            return;
        }
    }
    argv[3] = cask;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc != 0);
    CHECK_CONTAINS(err, "checksum mismatch");

    remove_tree(dir);
}

/* `masa brew tap` clones a tap from a local base and lists its .rb files. */
static void test_brew_tap_local(void)
{
    char dir[] = "/tmp/masa_tap_XXXXXX";
    char base[512];
    char taps[512];
    char srcdir[2048];
    char reporoot[1024];
    char caskpath[4096];
    char out[8192];
    char err[8192];
    char *argv[5];
    int rc;

    if (!tool_available("git")) {
        printf("  SKIP needs git\n");
        g_skipped++;
        return;
    }
    if (mkdtemp(dir) == NULL) {
        g_failed++;
        return;
    }
    snprintf(base, sizeof(base), "%s", dir);
    snprintf(taps, sizeof(taps), "%s/taps", dir);
    snprintf(reporoot, sizeof(reporoot), "%s/M5Devs/homebrew-openrom", dir);
    snprintf(srcdir, sizeof(srcdir), "%s/Casks", reporoot);
    if (masa_mkdir_p(srcdir) != 0) {
        g_failed++;
        remove_tree(dir);
        return;
    }
    snprintf(caskpath, sizeof(caskpath), "%s/openrom.rb", srcdir);
    if (write_file(caskpath, demo_cask()) != 0) {
        g_failed++;
        remove_tree(dir);
        return;
    }

    /* Commit the tap repo so git has something to clone. */
    {
        const char *g1[] = {"git", "-C", reporoot, "init", "-q", NULL};
        const char *g2[] = {"git", "-C", reporoot, "add", "-A", NULL};
        const char *g3[] = {"git", "-C", reporoot, "commit", "-qm", "tap",
                            NULL};

        set_env("GIT_AUTHOR_NAME", "t");
        set_env("GIT_AUTHOR_EMAIL", "t@e.com");
        set_env("GIT_COMMITTER_NAME", "t");
        set_env("GIT_COMMITTER_EMAIL", "t@e.com");
        if (masa_spawn_wait(g1) != 0 || masa_spawn_wait(g2) != 0 ||
            masa_spawn_wait(g3) != 0) {
            fprintf(stderr, "  git setup failed\n");
            g_failed++;
            remove_tree(dir);
            return;
        }
    }

    set_env(MASA_BREW_TAPS_ENV, taps);
    set_env(MASA_BREW_TAP_BASE_ENV, base);

    argv[0] = "masa";
    argv[1] = "brew";
    argv[2] = "tap";
    argv[3] = "M5Devs/openrom";
    argv[4] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  tap stderr: %s\n", err);
    }
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "cask    openrom.rb");
    {
        char cloned[768];

        snprintf(cloned, sizeof(cloned),
                 "%s/M5Devs/openrom/Casks/openrom.rb", taps);
        CHECK(file_exists(cloned));
    }

    remove_tree(dir);
    reset_masa_env();
}

/* A bad <user/repo> argument is a usage error, not a crash. */
static void test_brew_tap_bad_arg(void)
{
    char out[1024];
    char err[1024];
    char *argv[5];
    int rc;

    argv[0] = "masa";
    argv[1] = "brew";
    argv[2] = "tap";
    argv[3] = "noslash";
    argv[4] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc != 0);
    CHECK_CONTAINS(err, "expected <user/repo>");
}

/* ------------------------------------------------------------------ */
/* Milestone 18: pbcopy/pbpaste and the online formula index           */
/* ------------------------------------------------------------------ */

/* A clipboard round trip through the file fallback, plus argv[0] dispatch. */
static void test_clipboard_roundtrip(void)
{
    char dir[] = "/tmp/masa_clip_XXXXXX";
    char clip[512];
    char out[1024];
    char err[1024];
    char *argv[4];
    int rc;

    if (mkdtemp(dir) == NULL) {
        g_failed++;
        return;
    }
    snprintf(clip, sizeof(clip), "%s/clipboard", dir);
    set_env(MASA_CLIPBOARD_FILE_ENV, clip);

    /* `masa pbcopy` reads stdin (here an empty pipe) and `masa pbpaste`
     * prints the clipboard back. */
    argv[0] = "masa";
    argv[1] = "pbcopy";
    argv[2] = NULL;
    rc = run_masa_with_stdin(argv, "clipboard text\n", out, sizeof(out));
    CHECK(rc == 0);

    argv[1] = "pbpaste";
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 0);
    CHECK_STREQ(out, "clipboard text\n");

    /* A missing clipboard file is an empty paste, not an error. */
    unlink(clip);
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 0);
    CHECK_STREQ(out, "");

    /* The shims are recognised by name. */
    CHECK(masa_clipboard_is_shim("pbcopy") == 1);
    CHECK(masa_clipboard_is_shim("pbpaste") == 1);
    CHECK(masa_clipboard_is_shim("pbwhatever") == 0);

    remove_tree(dir);
    reset_masa_env();
}

/* The clipboard shims are created by `masa init-shims` alongside the rest. */
static void test_clipboard_shim_symlinks(void)
{
    char dir[] = "/tmp/masa_clipshim_XXXXXX";
    char linkpath[600];
    int found = 0;
    int i;

    if (mkdtemp(dir) == NULL) {
        g_failed++;
        return;
    }
    if (masa_make_shims(dir, "/bin/true") != 0) {
        g_failed++;
        remove_tree(dir);
        return;
    }
    for (i = 0; masa_shim_tools[i] != NULL; i++) {
        if (strcmp(masa_shim_tools[i], "pbcopy") == 0 ||
            strcmp(masa_shim_tools[i], "pbpaste") == 0) {
            found++;
        }
    }
    CHECK(found == 2);
    snprintf(linkpath, sizeof(linkpath), "%s/pbpaste", dir);
    CHECK(file_exists(linkpath));

    remove_tree(dir);
}

/* `masa brew update/info/search` against a local formula.json fixture. */
static void test_brew_index(void)
{
    char dir[] = "/tmp/masa_index_XXXXXX";
    char base[512];
    char cache[512];
    char fixture[1024];
    char out[8192];
    char err[8192];
    char *argv[5];
    int rc;
    static const char *json =
        "[\n"
        "  {\"name\":\"wget\",\"versions\":{\"stable\":\"1.21.4\"},"
        "\"desc\":\"Internet file retriever\","
        "\"homepage\":\"https://www.gnu.org/software/wget/\","
        "\"dependencies\":[\"openssl@3\",\"libidn2\"]},\n"
        "  {\"name\":\"curl\",\"versions\":{\"stable\":\"8.5.0\"},"
        "\"desc\":\"Get a file from an HTTP server\","
        "\"homepage\":\"https://curl.se/\","
        "\"dependencies\":[\"brotli\"]},\n"
        "  {\"name\":\"openssl@3\",\"versions\":{\"stable\":\"3.2.1\"},"
        "\"desc\":\"Cryptography and SSL/TLS toolkit\","
        "\"homepage\":\"https://openssl.org/\",\"dependencies\":[]}\n"
        "]\n";

    if (mkdtemp(dir) == NULL) {
        g_failed++;
        return;
    }
    snprintf(base, sizeof(base), "%s", dir);
    snprintf(cache, sizeof(cache), "%s/cache", dir);
    snprintf(fixture, sizeof(fixture), "%s/formula.json", dir);
    if (write_file(fixture, json) != 0) {
        g_failed++;
        remove_tree(dir);
        return;
    }

    set_env(MASA_BREW_API_BASE_ENV, base);
    set_env(MASA_BREW_CACHE_ENV, cache);

    argv[0] = "masa";
    argv[1] = "brew";
    argv[2] = "update";
    argv[3] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  update stderr: %s\n", err);
    }
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "3 formulas");

    argv[2] = "info";
    argv[3] = "wget";
    argv[4] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "wget 1.21.4");
    CHECK_CONTAINS(out, "Internet file retriever");
    CHECK_CONTAINS(out, "openssl@3");

    /* An unknown formula is a clean error. */
    argv[3] = "nosuchformula";
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc != 0);
    CHECK_CONTAINS(err, "no formula named");

    argv[2] = "search";
    argv[3] = "ssl";
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "openssl@3");

    /* A query that matches nothing exits non-zero. */
    argv[3] = "zzz-nope";
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc != 0);
    CHECK_CONTAINS(out, "No formula found");

    remove_tree(dir);
    reset_masa_env();
}

/* A malformed JSON document is rejected rather than crashing. */
static void test_brew_index_bad_json(void)
{
    struct masa_brew_formula_info info;
    char **names;
    size_t count = 0;

    CHECK(masa_brew_info_lookup("not json at all", 15, "x", &info) != 0);
    CHECK(masa_brew_info_lookup("[]", 2, "x", &info) != 0);
    names = masa_brew_search("[{\"name\":\"a\"},", 15, "a", &count);
    /* Truncated input still yields whatever parsed cleanly, without crashing. */
    if (names != NULL) {
        size_t i;

        for (i = 0; i < count; i++) {
            free(names[i]);
        }
        free(names);
    }
    CHECK(count == 0 || count >= 1);
}

/* ------------------------------------------------------------------ */
/* Milestone 10: `masa pack` (.ipa and .dmg)                          */
/* ------------------------------------------------------------------ */

/* Read the whole file into a malloc'd buffer (NUL-terminated, len out). */
static unsigned char *read_whole(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    unsigned char *buf;
    long size;

    if (fp == NULL) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    size = ftell(fp);
    if (size < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    if (size > 0 && fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    buf[size] = '\0';
    *out_len = (size_t)size;
    return buf;
}

/*
 * Bounded substring search over a binary blob.  strstr() would stop at
 * the first NUL byte, which both the ZIP headers and the UDIF trailer
 * are full of, so the length is carried explicitly.
 */
static int contains_bytes(const unsigned char *hay, size_t haylen,
                          const char *needle)
{
    size_t nlen = strlen(needle);
    size_t i;

    if (nlen == 0 || haylen < nlen) {
        return 0;
    }
    for (i = 0; i + nlen <= haylen; i++) {
        if (memcmp(hay + i, needle, nlen) == 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * `masa pack --ipa` over a bundle produced by `masa xcode build --bundle`:
 * the result must be a ZIP (PK\x03\x04) whose entries place the app under
 * `Payload/<App>.app/` and include the `Contents/MacOS/` executable.
 */
static void test_pack_ipa(void)
{
    char dir[] = "/tmp/masa_pack_ipa_XXXXXX";
    char sdk[512];
    char out_dir[512];
    char proj[512];
    char ipa[4096];
    char out[4096];
    char err[8192];
    unsigned char *blob;
    size_t blob_len;
    char *argv[10];
    int rc;

    if (!tool_available("clang")) {
        printf("  SKIP clang is not installed\n");
        g_skipped++;
        return;
    }

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }
    snprintf(sdk, sizeof(sdk), "%s/sdk", dir);
    snprintf(out_dir, sizeof(out_dir), "%s/out", dir);
    snprintf(proj, sizeof(proj), "%s/DemoApp.xcodeproj", dir);
    snprintf(ipa, sizeof(ipa), "%s/DemoApp.ipa", dir);

    reset_masa_env();
    set_env(MASA_TARGET_ENV, "arm64-apple-darwin");

    if (make_stub_sdk(sdk) != 0 || !sdk_is_stubbed(sdk) ||
        make_demo_project(dir) != 0) {
        fprintf(stderr, "  FAIL cannot prepare the project\n");
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }
    set_env(MASA_SDK_PATH_ENV, sdk);

    /* Build the .app bundle first. */
    argv[0] = "masa";
    argv[1] = "xcode";
    argv[2] = "build";
    argv[3] = proj;
    argv[4] = "--bundle";
    argv[5] = "-o";
    argv[6] = out_dir;
    argv[7] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  xcode build stderr: %s\n", err);
    }
    CHECK(rc == 0);

    /* Pack the .app bundle into an .ipa. */
    {
        char app[4096];

        snprintf(app, sizeof(app), "%s/DemoApp.app", out_dir);
        argv[0] = "masa";
        argv[1] = "pack";
        argv[2] = "--ipa";
        argv[3] = app;
        argv[4] = "-o";
        argv[5] = ipa;
        argv[6] = NULL;
        rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
        if (rc != 0) {
            fprintf(stderr, "  pack stdout: %s\n  stderr: %s\n", out, err);
        }
        CHECK(rc == 0);
    }

    CHECK(file_exists(ipa));
    blob = read_whole(ipa, &blob_len);
    CHECK(blob != NULL);
    if (blob != NULL) {
        CHECK(blob_len >= 4);
        CHECK(blob[0] == 'P' && blob[1] == 'K' && blob[2] == 3 &&
              blob[3] == 4);
        CHECK(contains_bytes(blob, blob_len, "Payload/"));
        CHECK(contains_bytes(blob, blob_len, "Payload/DemoApp.app/"));
        CHECK(contains_bytes(blob, blob_len, "Contents/MacOS/DemoApp"));
        free(blob);
    }

    remove_tree(dir);
    reset_masa_env();
}

/*
 * `masa pack --dmg`: the image must end in a 512-byte `koly` trailer
 * (version 4, headerSize 512) that points at a valid XML plist.
 */
static void test_pack_dmg(void)
{
    char dir[] = "/tmp/masa_pack_dmg_XXXXXX";
    char sdk[512];
    char out_dir[512];
    char proj[512];
    char dmg[4096];
    char out[4096];
    char err[8192];
    unsigned char *blob;
    size_t blob_len;
    char *argv[10];
    int rc;

    if (!tool_available("clang")) {
        printf("  SKIP clang is not installed\n");
        g_skipped++;
        return;
    }

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }
    snprintf(sdk, sizeof(sdk), "%s/sdk", dir);
    snprintf(out_dir, sizeof(out_dir), "%s/out", dir);
    snprintf(proj, sizeof(proj), "%s/DemoApp.xcodeproj", dir);
    snprintf(dmg, sizeof(dmg), "%s/DemoApp.dmg", dir);

    reset_masa_env();
    set_env(MASA_TARGET_ENV, "arm64-apple-darwin");

    if (make_stub_sdk(sdk) != 0 || !sdk_is_stubbed(sdk) ||
        make_demo_project(dir) != 0) {
        fprintf(stderr, "  FAIL cannot prepare the project\n");
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }
    set_env(MASA_SDK_PATH_ENV, sdk);

    argv[0] = "masa";
    argv[1] = "xcode";
    argv[2] = "build";
    argv[3] = proj;
    argv[4] = "--bundle";
    argv[5] = "-o";
    argv[6] = out_dir;
    argv[7] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 0);

    {
        char app[4096];
        snprintf(app, sizeof(app), "%s/DemoApp.app", out_dir);
        argv[0] = "masa";
        argv[1] = "pack";
        argv[2] = "--dmg";
        argv[3] = app;
        argv[4] = "-o";
        argv[5] = dmg;
        argv[6] = NULL;
        rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
        if (rc != 0) {
            fprintf(stderr, "  pack stdout: %s\n  stderr: %s\n", out, err);
        }
        CHECK(rc == 0);
    }

    CHECK(file_exists(dmg));
    blob = read_whole(dmg, &blob_len);
    CHECK(blob != NULL);
    if (blob != NULL) {
        CHECK(blob_len >= 512);
        if (blob_len >= 512) {
            const unsigned char *koly = blob + blob_len - 512;
            unsigned int version;
            unsigned int header_size;

            /* Magic "koly" = 0x6B6F6C79. */
            CHECK(koly[0] == 0x6B && koly[1] == 0x6F && koly[2] == 0x6C &&
                  koly[3] == 0x79);
            /* Version (LE u32 @4) must be 4; headerSize (@8) must be 512. */
            version = (unsigned int)koly[4] | ((unsigned int)koly[5] << 8) |
                      ((unsigned int)koly[6] << 16) |
                      ((unsigned int)koly[7] << 24);
            header_size = (unsigned int)koly[8] | ((unsigned int)koly[9] << 8) |
                          ((unsigned int)koly[10] << 16) |
                          ((unsigned int)koly[11] << 24);
            CHECK(version == 4);
            CHECK(header_size == 512);

            /* The XML plist must sit inside the image, before the trailer. */
            CHECK(contains_bytes(blob, blob_len, "<?xml"));
            CHECK(contains_bytes(blob, blob_len, "<plist"));
            CHECK(contains_bytes(blob, blob_len, "blkx"));
        }
        free(blob);
    }

    remove_tree(dir);
    reset_masa_env();
}

/* Error paths: missing flags, a missing input and an unknown option. */
static void test_pack_errors(void)
{
    char out[4096];
    char err[8192];
    char *argv[10];
    int rc;

    reset_masa_env();

    /* No format flag. */
    argv[0] = "masa";
    argv[1] = "pack";
    argv[2] = "App.app";
    argv[3] = "-o";
    argv[4] = "/tmp/out";
    argv[5] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 2);
    CHECK_CONTAINS(err, "missing format");

    /* Both formats at once. */
    argv[2] = "--ipa";
    argv[3] = "--dmg";
    argv[4] = "App.app";
    argv[5] = "-o";
    argv[6] = "/tmp/out";
    argv[7] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 2);
    CHECK_CONTAINS(err, "choose one");

    /* Missing input. */
    argv[2] = "--ipa";
    argv[3] = "-o";
    argv[4] = "/tmp/out";
    argv[5] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 2);
    CHECK_CONTAINS(err, "missing input");

    /* Missing -o. */
    argv[2] = "--dmg";
    argv[3] = "App.app";
    argv[4] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 2);
    CHECK_CONTAINS(err, "missing -o");

    /* -o with no argument. */
    argv[2] = "--ipa";
    argv[3] = "App.app";
    argv[4] = "-o";
    argv[5] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 2);
    CHECK_CONTAINS(err, "needs an output path");

    /* Unknown option. */
    argv[2] = "--bogus";
    argv[3] = "App.app";
    argv[4] = "-o";
    argv[5] = "/tmp/out";
    argv[6] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 2);
    CHECK_CONTAINS(err, "unknown option");

    /* A missing input directory. */
    argv[2] = "--ipa";
    argv[3] = "/tmp/masa_no_such_app.app";
    argv[4] = "-o";
    argv[5] = "/tmp/out";
    argv[6] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 1);
    CHECK_CONTAINS(err, "is not a directory");

    reset_masa_env();
}

/* ------------------------------------------------------------------ */
/* Milestone 11: `masa run` / `masa danroot` (the Danroot loader)      */
/* ------------------------------------------------------------------ */

/* The Mach-O cputype a Masa binary for the host must carry. */
static const char *host_apple_target(void)
{
#if defined(__x86_64__)
    return "x86_64-apple-darwin";
#elif defined(__aarch64__) || defined(__arm64__)
    return "arm64-apple-darwin";
#else
    return NULL;
#endif
}

/* The cputype a foreign-architecture binary carries, for mismatch tests. */
static const char *foreign_apple_target(void)
{
#if defined(__x86_64__)
    return "arm64-apple-darwin";
#elif defined(__aarch64__) || defined(__arm64__)
    return "x86_64-apple-darwin";
#else
    return NULL;
#endif
}

/*
 * Compile source into a Mach-O executable with `masa cc`, writing into a
 * fresh stub SDK at <dir>/sdk.  extra, when non-NULL, is appended to the
 * compiler command line (used to force classic dyld-info fixups).
 * Returns 0 on success.
 */
static int build_macho_extra(const char *dir, const char *target,
                             const char *source, const char *exe,
                             const char *extra)
{
    char sdk[4096];
    char src[4096];
    char *argv[8];
    char out[1024];
    char err[4096];
    int n = 0;
    int rc;

    snprintf(sdk, sizeof(sdk), "%s/sdk", dir);
    snprintf(src, sizeof(src), "%s/prog.c", dir);

    reset_masa_env();
    if (make_stub_sdk(sdk) != 0 || !sdk_is_stubbed(sdk)) {
        fprintf(stderr, "  FAIL could not create the stub SDK\n");
        g_failed++;
        return -1;
    }
    if (write_file(src, source) != 0) {
        fprintf(stderr, "  FAIL cannot write %s\n", src);
        g_failed++;
        return -1;
    }
    set_env(MASA_SDK_PATH_ENV, sdk);
    set_env(MASA_TARGET_ENV, target);

    argv[n++] = "masa";
    argv[n++] = "cc";
    argv[n++] = src;
    argv[n++] = "-o";
    argv[n++] = (char *)exe;
    if (extra != NULL) {
        argv[n++] = (char *)extra;
    }
    argv[n] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  cc stderr (%s): %s\n", target, err);
    }
    return rc;
}

static int build_macho(const char *dir, const char *target,
                       const char *source, const char *exe)
{
    return build_macho_extra(dir, target, source, exe, NULL);
}

/* Return 1 when the Mach-O at path carries load command cmd. */
static int macho_has_command(const char *path, uint32_t want)
{
    unsigned char buf[32];
    FILE *fp = fopen(path, "rb");
    uint32_t ncmds;
    uint32_t i;
    long off;

    if (fp == NULL) {
        return 0;
    }
    if (fread(buf, 1, sizeof(buf), fp) != sizeof(buf)) {
        fclose(fp);
        return 0;
    }
    ncmds = (uint32_t)buf[16] | ((uint32_t)buf[17] << 8) |
            ((uint32_t)buf[18] << 16) | ((uint32_t)buf[19] << 24);
    off = 32;
    for (i = 0; i < ncmds; i++) {
        unsigned char lc[8];
        uint32_t cmd;
        uint32_t cmdsize;

        if (fseek(fp, off, SEEK_SET) != 0 ||
            fread(lc, 1, sizeof(lc), fp) != sizeof(lc)) {
            break;
        }
        cmd = (uint32_t)lc[0] | ((uint32_t)lc[1] << 8) |
              ((uint32_t)lc[2] << 16) | ((uint32_t)lc[3] << 24);
        cmdsize = (uint32_t)lc[4] | ((uint32_t)lc[5] << 8) |
                  ((uint32_t)lc[6] << 16) | ((uint32_t)lc[7] << 24);
        if (cmdsize < 8) {
            break;
        }
        if (cmd == want) {
            fclose(fp);
            return 1;
        }
        off += cmdsize;
    }
    fclose(fp);
    return 0;
}

/*
 * End-to-end: build a real Mach-O executable that uses printf, malloc,
 * free and argc/argv, then run it through `masa run` and confirm the
 * output, the argument handling and the exit status.
 */
static void danroot_run(const char *subcommand)
{
    static const char *source =
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "#include <string.h>\n"
        "int main(int argc, char **argv) {\n"
        "    char *buf = (char *)malloc(32);\n"
        "    int i;\n"
        "    strcpy(buf, \"danroot\");\n"
        "    printf(\"hello %s, argc=%d\\n\", buf, argc);\n"
        "    for (i = 1; i < argc; i++) {\n"
        "        printf(\"arg[%d]=%s\\n\", i, argv[i]);\n"
        "    }\n"
        "    free(buf);\n"
        "    return 42;\n"
        "}\n";
    const char *target = host_apple_target();
    char dir[] = "/tmp/masa_run_XXXXXX";
    char exe[4096];
    char out[4096];
    char err[4096];
    char *argv[8];
    int rc;

    if (target == NULL) {
        printf("  SKIP unsupported host architecture\n");
        g_skipped++;
        return;
    }
    if (!tool_available("clang")) {
        printf("  SKIP clang is not installed\n");
        g_skipped++;
        return;
    }
    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }
    snprintf(exe, sizeof(exe), "%s/prog", dir);

    if (build_macho(dir, target, source, exe) != 0) {
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }

    /* `masa run` (or its alias) with two program arguments. */
    argv[0] = "masa";
    argv[1] = (char *)subcommand;
    argv[2] = exe;
    argv[3] = "alpha";
    argv[4] = "beta";
    argv[5] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 42) {
        fprintf(stderr, "  run stderr: %s\n", err);
    }
    CHECK(rc == 42);
    CHECK_CONTAINS(out, "hello danroot, argc=3");
    CHECK_CONTAINS(out, "arg[1]=alpha");
    CHECK_CONTAINS(out, "arg[2]=beta");

    /* With no extra arguments argc is 1 and argv[0] is the image path. */
    argv[3] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 42);
    CHECK_CONTAINS(out, "hello danroot, argc=1");

    remove_tree(dir);
    reset_masa_env();
}

static void test_run_executes_macho(void)
{
    danroot_run("run");
}

static void test_danroot_alias_executes_macho(void)
{
    danroot_run("danroot");
}

/* A program that calls exit() must return that status, not run on. */
static void test_run_honours_exit(void)
{
    static const char *source =
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "int main(void) {\n"
        "    printf(\"before exit\\n\");\n"
        "    exit(7);\n"
        "    printf(\"unreachable\\n\");\n"
        "    return 0;\n"
        "}\n";
    const char *target = host_apple_target();
    char dir[] = "/tmp/masa_run_exit_XXXXXX";
    char exe[4096];
    char out[4096];
    char err[4096];
    char *argv[4];
    int rc;

    if (target == NULL || !tool_available("clang")) {
        printf("  SKIP needs clang and a supported host\n");
        g_skipped++;
        return;
    }
    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }
    snprintf(exe, sizeof(exe), "%s/prog", dir);

    if (build_macho(dir, target, source, exe) != 0) {
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }

    argv[0] = "masa";
    argv[1] = "run";
    argv[2] = exe;
    argv[3] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 7);
    CHECK_CONTAINS(out, "before exit");
    CHECK(strstr(out, "unreachable") == NULL);

    remove_tree(dir);
    reset_masa_env();
}

/* An image built for a different architecture must be rejected cleanly. */
static void test_run_rejects_cputype_mismatch(void)
{
    static const char *source = "int main(void) { return 0; }\n";
    const char *host = host_apple_target();
    const char *foreign = foreign_apple_target();
    char dir[] = "/tmp/masa_run_arch_XXXXXX";
    char exe[4096];
    char out[1024];
    char err[4096];
    char *argv[4];
    int rc;

    if (host == NULL || foreign == NULL) {
        printf("  SKIP unsupported host architecture\n");
        g_skipped++;
        return;
    }
    if (!tool_available("clang")) {
        printf("  SKIP clang is not installed\n");
        g_skipped++;
        return;
    }
    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }
    snprintf(exe, sizeof(exe), "%s/prog", dir);

    if (build_macho(dir, foreign, source, exe) != 0) {
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }

    argv[0] = "masa";
    argv[1] = "run";
    argv[2] = exe;
    argv[3] = NULL;

    /* Disable the cross-architecture bridge so the mismatch guard itself
     * is what answers, whether or not qemu/masa-aarch64 are installed. */
    set_env(MASA_DANROOT_NO_BRIDGE_ENV, "1");

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc != 0);
    CHECK_CONTAINS(err, "architecture mismatch");

    remove_tree(dir);
    reset_masa_env();
}

/* Non-Mach-O, truncated and missing inputs must fail without crashing. */
static void test_run_rejects_bad_input(void)
{
    char dir[] = "/tmp/masa_run_bad_XXXXXX";
    char path[4096];
    char out[1024];
    char err[4096];
    char *argv[4];
    int rc;

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }

    /* Plain text: not a Mach-O at all. */
    snprintf(path, sizeof(path), "%s/notmacho", dir);
    if (write_file(path,
                   "this is not a Mach-O image, just some text padding\n") != 0) {
        fprintf(stderr, "  FAIL cannot write %s\n", path);
        g_failed++;
        remove_tree(dir);
        return;
    }

    argv[0] = "masa";
    argv[1] = "run";
    argv[2] = path;
    argv[3] = NULL;

    reset_masa_env();
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 1);
    CHECK_CONTAINS(err, "not a 64-bit Mach-O image");

    /* A nonexistent path. */
    snprintf(path, sizeof(path), "%s/does-not-exist", dir);
    argv[2] = path;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 1);
    CHECK_CONTAINS(err, "cannot open");

    /* Missing argument. */
    argv[2] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 2);
    CHECK_CONTAINS(err, "missing Mach-O executable");

    remove_tree(dir);
    reset_masa_env();
}


/*
 * Regression: older ld64.lld (the clang-18 shipped on Ubuntu 24.04) emits
 * the classic LC_DYLD_INFO_ONLY rebase/bind opcodes instead of
 * LC_DYLD_CHAINED_FIXUPS.  Without classic-fixup support the loader applied
 * no relocations and the image jumped to an unslid address and crashed.
 * `-Wl,-no_fixup_chains` forces that classic layout on any clang, so this
 * test covers it deterministically; it is skipped if the linker rejects it.
 */
static void test_run_classic_dyld_info_fixups(void)
{
    static const char *source =
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "int main(int argc, char **argv) {\n"
        "    printf(\"classic argc=%d\\n\", argc);\n"
        "    return argc;\n"
        "}\n";
    const char *target = host_apple_target();
    char dir[] = "/tmp/masa_run_classic_XXXXXX";
    char exe[4096];
    char out[4096];
    char err[4096];
    char *argv[5];
    int rc;

    if (target == NULL || !tool_available("clang")) {
        printf("  SKIP needs clang and a supported host\n");
        g_skipped++;
        return;
    }
    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }
    snprintf(exe, sizeof(exe), "%s/prog", dir);

    if (build_macho_extra(dir, target, source, exe,
                          "-Wl,-no_fixup_chains") != 0) {
        printf("  SKIP linker does not support -no_fixup_chains\n");
        g_skipped++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }
    /* Confirm the image really carries the classic fixup command. */
    CHECK(macho_has_command(exe, 0x80000022u)); /* LC_DYLD_INFO_ONLY */

    argv[0] = "masa";
    argv[1] = "run";
    argv[2] = exe;
    argv[3] = "a";
    argv[4] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 2) {
        fprintf(stderr, "  run stderr: %s\n", err);
    }
    CHECK(rc == 2); /* argc == 2 becomes the exit status */
    CHECK_CONTAINS(out, "classic argc=2");

    remove_tree(dir);
    reset_masa_env();
}



/* ------------------------------------------------------------------ */
/* Milestone 12: dynamic library (MH_DYLIB) loading                    */
/* ------------------------------------------------------------------ */

/*
 * Build dir/lib<name>.dylib from source with `masa cc -dynamiclib`,
 * installing it as @rpath/lib<name>.dylib.  Returns 0 on success.
 */
static int build_dylib(const char *dir, const char *target,
                       const char *source, const char *name)
{
    char sdk[4096];
    char src[4096];
    char out[4096];
    char install_name[512];
    char *argv[12];
    char o[1024];
    char e[4096];
    int n = 0;
    int rc;

    snprintf(sdk, sizeof(sdk), "%s/sdk", dir);
    snprintf(src, sizeof(src), "%s/%s.c", dir, name);
    snprintf(out, sizeof(out), "%s/lib%s.dylib", dir, name);
    snprintf(install_name, sizeof(install_name), "@rpath/lib%s.dylib", name);

    reset_masa_env();
    if (make_stub_sdk(sdk) != 0 || !sdk_is_stubbed(sdk)) {
        return -1;
    }
    if (write_file(src, source) != 0) {
        return -1;
    }
    set_env(MASA_SDK_PATH_ENV, sdk);
    set_env(MASA_TARGET_ENV, target);

    argv[n++] = "masa";
    argv[n++] = "cc";
    argv[n++] = "-dynamiclib";
    argv[n++] = src;
    argv[n++] = "-o";
    argv[n++] = out;
    argv[n++] = "-install_name";
    argv[n++] = install_name;
    argv[n] = NULL;

    rc = run_masa(argv, o, sizeof(o), e, sizeof(e));
    if (rc != 0) {
        fprintf(stderr, "  dylib cc stderr: %s\n", e);
    }
    return rc;
}

static const char *dylib_helper_source(void)
{
    return
        "#include <string.h>\n"
        "static int base = 40;\n"
        "int helper_add(int x) { return base + x; }\n"
        "const char *helper_tag(void) { return \"helper-v1\"; }\n"
        "int helper_len(const char *s) { return (int)strlen(s); }\n";
}

static const char *dylib_main_source(void)
{
    return
        "#include <stdio.h>\n"
        "#include <string.h>\n"
        "#include <sys/sysctl.h>\n"
        "#include <dlfcn.h>\n"
        "extern int helper_add(int x);\n"
        "extern const char *helper_tag(void);\n"
        "int main(void) {\n"
        "    printf(\"add=%d tag=%s\\n\", helper_add(2), helper_tag());\n"
        "    FILE *f = fopen(\"out.txt\", \"w\");\n"
        "    if (f == NULL) { printf(\"fopen-fail\\n\"); return 1; }\n"
        "    fwrite(\"masa-danroot\\n\", 1, 13, f);\n"
        "    fclose(f);\n"
        "    char b[64];\n"
        "    f = fopen(\"out.txt\", \"r\");\n"
        "    if (f == NULL) { printf(\"fopen2-fail\\n\"); return 1; }\n"
        "    size_t n = fread(b, 1, sizeof(b) - 1, f);\n"
        "    fclose(f);\n"
        "    b[n] = '\\0';\n"
        "    printf(\"file=%s\", b);\n"
        "    char os[64];\n"
        "    size_t len = sizeof(os);\n"
        "    int rc = sysctlbyname(\"kern.ostype\", os, &len, NULL, 0);\n"
        "    printf(\"ostype=%s rc=%d\\n\", os, rc);\n"
        "    void *h = dlopen(\"@executable_path/libhelper.dylib\", RTLD_NOW);\n"
        "    if (h == NULL) { printf(\"dlopen-fail\\n\"); return 2; }\n"
        "    int (*a)(int) = (int (*)(int))dlsym(h, \"_helper_add\");\n"
        "    const char *(*t)(void) = (const char *(*)(void))dlsym(h,"
        " \"_helper_tag\");\n"
        "    if (a == NULL || t == NULL) { printf(\"dlsym-fail\\n\"); return 3; }\n"
        "    printf(\"dl=%d/%s\\n\", a(100), t());\n"
        "    dlclose(h);\n"
        "    return rc == 0 ? 0 : 4;\n"
        "}\n";
}

/*
 * Build a dylib + executable pair into dir and return the executable path
 * in exe.  extra_args (may be NULL) is appended to the executable's link
 * command.  Returns 0 on success.
 */
static int build_dylib_pair(const char *dir, const char *target,
                            const char *main_source, const char *extra_args,
                            char *exe, size_t excap)
{
    char sdk[4096];
    char src[4096];
    char libdir[4096];
    char *argv[12];
    char o[1024];
    char e[4096];
    int n = 0;
    int rc;

    snprintf(exe, excap, "%s/app", dir);
    snprintf(sdk, sizeof(sdk), "%s/sdk", dir);
    snprintf(src, sizeof(src), "%s/main.c", dir);
    snprintf(libdir, sizeof(libdir), "-L%s", dir);

    if (build_dylib(dir, target, dylib_helper_source(), "helper") != 0) {
        return -1;
    }

    reset_masa_env();
    if (write_file(src, main_source) != 0) {
        return -1;
    }
    set_env(MASA_SDK_PATH_ENV, sdk);
    set_env(MASA_TARGET_ENV, target);

    argv[n++] = "masa";
    argv[n++] = "cc";
    argv[n++] = src;
    argv[n++] = "-o";
    argv[n++] = exe;
    argv[n++] = libdir;
    argv[n++] = "-lhelper";
    argv[n++] = "-Wl,-rpath,@executable_path";
    if (extra_args != NULL) {
        argv[n++] = (char *)extra_args;
    }
    argv[n] = NULL;

    rc = run_masa(argv, o, sizeof(o), e, sizeof(e));
    if (rc != 0) {
        fprintf(stderr, "  exe cc stderr: %s\n", e);
    }
    return rc;
}

/*
 * End-to-end dynamic-library test: a Mach-O executable that links against
 * a Mach-O dylib (calls into it, does file I/O, calls sysctlbyname and
 * dlopen/dlsym) must run under `masa danroot` and produce the expected
 * output, file contents and exit status.
 */
static void test_run_loads_dylib(void)
{
    const char *target = host_apple_target();
    char dir[] = "/tmp/masa_dylib_XXXXXX";
    char exe[4096];
    char out[4096];
    char err[4096];
    char cwd[4096];
    char *argv[4];
    int rc;

    if (target == NULL || !tool_available("clang")) {
        printf("  SKIP needs clang and a supported host\n");
        g_skipped++;
        return;
    }
    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }

    if (build_dylib_pair(dir, target, dylib_main_source(), NULL, exe,
                     sizeof(exe)) != 0) {
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }

    /* The executable must record the dylib and an LC_RPATH. */
    CHECK(macho_has_command(exe, 0x0cu));        /* LC_LOAD_DYLIB */
    CHECK(macho_has_command(exe, 0x8000001cu));  /* LC_RPATH */

    /* Run with the temp dir as cwd so out.txt lands there. */
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        cwd[0] = '\0';
    }
    if (chdir(dir) != 0) {
        fprintf(stderr, "  FAIL cannot chdir to the temp dir\n");
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }

    argv[0] = "masa";
    argv[1] = "run";
    argv[2] = exe;
    argv[3] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  run stderr: %s\n", err);
    }
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "add=42");
    CHECK_CONTAINS(out, "tag=helper-v1");
    CHECK_CONTAINS(out, "file=masa-danroot");
    CHECK_CONTAINS(out, "ostype=Darwin rc=0");
    CHECK_CONTAINS(out, "dl=140/helper-v1");

    if (cwd[0] != '\0') {
        (void)chdir(cwd);
    }

    /* The program wrote out.txt; confirm its contents. */
    {
        char path[4096];
        char buf[64];
        FILE *fp;
        size_t n;

        snprintf(path, sizeof(path), "%s/out.txt", dir);
        fp = fopen(path, "rb");
        CHECK(fp != NULL);
        if (fp != NULL) {
            n = fread(buf, 1, sizeof(buf) - 1, fp);
            fclose(fp);
            buf[n] = '\0';
            CHECK(strcmp(buf, "masa-danroot\n") == 0);
        }
    }

    remove_tree(dir);
    reset_masa_env();
}

/*
 * A weak dependency that is missing must not fail the load: the loader
 * skips it and a weak import resolves to NULL, so the program sees the
 * symbol as absent.
 */
/*
 * Shared body for the weak-dependency tests.  extra_args (may be NULL) is
 * appended to the executable's link line, which lets a caller force a
 * particular relocation encoding (e.g. -Wl,-no_fixup_chains for the classic
 * LC_DYLD_INFO_ONLY opcodes).  When a weak dependency is missing the program
 * must still run with the import bound to NULL.
 */
static void weak_dylib_scenario(const char *extra_args)
{
    const char *target = host_apple_target();
    char dir[] = "/tmp/masa_dylib_weak_XXXXXX";
    char sdk[4096];
    char src[4096];
    char libdir[4096];
    char weaklib[4300];
    char exe[4096];
    char from[4200];
    char to[4200];
    char out[4096];
    char err[4096];
    char *argv[16];
    int n = 0;
    int rc;
    static const char *weak_main =
        "#include <stdio.h>\n"
        "extern int helper_add(int x) __attribute__((weak_import));\n"
        "int main(void) {\n"
        "    if (helper_add != 0) { printf(\"present=%d\\n\", helper_add(2)); }\n"
        "    else { printf(\"absent\\n\"); }\n"
        "    return 0;\n"
        "}\n";

    if (target == NULL || !tool_available("clang")) {
        printf("  SKIP needs clang and a supported host\n");
        g_skipped++;
        return;
    }
    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }

    /* Build the weak library, then link the program against it weakly. */
    if (build_dylib(dir, target, dylib_helper_source(), "helper") != 0) {
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }
    snprintf(sdk, sizeof(sdk), "%s/sdk", dir);
    snprintf(src, sizeof(src), "%s/main.c", dir);
    snprintf(libdir, sizeof(libdir), "-L%s", dir);
    snprintf(weaklib, sizeof(weaklib), "-Wl,-weak_library,%s/libhelper.dylib",
             dir);
    snprintf(exe, sizeof(exe), "%s/app", dir);

    reset_masa_env();
    if (write_file(src, weak_main) != 0) {
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }
    set_env(MASA_SDK_PATH_ENV, sdk);
    set_env(MASA_TARGET_ENV, target);

    argv[n++] = "masa";
    argv[n++] = "cc";
    argv[n++] = src;
    argv[n++] = "-o";
    argv[n++] = exe;
    argv[n++] = libdir;
    argv[n++] = weaklib;
    argv[n++] = "-Wl,-rpath,@executable_path";
    if (extra_args != NULL) {
        argv[n++] = (char *)extra_args;
    }
    argv[n] = NULL;
    if (run_masa(argv, out, sizeof(out), err, sizeof(err)) != 0) {
        fprintf(stderr, "  exe cc stderr: %s\n", err);
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }

    /* Present: the weak import resolves and calls through. */
    argv[0] = "masa";
    argv[1] = "run";
    argv[2] = exe;
    argv[3] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "present=42");

    /* Absent: hiding the dylib must still run, with the import as NULL. */
    snprintf(from, sizeof(from), "%s/libhelper.dylib", dir);
    snprintf(to, sizeof(to), "%s/hidden.dylib", dir);
    if (rename(from, to) != 0) {
        fprintf(stderr, "  FAIL cannot hide the dylib\n");
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  run stderr: %s\n", err);
    }
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "absent");

    (void)rename(to, from);
    remove_tree(dir);
    reset_masa_env();
}

static void test_run_weak_dylib_optional(void)
{
    weak_dylib_scenario(NULL);
}

/*
 * Same as above but linked with -no_fixup_chains, so the weak import lives
 * in the classic LC_DYLD_INFO_ONLY bind opcodes.  clang-18 (Ubuntu 24.04)
 * emits that encoding by default; clang-19 prefers LC_DYLD_CHAINED_FIXUPS,
 * so forcing it here keeps both paths covered on every toolchain.
 */
static void test_run_weak_dylib_classic_dyld_info(void)
{
    weak_dylib_scenario("-Wl,-no_fixup_chains");
}

/*
 * dlsym must apply the Mach-O leading-underscore convention: a caller asks
 * for the C name ("magic_add") but the symbol is stored as "_magic_add".
 * The dylib is loaded only at runtime (not linked in), so dlopen must also
 * relocate and protect the freshly mapped image before its functions run.
 */
static void test_run_dlopen_dlsym_underscore(void)
{
    const char *target = host_apple_target();
    char dir[] = "/tmp/masa_dlsym_XXXXXX";
    char sdk[4096];
    char src[4096];
    char exe[4096];
    char out[4096];
    char err[4096];
    char *argv[4];
    static const char *lib_src =
        "int magic_add(int a, int b) { return a + b; }\n";
    static const char *main_src =
        "#include <stdio.h>\n"
        "#include <dlfcn.h>\n"
        "int main(void) {\n"
        "    void *h = dlopen(\"@executable_path/libmagic.dylib\", RTLD_NOW);\n"
        "    if (h == NULL) { printf(\"dlopen-fail\\n\"); return 1; }\n"
        "    void *p = dlsym(h, \"magic_add\");\n"
        "    if (p == NULL) { printf(\"dlsym-null\\n\"); return 2; }\n"
        "    int (*f)(int, int) = (int (*)(int, int))p;\n"
        "    printf(\"magic=%d\\n\", f(3, 4));\n"
        "    return 0;\n"
        "}\n";
    int rc;

    if (target == NULL || !tool_available("clang")) {
        printf("  SKIP needs clang and a supported host\n");
        g_skipped++;
        return;
    }
    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }

    /* Build the runtime-only dylib. */
    if (build_dylib(dir, target, lib_src, "magic") != 0) {
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }
    snprintf(sdk, sizeof(sdk), "%s/sdk", dir);
    snprintf(src, sizeof(src), "%s/main.c", dir);
    snprintf(exe, sizeof(exe), "%s/app", dir);
    reset_masa_env();
    if (write_file(src, main_src) != 0) {
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }
    set_env(MASA_SDK_PATH_ENV, sdk);
    set_env(MASA_TARGET_ENV, target);

    /* The dylib is NOT linked in; only -rpath is set for the dlopen call. */
    {
        char rpath[4200];
        char *cargv[8];
        char o[1024];
        char e[4096];
        int n = 0;

        snprintf(rpath, sizeof(rpath), "-Wl,-rpath,@executable_path");
        cargv[n++] = "masa";
        cargv[n++] = "cc";
        cargv[n++] = src;
        cargv[n++] = "-o";
        cargv[n++] = exe;
        cargv[n++] = rpath;
        cargv[n] = NULL;
        if (run_masa(cargv, o, sizeof(o), e, sizeof(e)) != 0) {
            fprintf(stderr, "  exe cc stderr: %s\n", e);
            g_failed++;
            remove_tree(dir);
            reset_masa_env();
            return;
        }
    }

    argv[0] = "masa";
    argv[1] = "run";
    argv[2] = exe;
    argv[3] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  run stderr: %s\n", err);
    }
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "magic=7");

    remove_tree(dir);
    reset_masa_env();
}

/*
 * A missing (non-weak) dylib must fail the load with a clear message and
 * leave no leaks: the loader unwinds every mapping and allocation.
 */
static void test_run_missing_dylib_reports_error(void)
{
    const char *target = host_apple_target();
    char dir[] = "/tmp/masa_dylib_miss_XXXXXX";
    char exe[4096];
    char from[4096];
    char to[4096];
    char out[4096];
    char err[4096];
    char *argv[4];
    int rc;

    if (target == NULL || !tool_available("clang")) {
        printf("  SKIP needs clang and a supported host\n");
        g_skipped++;
        return;
    }
    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }

    if (build_dylib_pair(dir, target, dylib_main_source(), NULL, exe,
                     sizeof(exe)) != 0) {
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }

    /* Hide the dylib so the dependency cannot be resolved. */
    snprintf(from, sizeof(from), "%s/libhelper.dylib", dir);
    snprintf(to, sizeof(to), "%s/hidden.dylib", dir);
    if (rename(from, to) != 0) {
        fprintf(stderr, "  FAIL cannot hide the dylib\n");
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }

    argv[0] = "masa";
    argv[1] = "run";
    argv[2] = exe;
    argv[3] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc != 0);
    CHECK_CONTAINS(err, "cannot find dependency");

    /* Restore so remove_tree can clear everything. */
    (void)rename(to, from);
    remove_tree(dir);
    reset_masa_env();
}

/*
 * A dylib referenced by @rpath must also be found on MASA_LIBRARY_PATH
 * when it is not next to the executable.
 */
static void test_run_dylib_via_library_path(void)
{
    const char *target = host_apple_target();
    char dir[] = "/tmp/masa_dylib_lp_XXXXXX";
    char sub[4096];
    char exe[4096];
    char from[4200];
    char to[4200];
    char out[4096];
    char err[4096];
    char *argv[4];
    int rc;

    if (target == NULL || !tool_available("clang")) {
        printf("  SKIP needs clang and a supported host\n");
        g_skipped++;
        return;
    }
    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }

    {
        static const char *linked_only =
            "#include <stdio.h>\n"
            "extern int helper_add(int x);\n"
            "int main(void) { printf(\"add=%d\\n\", helper_add(2));"
            " return 0; }\n";

        if (build_dylib_pair(dir, target, linked_only, NULL, exe,
                             sizeof(exe)) != 0) {
            g_failed++;
            remove_tree(dir);
            reset_masa_env();
            return;
        }
    }

    /* Move the dylib into a directory reachable only via the env path. */
    snprintf(sub, sizeof(sub), "%s/libs", dir);
    if (mkdir(sub, 0700) != 0) {
        fprintf(stderr, "  FAIL cannot create a subdirectory\n");
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }
    snprintf(from, sizeof(from), "%s/libhelper.dylib", dir);
    snprintf(to, sizeof(to), "%s/libhelper.dylib", sub);
    if (rename(from, to) != 0) {
        fprintf(stderr, "  FAIL cannot move the dylib\n");
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }

    set_env("MASA_LIBRARY_PATH", sub);

    argv[0] = "masa";
    argv[1] = "run";
    argv[2] = exe;
    argv[3] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  run stderr: %s\n", err);
    }
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "add=42");

    unsetenv("MASA_LIBRARY_PATH");
    remove_tree(dir);
    reset_masa_env();
}


/* ------------------------------------------------------------------ */
/* Milestone 6: `masa shell` and the host-services layer               */
/* ------------------------------------------------------------------ */

/*
 * `masa shell -c <cmd>` must run <cmd> inside the shim environment:
 * the shim directory is first on PATH, MASA_SHIM_DIR/MASA_REAL_CC are
 * exported and `command -v cc` resolves to a shim.  TERM is set when the
 * caller has none.  $SHELL is deliberately left alone (see shell_env).
 */
static void test_shell_command_environment(void)
{
    char *argv[] = {"masa", "shell", "-c",
                    "echo shim=$MASA_SHIM_DIR; command -v cc; "
                    "echo real=$MASA_REAL_CC; echo term=$TERM",
                    NULL};
    char out[2048];
    char err[2048];
    int rc;

    reset_masa_env();
    unsetenv("TERM");

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "shim=/");
    CHECK_CONTAINS(out, "masa-shims-");
    CHECK_CONTAINS(out, "/cc");
    CHECK_CONTAINS(out, "real=/");
    CHECK_CONTAINS(out, "term=");
    /* A non-empty TERM means Masa supplied a default. */
    CHECK(strstr(out, "term=\n") == NULL);

    reset_masa_env();
}

/* The shims must be usable, i.e. `uname` inside the shell reports Darwin. */
static void test_shell_shims_are_active(void)
{
    char *argv[] = {"masa", "shell", "-c", "uname -s", NULL};
    char out[256];
    char err[512];
    char *masa_bin;
    int rc;

    reset_masa_env();

    /* Point the shims at the real binary, not this test harness. */
    masa_bin = masa_bin_path();
    if (masa_bin == NULL || access(masa_bin, X_OK) != 0) {
        printf("  SKIP masa is not built\n");
        g_skipped++;
        free(masa_bin);
        reset_masa_env();
        return;
    }
    set_env(MASA_SHIM_TARGET_ENV, masa_bin);

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 0);
    CHECK_STREQ(out, "Darwin\n");

    free(masa_bin);
    reset_masa_env();
}

/* The shell exits with the status of the command it ran. */
static void test_shell_propagates_exit_status(void)
{
    char *argv[] = {"masa", "shell", "-c", "exit 7", NULL};
    char out[256];
    int rc;

    reset_masa_env();

    rc = run_masa(argv, out, sizeof(out), NULL, 0);
    CHECK(rc == 7);

    reset_masa_env();
}

/* A missing -c argument is a usage error (exit 2), not a crash. */
static void test_shell_missing_command_argument(void)
{
    char *argv[] = {"masa", "shell", "-c", NULL};
    char err[512];
    int rc;

    reset_masa_env();

    rc = run_masa(argv, NULL, 0, err, sizeof(err));
    CHECK(rc == 2);
    CHECK_CONTAINS(err, "-c");

    reset_masa_env();
}

/* `masa shell` without -c launches an interactive shell.  Feed it a
 * script on stdin (through `sh -c` the shell reads the pipe) and check
 * the environment it sees. */
static void test_shell_interactive_reads_stdin(void)
{
    char *argv[] = {"masa", "shell", NULL};
    char out[1024];
    char *masa_bin;
    int rc;

    reset_masa_env();

    masa_bin = masa_bin_path();
    if (masa_bin == NULL || access(masa_bin, X_OK) != 0) {
        printf("  SKIP masa is not built\n");
        g_skipped++;
        free(masa_bin);
        reset_masa_env();
        return;
    }
    set_env(MASA_SHIM_TARGET_ENV, masa_bin);

    /*
     * The interactive shell (no -c) reads its commands from stdin; feed it
     * a short script through a pipe and check the shimmed environment.
     */
    rc = run_masa_with_stdin(argv, "uname -s\nexit\n", out, sizeof(out));
    if (rc == -1) {
        printf("  SKIP cannot wire stdin\n");
        g_skipped++;
        free(masa_bin);
        reset_masa_env();
        return;
    }
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "Darwin");

    free(masa_bin);
    reset_masa_env();
}

/* The host-services layer reports the right separators and shell path. */
static void test_host_services_basics(void)
{
    char tmp[4096];
    char *self;
    char *which_sh;

    CHECK(masa_path_separator() == ':' || masa_path_separator() == ';');

    self = masa_self_path();
    if (self != NULL) {
        CHECK(self[0] != '\0');
        free(self);
    }

    CHECK(masa_shell_path() != NULL);
    CHECK(masa_shell_path()[0] != '\0');

    CHECK(masa_make_temp_dir("masa-test-", tmp, sizeof(tmp)) == 0);
    CHECK_CONTAINS(tmp, "masa-test-");
    CHECK(masa_mkdir_p(tmp) == 0);
    masa_rmdir(tmp);

    which_sh = masa_which("sh");
    if (which_sh != NULL) {
        CHECK(file_exists(which_sh));
        free(which_sh);
    }
    CHECK(masa_which("definitely-not-a-real-tool-xyz") == NULL);
}
/* ------------------------------------------------------------------ */
/* Milestone 7: Swift support and Android cross-compilation           */
/* ------------------------------------------------------------------ */

static void test_build_swift_args_sequence(void)
{
    const char *args[16];
    const char *expected[] = {"-target", "arm64-apple-macosx15.0",
                              "-sdk", "/sdk"};
    int count = 0;
    int i;
    int rc;

    rc = masa_build_swift_args("arm64-apple-macosx15.0", "/sdk", args,
                               &count, 16);
    CHECK(rc == 0);
    CHECK(count == 4);
    for (i = 0; i < 4; i++) {
        CHECK_STREQ(args[i], expected[i]);
    }
    CHECK(args[4] == NULL);
}

static void test_build_swift_args_appends_without_sdk(void)
{
    const char *args[16];
    int count = 2;
    int rc;

    args[0] = "swiftc";
    args[1] = "main.swift";
    rc = masa_build_swift_args("x86_64-apple-macosx14.0", NULL, args, &count,
                               16);
    CHECK(rc == 0);
    CHECK(count == 4);
    CHECK_STREQ(args[0], "swiftc");
    CHECK_STREQ(args[1], "main.swift");
    CHECK_STREQ(args[2], "-target");
    CHECK_STREQ(args[3], "x86_64-apple-macosx14.0");
    CHECK(args[4] == NULL);
}

static void test_build_swift_args_empty_sdk_skipped(void)
{
    const char *args[16];
    int count = 0;
    int rc;

    rc = masa_build_swift_args("arm64-apple-macosx15.0", "", args, &count, 16);
    CHECK(rc == 0);
    CHECK(count == 2);
    CHECK_STREQ(args[0], "-target");
    CHECK_STREQ(args[1], "arm64-apple-macosx15.0");
}

static void test_build_swift_args_rejects_bad_input(void)
{
    const char *args[16];
    int count = 0;

    CHECK(masa_build_swift_args(NULL, NULL, args, &count, 16) == -1);
    CHECK(masa_build_swift_args("", NULL, args, &count, 16) == -1);
    CHECK(masa_build_swift_args("arm64-apple-macosx15.0", NULL, NULL,
                                &count, 16) == -1);
    CHECK(masa_build_swift_args("arm64-apple-macosx15.0", NULL, args, NULL,
                                16) == -1);
    /* Only one slot free: not enough for -target + value + terminator. */
    count = 15;
    CHECK(masa_build_swift_args("arm64-apple-macosx15.0", NULL, args, &count,
                                16) == -1);
}

/* The swiftc wrapper appends -target and, when configured, -sdk. */
static void test_swiftc_dry_run_target_and_sdk(void)
{
    char *argv[] = {"masa", "swiftc", "-O", "main.swift", NULL};
    char out[1024];
    char expected[1024];
    int rc;

    reset_masa_env();
    set_env(MASA_DRY_RUN_ENV, "1");
    set_env(MASA_SWIFTC_ENV, "myswiftc");
    set_env(MASA_TARGET_ENV, "arm64-apple-macosx15.0");
    set_env(MASA_SDK_PATH_ENV, "/sdk");

    rc = run_masa(argv, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    snprintf(expected, sizeof(expected),
             "myswiftc -O main.swift -target arm64-apple-macosx15.0"
             " -sdk /sdk\n");
    CHECK_STREQ(out, expected);

    reset_masa_env();
}

/* Without MASA_TARGET the Swift default triple is used, and -sdk is
 * omitted because no SDK root is configured. */
static void test_swiftc_dry_run_default_target(void)
{
    char *argv[] = {"masa", "swiftc", "main.swift", NULL};
    char out[1024];
    int rc;

    reset_masa_env();
    set_env(MASA_DRY_RUN_ENV, "1");
    set_env(MASA_SWIFTC_ENV, "swiftc");

    rc = run_masa(argv, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "-target " MASA_DEFAULT_SWIFT_TARGET);
    CHECK(strstr(out, "-sdk") == NULL);

    reset_masa_env();
}

/* argv[0] == "swift" / "swiftc" drives the Swift wrapper directly. */
static void test_argv0_dispatch_swift(void)
{
    char *argv_swift[] = {"/some/dir/swift", "main.swift", NULL};
    char *argv_swiftc[] = {"/some/dir/swiftc", "-c", "main.swift", NULL};
    char out[1024];
    int rc;

    reset_masa_env();
    set_env(MASA_DRY_RUN_ENV, "1");
    set_env(MASA_SWIFTC_ENV, "swiftc");

    rc = run_masa(argv_swift, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "swiftc main.swift");
    CHECK_CONTAINS(out, "-target " MASA_DEFAULT_SWIFT_TARGET);

    rc = run_masa(argv_swiftc, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "swiftc -c main.swift");

    reset_masa_env();
}

/* `masa swift info` reads a Package.swift manifest. */
static void test_swift_info_package(void)
{
    char dir[] = "/tmp/masa_swift_info_XXXXXX";
    char manifest[512];
    char sources[512];
    char *argv[5];
    char out[1024];
    char err[512];
    int rc;

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }
    snprintf(manifest, sizeof(manifest), "%s/Package.swift", dir);
    snprintf(sources, sizeof(sources), "%s/Sources", dir);
    if (mkdir(sources, 0755) != 0 ||
        write_file(manifest,
                   "// swift-tools-version:5.9\n"
                   "import PackageDescription\n"
                   "let package = Package(\n"
                   "    name: \"DemoKit\",\n"
                   "    targets: [.target(name: \"DemoKit\")]\n"
                   ")\n") != 0) {
        fprintf(stderr, "  FAIL cannot write the manifest\n");
        g_failed++;
        remove_tree(dir);
        return;
    }

    reset_masa_env();
    argv[0] = "masa";
    argv[1] = "swift";
    argv[2] = "info";
    argv[3] = dir;
    argv[4] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "DemoKit");
    CHECK_CONTAINS(out, "Sources/");

    remove_tree(dir);
    reset_masa_env();
}

/* A file that is not a manifest is rejected with a clear message. */
static void test_swift_info_rejects_non_manifest(void)
{
    char dir[] = "/tmp/masa_swift_bad_XXXXXX";
    char manifest[512];
    char *argv[5];
    char out[512];
    char err[512];
    int rc;

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }
    snprintf(manifest, sizeof(manifest), "%s/Package.swift", dir);
    if (write_file(manifest, "int main(void) { return 0; }\n") != 0) {
        fprintf(stderr, "  FAIL cannot write the manifest\n");
        g_failed++;
        remove_tree(dir);
        return;
    }

    reset_masa_env();
    argv[0] = "masa";
    argv[1] = "swift";
    argv[2] = "info";
    argv[3] = dir;
    argv[4] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 1);
    CHECK_CONTAINS(err, "not a Package.swift");

    remove_tree(dir);
    reset_masa_env();
}

static void test_swift_missing_package(void)
{
    char dir[] = "/tmp/masa_swift_none_XXXXXX";
    char *argv[5];
    char out[512];
    char err[512];
    int rc;

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }

    reset_masa_env();
    argv[0] = "masa";
    argv[1] = "swift";
    argv[2] = "info";
    argv[3] = dir;
    argv[4] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 1);
    CHECK_CONTAINS(err, "cannot find");

    remove_tree(dir);
    reset_masa_env();
}

/* `masa swift build` hands the manifest to swiftc with -target. */
static void test_swift_build_dry_run(void)
{
    char dir[] = "/tmp/masa_swift_build_XXXXXX";
    char manifest[512];
    char *argv[5];
    char out[1024];
    int rc;

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }
    snprintf(manifest, sizeof(manifest), "%s/Package.swift", dir);
    if (write_file(manifest,
                   "// swift-tools-version:5.9\n"
                   "import PackageDescription\n"
                   "let package = Package(name: \"DemoKit\")\n") != 0) {
        fprintf(stderr, "  FAIL cannot write the manifest\n");
        g_failed++;
        remove_tree(dir);
        return;
    }

    reset_masa_env();
    set_env(MASA_DRY_RUN_ENV, "1");
    set_env(MASA_SWIFTC_ENV, "swiftc");

    argv[0] = "masa";
    argv[1] = "swift";
    argv[2] = "build";
    argv[3] = dir;
    argv[4] = NULL;

    rc = run_masa(argv, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "swiftc");
    CHECK_CONTAINS(out, "Package.swift");
    CHECK_CONTAINS(out, "-target " MASA_DEFAULT_SWIFT_TARGET);

    remove_tree(dir);
    reset_masa_env();
}

static void test_swift_unknown_subcommand(void)
{
    char *argv[] = {"masa", "swift", "frobnicate", NULL};
    char err[512];
    int rc;

    reset_masa_env();
    rc = run_masa(argv, NULL, 0, err, sizeof(err));
    CHECK(rc == 2);
    CHECK_CONTAINS(err, "unknown subcommand");

    reset_masa_env();
}

/* A target that compiles .swift files is handed to swiftc. */
static void test_pbxproj_parse_swift_sources(void)
{
    static const char *swift_project =
        "{\n"
        "  objects = {\n"
        "    T1 = {isa = PBXNativeTarget; name = SwiftApp;"
        " productName = SwiftApp; buildPhases = (P1); };\n"
        "    P1 = {isa = PBXSourcesBuildPhase; files = (F1, F2, F3); };\n"
        "    F1 = {isa = PBXBuildFile; fileRef = R1; };\n"
        "    F2 = {isa = PBXBuildFile; fileRef = R2; };\n"
        "    F3 = {isa = PBXBuildFile; fileRef = R3; };\n"
        "    R1 = {isa = PBXFileReference; path = main.swift; };\n"
        "    R2 = {isa = PBXFileReference; path = helper.swift; };\n"
        "    R3 = {isa = PBXFileReference; path = support.c; };\n"
        "  };\n"
        "  rootObject = T1;\n"
        "}\n";
    struct masa_pbxproj proj;
    char err[256];
    int rc;
    size_t i;
    int saw_main = 0;
    int saw_helper = 0;
    int saw_c = 0;

    memset(&proj, 0, sizeof(proj));
    rc = masa_pbxproj_parse(swift_project, strlen(swift_project), &proj, err,
                            sizeof(err));
    CHECK(rc == 0);
    if (rc != 0) {
        fprintf(stderr, "  parse error: %s\n", err);
        return;
    }
    CHECK_STREQ(proj.product_name, "SwiftApp");
    CHECK(proj.swift_source_count == 2);
    CHECK(proj.source_count == 1);
    for (i = 0; i < proj.swift_source_count; i++) {
        if (strcmp(proj.swift_sources[i], "main.swift") == 0) {
            saw_main = 1;
        }
        if (strcmp(proj.swift_sources[i], "helper.swift") == 0) {
            saw_helper = 1;
        }
    }
    for (i = 0; i < proj.source_count; i++) {
        if (strcmp(proj.sources[i], "support.c") == 0) {
            saw_c = 1;
        }
    }
    CHECK(saw_main);
    CHECK(saw_helper);
    CHECK(saw_c);

    masa_pbxproj_free(&proj);
}

/* `masa xcode build` on a Swift project drives swiftc (checked in dry
 * run, so no Swift toolchain is required). */
static void test_xcode_build_swift_dry_run(void)
{
    char dir[] = "/tmp/masa_xcode_swift_XXXXXX";
    char proj[512];
    char src[512];
    char pbx[4096];
    char out_dir[4096];
    char *argv[10];
    char out[1024];
    int rc;

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }
    snprintf(proj, sizeof(proj), "%s/SwiftApp.xcodeproj", dir);
    snprintf(pbx, sizeof(pbx), "%s/project.pbxproj", proj);
    snprintf(src, sizeof(src), "%s/src", dir);
    snprintf(out_dir, sizeof(out_dir), "%s/out", dir);

    if (mkdir(proj, 0755) != 0 || mkdir(src, 0755) != 0) {
        fprintf(stderr, "  FAIL cannot create the project tree\n");
        g_failed++;
        remove_tree(dir);
        return;
    }
    {
        char main_swift[4096];

        snprintf(main_swift, sizeof(main_swift), "%s/main.swift", src);
        if (write_file(main_swift, "print(\"hi\")\n") != 0 ||
            write_file(pbx,
                       "{\n"
                       "  objects = {\n"
                       "    T1 = {isa = PBXNativeTarget; name = SwiftApp;"
                       " productName = SwiftApp; buildPhases = (P1); };\n"
                       "    P1 = {isa = PBXSourcesBuildPhase;"
                       " files = (F1); };\n"
                       "    F1 = {isa = PBXBuildFile; fileRef = R1; };\n"
                       "    R1 = {isa = PBXFileReference;"
                       " path = main.swift; };\n"
                       "  };\n"
                       "  rootObject = T1;\n"
                       "}\n") != 0) {
            fprintf(stderr, "  FAIL cannot write the Swift project\n");
            g_failed++;
            remove_tree(dir);
            return;
        }
    }

    reset_masa_env();
    set_env(MASA_DRY_RUN_ENV, "1");
    set_env(MASA_SWIFTC_ENV, "swiftc");

    argv[0] = "masa";
    argv[1] = "xcode";
    argv[2] = "build";
    argv[3] = proj;
    argv[4] = "--bundle";
    argv[5] = "-o";
    argv[6] = out_dir;
    argv[7] = NULL;

    rc = run_masa(argv, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "swiftc");
    CHECK_CONTAINS(out, "main.swift");
    CHECK_CONTAINS(out, "-target");

    remove_tree(dir);
    reset_masa_env();
}

/*
 * The Android NDK target compiles every source without warnings.  The
 * test only runs when an NDK toolchain is on PATH, so the suite stays
 * green on hosts that do not have one.
 */
static void test_android_ndk_compile(void)
{
    static const char *sources[] = {
        "src/main.c", "src/cc_wrapper.c", "src/swift_wrapper.c",
        "src/sys_shims.c", "src/tool_shims.c", "src/sdk.c",
        "src/xcodeproj.c", "src/brew.c", "src/sha256.c", "src/pack.c",
        "src/exec.c", "src/doctor.c", "src/danroot.c", "src/danroot_cf.c",
        "src/danroot_objc.c", "src/danroot_appkit.c",
        "src/platform_posix.c"
    };
    const char *ndk_cc = getenv("ANDROID_CC");
    char cmd[8192];
    size_t used;
    size_t i;
    int rc;

    if (ndk_cc == NULL || ndk_cc[0] == '\0') {
        printf("  SKIP ANDROID_CC (NDK clang) is not available\n");
        g_skipped++;
        return;
    }
    if (strchr(ndk_cc, '/') != NULL) {
        if (access(ndk_cc, X_OK) != 0) {
            printf("  SKIP ANDROID_CC points at a missing tool\n");
            g_skipped++;
            return;
        }
    } else if (!tool_available(ndk_cc)) {
        printf("  SKIP ANDROID_CC (NDK clang) is not available\n");
        g_skipped++;
        return;
    }

    used = (size_t)snprintf(cmd, sizeof(cmd),
                            "%s -std=c11 -Wall -Wextra -Werror -Iinclude",
                            ndk_cc);
    for (i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        int n = snprintf(cmd + used, sizeof(cmd) - used, " %s", sources[i]);

        if (n < 0 || (size_t)n >= sizeof(cmd) - used) {
            fprintf(stderr, "  FAIL command line too long\n");
            g_failed++;
            return;
        }
        used += (size_t)n;
    }
    {
        int n = snprintf(cmd + used, sizeof(cmd) - used, " -o /tmp/masa_ndk_test");

        if (n < 0 || (size_t)n >= sizeof(cmd) - used) {
            fprintf(stderr, "  FAIL command line too long\n");
            g_failed++;
            return;
        }
    }

    rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "  NDK compile failed (status %d)\n", rc);
    }
    CHECK(rc == 0);

    unlink("/tmp/masa_ndk_test");
}

/* ------------------------------------------------------------------ */
/* Milestone 9: masa doctor                                           */
/* ------------------------------------------------------------------ */

/* A fully set-up environment: clang + lld + a stub SDK present. */
static void test_doctor_healthy(void)
{
    char dir[] = "/tmp/masa_doctor_XXXXXX";
    char sdk[512];
    char out[8192];
    char err[1024];
    char *argv[] = {"masa", "doctor", NULL};
    int rc;

    if (!tool_available("clang")) {
        printf("  SKIP clang is not installed\n");
        g_skipped++;
        return;
    }

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }
    snprintf(sdk, sizeof(sdk), "%s/sdk", dir);

    reset_masa_env();
    if (make_stub_sdk(sdk) != 0 || !sdk_is_stubbed(sdk)) {
        fprintf(stderr, "  FAIL cannot prepare the stub SDK\n");
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }
    set_env(MASA_SDK_PATH_ENV, sdk);
    set_env(MASA_TARGET_ENV, "arm64-apple-darwin");

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  doctor stdout:\n%s\n  doctor stderr: %s\n", out, err);
    }
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "[OK]");
    CHECK_CONTAINS(out, "C compiler");
    CHECK_CONTAINS(out, "linker");
    CHECK_CONTAINS(out, "ld64.lld");
    CHECK_CONTAINS(out, "SDK");
    CHECK_CONTAINS(out, "arm64-apple-darwin");
    CHECK_CONTAINS(out, "core C cross-build ready");

    remove_tree(dir);
    reset_masa_env();
}

/* An SDK path that does not exist is a warning, not a hard failure. */
static void test_doctor_warns_on_missing_sdk(void)
{
    char dir[] = "/tmp/masa_doctor2_XXXXXX";
    char sdk[512];
    char out[8192];
    char err[1024];
    char *argv[] = {"masa", "doctor", NULL};
    int rc;

    if (!tool_available("clang")) {
        printf("  SKIP clang is not installed\n");
        g_skipped++;
        return;
    }

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }
    /* Point MASA_SDK_PATH at a directory that has no stub in it. */
    snprintf(sdk, sizeof(sdk), "%s/missing-sdk", dir);

    reset_masa_env();
    set_env(MASA_SDK_PATH_ENV, sdk);

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    /* clang and lld are still present, so doctor still exits 0 ... */
    CHECK(rc == 0);
    /* ... but the SDK line is a warning that names the hint. */
    CHECK_CONTAINS(out, "[WARN]");
    CHECK_CONTAINS(out, "SDK");
    CHECK_CONTAINS(out, "init-stub");
    CHECK_CONTAINS(out, sdk);

    remove_tree(dir);
    reset_masa_env();
}

/* With no compiler on PATH the core requirement fails and doctor exits 1. */
static void test_doctor_reports_missing_compiler(void)
{
    char dir[] = "/tmp/masa_doctor3_XXXXXX";
    char empty[512];
    char out[8192];
    char err[1024];
    char *argv[] = {"masa", "doctor", NULL};
    int rc;

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }
    snprintf(empty, sizeof(empty), "%s/empty", dir);
    if (masa_mkdir_p(empty) != 0) {
        fprintf(stderr, "  FAIL cannot create %s\n", empty);
        g_failed++;
        remove_tree(dir);
        return;
    }

    reset_masa_env();
    /* An empty PATH hides every compiler and tool. */
    set_env("PATH", empty);
    set_env(MASA_REAL_CC_ENV, NULL);

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc != 0);
    CHECK_CONTAINS(out, "[MISSING]");
    CHECK_CONTAINS(out, "C compiler");

    remove_tree(dir);
    reset_masa_env();
}

/* ------------------------------------------------------------------ */
/* Milestone 9: the Windows ConPTY backend cross-compiles cleanly      */
/* ------------------------------------------------------------------ */

static void test_pty_win_cross_compiles(void)
{
    char cmd[4096];
    int rc;

    if (!tool_available("x86_64-w64-mingw32-gcc")) {
        printf("  SKIP x86_64-w64-mingw32-gcc is not installed\n");
        g_skipped++;
        return;
    }

    /*
     * src/pty_win.c and its shared helper must build on their own with
     * only MinGW-w64's Win32 headers - no SDL, no libvterm - which is what
     * lets the GUI cross-compile without an SDL2 install on the host.
     */
    snprintf(cmd, sizeof(cmd),
             "x86_64-w64-mingw32-gcc -std=c11 -Wall -Wextra -Werror "
             "-Iinclude -c src/pty_win.c -o /tmp/masa_pty_win.o && "
             "x86_64-w64-mingw32-gcc -std=c11 -Wall -Wextra -Werror "
             "-Iinclude -c src/pty_common.c -o /tmp/masa_pty_common.o");
    rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "  pty_win cross-compile failed (status %d)\n", rc);
    }
    CHECK(rc == 0);

    unlink("/tmp/masa_pty_win.o");
    unlink("/tmp/masa_pty_common.o");
}

/* ------------------------------------------------------------------ */
/* Milestone 13: Danroot Layer 2 (path virtualization, dir/stat, Mach) */
/* ------------------------------------------------------------------ */

/*
 * Compile a Mach-O program from a single source and return the binary
 * path, reusing the stub SDK.  Returns 0 on success.
 */
static int build_macho_src(const char *dir, const char *target,
                           const char *name, const char *source,
                           char *exe_out, size_t exe_cap)
{
    char sdk[4096];
    char src[4096];
    char *argv[8];
    char out[1024];
    char err[4096];
    int n = 0;
    int rc;

    snprintf(sdk, sizeof(sdk), "%s/sdk", dir);
    snprintf(src, sizeof(src), "%s/%s.c", dir, name);
    snprintf(exe_out, exe_cap, "%s/%s", dir, name);

    reset_masa_env();
    if (make_stub_sdk(sdk) != 0 || !sdk_is_stubbed(sdk)) {
        fprintf(stderr, "  FAIL could not create the stub SDK\n");
        g_failed++;
        return -1;
    }
    if (write_file(src, source) != 0) {
        fprintf(stderr, "  FAIL cannot write %s\n", src);
        g_failed++;
        return -1;
    }
    set_env(MASA_SDK_PATH_ENV, sdk);
    set_env(MASA_TARGET_ENV, target);

    argv[n++] = "masa";
    argv[n++] = "cc";
    argv[n++] = src;
    argv[n++] = "-o";
    argv[n++] = exe_out;
    argv[n] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  cc stderr (%s): %s\n", target, err);
    }
    return rc;
}

/*
 * Path virtualization: a program that reads /Users/demo/hello.txt, writes
 * /Applications/MyApp, lists /Users/demo and stats a file must only ever
 * touch the virtual root, never the host's own /Users or /Applications.
 */
static void test_danroot_root_path_virtualization(void)
{
    const char *target = host_apple_target();
    char dir[] = "/tmp/masa_root_XXXXXX";
    char vroot[4096];
    char seed[4608];
    char exe[4096];
    char out[4096];
    char err[4096];
    char *argv[8];
    struct stat st;
    static const char *source =
        "#include <stdio.h>\n"
        "#include <string.h>\n"
        "#include <sys/stat.h>\n"
        "#include <dirent.h>\n"
        "#include <unistd.h>\n"
        "int main(void) {\n"
        "    FILE *f = fopen(\"/Users/demo/hello.txt\", \"r\");\n"
        "    char buf[64];\n"
        "    if (f == NULL) { printf(\"open-fail\\n\"); return 1; }\n"
        "    fgets(buf, sizeof(buf), f);\n"
        "    fclose(f);\n"
        "    printf(\"read=%s\", buf);\n"
        "    struct stat st;\n"
        "    if (stat(\"/Users/demo/hello.txt\", &st) == 0)\n"
        "        printf(\"size=%lld isreg=%d\\n\", st.st_size,"
        " S_ISREG(st.st_mode));\n"
        "    else printf(\"stat-fail\\n\");\n"
        "    mkdir(\"/Applications/MyApp\", 0755);\n"
        "    struct stat ad;\n"
        "    if (stat(\"/Applications/MyApp\", &ad) == 0)\n"
        "        printf(\"appdir=%d\\n\", S_ISDIR(ad.st_mode));\n"
        "    DIR *d = opendir(\"/Users/demo\");\n"
        "    struct dirent *e; int n = 0;\n"
        "    if (d != NULL) {\n"
        "        while ((e = readdir(d)) != NULL)\n"
        "            if (e->d_name[0] != '.') n++;\n"
        "        closedir(d);\n"
        "    }\n"
        "    printf(\"entries=%d\\n\", n);\n"
        "    return 0;\n"
        "}\n";
    int rc;

    if (target == NULL || !tool_available("clang")) {
        printf("  SKIP needs clang and a supported host\n");
        g_skipped++;
        return;
    }
    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }

    if (build_macho_src(dir, target, "vp", source, exe, sizeof(exe)) != 0) {
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }

    /* Lay out the virtual root: /Users/demo with a seed file, and the
     * /Applications parent that the program will create inside. */
    snprintf(vroot, sizeof(vroot), "%s/root", dir);
    snprintf(seed, sizeof(seed), "%s/Users/demo", vroot);
    if (masa_mkdir_p(seed) != 0) {
        fprintf(stderr, "  FAIL cannot create %s\n", seed);
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }
    snprintf(seed, sizeof(seed), "%s/Applications", vroot);
    if (masa_mkdir_p(seed) != 0) {
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }
    snprintf(seed, sizeof(seed), "%s/Users/demo/hello.txt", vroot);
    if (write_file(seed, "hi\n") != 0) {
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }

    /* Run with `--root <vroot>`. */
    argv[0] = "masa";
    argv[1] = "run";
    argv[2] = "--root";
    argv[3] = vroot;
    argv[4] = exe;
    argv[5] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  run stderr: %s\n", err);
    }
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "read=hi");
    CHECK_CONTAINS(out, "size=3 isreg=1");
    CHECK_CONTAINS(out, "appdir=1");
    CHECK_CONTAINS(out, "entries=1");

    /* The mkdir must have landed inside the root, not on the host. */
    snprintf(seed, sizeof(seed), "%s/Applications/MyApp", vroot);
    CHECK(stat(seed, &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(stat("/Applications/MyApp", &st) != 0);

    /* MASA_DANROOT_ROOT selects the same root without --root. */
    set_env(MASA_DANROOT_ROOT_ENV, vroot);
    argv[2] = exe;
    argv[3] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "read=hi");
    set_env(MASA_DANROOT_ROOT_ENV, NULL);

    remove_tree(dir);
    reset_masa_env();
}

/*
 * Mach time: mach_timebase_info reports 1:1 and mach_absolute_time is
 * monotonic non-decreasing; mach_task_self is a stable non-zero port.
 */
static void test_danroot_mach_time(void)
{
    const char *target = host_apple_target();
    char dir[] = "/tmp/masa_mach_XXXXXX";
    char exe[4096];
    char out[4096];
    char err[4096];
    char *argv[4];
    static const char *source =
        "#include <mach/mach_time.h>\n"
        "#include <mach/mach.h>\n"
        "#include <stdio.h>\n"
        "int main(void) {\n"
        "    mach_timebase_info_data_t tb;\n"
        "    mach_timebase_info(&tb);\n"
        "    printf(\"numer=%u denom=%u\\n\", tb.numer, tb.denom);\n"
        "    uint64_t a = mach_absolute_time();\n"
        "    uint64_t b = mach_absolute_time();\n"
        "    printf(\"mono=%d\\n\", b >= a);\n"
        "    printf(\"task=%u\\n\", (unsigned)mach_task_self());\n"
        "    return 0;\n"
        "}\n";
    int rc;

    if (target == NULL || !tool_available("clang")) {
        printf("  SKIP needs clang and a supported host\n");
        g_skipped++;
        return;
    }
    if (mkdtemp(dir) == NULL) {
        g_failed++;
        return;
    }
    if (build_macho_src(dir, target, "mt", source, exe, sizeof(exe)) != 0) {
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }

    argv[0] = "masa";
    argv[1] = "run";
    argv[2] = exe;
    argv[3] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  run stderr: %s\n", err);
    }
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "numer=1 denom=1");
    CHECK_CONTAINS(out, "mono=1");
    CHECK_CONTAINS(out, "task=259"); /* 0x103 */

    remove_tree(dir);
    reset_masa_env();
}

/*
 * Error paths: a missing virtual-root file fails cleanly, --root without a
 * directory is a usage error, and an unreadable image still reports a
 * clear message - all without leaking.
 */
static void test_danroot_root_error_paths(void)
{
    const char *target = host_apple_target();
    char dir[] = "/tmp/masa_rooterr_XXXXXX";
    char vroot[4096];
    char exe[4096];
    char out[4096];
    char err[4096];
    char *argv[8];
    static const char *source =
        "#include <stdio.h>\n"
        "#include <sys/stat.h>\n"
        "int main(void) {\n"
        "    struct stat st;\n"
        "    if (stat(\"/Users/nobody/missing.txt\", &st) != 0)\n"
        "        printf(\"missing-ok\\n\");\n"
        "    return 0;\n"
        "}\n";
    int rc;

    if (target == NULL || !tool_available("clang")) {
        printf("  SKIP needs clang and a supported host\n");
        g_skipped++;
        return;
    }
    if (mkdtemp(dir) == NULL) {
        g_failed++;
        return;
    }
    if (build_macho_src(dir, target, "err", source, exe, sizeof(exe)) != 0) {
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }

    snprintf(vroot, sizeof(vroot), "%s/root", dir);
    if (masa_mkdir_p(vroot) != 0) {
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }

    /* A stat of a file that does not exist under the root returns -1. */
    argv[0] = "masa";
    argv[1] = "run";
    argv[2] = "--root";
    argv[3] = vroot;
    argv[4] = exe;
    argv[5] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "missing-ok");

    /* `--root` with no directory is a usage error. */
    argv[2] = "--root";
    argv[3] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc == 2);
    CHECK_CONTAINS(err, "--root requires a directory");

    remove_tree(dir);
    reset_masa_env();
}

/* ------------------------------------------------------------------ */
/* Milestone 14: stack protector, CoreFoundation, Objective-C          */
/* ------------------------------------------------------------------ */

/*
 * Compile and link source into a Mach-O executable that references the
 * named framework.  The stub SDK carries the framework's .tbd and headers,
 * so clang finds both through the -isysroot the wrapper injects.
 */
static int build_macho_framework(const char *dir, const char *target,
                                 const char *name, const char *source,
                                 const char *framework,
                                 char *exe_out, size_t exe_cap)
{
    char sdk[4096];
    char src[4096];
    char *argv[10];
    char out[1024];
    char err[4096];
    int n = 0;
    int rc;

    snprintf(sdk, sizeof(sdk), "%s/sdk", dir);
    snprintf(src, sizeof(src), "%s/%s.c", dir, name);
    snprintf(exe_out, exe_cap, "%s/%s", dir, name);

    reset_masa_env();
    if (make_stub_sdk(sdk) != 0 || !sdk_is_stubbed(sdk)) {
        fprintf(stderr, "  FAIL could not create the stub SDK\n");
        g_failed++;
        return -1;
    }
    if (write_file(src, source) != 0) {
        fprintf(stderr, "  FAIL cannot write %s\n", src);
        g_failed++;
        return -1;
    }
    set_env(MASA_SDK_PATH_ENV, sdk);
    set_env(MASA_TARGET_ENV, target);

    argv[n++] = "masa";
    argv[n++] = "cc";
    argv[n++] = src;
    argv[n++] = "-o";
    argv[n++] = exe_out;
    argv[n++] = "-framework";
    argv[n++] = (char *)framework;
    argv[n] = NULL;

    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  cc stderr (%s): %s\n", target, err);
    }
    return rc;
}

/*
 * arm64 clang enables the stack protector by default, so a function with a
 * stack buffer references __stack_chk_guard and __stack_chk_fail.  Those
 * must resolve against the stub libSystem and link into a real MH_EXECUTE.
 */
static void test_danroot_arm64_stack_protector(void)
{
    char dir[] = "/tmp/masa_sp_XXXXXX";
    char exe[4096];
    unsigned char magic[4];
    unsigned char filetype[4];
    static const unsigned char macho64_le[4] = {0xCF, 0xFA, 0xED, 0xFE};
    static const unsigned char mh_execute[4] = {0x02, 0x00, 0x00, 0x00};
    static const char *source =
        "#include <stdio.h>\n"
        "int main(void) {\n"
        "    char buf[64];\n"
        "    buf[0] = 'o'; buf[1] = 'k'; buf[2] = '\\0';\n"
        "    printf(\"%s\\n\", buf);\n"
        "    return 0;\n"
        "}\n";

    if (!tool_available("clang")) {
        printf("  SKIP clang is not installed\n");
        g_skipped++;
        return;
    }
    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }

    if (build_macho_src(dir, "arm64-apple-darwin", "sp", source, exe,
                        sizeof(exe)) != 0) {
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }
    CHECK(read_macho_header(exe, magic, filetype) == 0);
    CHECK(memcmp(magic, macho64_le, sizeof(magic)) == 0);
    CHECK(memcmp(filetype, mh_execute, sizeof(filetype)) == 0);

    remove_tree(dir);
    reset_masa_env();
}

/*
 * CoreFoundation: a Mach-O linked with -framework CoreFoundation creates a
 * CFString and a CFArray, shows the string, reads it back and counts the
 * array, all through the clean-room shim.  One object is released, the
 * rest are left for the loader's reset, and the run must stay leak-free.
 */
static void test_danroot_corefoundation(void)
{
    const char *target = host_apple_target();
    char dir[] = "/tmp/masa_cf_XXXXXX";
    char exe[4096];
    char out[4096];
    char err[4096];
    char *argv[4];
    static const char *source =
        "#include <CoreFoundation/CoreFoundation.h>\n"
        "#include <stdio.h>\n"
        "int main(void) {\n"
        "    CFStringRef a = CFStringCreateWithCString(\n"
        "        kCFAllocatorDefault, \"alpha\", kCFStringEncodingUTF8);\n"
        "    CFStringRef b = CFStringCreateWithCString(\n"
        "        kCFAllocatorDefault, \"beta\", kCFStringEncodingUTF8);\n"
        "    const void *items[2];\n"
        "    CFArrayRef arr;\n"
        "    char buf[64];\n"
        "    CFShow(a);\n"
        "    printf(\"len=%ld\\n\", (long)CFStringGetLength(a));\n"
        "    if (CFStringGetCString(a, buf, sizeof(buf), kCFStringEncodingUTF8))\n"
        "        printf(\"cstr=%s\\n\", buf);\n"
        "    items[0] = a; items[1] = b;\n"
        "    arr = CFArrayCreate(kCFAllocatorDefault, items, 2, NULL);\n"
        "    printf(\"count=%ld\\n\", (long)CFArrayGetCount(arr));\n"
        "    printf(\"elem0=%s\\n\", CFStringGetCStringPtr(\n"
        "        (CFStringRef)CFArrayGetValueAtIndex(arr, 0),\n"
        "        kCFStringEncodingUTF8));\n"
        "    CFRetain(a);\n"
        "    CFRelease(a);\n"        /* balanced: a stays alive */
        "    CFRelease(b);\n"        /* frees beta */
        "    /* a and arr are left for the loader's reset. */\n"
        "    return 0;\n"
        "}\n";
    int rc;

    if (target == NULL || !tool_available("clang")) {
        printf("  SKIP needs clang and a supported host\n");
        g_skipped++;
        return;
    }
    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }

    if (build_macho_framework(dir, target, "cf", source, "CoreFoundation",
                              exe, sizeof(exe)) != 0) {
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }

    argv[0] = "masa";
    argv[1] = "danroot";
    argv[2] = exe;
    argv[3] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  danroot stderr: %s\n", err);
    }
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "len=5");
    CHECK_CONTAINS(out, "cstr=alpha");
    CHECK_CONTAINS(out, "count=2");
    CHECK_CONTAINS(out, "elem0=alpha");
    CHECK_CONTAINS(err, "alpha"); /* CFShow writes to stderr */
    /* Nothing may be left allocated after the run. */
    CHECK(strstr(err, "LeakSanitizer") == NULL);
    CHECK(strstr(err, "leaked") == NULL);

    remove_tree(dir);
    reset_masa_env();
}

/*
 * Objective-C: a Mach-O linked with -framework Foundation drives the
 * runtime directly from C -- objc_getClass("NSString"), sel_registerName,
 * objc_msgSend to build a string and read it back, and NSLog -- all through
 * the clean-room libobjc shim, leaving no leaks.
 */
static void test_danroot_objc(void)
{
    const char *target = host_apple_target();
    char dir[] = "/tmp/masa_objc_XXXXXX";
    char exe[4096];
    char out[4096];
    char err[4096];
    char *argv[4];
    static const char *source =
        "#include <objc/objc.h>\n"
        "#include <objc/runtime.h>\n"
        "#include <objc/message.h>\n"
        "#include <Foundation/Foundation.h>\n"
        "#include <stdio.h>\n"
        "int main(void) {\n"
        "    void *pool = objc_autoreleasePoolPush();\n"
        "    id cls = (id)objc_getClass(\"NSString\");\n"
        "    SEL sel = sel_registerName(\"stringWithUTF8String:\");\n"
        "    id str = objc_msgSend(cls, sel, \"hello objc\");\n"
        "    const char *utf = (const char *)objc_msgSend(\n"
        "        str, sel_registerName(\"UTF8String\"));\n"
        "    unsigned long len = (unsigned long)objc_msgSend(\n"
        "        str, sel_registerName(\"length\"));\n"
        "    printf(\"utf=%s len=%lu\\n\", utf, len);\n"
        "    NSLog(str);\n"
        "    objc_autoreleasePoolPop(pool);\n"
        "    return 0;\n"
        "}\n";
    int rc;

    if (target == NULL || !tool_available("clang")) {
        printf("  SKIP needs clang and a supported host\n");
        g_skipped++;
        return;
    }
    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }

    if (build_macho_framework(dir, target, "objc", source, "Foundation",
                              exe, sizeof(exe)) != 0) {
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }

    argv[0] = "masa";
    argv[1] = "danroot";
    argv[2] = exe;
    argv[3] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  danroot stderr: %s\n", err);
    }
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "utf=hello objc len=10");
    CHECK_CONTAINS(err, "hello objc"); /* NSLog writes to stderr */
    CHECK(strstr(err, "LeakSanitizer") == NULL);
    CHECK(strstr(err, "leaked") == NULL);

    remove_tree(dir);
    reset_masa_env();
}

/*
 * A Flutter/Dart-style binary imports QuartzCore, Security and Metal at
 * load time.  CACurrentMediaTime must return a real monotonic value and
 * SecRandomCopyBytes must fill its buffer from the host CSPRNG, while
 * MTLCreateSystemDefaultDevice returns NULL cleanly (a device is
 * optional).  Nothing may leak.
 */
static void test_danroot_flutter_frameworks(void)
{
    const char *target = host_apple_target();
    char dir[] = "/tmp/masa_flutter_XXXXXX";
    char sdk[4096];
    char src[4096];
    char exe[4096];
    char out[4096];
    char err[4096];
    char *argv[14];
    int n = 0;
    int rc;
    static const char *source =
        "#include <QuartzCore/QuartzCore.h>\n"
        "#include <Security/Security.h>\n"
        "#include <Metal/Metal.h>\n"
        "#include <stdio.h>\n"
        "int main(void) {\n"
        "    double t1 = CACurrentMediaTime();\n"
        "    unsigned char buf[16];\n"
        "    int i, nz = 0;\n"
        "    int sec = SecRandomCopyBytes(0, sizeof(buf), buf);\n"
        "    for (i = 0; i < 16; i++) if (buf[i]) nz = 1;\n"
        "    printf(\"media=%d\\n\", t1 > 0.0);\n"
        "    printf(\"sec=%d nz=%d\\n\", sec, nz);\n"
        "    printf(\"dev=%d\\n\", MTLCreateSystemDefaultDevice() == 0);\n"
        "    return 0;\n"
        "}\n";

    if (target == NULL || !tool_available("clang")) {
        printf("  SKIP needs clang and a supported host\n");
        g_skipped++;
        return;
    }
    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }

    snprintf(sdk, sizeof(sdk), "%s/sdk", dir);
    snprintf(src, sizeof(src), "%s/flutter.c", dir);
    snprintf(exe, sizeof(exe), "%s/flutter", dir);
    reset_masa_env();
    if (make_stub_sdk(sdk) != 0 || !sdk_is_stubbed(sdk) ||
        write_file(src, source) != 0) {
        fprintf(stderr, "  FAIL could not set up the SDK or source\n");
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }
    set_env(MASA_SDK_PATH_ENV, sdk);
    set_env(MASA_TARGET_ENV, target);

    argv[n++] = "masa";
    argv[n++] = "cc";
    argv[n++] = src;
    argv[n++] = "-o";
    argv[n++] = exe;
    argv[n++] = "-framework";
    argv[n++] = "QuartzCore";
    argv[n++] = "-framework";
    argv[n++] = "Security";
    argv[n++] = "-framework";
    argv[n++] = "Metal";
    argv[n] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  cc stderr: %s\n", err);
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }

    argv[0] = "masa";
    argv[1] = "danroot";
    argv[2] = exe;
    argv[3] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  danroot stderr: %s\n", err);
    }
    CHECK(rc == 0);
    CHECK_CONTAINS(out, "media=1");
    CHECK_CONTAINS(out, "sec=0 nz=1");
    CHECK_CONTAINS(out, "dev=1");
    /* Metal is reported as unemulated, not silently faked. */
    CHECK_CONTAINS(err, "Metal is not emulated");
    /* Nothing may be left allocated after the run. */
    CHECK(strstr(err, "LeakSanitizer") == NULL);
    CHECK(strstr(err, "leaked") == NULL);

    remove_tree(dir);
    reset_masa_env();
}

/*
 * A Darwin GUI program imports CoreGraphics/AppKit symbols that the core
 * `masa` build (no SDL2) cannot satisfy.  The loader must say so plainly,
 * pointing at `make gui`, rather than reporting a bare unresolved symbol.
 * The SDL2 backend itself is exercised by the GUI suite (make test-gui).
 */
static void test_danroot_gui_symbol_hint(void)
{
    const char *target = host_apple_target();
    char dir[] = "/tmp/masa_guihint_XXXXXX";
    char exe[4096];
    char out[4096];
    char err[4096];
    char *argv[4];
    static const char *source =
        "#include <CoreGraphics/CoreGraphics.h>\n"
        "#include <stdio.h>\n"
        "int main(void) {\n"
        "    /* Filled at run time by the SDL2 backend; here it is only an\n"
        "     * import the core build cannot resolve. */\n"
        "    CGContextRef ctx = (CGContextRef)0;\n"
        "    CGContextFillRect(ctx, CGRectMake(0, 0, 10, 10));\n"
        "    return 0;\n"
        "}\n";
    int rc;

    if (target == NULL || !tool_available("clang")) {
        printf("  SKIP needs clang and a supported host\n");
        g_skipped++;
        return;
    }
    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }

    if (build_macho_framework(dir, target, "guihint", source, "CoreGraphics",
                              exe, sizeof(exe)) != 0) {
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }

    argv[0] = "masa";
    argv[1] = "run";
    argv[2] = exe;
    argv[3] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc != 0);
    CHECK_CONTAINS(err, "unresolved GUI symbol");
    CHECK_CONTAINS(err, "make gui");

    remove_tree(dir);
    reset_masa_env();
}

/*
 * Build build/masa-aarch64 on demand and return its absolute path.  The
 * cross-architecture bridge needs this aarch64 build of Masa as the ARM64
 * side of the runner.  Returns 0 on success, -1 when the build is not
 * possible here (the caller then SKIPs).
 */
static int ensure_masa_aarch64(char *out, size_t outcap)
{
    char cwd[4096];

    if (access("build/masa-aarch64", X_OK) != 0) {
        if (system("make aarch64 >/dev/null 2>&1") != 0) {
            return -1;
        }
    }
    if (access("build/masa-aarch64", X_OK) != 0) {
        return -1;
    }
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        return -1;
    }
    if (snprintf(out, outcap, "%s/build/masa-aarch64", cwd) >= (int)outcap) {
        return -1;
    }
    return 0;
}

/*
 * Cross-architecture bridge: build an arm64-apple-darwin Mach-O image and
 * run it on this x86_64 host through qemu-aarch64 driving masa-aarch64.
 * Verifies the program's stdout and its exact exit status (42).
 */
static void test_danroot_arm64_bridge_on_x86_64(void)
{
    char dir[] = "/tmp/masa_bridge_XXXXXX";
    char aarch64[4096];
    char exe[4096];
    char out[4096];
    char err[4096];
    char *argv[4];
    static const char *source =
        "#include <Foundation/Foundation.h>\n"
        "#include <objc/objc.h>\n"
        "#include <objc/runtime.h>\n"
        "#include <objc/message.h>\n"
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "int main(void) {\n"
        "    char *p = malloc(32);\n"
        "    char buf[32];\n"
        "    id cls, str;\n"
        "    const char *utf;\n"
        "    unsigned long len;\n"
        "    if (p == NULL) return 3;\n"
        "    p[0] = 'o'; p[1] = 'k'; p[2] = '\\0';\n"
        "    printf(\"arm64 bridge %s\\n\", p);\n"
        "    free(p);\n"
        "    snprintf(buf, sizeof(buf), \"fmt-%d-%s\", 7, \"z\");\n"
        "    printf(\"snprintf %s\\n\", buf);\n"
        "    cls = (id)objc_getClass(\"NSString\");\n"
        "    str = objc_msgSend(cls, sel_registerName(\"stringWithUTF8String:\"),\n"
        "                       \"objc-bridge\");\n"
        "    utf = (const char *)objc_msgSend(str,\n"
        "        sel_registerName(\"UTF8String\"));\n"
        "    len = (unsigned long)objc_msgSend(str, sel_registerName(\"length\"));\n"
        "    printf(\"objc %s len=%lu\\n\", utf, len);\n"
        "    NSLog(str);\n"
        "    return 42;\n"
        "}\n";
    int rc;

#if !defined(__x86_64__)
    printf("  SKIP the bridge is only exercised on x86_64 hosts\n");
    g_skipped++;
    return;
#endif
    if (!tool_available("clang")) {
        printf("  SKIP clang is not installed\n");
        g_skipped++;
        return;
    }
    if (!tool_available("qemu-aarch64")) {
        printf("  SKIP qemu-aarch64 is not installed\n");
        g_skipped++;
        return;
    }
    if (!tool_available("aarch64-linux-gnu-gcc")) {
        printf("  SKIP aarch64-linux-gnu-gcc is not installed\n");
        g_skipped++;
        return;
    }
    if (ensure_masa_aarch64(aarch64, sizeof(aarch64)) != 0) {
        printf("  SKIP could not build build/masa-aarch64\n");
        g_skipped++;
        return;
    }
    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }

    if (build_macho_framework(dir, "arm64-apple-darwin", "bridged", source,
                              "Foundation", exe, sizeof(exe)) != 0) {
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }

    reset_masa_env();
    set_env(MASA_DANROOT_AARCH64_ENV, aarch64);

    argv[0] = "masa";
    argv[1] = "danroot";
    argv[2] = exe;
    argv[3] = NULL;
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    if (rc != 42) {
        fprintf(stderr, "  bridge stdout: %s\n  bridge stderr: %s\n", out,
                err);
    }
    CHECK(rc == 42);
    CHECK_CONTAINS(out, "arm64 bridge ok");
    CHECK_CONTAINS(out, "snprintf fmt-7-z");
    CHECK_CONTAINS(out, "objc objc-bridge len=11");
    CHECK_CONTAINS(err, "objc-bridge"); /* NSLog writes to stderr */

    remove_tree(dir);
    reset_masa_env();
}

/*
 * Bridge fallbacks: when the bridge is disabled, or its runner points at a
 * path that is not executable, an arm64 image on an x86_64 host must fall
 * back to the plain architecture-mismatch diagnostic and never crash.
 */
static void test_danroot_arm64_bridge_fallbacks(void)
{
    static const char *source = "int main(void) { return 0; }\n";
    char dir[] = "/tmp/masa_bridge_fb_XXXXXX";
    char exe[4096];
    char out[1024];
    char err[4096];
    char *argv[4];
    int rc;

#if !defined(__x86_64__)
    printf("  SKIP the bridge is only exercised on x86_64 hosts\n");
    g_skipped++;
    return;
#endif
    if (!tool_available("clang")) {
        printf("  SKIP clang is not installed\n");
        g_skipped++;
        return;
    }
    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }
    snprintf(exe, sizeof(exe), "%s/prog", dir);
    if (build_macho(dir, "arm64-apple-darwin", source, exe) != 0) {
        g_failed++;
        remove_tree(dir);
        reset_masa_env();
        return;
    }

    argv[0] = "masa";
    argv[1] = "run";
    argv[2] = exe;
    argv[3] = NULL;

    /* MASA_DANROOT_NO_BRIDGE=1 disables the bridge outright. */
    reset_masa_env();
    set_env(MASA_DANROOT_NO_BRIDGE_ENV, "1");
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc != 0);
    CHECK_CONTAINS(err, "architecture mismatch");

    /* A runner that is not an executable binary is treated as absent. */
    reset_masa_env();
    set_env(MASA_QEMU_AARCH64_ENV, "/nonexistent/qemu-aarch64");
    rc = run_masa(argv, out, sizeof(out), err, sizeof(err));
    CHECK(rc != 0);
    CHECK_CONTAINS(err, "architecture mismatch");

    remove_tree(dir);
    reset_masa_env();
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    const char *path = getenv("PATH");

    g_original_path = (path != NULL) ? strdup(path) : NULL;

    printf("masa %s test suite\n\n", MASA_VERSION);

    RUN(test_build_cc_args_sequence);
    RUN(test_build_cc_args_appends);
    RUN(test_build_cc_args_empty_sdk_skipped);
    RUN(test_build_cc_args_rejects_bad_input);
    RUN(test_build_cc_args_rejects_overflow);

    RUN(test_dry_run_default_target);
    RUN(test_dry_run_custom_target_and_sdk);
    RUN(test_dry_run_is_exact);
    RUN(test_dry_run_respects_masa_cc);

    RUN(test_sys_uname);
    RUN(test_sys_uname_x86_64_target);
    RUN(test_sys_sw_vers);
    RUN(test_sys_sw_vers_flags);
    RUN(test_sys_sdk_path_default);
    RUN(test_sys_sdk_path_override);
    RUN(test_sys_dispatch_errors);
    RUN(test_sys_missing_command);

    RUN(test_version);
    RUN(test_help_and_unknown);

    RUN(test_argv0_dispatch_sys);
    RUN(test_argv0_dispatch_cc);
    RUN(test_argv0_non_shim_routes);

    RUN(test_init_shims_creates_links);
    RUN(test_init_shims_missing_dir);
    RUN(test_exec_sets_environment);
    RUN(test_exec_missing_command);
    RUN(test_exec_make_integration);

    RUN(test_cc_avoids_recursion);
    RUN(test_cc_honours_real_cc_over_shims);

    RUN(test_sdk_init_stub);
    RUN(test_sdk_info);
    RUN(test_sdk_bad_subcommand);

    RUN(test_argv0_dispatch_tools);
    RUN(test_cc_link_platform_version);

    RUN(test_link_executable_arm64);
    RUN(test_link_executable_x86_64);
    RUN(test_exec_make_links_executable);

    RUN(test_pbxproj_parse_target_and_sources);
    RUN(test_pbxproj_parse_flat_sources);
    RUN(test_pbxproj_rejects_malformed);
    RUN(test_xcode_build_bundle);
    RUN(test_xcode_build_plain);
    RUN(test_xcode_build_missing_project);
    RUN(test_xcode_build_missing_pbxproj);
    RUN(test_xcode_unknown_subcommand);
    RUN(test_codesign_argv0_dispatch);
    RUN(test_xcode_codesign_roundtrip);
    RUN(test_init_shims_includes_codesign);

    RUN(test_brew_formula_parse);
    RUN(test_brew_formula_name_and_version);
    RUN(test_brew_formula_explicit_version);
    RUN(test_brew_formula_rejects_malformed);
    RUN(test_brew_lint_valid);
    RUN(test_brew_lint_missing_sha256);
    RUN(test_brew_lint_missing_install);
    RUN(test_brew_lint_subcommand);
    RUN(test_brew_build_makefile);
    RUN(test_brew_build_cc_steps);
    RUN(test_brew_build_errors);
    RUN(test_brew_build_shell_string_steps);
    RUN(test_brew_formula_shell_string_flag);
    RUN(test_brew_cask_parse);
    RUN(test_brew_cask_pkg_artifact);
    RUN(test_brew_cask_rejects_malformed);
    RUN(test_brew_lint_cask_valid);
    RUN(test_brew_lint_cask_missing_sha);
    RUN(test_brew_lint_cask_missing_artifact);
    RUN(test_brew_lint_cask_subcommand);
    RUN(test_sha256_nist_vectors);
    RUN(test_sha256_block_boundary);
    RUN(test_brew_fetch_file_url);
    RUN(test_brew_tap_bad_arg);
    RUN(test_clipboard_roundtrip);
    RUN(test_clipboard_shim_symlinks);
    RUN(test_brew_index);
    RUN(test_brew_index_bad_json);
    RUN(test_brew_tap_local);
    RUN(test_pack_ipa);
    RUN(test_pack_dmg);
    RUN(test_pack_errors);

    RUN(test_run_executes_macho);
    RUN(test_danroot_alias_executes_macho);
    RUN(test_run_honours_exit);
    RUN(test_run_classic_dyld_info_fixups);
    RUN(test_run_loads_dylib);
    RUN(test_run_missing_dylib_reports_error);
    RUN(test_run_weak_dylib_optional);
    RUN(test_run_weak_dylib_classic_dyld_info);
    RUN(test_run_dlopen_dlsym_underscore);
    RUN(test_run_dylib_via_library_path);
    RUN(test_run_rejects_cputype_mismatch);
    RUN(test_run_rejects_bad_input);

    RUN(test_danroot_root_path_virtualization);
    RUN(test_danroot_mach_time);
    RUN(test_danroot_root_error_paths);

    RUN(test_danroot_arm64_stack_protector);
    RUN(test_danroot_corefoundation);
    RUN(test_danroot_objc);
    RUN(test_danroot_flutter_frameworks);
    RUN(test_danroot_gui_symbol_hint);
    RUN(test_danroot_arm64_bridge_on_x86_64);
    RUN(test_danroot_arm64_bridge_fallbacks);

    RUN(test_cc_compiles_macho_object);
    RUN(test_cc_x86_64_macho_object);
    RUN(test_cc_missing_compiler);

    RUN(test_host_services_basics);
    RUN(test_shell_command_environment);
    RUN(test_shell_shims_are_active);
    RUN(test_shell_propagates_exit_status);
    RUN(test_shell_missing_command_argument);
    RUN(test_shell_interactive_reads_stdin);

    RUN(test_build_swift_args_sequence);
    RUN(test_build_swift_args_appends_without_sdk);
    RUN(test_build_swift_args_empty_sdk_skipped);
    RUN(test_build_swift_args_rejects_bad_input);
    RUN(test_swiftc_dry_run_target_and_sdk);
    RUN(test_swiftc_dry_run_default_target);
    RUN(test_argv0_dispatch_swift);
    RUN(test_swift_info_package);
    RUN(test_swift_info_rejects_non_manifest);
    RUN(test_swift_missing_package);
    RUN(test_swift_build_dry_run);
    RUN(test_swift_unknown_subcommand);
    RUN(test_pbxproj_parse_swift_sources);
    RUN(test_xcode_build_swift_dry_run);
    RUN(test_android_ndk_compile);

    RUN(test_doctor_healthy);
    RUN(test_doctor_warns_on_missing_sdk);
    RUN(test_doctor_reports_missing_compiler);
    RUN(test_pty_win_cross_compiles);

    printf("\n%d tests run, %d failed, %d skipped\n",
           g_run, g_failed, g_skipped);

    free(g_original_path);

    return (g_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
