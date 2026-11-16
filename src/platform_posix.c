/*
 * platform_posix.c - process spawning and host services for POSIX hosts.
 *
 * All child-process creation in Masa funnels through this file so the
 * wrapper and the shims stay portable.  It targets both Linux and
 * Android (Termux), where /tmp may not exist and the shell lives in
 * /system/bin, so temporary directories honour $TMPDIR and the shell
 * path is probed rather than assumed.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "masa.h"
#include "masa_compat.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Translate a waitpid status into the shell-style exit code Masa reports. */
static int decode_status(int status)
{
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return -1;
}

static int wait_for(pid_t pid)
{
    int status;

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "masa: waitpid failed: %s\n", strerror(errno));
            return -1;
        }
    }
    return decode_status(status);
}

int masa_spawn_wait_in(const char *dir, const char *const argv[])
{
    pid_t pid;

    if (argv == NULL || argv[0] == NULL || argv[0][0] == '\0') {
        fprintf(stderr, "masa: no command to run\n");
        return -1;
    }

    /* Flush before forking so the child does not duplicate pending output. */
    fflush(stdout);
    fflush(stderr);

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "masa: fork failed: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        if (dir != NULL && dir[0] != '\0' && chdir(dir) != 0) {
            fprintf(stderr, "masa: cannot chdir to '%s': %s\n",
                    dir, strerror(errno));
            _exit(MASA_EXIT_SPAWN_ERROR);
        }
        execvp(argv[0], (char *const *)argv);
        fprintf(stderr, "masa: cannot execute '%s': %s\n",
                argv[0], strerror(errno));
        _exit(MASA_EXIT_SPAWN_ERROR);
    }

    return wait_for(pid);
}

int masa_spawn_wait(const char *const argv[])
{
    return masa_spawn_wait_in(NULL, argv);
}

int masa_capture(const char *const argv[], char *out, size_t outcap)
{
    int fds[2];
    pid_t pid;
    char *buf;
    size_t total = 0;
    ssize_t n;
    int status;

    if (out != NULL && outcap > 0) {
        out[0] = '\0';
    }
    if (argv == NULL || argv[0] == NULL || argv[0][0] == '\0') {
        fprintf(stderr, "masa: no command to run\n");
        return -1;
    }

    buf = malloc(outcap + 1);
    if (buf == NULL) {
        return -1;
    }

    fflush(stdout);
    fflush(stderr);

    if (pipe(fds) != 0) {
        fprintf(stderr, "masa: pipe failed: %s\n", strerror(errno));
        free(buf);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "masa: fork failed: %s\n", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        free(buf);
        return -1;
    }

    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) {
            _exit(MASA_EXIT_SPAWN_ERROR);
        }
        close(fds[1]);
        execvp(argv[0], (char *const *)argv);
        fprintf(stderr, "masa: cannot execute '%s': %s\n",
                argv[0], strerror(errno));
        _exit(MASA_EXIT_SPAWN_ERROR);
    }

    close(fds[1]);
    while (total < outcap) {
        n = read(fds[0], buf + total, outcap - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        total += (size_t)n;
    }
    close(fds[0]);

    status = wait_for(pid);

    buf[total] = '\0';
    if (out != NULL && outcap > 0) {
        snprintf(out, outcap, "%s", buf);
    }
    free(buf);
    return status;
}

/* True when the path exists and is a directory. */
static int is_dir(const char *path)
{
    struct stat st;

    return (path != NULL && path[0] != '\0' && stat(path, &st) == 0 &&
            S_ISDIR(st.st_mode));
}

int masa_make_temp_dir(const char *prefix, char *buf, size_t bufcap)
{
    const char *base;
    char template_[MASA_PATH_MAX];
    const char *sep;

    if (prefix == NULL) {
        prefix = "masa-";
    }

    if (buf == NULL || bufcap == 0) {
        return -1;
    }

    /* $TMPDIR first: Android/Termux and sandboxes redirect it. */
    base = getenv(MASA_TMPDIR_ENV);
    if (!is_dir(base)) {
        base = "/tmp";
        if (!is_dir(base)) {
            base = "/data/local/tmp";
            if (!is_dir(base)) {
                base = ".";
            }
        }
    }

    sep = (base[strlen(base) - 1] == '/') ? "" : "/";
    if (snprintf(template_, sizeof(template_), "%s%s%sXXXXXX", base, sep,
                 prefix) >= (int)sizeof(template_)) {
        return -1;
    }
    if (mkdtemp(template_) == NULL) {
        fprintf(stderr, "masa: cannot create a temporary directory under "
                        "'%s': %s\n", base, strerror(errno));
        return -1;
    }
    if (strlen(template_) >= bufcap) {
        rmdir(template_);
        return -1;
    }
    snprintf(buf, bufcap, "%s", template_);
    return 0;
}

char masa_path_separator(void)
{
    return ':';
}

const char *masa_shell_path(void)
{
    if (access("/bin/sh", X_OK) == 0) {
        return "/bin/sh";
    }
    /* Android keeps its shell here; there is no /bin/sh. */
    if (access("/system/bin/sh", X_OK) == 0) {
        return "/system/bin/sh";
    }
    return "/bin/sh";
}

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

/* 1 when path is a regular, executable file. */
static int is_exec_file(const char *path)
{
    struct stat st;

    return (path != NULL && path[0] != '\0' && stat(path, &st) == 0 &&
            S_ISREG(st.st_mode) && access(path, X_OK) == 0);
}

char *masa_which(const char *name)
{
    const char *path_env;
    char *copy;
    char *dir;

    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
    if (strchr(name, '/') != NULL) {
        return is_exec_file(name) ? strdup(name) : NULL;
    }

    path_env = getenv("PATH");
    if (path_env == NULL) {
        return NULL;
    }
    copy = strdup(path_env);
    if (copy == NULL) {
        return NULL;
    }

    for (dir = strtok(copy, ":"); dir != NULL; dir = strtok(NULL, ":")) {
        char candidate[MASA_PATH_MAX];
        const char *d = (dir[0] == '\0') ? "." : dir;

        if (snprintf(candidate, sizeof(candidate), "%s/%s", d, name) >=
            (int)sizeof(candidate)) {
            continue;
        }
        if (is_exec_file(candidate)) {
            char *result = strdup(candidate);

            free(copy);
            return result;
        }
    }

    free(copy);
    return NULL;
}

int masa_setenv(const char *name, const char *value, int overwrite)
{
    return setenv(name, value, overwrite);
}

int masa_unsetenv(const char *name)
{
    return unsetenv(name);
}

int masa_mkdir(const char *path)
{
    return mkdir(path, 0755);
}

int masa_make_shim(const char *dir, const char *name, const char *target)
{
    char linkpath[MASA_PATH_MAX];

    if (snprintf(linkpath, sizeof(linkpath), "%s/%s", dir, name) >=
        (int)sizeof(linkpath)) {
        return -1;
    }
    unlink(linkpath);
    return (symlink(target, linkpath) == 0) ? 0 : -1;
}

int masa_unlink(const char *path)
{
    return unlink(path);
}

int masa_rmdir(const char *path)
{
    return rmdir(path);
}

int masa_mkdir_p(const char *path)
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

char *masa_realpath(const char *path)
{
    char buf[MASA_PATH_MAX];

    if (path == NULL || realpath(path, buf) == NULL) {
        return NULL;
    }
    return strdup(buf);
}

int masa_symlink_or_copy(const char *target, const char *linkpath)
{
    unlink(linkpath);
    return (symlink(target, linkpath) == 0) ? 0 : -1;
}
