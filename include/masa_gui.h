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

#include <SDL.h>
#include <vterm.h>

#include "masa_pty.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The PTY interface (struct masa_pty, masa_pty_* and the `masa shell`
 * argv/env builders) lives in masa_pty.h, which both PTY backends share. */

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
