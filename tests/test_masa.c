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

#include <dirent.h>
#include <errno.h>
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
    set_env(MASA_REAL_CC_ENV, NULL);
    set_env(MASA_SHIM_DIR_ENV, NULL);
    set_env(MASA_SHIM_TARGET_ENV, NULL);
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
    char buf[512];
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
    snprintf(path, sizeof(path), "%s/usr/lib/crt1.o", dir);
    CHECK(file_exists(path));
    snprintf(path, sizeof(path), "%s/usr/include/stdlib.h", dir);
    CHECK(file_exists(path));
    snprintf(path, sizeof(path), "%s/usr/include/string.h", dir);
    CHECK(file_exists(path));
    snprintf(path, sizeof(path), "%s/usr/include/stdio.h", dir);
    CHECK(file_exists(path));

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

    RUN(test_cc_compiles_macho_object);
    RUN(test_cc_x86_64_macho_object);
    RUN(test_cc_missing_compiler);

    printf("\n%d tests run, %d failed, %d skipped\n",
           g_run, g_failed, g_skipped);

    free(g_original_path);

    return (g_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
