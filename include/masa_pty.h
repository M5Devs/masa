/*
 * masa_pty.h - the pseudo-terminal interface used by masa-term.
 *
 * This header is deliberately free of SDL and libvterm so that the
 * platform PTY backends (src/pty_posix.c on POSIX, src/pty_win.c on
 * Windows) can be compiled on their own, including a Windows cross-build
 * on a Linux host where no SDL is installed.  The GUI renderer's own
 * interface lives in masa_gui.h, which includes this one.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MASA_PTY_H
#define MASA_PTY_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A pseudo-terminal running a child process.  The parent-side stream is
 * non-blocking: a read returns 0 bytes (rather than blocking) when no
 * output is pending.
 *
 * The handle fields are platform-specific and private to the backend;
 * callers treat a struct masa_pty as opaque.
 */
struct masa_pty {
#ifdef _WIN32
    void *input_write;      /* parent -> child (ConPTY stdin) */
    void *output_read;      /* child -> parent (ConPTY stdout) */
    void *process;          /* child process handle */
    void *pseudo_console;   /* HPCON returned by CreatePseudoConsole */
    unsigned long exit_code;
    int exited;
#else
    int master_fd;
    pid_t child_pid;
#endif
    int closed;
};

/*
 * Spawn argv[0] on a fresh pseudo-terminal of the given size.  argv must
 * be NULL-terminated; envp may be NULL to inherit the parent's
 * environment.  Returns 0 on success and -1 on failure.
 */
int masa_pty_open(struct masa_pty *pty, int rows, int cols,
                  char *const argv[], char *const envp[]);

/*
 * Read whatever the child has produced without blocking.  Returns the
 * number of bytes read, 0 when nothing is available, or -1 on error.
 */
ssize_t masa_pty_read(struct masa_pty *pty, char *buf, size_t len);

/*
 * Write the whole buffer to the child, retrying short writes.  Returns
 * the number of bytes written (== len on success) or -1 on error.
 */
ssize_t masa_pty_write(struct masa_pty *pty, const char *buf, size_t len);

/*
 * Tell the child's terminal its new size, so full-screen programs such
 * as `less` or `vi` reflow.  Returns 0 on success.
 */
int masa_pty_set_size(struct masa_pty *pty, int rows, int cols);

/*
 * Reap the child if it has exited.  On success *status holds the exit
 * status and the return value is 1; the return value is 0 while the
 * child is still running and -1 on error.
 */
int masa_pty_child_status(struct masa_pty *pty, int *status);

/*
 * Close the terminal, terminate and reap the child.  Safe to call twice.
 * Returns the child's exit status, or -1 if it could not be collected.
 */
int masa_pty_close(struct masa_pty *pty);

/*
 * Build the argv/envp used to run `masa shell` on the PTY.  The Masa
 * executable is located through $MASA_BIN, then next to the running
 * binary, then on PATH.  Both results are malloc'd, NULL-terminated
 * arrays released by masa_gui_free_argv(); either may be NULL on error.
 *
 * These helpers are shared by both backends and live in src/pty_common.c.
 */
char **masa_gui_shell_argv(void);
char **masa_gui_shell_env(void);
void masa_gui_free_argv(char **argv);

#ifdef __cplusplus
}
#endif

#endif /* MASA_PTY_H */
