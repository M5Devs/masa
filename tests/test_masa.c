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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Tiny test harness                                                  */
/* ------------------------------------------------------------------ */

static int g_run;
static int g_failed;
static int g_skipped;

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
 * Run masa_main in a forked child with stdout/stderr redirected into
 * pipes, and return the child's exit status.  Output buffers are always
 * NUL terminated.  The captured streams are small, so draining stdout
 * before stderr cannot deadlock in practice.
 */
static int run_masa(char *const argv[], char *out, size_t outcap,
                    char *err, size_t errcap)
{
    int out_pipe[2];
    int err_pipe[2];
    int status = -1;
    pid_t pid;
    ssize_t n;
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

        status = masa_main(argc, argv);

        /* _exit() would skip stdio flushing and lose buffered output. */
        fflush(NULL);
        _exit(status);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);

    if (out != NULL && outcap > 0) {
        used = 0;
        while ((n = read(out_pipe[0], out + used, outcap - 1 - used)) > 0) {
            used += (size_t)n;
            if (used >= outcap - 1) {
                break;
            }
        }
        out[used] = '\0';
    }
    close(out_pipe[0]);

    if (err != NULL && errcap > 0) {
        used = 0;
        while ((n = read(err_pipe[0], err + used, errcap - 1 - used)) > 0) {
            used += (size_t)n;
            if (used >= errcap - 1) {
                break;
            }
        }
        err[used] = '\0';
    }
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
}

/* ------------------------------------------------------------------ */
/* Argument builder                                                   */
/* ------------------------------------------------------------------ */

static void test_build_cc_args_sequence(void)
{
    const char *args[16];
    const char *expected[] = {"-target", "arm64-apple-darwin", "-fuse-ld=lld",
                              "-isysroot", "/sdk"};
    int count = 0;
    int i;
    int rc;

    rc = masa_build_cc_args("arm64-apple-darwin", "/sdk", args, &count, 16);
    CHECK(rc == 0);
    CHECK(count == 5);
    for (i = 0; i < 5; i++) {
        CHECK_STREQ(args[i], expected[i]);
    }
    CHECK(args[5] == NULL);
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
    CHECK(count == 5);
    CHECK_STREQ(args[0], "clang");
    CHECK_STREQ(args[1], "-c");
    CHECK_STREQ(args[2], "-target");
    CHECK_STREQ(args[3], "x86_64-apple-darwin");
    CHECK_STREQ(args[4], "-fuse-ld=lld");
    CHECK(args[5] == NULL);
}

static void test_build_cc_args_empty_sdk_skipped(void)
{
    const char *args[16];
    int count = 0;
    int rc;

    rc = masa_build_cc_args("arm64-apple-darwin", "", args, &count, 16);
    CHECK(rc == 0);
    CHECK(count == 3);
    CHECK_STREQ(args[2], "-fuse-ld=lld");
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

    rc = run_masa(argv, out, sizeof(out), NULL, 0);
    CHECK(rc == 0);
    snprintf(expected, sizeof(expected),
             "clang -c foo.c -target arm64-apple-darwin -fuse-ld=lld -isysroot /sdk\n");
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
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
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

    RUN(test_cc_compiles_macho_object);
    RUN(test_cc_x86_64_macho_object);
    RUN(test_cc_missing_compiler);

    printf("\n%d tests run, %d failed, %d skipped\n",
           g_run, g_failed, g_skipped);

    return (g_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
