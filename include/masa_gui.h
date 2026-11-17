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

/* ------------------------------------------------------------------ */
/* Tabs, scrollback and clipboard (src/gui_term.c)                     */
/* ------------------------------------------------------------------ */

/* Upper bounds: at most 16 tabs, each retaining 1000 scrollback lines. */
#define MASA_GUI_MAX_TABS 16
#define MASA_GUI_SCROLLBACK 1000

/* Number of open tabs. */
int masa_gui_tab_count(const struct masa_gui *gui);

/* Index of the active tab (0-based). */
int masa_gui_tab_active(const struct masa_gui *gui);

/* Which tab the pixel (x, y) falls on in the tab bar, or -1 when the point
 * is not over a tab.  Used to switch tabs by clicking. */
int masa_gui_tab_at(const struct masa_gui *gui, int x, int y);

/* Open a new tab (with its own libvterm screen) and make it active.
 * Returns the new tab's index, or -1 when the tab limit is reached. */
int masa_gui_tab_new(struct masa_gui *gui);

/* Make tab `index` active.  Returns 0 on success and -1 on a bad index. */
int masa_gui_tab_select(struct masa_gui *gui, int index);

/* Close tab `index`, reaping its PTY and freeing its vterm and scrollback.
 * Returns the number of tabs remaining, or -1 on a bad index.  Closing the
 * last tab leaves zero tabs; the caller decides whether to exit. */
int masa_gui_tab_close(struct masa_gui *gui, int index);

/* The embedded PTY of tab `index` (open it with masa_gui_tab_open_pty()),
 * or NULL on a bad index. */
struct masa_pty *masa_gui_tab_pty(struct masa_gui *gui, int index);

/* Open tab `index`'s PTY at the current grid size.  Returns 0 on success. */
int masa_gui_tab_open_pty(struct masa_gui *gui, int index,
                          char *const argv[], char *const envp[]);

/* The title tab `index` reported (OSC 0/2), or "" when none. */
const char *masa_gui_tab_title(const struct masa_gui *gui, int index);

/* Feed bytes into a specific tab's screen. */
void masa_gui_tab_feed(struct masa_gui *gui, int index, const char *bytes,
                       size_t len);

/* Scrollback for the active tab. */
int masa_gui_scrollback_count(const struct masa_gui *gui);

/* Copy retained scrollback line `index` (0 = oldest) into out as UTF-8,
 * without a trailing newline.  Returns the number of bytes written. */
size_t masa_gui_scrollback_line(const struct masa_gui *gui, int index,
                                char *out, size_t cap);

/* Scroll the view by `lines` (positive = back into history, negative =
 * toward the live view).  Returns the resulting offset (0 = live). */
int masa_gui_scroll(struct masa_gui *gui, int lines);

/* The current scroll offset (0 = live). */
int masa_gui_scroll_offset(const struct masa_gui *gui);

/* Snap the view back to the live screen. */
void masa_gui_scroll_reset(struct masa_gui *gui);

/* Translate clipboard text into the bytes the child expects and store them
 * in out (a paste).  Returns the number of bytes written. */
size_t masa_gui_paste(struct masa_gui *gui, const char *text, char *out,
                      size_t outcap);

/* Record a selection between two screen cells (live-view row/col). */
void masa_gui_set_selection(struct masa_gui *gui, int r0, int c0, int r1,
                            int c1);
void masa_gui_clear_selection(struct masa_gui *gui);

/* Copy the current selection into out as UTF-8 (one line per row, trailing
 * blanks trimmed).  Returns the number of bytes written (0 = no selection). */
size_t masa_gui_copy_selection(const struct masa_gui *gui, char *out,
                               size_t cap);

/*
 * `masa-term` entry point: open a PTY running `masa shell`, then run the
 * SDL event loop until the window is closed.  Recognises:
 *   --headless        use the dummy video driver and exit after a few
 *                     frames (used by the test suite)
 *   --frames <n>      number of frames to draw in headless mode
 * Returns the process exit status.
 */
int masa_gui_main(int argc, char *const argv[]);

/* ------------------------------------------------------------------ */
/* Danroot AppKit/CoreGraphics SDL2 backend (src/danroot_appkit.c)     */
/* ------------------------------------------------------------------ */

/*
 * The RGBA pixel at the centre of the last frame the AppKit run loop
 * presented, captured for the test harness.  Returns 1 and fills the four
 * bytes when a frame has been captured, and 0 otherwise.  This is the
 * hook a test uses to verify that CGContextFillRect actually painted the
 * expected colour on screen.
 */
int masa_gui_read_pixel(unsigned char *rgba);

#ifdef __cplusplus
}
#endif

#endif /* MASA_GUI_H */
