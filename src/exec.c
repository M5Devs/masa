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

int masa_exec_main(int argc, char *const argv[])
{
    char temp_dir[MASA_PATH_MAX];
    const char *dir_env;
    const char *shim_dir;
    char *target;
    char *real_cc;
    char *saved_path;
    char *saved_shim_dir;
    char *saved_real_cc;
    char *new_path;
    size_t need;
    int created = 0;
    int status;

    if (argc < 1 || argv[0] == NULL || argv[0][0] == '\0') {
        fprintf(stderr, "masa: exec: missing command\n");
        return 2;
    }

    /* Resolve the real compiler before PATH starts pointing at our shims. */
    real_cc = masa_resolve_cc();

    dir_env = getenv(MASA_SHIM_DIR_ENV);
    if (dir_env != NULL && dir_env[0] != '\0') {
        shim_dir = dir_env;
    } else {
        snprintf(temp_dir, sizeof(temp_dir), "/tmp/masa-shims-XXXXXX");
        if (mkdtemp(temp_dir) == NULL) {
            fprintf(stderr, "masa: exec: cannot create a shim directory: %s\n",
                    strerror(errno));
            free(real_cc);
            return 1;
        }
        shim_dir = temp_dir;
        created = 1;
    }

    target = shim_target();
    if (target == NULL) {
        fprintf(stderr, "masa: exec: cannot determine the Masa executable "
                        "(set %s)\n", MASA_SHIM_TARGET_ENV);
        if (created) {
            rmdir(shim_dir);
        }
        free(real_cc);
        return 1;
    }

    if (masa_make_shims(shim_dir, target) != 0) {
        free(target);
        if (created) {
            remove_shims(shim_dir);
        }
        free(real_cc);
        return 1;
    }
    free(target);

    saved_path = env_dup("PATH");
    saved_shim_dir = env_dup(MASA_SHIM_DIR_ENV);
    saved_real_cc = env_dup(MASA_REAL_CC_ENV);

    setenv(MASA_SHIM_DIR_ENV, shim_dir, 1);
    if (real_cc != NULL) {
        setenv(MASA_REAL_CC_ENV, real_cc, 1);
    }

    need = strlen(shim_dir) + 1 +
           ((saved_path != NULL) ? strlen(saved_path) : 0) + 1;
    new_path = malloc(need);
    if (new_path == NULL) {
        env_restore("PATH", saved_path);
        env_restore(MASA_SHIM_DIR_ENV, saved_shim_dir);
        env_restore(MASA_REAL_CC_ENV, saved_real_cc);
        free(saved_path);
        free(saved_shim_dir);
        free(saved_real_cc);
        free(real_cc);
        if (created) {
            remove_shims(shim_dir);
        }
        return 1;
    }
    snprintf(new_path, need, "%s:%s", shim_dir,
             (saved_path != NULL) ? saved_path : "");
    setenv("PATH", new_path, 1);

    status = masa_spawn_wait((const char *const *)argv);

    env_restore("PATH", saved_path);
    env_restore(MASA_SHIM_DIR_ENV, saved_shim_dir);
    env_restore(MASA_REAL_CC_ENV, saved_real_cc);

    free(saved_path);
    free(saved_shim_dir);
    free(saved_real_cc);
    free(new_path);
    free(real_cc);

    if (created) {
        remove_shims(shim_dir);
    }
    return status;
}
