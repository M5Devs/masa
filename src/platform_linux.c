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
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int masa_spawn_wait(const char *const argv[])
{
    pid_t pid;
    int status;

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
        execvp(argv[0], (char *const *)argv);
        fprintf(stderr, "masa: cannot execute '%s': %s\n",
                argv[0], strerror(errno));
        _exit(MASA_EXIT_SPAWN_ERROR);
    }

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "masa: waitpid failed: %s\n", strerror(errno));
            return -1;
        }
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return -1;
}
