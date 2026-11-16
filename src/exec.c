/*
 * exec.c - BusyBox-style environment runner.
 *
 * `masa init-shims <dir>` lays down symlinks named after the commands
 * Masa emulates; `masa exec <command>` builds such a directory, prepends
 * it to PATH and runs the command so that its `cc`, `uname`, `sw_vers`,
 * `xcode-select` and `xcrun` calls land back in Masa.
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
#include <sys/types.h>
#include <unistd.h>

#define MASA_PATH_MAX 4096

const char *const masa_shim_tools[] = {
    "cc",
    "clang",
    "gcc",
    "uname",
    "sw_vers",
    "xcode-select",
    "xcrun",
    "ld",
    "ar",
    "ranlib",
    "strip",
    "nm",
    "otool",
    "codesign",
    NULL
};

char *masa_self_path(void)
{
    char buf[MASA_PATH_MAX];
    ssize_t n;

    n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return NULL;
    }
    buf[n] = '\0';
    return strdup(buf);
}

/* The binary the shims should point at. */
static char *shim_target(void)
{
    const char *override = getenv(MASA_SHIM_TARGET_ENV);

    if (override != NULL && override[0] != '\0') {
        return strdup(override);
    }
    return masa_self_path();
}

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

int masa_make_shims(const char *dir, const char *target_exe)
{
    size_t i;

    if (dir == NULL || dir[0] == '\0' || target_exe == NULL ||
        target_exe[0] == '\0') {
        return -1;
    }
    if (mkdir_p(dir) != 0) {
        fprintf(stderr, "masa: cannot create shim directory '%s': %s\n",
                dir, strerror(errno));
        return -1;
    }

    for (i = 0; masa_shim_tools[i] != NULL; i++) {
        char linkpath[MASA_PATH_MAX];

        if (snprintf(linkpath, sizeof(linkpath), "%s/%s", dir,
                     masa_shim_tools[i]) >= (int)sizeof(linkpath)) {
            return -1;
        }
        unlink(linkpath);
        if (symlink(target_exe, linkpath) != 0) {
            fprintf(stderr, "masa: cannot create symlink '%s': %s\n",
                    linkpath, strerror(errno));
            return -1;
        }
    }
    return 0;
}

static void remove_shims(const char *dir)
{
    size_t i;

    for (i = 0; masa_shim_tools[i] != NULL; i++) {
        char linkpath[MASA_PATH_MAX];

        if (snprintf(linkpath, sizeof(linkpath), "%s/%s", dir,
                     masa_shim_tools[i]) < (int)sizeof(linkpath)) {
            unlink(linkpath);
        }
    }
    rmdir(dir);
}

int masa_init_shims_main(int argc, char *const argv[])
{
    const char *dir;
    char *target;
    int rc;

    if (argc < 1 || argv[0] == NULL || argv[0][0] == '\0') {
        fprintf(stderr, "masa: init-shims: missing directory\n");
        return 2;
    }
    dir = argv[0];

    target = shim_target();
    if (target == NULL) {
        fprintf(stderr,
                "masa: init-shims: cannot determine the Masa executable "
                "(set %s)\n", MASA_SHIM_TARGET_ENV);
        return 1;
    }

    rc = masa_make_shims(dir, target);
    free(target);
    if (rc != 0) {
        return 1;
    }

    printf("masa: created %zu shims in %s\n",
           (sizeof(masa_shim_tools) / sizeof(masa_shim_tools[0])) - 1, dir);
    return 0;
}

static char *env_dup(const char *name)
{
    const char *value = getenv(name);

    return (value != NULL) ? strdup(value) : NULL;
}

static void env_restore(const char *name, const char *saved)
{
    if (saved != NULL) {
        setenv(name, saved, 1);
    } else {
        unsetenv(name);
    }
}

/*
 * A shim environment that has been installed onto PATH.  setup() creates
 * (or reuses) the shim directory and records the values it overrides;
 * teardown() restores them and removes a directory setup() created.
 */
struct shim_env {
    char dir[MASA_PATH_MAX];
    char *saved_path;
    char *saved_shim_dir;
    char *saved_real_cc;
    char *new_path;
    int created;
};

