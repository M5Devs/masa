/*
 * masa_gui.h - interface for masa-term, the standalone GUI terminal
 * emulator that ships with Masa.
 *
 * The core command line tool (src/main.c and friends) stays free of any
 * third-party dependency so it keeps building for Android and Windows.
 * This header belongs to the optional GUI build (`make gui`), which pulls
 * in SDL2, SDL2_ttf and libvterm and produces build/masa-term.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MASA_GUI_H
#define MASA_GUI_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <SDL.h>
#include <vterm.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* PTY backend (src/pty_posix.c)                                       */
/* ------------------------------------------------------------------ */

/*
 * A pseudo-terminal running a child process.  The master file descriptor
 * is non-blocking, so reads return 0 bytes (rather than blocking) when no
 * output is pending.
 */
struct masa_pty {
    int master_fd;
    pid_t child_pid;
    int closed;
};

/*
 * Spawn argv[0] on a fresh pseudo-terminal of the given size.  argv must
 * be NULL-terminated; envp may be NULL to inherit the parent's environ.
 * Returns 0 on success and -1 on failure (errno is left set).
 */
int masa_pty_open(struct masa_pty *pty, int rows, int cols,
                  char *const argv[], char *const envp[]);

/*
 * Read whatever the child has produced without blocking.  Returns the
 * number of bytes read, 0 when nothing is available, or -1 on error.
 */
ssize_t masa_pty_read(struct masa_pty *pty, char *buf, size_t len);

/*
 * Write the whole buffer to the child, retrying short writes and EAGAIN.
 * Returns the number of bytes written (== len on success) or -1 on error.
 */
ssize_t masa_pty_write(struct masa_pty *pty, const char *buf, size_t len);

/*
 * Tell the child's terminal its new size (TIOCSWINSZ), so full-screen
 * programs such as `less` or `vi` reflow.  Returns 0 on success.
 */
int masa_pty_set_size(struct masa_pty *pty, int rows, int cols);

/*
 * Reap the child if it has exited.  On success *status holds the wait
 * status and the return value is 1; the return value is 0 while the
 * child is still running and -1 on error.
 */
int masa_pty_child_status(struct masa_pty *pty, int *status);

/*
 * Close the master, terminate and reap the child.  Safe to call twice.
 * Returns the child's exit status, or -1 if it could not be collected.
 */
int masa_pty_close(struct masa_pty *pty);

/*
 * Build the argv/envp used to run `masa shell` on the PTY.  The Masa
 * executable is located through $MASA_BIN, then next to the running
 * binary, then on PATH.  Both results are malloc'd, NULL-terminated
 * arrays released by masa_gui_free_argv(); either may be NULL on error.
 */
char **masa_gui_shell_argv(void);
char **masa_gui_shell_env(void);
void masa_gui_free_argv(char **argv);

/* ------------------------------------------------------------------ */
/* Terminal state + SDL renderer (src/gui_term.c)                      */
/* ------------------------------------------------------------------ */

/* An opaque handle holding the SDL objects, the font and the libvterm
 * screen the renderer draws from. */
struct masa_gui;

/*
 * Create the window, renderer, font and terminal screen.  On failure NULL
 * is returned and, when err is non-NULL, a human-readable reason is
 * written into it.
 */
struct masa_gui *masa_gui_init(const char *title, int win_w, int win_h,
                               char *err, size_t errcap);

/* Feed raw bytes (normally PTY output) into the terminal screen. */
void masa_gui_feed(struct masa_gui *gui, const char *bytes, size_t len);

/* Translate a key press into the bytes the child expects (escape
 * sequences for arrows/function keys, control codes for Ctrl-<letter>)
 * and store them in out for the caller to write to the PTY.  Returns the
 * number of bytes stored (0 when the key produces nothing). */
size_t masa_gui_input(struct masa_gui *gui, const SDL_Keycode key,
                      uint16_t mod, char *out, size_t outcap);

/* Translate a typed UTF-8 string (SDL_TEXTINPUT) into child input, like
 * masa_gui_input().  Returns the bytes placed in out. */
size_t masa_gui_input_text(struct masa_gui *gui, const char *utf8,
                           char *out, size_t outcap);

/* The terminal's current size in cells. */
void masa_gui_size(const struct masa_gui *gui, int *rows, int *cols);

/* Resize the terminal to fit a window of the given pixel size; updates
 * libvterm and reports the new cell grid. */
void masa_gui_resize(struct masa_gui *gui, int win_w, int win_h);

/* Render one frame. */
void masa_gui_render(struct masa_gui *gui);

/* Read the whole visible screen into out as UTF-8, one row per line.
 * Returns the number of bytes written (excluding the NUL). */
size_t masa_gui_screen_text(const struct masa_gui *gui, char *out, size_t cap);

/* Feed bytes into a caller-owned libvterm screen until the PTY goes idle
 * for idle_ms, the child exits, or max_ms elapses.  Returns the number of
 * bytes fed, or -1 on error. */
int masa_gui_pump(struct masa_pty *pty, VTerm *vt, int idle_ms, int max_ms);

/* Release everything masa_gui_init() created.  Passing NULL is a no-op. */
void masa_gui_shutdown(struct masa_gui *gui);

/*
 * `masa-term` entry point: open a PTY running `masa shell`, then run the
 * SDL event loop until the window is closed.  Recognises:
 *   --headless        use the dummy video driver and exit after a few
 *                     frames (used by the test suite)
 *   --frames <n>      number of frames to draw in headless mode
 * Returns the process exit status.
 */
int masa_gui_main(int argc, char *const argv[]);

#ifdef __cplusplus
}
#endif

#endif /* MASA_GUI_H */
