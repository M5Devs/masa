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
#include "masa_compat.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* The binary the shims should point at. */
static char *shim_target(void)
{
    const char *override = getenv(MASA_SHIM_TARGET_ENV);

    if (override != NULL && override[0] != '\0') {
        return strdup(override);
    }
    return masa_self_path();
}

int masa_make_shims(const char *dir, const char *target_exe)
{
    size_t i;

    if (dir == NULL || dir[0] == '\0' || target_exe == NULL ||
        target_exe[0] == '\0') {
        return -1;
    }
    if (masa_mkdir_p(dir) != 0) {
        fprintf(stderr, "masa: cannot create shim directory '%s': %s\n",
                dir, strerror(errno));
        return -1;
    }

    for (i = 0; masa_shim_tools[i] != NULL; i++) {
        if (masa_make_shim(dir, masa_shim_tools[i], target_exe) != 0) {
            fprintf(stderr, "masa: cannot create the '%s' shim in '%s': %s\n",
                    masa_shim_tools[i], dir, strerror(errno));
            return -1;
        }
    }
    return 0;
}

/* Remove the shims masa_make_shims() created, then the directory. */
static void remove_shims(const char *dir)
{
    size_t i;

    if (dir == NULL || dir[0] == '\0') {
        return;
    }
    for (i = 0; masa_shim_tools[i] != NULL; i++) {
        char linkpath[MASA_PATH_MAX];

        if (snprintf(linkpath, sizeof(linkpath), "%s/%s", dir,
                     masa_shim_tools[i]) < (int)sizeof(linkpath)) {
            masa_unlink(linkpath);
        }
    }
    masa_rmdir(dir);
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

    printf("masa: created " MASA_SIZE_FMT " shims in %s\n",
           MASA_SIZE_ARG((sizeof(masa_shim_tools) /
                          sizeof(masa_shim_tools[0])) - 1), dir);
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
        masa_setenv(name, saved, 1);
    } else {
        masa_unsetenv(name);
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
        if (masa_make_temp_dir("masa-shims-", env->dir,
                               sizeof(env->dir)) != 0) {
            fprintf(stderr, "masa: exec: cannot create a shim directory\n");
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

    masa_setenv(MASA_SHIM_DIR_ENV, env->dir, 1);
    if (real_cc != NULL) {
        masa_setenv(MASA_REAL_CC_ENV, real_cc, 1);
    }

    need = strlen(env->dir) + 1 +
           ((env->saved_path != NULL) ? strlen(env->saved_path) : 0) + 1;
    env->new_path = malloc(need);
    if (env->new_path == NULL) {
        goto fail;
    }
    snprintf(env->new_path, need, "%s%c%s", env->dir, masa_path_separator(),
             (env->saved_path != NULL) ? env->saved_path : "");
    masa_setenv("PATH", env->new_path, 1);

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

/* ------------------------------------------------------------------ */
/* masa shell                                                         */
/* ------------------------------------------------------------------ */

/*
 * Pick the interactive shell.  zsh is preferred (its prompt syntax is
 * the nicer one), then bash, then sh; on Windows $COMSPEC is used.
 * masa_shell_path() is the last resort and always returns a usable path.
 */
static const char *choose_shell(void)
{
    static const char *candidates[] = {"zsh", "bash", "sh"};
    size_t i;

#ifdef _WIN32
    {
        const char *comspec = getenv("COMSPEC");

        if (comspec != NULL && comspec[0] != '\0') {
            return comspec;
        }
    }
#endif

    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        char *found = masa_which(candidates[i]);

        if (found != NULL) {
            /* Only the name matters; the shell resolves it on PATH again. */
            free(found);
            return candidates[i];
        }
    }
    return masa_shell_path();
}

/* Basename of the shell, so "/bin/bash" and "bash" both read as "bash". */
static const char *shell_base(const char *shell)
{
    const char *slash = strrchr(shell, '/');

    return (slash != NULL) ? slash + 1 : shell;
}

/*
 * The prompt Masa installs.  zsh needs %~/%# and reads PROMPT/PS1; bash
 * and sh need \w/$ and read PS1.
 */
static const char *shell_prompt_var(const char *base)
{
    return (strcmp(base, "zsh") == 0) ? "PROMPT" : "PS1";
}

static const char *shell_prompt_value(const char *base)
{
    if (strcmp(base, "zsh") == 0) {
        return "masa@Darwin %~ %# ";
    }
    return "masa@Darwin:\\w$ ";
}

int masa_shell_main(int argc, char *const argv[])
{
    struct shim_env env;
    const char *command = NULL;
    const char *shell;
    const char *base;
    const char *saved_ps1 = NULL;
    char *saved_ps1_alloc = NULL;
    const char *spawn_argv[8];
    int i;
    int n = 0;
    int status;

    /* Parse `masa shell [-c <command>]`; anything else is passed through. */
    for (i = 0; i < argc; i++) {
        if (argv[i] == NULL) {
            break;
        }
        if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc || argv[i + 1] == NULL) {
                fprintf(stderr, "masa: shell: -c requires a command\n");
                return 2;
            }
            command = argv[i + 1];
            break;
        }
    }

    if (shim_env_setup(&env, NULL) != 0) {
        return 1;
    }

    shell = choose_shell();
    base = shell_base(shell);

    /* A clean, Mac-looking prompt unless the caller asked to keep theirs. */
    if (getenv(MASA_KEEP_PS1_ENV) == NULL) {
        const char *var = shell_prompt_var(base);
        const char *value = shell_prompt_value(base);

        saved_ps1 = getenv(var);
        if (saved_ps1 != NULL) {
            saved_ps1_alloc = strdup(saved_ps1);
            if (saved_ps1_alloc == NULL) {
                saved_ps1 = NULL;
            }
        }
        masa_setenv(var, value, 1);
    }

    if (getenv("TERM") == NULL) {
        masa_setenv("TERM", "xterm-256color", 0);
    }

    spawn_argv[n++] = shell;
    if (command != NULL) {
        spawn_argv[n++] = "-c";
        spawn_argv[n++] = command;
    }
    spawn_argv[n] = NULL;

    status = masa_spawn_wait(spawn_argv);

    /* Restore the caller's prompt (only if we changed it). */
    if (getenv(MASA_KEEP_PS1_ENV) == NULL) {
        const char *var = shell_prompt_var(base);

        if (saved_ps1_alloc != NULL) {
            masa_setenv(var, saved_ps1_alloc, 1);
            free(saved_ps1_alloc);
        } else {
            masa_unsetenv(var);
        }
    }

    shim_env_teardown(&env);
    return status;
}