static void shim_env_teardown(struct shim_env *env)
{
    env_restore("PATH", env->saved_path);
    env_restore(MASA_SHIM_DIR_ENV, env->saved_shim_dir);
    env_restore(MASA_REAL_CC_ENV, env->saved_real_cc);

    free(env->saved_path);
    free(env->saved_shim_dir);
    free(env->saved_real_cc);
    free(env->new_path);
    env->saved_path = NULL;
    env->saved_shim_dir = NULL;
    env->saved_real_cc = NULL;
    env->new_path = NULL;

    if (env->created) {
        remove_shims(env->dir);
        env->created = 0;
    }
}

/*
 * Install the shim environment.  When reuse_dir is non-NULL it is used as
 * the shim directory (and left in place on teardown); otherwise a fresh
 * temporary directory is created and later removed.
 *
 * Returns 0 on success and -1 on failure, having cleaned up after itself.
 */
static int shim_env_setup(struct shim_env *env, const char *reuse_dir)
{
    char *real_cc;
    char *target;
    size_t need;

    memset(env, 0, sizeof(*env));

    /* Resolve the real compiler before PATH starts pointing at our shims. */
    real_cc = masa_resolve_cc();

    if (reuse_dir != NULL && reuse_dir[0] != '\0') {
        snprintf(env->dir, sizeof(env->dir), "%s", reuse_dir);
    } else {
        snprintf(env->dir, sizeof(env->dir), "/tmp/masa-shims-XXXXXX");
        if (mkdtemp(env->dir) == NULL) {
            fprintf(stderr, "masa: exec: cannot create a shim directory: %s\n",
                    strerror(errno));
            free(real_cc);
            return -1;
        }
        env->created = 1;
    }

    target = shim_target();
    if (target == NULL) {
        fprintf(stderr, "masa: exec: cannot determine the Masa executable "
                        "(set %s)\n", MASA_SHIM_TARGET_ENV);
        goto fail;
    }
    if (masa_make_shims(env->dir, target) != 0) {
        free(target);
        goto fail;
    }
    free(target);

    env->saved_path = env_dup("PATH");
    env->saved_shim_dir = env_dup(MASA_SHIM_DIR_ENV);
    env->saved_real_cc = env_dup(MASA_REAL_CC_ENV);

    setenv(MASA_SHIM_DIR_ENV, env->dir, 1);
    if (real_cc != NULL) {
        setenv(MASA_REAL_CC_ENV, real_cc, 1);
    }

    need = strlen(env->dir) + 1 +
           ((env->saved_path != NULL) ? strlen(env->saved_path) : 0) + 1;
    env->new_path = malloc(need);
    if (env->new_path == NULL) {
        goto fail;
    }
    snprintf(env->new_path, need, "%s:%s", env->dir,
             (env->saved_path != NULL) ? env->saved_path : "");
    setenv("PATH", env->new_path, 1);

    free(real_cc);
    return 0;

fail:
    free(real_cc);
    shim_env_teardown(env);
    return -1;
}

int masa_exec_main(int argc, char *const argv[])
{
    struct shim_env env;
    const char *dir_env;
    int status;

    if (argc < 1 || argv[0] == NULL || argv[0][0] == '\0') {
        fprintf(stderr, "masa: exec: missing command\n");
        return 2;
    }

    dir_env = getenv(MASA_SHIM_DIR_ENV);
    if (shim_env_setup(&env, dir_env) != 0) {
        return 1;
    }

    status = masa_spawn_wait((const char *const *)argv);

    shim_env_teardown(&env);
    return status;
}

int masa_exec_run_all(const char *dir, char *const *const *cmds, size_t count)
{
    struct shim_env env;
    size_t i;
    int status = 0;

    if (cmds == NULL || count == 0) {
        return 0;
    }

    if (shim_env_setup(&env, NULL) != 0) {
        return 1;
    }

    for (i = 0; i < count; i++) {
        status = masa_spawn_wait_in(dir, (const char *const *)cmds[i]);
        if (status != 0) {
            break;
        }
    }

    shim_env_teardown(&env);
    return status;
}
