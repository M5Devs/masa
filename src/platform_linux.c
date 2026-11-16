/*
 * platform_linux.c - process spawning for POSIX hosts.
 *
 * All child-process creation in Masa funnels through this file so that
 * additional backends (Windows, Android) can be added later without
 * touching the wrapper or the shims.
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
