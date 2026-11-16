/*
 * pty_posix.c - pseudo-terminal backend for masa-term.
 *
 * A terminal emulator needs the child to believe it is talking to a real
 * tty, so that line editing, job control, colour detection and full-screen
 * programs all work.  This file forks `masa shell` onto a fresh PTY and
 * exposes the master side as a non-blocking byte stream plus a window
 * resize call.  It is POSIX-only and is compiled solely by `make gui`.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "masa.h"
#include "masa_compat.h"
#include "masa_gui.h"

/*
 * openpty(3) lives in libutil on glibc (Linux) but in libc on BSD/macOS.
 * The Makefile links -lutil for the GUI build on Linux; on other POSIX
 * hosts the symbol resolves from libc.
 */
int openpty(int *amaster, int *aslave, char *name,
            const struct termios *termp, const struct winsize *winp);

int masa_pty_open(struct masa_pty *pty, int rows, int cols,
                  char *const argv[], char *const envp[])
{
    int master = -1;
    int slave = -1;
    struct winsize ws;
    pid_t pid;

    if (pty == NULL || argv == NULL || argv[0] == NULL) {
        errno = EINVAL;
        return -1;
    }

    pty->master_fd = -1;
    pty->child_pid = -1;
    pty->closed = 0;

    memset(&ws, 0, sizeof(ws));
    ws.ws_row = (unsigned short)((rows > 0) ? rows : 24);
    ws.ws_col = (unsigned short)((cols > 0) ? cols : 80);

    if (openpty(&master, &slave, NULL, NULL, &ws) != 0) {
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(master);
        close(slave);
        return -1;
    }

    if (pid == 0) {
        /* Child: make the slave our controlling terminal and exec. */
        if (setsid() < 0) {
            _exit(127);
        }
#ifdef TIOCSCTTY
        ioctl(slave, TIOCSCTTY, 0);
#endif
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO) {
            close(slave);
        }
        if (master >= 0) {
            close(master);
        }

        if (envp != NULL) {
            execve(argv[0], argv, envp);
        } else {
            execv(argv[0], argv);
        }
        _exit(127);
    }

    /* Parent. */
    close(slave);
    pty->master_fd = master;
    pty->child_pid = pid;

    {
        int flags = fcntl(master, F_GETFL, 0);

        if (flags >= 0) {
            fcntl(master, F_SETFL, flags | O_NONBLOCK);
        }
    }

    return 0;
}

ssize_t masa_pty_read(struct masa_pty *pty, char *buf, size_t len)
{
    ssize_t n;

    if (pty == NULL || pty->master_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    do {
        n = read(pty->master_fd, buf, len);
    } while (n < 0 && errno == EINTR);

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0;
    }
    if (n < 0 && errno == EIO) {
        /* The child closed the slave; treat it as end of input. */
        return 0;
    }
    return n;
}

ssize_t masa_pty_write(struct masa_pty *pty, const char *buf, size_t len)
{
    size_t done = 0;

    if (pty == NULL || pty->master_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    while (done < len) {
        ssize_t n = write(pty->master_fd, buf + done, len - done);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return -1;
        }
        done += (size_t)n;
    }
    return (ssize_t)done;
}

