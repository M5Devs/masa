/*
 * pty_common.c - helpers shared by the pseudo-terminal backends.
 *
 * Building the `masa shell` command line and the environment it runs in
 * is platform-independent, so it lives here rather than being duplicated
 * in src/pty_posix.c and src/pty_win.c.  This file only uses libc (plus
 * a little Win32 for the environment block), never SDL or libvterm, so
 * it compiles anywhere the backends do.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "masa.h"
#include "masa_compat.h"
#include "masa_pty.h"

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

#ifdef _WIN32
            if (slash == NULL) {
                slash = strrchr(self, '\\');
            }
#endif
            if (slash != NULL) {
                size_t need;
                struct stat st;

                *slash = '\0';
                need = strlen(self) + strlen("/masa") + 1;
                cand = malloc(need);
                if (cand != NULL) {
                    snprintf(cand, need, "%s/masa", self);
                    if (stat(cand, &st) != 0 || !S_ISREG(st.st_mode)) {
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

#ifdef _WIN32

/* Copy the process environment into a NULL-terminated array. */
static char **windows_environ(void)
{
    char *block = GetEnvironmentStringsA();
    char **out = NULL;
    size_t count = 0;
    const char *p;

    if (block == NULL) {
        return NULL;
    }

    for (p = block; *p != '\0'; p += strlen(p) + 1) {
        char *copy;

        if (strncmp(p, "TERM=", 5) == 0) {
            copy = strdup("TERM=xterm-256color");
        } else {
            copy = strdup(p);
        }
        if (copy == NULL) {
            masa_gui_free_argv(out);
            FreeEnvironmentStringsA(block);
            return NULL;
        }
        out = argv_push(out, count, copy);
        if (out == NULL) {
            free(copy);
            FreeEnvironmentStringsA(block);
            return NULL;
        }
        count++;
    }
    FreeEnvironmentStringsA(block);

    if (getenv("TERM") == NULL) {
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

char **masa_gui_shell_env(void)
{
    return windows_environ();
}

#else /* !_WIN32 */

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

#endif /* _WIN32 */

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