int masa_pty_set_size(struct masa_pty *pty, int rows, int cols)
{
    struct winsize ws;

    if (pty == NULL || pty->master_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    memset(&ws, 0, sizeof(ws));
    ws.ws_row = (unsigned short)((rows > 0) ? rows : 24);
    ws.ws_col = (unsigned short)((cols > 0) ? cols : 80);

    return ioctl(pty->master_fd, TIOCSWINSZ, &ws);
}

int masa_pty_child_status(struct masa_pty *pty, int *status)
{
    pid_t r;

    if (pty == NULL || pty->child_pid <= 0) {
        return -1;
    }

    r = waitpid(pty->child_pid, status, WNOHANG);
    if (r == 0) {
        return 0;
    }
    if (r < 0) {
        return -1;
    }
    return 1;
}

int masa_pty_close(struct masa_pty *pty)
{
    int status = -1;

    if (pty == NULL || pty->closed) {
        return -1;
    }
    pty->closed = 1;

    if (pty->child_pid > 0) {
        int child_status = 0;

        if (masa_pty_child_status(pty, &child_status) == 0) {
            /* Still running: ask it to stop, then insist. */
            kill(pty->child_pid, SIGHUP);
            if (waitpid(pty->child_pid, &child_status, WNOHANG) == 0) {
                kill(pty->child_pid, SIGKILL);
            }
            waitpid(pty->child_pid, &child_status, 0);
        }
        if (WIFEXITED(child_status)) {
            status = WEXITSTATUS(child_status);
        } else if (WIFSIGNALED(child_status)) {
            status = 128 + WTERMSIG(child_status);
        }
    }

    if (pty->master_fd >= 0) {
        close(pty->master_fd);
        pty->master_fd = -1;
    }
    return status;
}

/* ------------------------------------------------------------------ */
/* `masa shell` command construction                                   */
/* ------------------------------------------------------------------ */

/* Append an element to a NULL-terminated, malloc'd array. */
static char **argv_push(char **list, size_t count, char *item)
{
    char **grown = realloc(list, (count + 2) * sizeof(*grown));

    if (grown == NULL) {
        return NULL;
    }
    grown[count] = item;
    grown[count + 1] = NULL;
    return grown;
}

static char *locate_masa(void)
{
    const char *env = getenv("MASA_BIN");

    if (env != NULL && env[0] != '\0') {
        return strdup(env);
    }

    {
        char *self = masa_self_path();

        if (self != NULL) {
            char *slash = strrchr(self, '/');
            char *cand = NULL;

            if (slash != NULL) {
                size_t need;
                struct stat st;

                *slash = '\0';
                need = strlen(self) + strlen("/masa") + 1;
                cand = malloc(need);
                if (cand != NULL) {
                    snprintf(cand, need, "%s/masa", self);
                    if (stat(cand, &st) != 0) {
                        free(cand);
                        cand = NULL;
                    }
                }
            }
            free(self);
            if (cand != NULL) {
                return cand;
            }
        }
    }

    return masa_which("masa");
}

char **masa_gui_shell_argv(void)
{
    char *exe = locate_masa();
    char **list = NULL;
    char *sub;

    if (exe == NULL) {
        return NULL;
    }

    /* Every element must be owned, because masa_gui_free_argv() frees them. */
    sub = strdup("shell");
    if (sub == NULL) {
        free(exe);
        return NULL;
    }

    list = argv_push(list, 0, exe);
    if (list == NULL) {
        free(exe);
        free(sub);
        return NULL;
    }
    list = argv_push(list, 1, sub);
    if (list == NULL) {
        free(exe);
        free(sub);
        return NULL;
    }
    return list;
}

char **masa_gui_shell_env(void)
{
    extern char **environ;
    char **out = NULL;
    size_t count = 0;
    char **p;
    int have_term = 0;

    for (p = environ; p != NULL && *p != NULL; p++) {
        char *copy;

        if (strncmp(*p, "TERM=", 5) == 0) {
            copy = strdup("TERM=xterm-256color");
            have_term = 1;
        } else {
            copy = strdup(*p);
        }
        if (copy == NULL) {
            masa_gui_free_argv(out);
            return NULL;
        }
        out = argv_push(out, count, copy);
        if (out == NULL) {
            free(copy);
            return NULL;
        }
        count++;
    }

    if (!have_term) {
        char *copy = strdup("TERM=xterm-256color");

        if (copy == NULL) {
            masa_gui_free_argv(out);
            return NULL;
        }
        out = argv_push(out, count, copy);
        if (out == NULL) {
            free(copy);
            masa_gui_free_argv(out);
            return NULL;
        }
    }

    return out;
}

void masa_gui_free_argv(char **argv)
{
    size_t i;

    if (argv == NULL) {
        return;
    }
    for (i = 0; argv[i] != NULL; i++) {
        free(argv[i]);
    }
    free(argv);
}

/*
 * `masa_gui_pump` drives the terminal without the SDL event loop.  It is
 * shared by the renderer's input path and the test suite, which runs
 * headless and only needs the screen contents.
 */
int masa_gui_pump(struct masa_pty *pty, VTerm *vt, int idle_ms, int max_ms)
{
    char buf[4096];
    int total = 0;
    int idle = 0;
    int elapsed = 0;
    int child_done = 0;

    if (pty == NULL || vt == NULL) {
        errno = EINVAL;
        return -1;
    }

    for (;;) {
        ssize_t n = masa_pty_read(pty, buf, sizeof(buf));

        if (n < 0) {
            return -1;
        }
        if (n > 0) {
            vterm_input_write(vt, buf, (size_t)n);
            total += (int)n;
            idle = 0;
            continue;
        }

        if (!child_done) {
            int status = 0;
            int r = masa_pty_child_status(pty, &status);

            if (r < 0) {
                break;
            }
            if (r == 1) {
                /* Give the kernel a moment to hand over the last bytes. */
                child_done = 1;
                usleep(1000);
                continue;
            }
        }

        if (child_done) {
            break;
        }
        if (idle_ms > 0 && idle >= idle_ms) {
            break;
        }
        if (max_ms > 0 && elapsed >= max_ms) {
            break;
        }

        usleep(1000);
        idle++;
        elapsed++;
    }

    return total;
}
