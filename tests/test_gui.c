/*
 * test_gui.c - tests for masa-term, the SDL2/libvterm terminal emulator.
 *
 * Two things are exercised here without a display:
 *   1. The PTY backend runs `masa shell -c "uname -s && sw_vers -productName"`
 *      and the bytes it produces are fed through libvterm, whose screen
 *      buffer must contain "Darwin" and "macOS".
 *   2. The SDL2 + SDL2_ttf renderer comes up under SDL_VIDEODRIVER=dummy,
 *      draws a frame, resizes and tears down with no ASan/LSan leaks.
 *
 * Build and run with `make test-gui`.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL.h>
#include <vterm.h>

#include "masa.h"
#include "masa_compat.h"
#include "masa_gui.h"

static int g_run;
static int g_failed;
static int g_skipped;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_run++;                                                           \
        if (!(cond)) {                                                     \
            g_failed++;                                                    \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                  \
    } while (0)

#define CHECK_CONTAINS(hay, needle)                                        \
    do {                                                                   \
        g_run++;                                                           \
        if ((hay) == NULL || strstr((hay), (needle)) == NULL) {            \
            g_failed++;                                                    \
            printf("  FAIL %s:%d: expected to find \"%s\" in:\n%s\n",      \
                   __FILE__, __LINE__, (needle),                           \
                   (hay) != NULL ? (hay) : "(null)");                      \
        }                                                                  \
    } while (0)

/* Locate the `masa` executable: $MASA_BIN, then next to this test binary,
 * then on PATH.  Returns a malloc'd string or NULL. */
static char *find_masa(void)
{
    const char *env = getenv("MASA_BIN");
    char *self;
    char *slash;
    char *cand;

    if (env != NULL && env[0] != '\0') {
        return strdup(env);
    }

    self = masa_self_path();
    if (self != NULL) {
        slash = strrchr(self, '/');
        if (slash != NULL) {
            size_t need;

            *slash = '\0';
            need = strlen(self) + strlen("/masa") + 1;
            cand = malloc(need);
            if (cand != NULL) {
                snprintf(cand, need, "%s/masa", self);
                if (access(cand, X_OK) == 0) {
                    free(self);
                    return cand;
                }
                free(cand);
            }
        }
        free(self);
    }

    return masa_which("masa");
}

/* Render the whole libvterm screen into out as plain text. */
static void screen_dump(VTerm *vt, char *out, size_t cap)
{
    VTermScreen *screen = vterm_obtain_screen(vt);
    int rows = 0;
    int cols = 0;
    int r;
    size_t used = 0;

    vterm_get_size(vt, &rows, &cols);
    if (cap == 0) {
        return;
    }

    for (r = 0; r < rows; r++) {
        int last = -1;
        int c;

        for (c = 0; c < cols; c++) {
            VTermPos pos;
            VTermScreenCell cell;

            pos.row = r;
            pos.col = c;
            memset(&cell, 0, sizeof(cell));
            if (vterm_screen_get_cell(screen, pos, &cell) &&
                cell.chars[0] != 0 && cell.chars[0] != ' ') {
                last = c;
            }
        }
        for (c = 0; c <= last; c++) {
            VTermPos pos;
            VTermScreenCell cell;
            uint32_t cp;

            pos.row = r;
            pos.col = c;
            memset(&cell, 0, sizeof(cell));
            vterm_screen_get_cell(screen, pos, &cell);
            cp = (cell.chars[0] != 0) ? cell.chars[0] : ' ';
            if (used + 1 < cap) {
                out[used++] = (cp < 128) ? (char)cp : '?';
            }
        }
        if (used + 1 < cap) {
            out[used++] = '\n';
        }
    }
    out[(used < cap) ? used : cap - 1] = '\0';
}

static void test_pty_shell_through_vterm(void)
{
    char *masa = find_masa();
    struct masa_pty pty;
    VTerm *vt;
    char *argv[5];
    char text[8192];
    int rc;

    printf("[ RUN  ] test_pty_shell_through_vterm\n");

    if (masa == NULL) {
        printf("  SKIP masa executable not found\n");
        g_skipped++;
        return;
    }

    argv[0] = masa;
    argv[1] = (char *)"shell";
    argv[2] = (char *)"-c";
    argv[3] = (char *)"uname -s && sw_vers -productName";
    argv[4] = NULL;

    vt = vterm_new(24, 80);
    CHECK(vt != NULL);
    if (vt == NULL) {
        free(masa);
        return;
    }
    vterm_set_utf8(vt, 1);
    vterm_obtain_screen(vt);
    vterm_screen_reset(vterm_obtain_screen(vt), 1);

    rc = masa_pty_open(&pty, 24, 80, argv, NULL);
    if (rc != 0) {
        printf("  SKIP cannot open a pseudo-terminal: %s\n", strerror(errno));
        g_skipped++;
        vterm_free(vt);
        free(masa);
        return;
    }

    rc = masa_gui_pump(&pty, vt, 500, 8000);
    CHECK(rc >= 0);

    screen_dump(vt, text, sizeof(text));
    CHECK_CONTAINS(text, "Darwin");
    CHECK_CONTAINS(text, "macOS");

    masa_pty_close(&pty);
    vterm_free(vt);
    free(masa);
}

static void test_pty_resize(void)
{
    char *masa = find_masa();
    struct masa_pty pty;
    char *argv[5];
    int rc;

    printf("[ RUN  ] test_pty_resize\n");

    if (masa == NULL) {
        printf("  SKIP masa executable not found\n");
        g_skipped++;
        return;
    }

    argv[0] = masa;
    argv[1] = (char *)"shell";
    argv[2] = (char *)"-c";
    argv[3] = (char *)"true";
    argv[4] = NULL;

    rc = masa_pty_open(&pty, 24, 80, argv, NULL);
    if (rc != 0) {
        printf("  SKIP cannot open a pseudo-terminal\n");
        g_skipped++;
        free(masa);
        return;
    }

    CHECK(masa_pty_set_size(&pty, 40, 120) == 0);
    CHECK(masa_pty_write(&pty, "exit\n", 5) >= 0);

    masa_pty_close(&pty);
    free(masa);
}

static void test_headless_sdl_render(void)
{
    struct masa_gui *gui;
    char err[256];
    int rows = 0;
    int cols = 0;

    printf("[ RUN  ] test_headless_sdl_render\n");

    setenv("SDL_VIDEODRIVER", "dummy", 1);

    err[0] = '\0';
    gui = masa_gui_init("masa \xe2\x80\x94 Darwin", 640, 400, err, sizeof(err));
    if (gui == NULL) {
        printf("  SKIP SDL init failed: %s\n", err[0] ? err : "?");
        g_skipped++;
        return;
    }

    masa_gui_size(gui, &rows, &cols);
    CHECK(rows > 0);
    CHECK(cols > 0);

    /* A little output plus colour, then a frame. */
    masa_gui_feed(gui, "\x1b[31mhello\x1b[0m \x1b[1;34mworld\x1b[0m\r\n", 30);
    masa_gui_render(gui);

    /* Resize and redraw. */
    masa_gui_resize(gui, 800, 520);
    masa_gui_size(gui, &rows, &cols);
    CHECK(rows > 0);
    CHECK(cols > 0);
    masa_gui_render(gui);

    /* Screen text extraction must see the fed line. */
    {
        char text[4096];

        masa_gui_screen_text(gui, text, sizeof(text));
        CHECK_CONTAINS(text, "hello");
        CHECK_CONTAINS(text, "world");
    }

    masa_gui_shutdown(gui);
}

static void test_shell_argv_env(void)
{
    char **argv;
    char **env;
    int found_term = 0;
    int i;

    printf("[ RUN  ] test_shell_argv_env\n");

    argv = masa_gui_shell_argv();
    CHECK(argv != NULL);
    if (argv != NULL) {
        CHECK(argv[0] != NULL);
        CHECK(argv[1] != NULL);
        CHECK(strcmp(argv[1], "shell") == 0);
        CHECK(argv[2] == NULL);
        /* Freeing must not touch any string literal. */
        masa_gui_free_argv(argv);
    }

    env = masa_gui_shell_env();
    CHECK(env != NULL);
    if (env != NULL) {
        for (i = 0; env[i] != NULL; i++) {
            if (strcmp(env[i], "TERM=xterm-256color") == 0) {
                found_term = 1;
            }
        }
        CHECK(found_term);
        masa_gui_free_argv(env);
    }
}

/* Drive the real entry point headless: it must spawn `masa shell` on a
 * PTY, render a few frames and tear down cleanly (this path frees the
 * argv/env arrays, so it guards against the literal-free bug). */
static void test_gui_main_headless(void)
{
    char *masa = find_masa();
    char *argv[5];
    int rc;

    printf("[ RUN  ] test_gui_main_headless\n");

    if (masa == NULL) {
        printf("  SKIP masa executable not found\n");
        g_skipped++;
        return;
    }

    setenv("MASA_BIN", masa, 1);
    setenv("SDL_VIDEODRIVER", "dummy", 1);

    argv[0] = (char *)"masa-term";
    argv[1] = (char *)"--headless";
    argv[2] = (char *)"--frames";
    argv[3] = (char *)"3";
    argv[4] = NULL;

    rc = masa_gui_main(4, argv);
    CHECK(rc == 0);

    free(masa);
}

static void test_gui_tabs(void)
{
    struct masa_gui *gui;
    char err[256];

    printf("[ RUN  ] test_gui_tabs\n");

    setenv("SDL_VIDEODRIVER", "dummy", 1);
    err[0] = '\0';
    gui = masa_gui_init("masa", 640, 400, err, sizeof(err));
    if (gui == NULL) {
        printf("  SKIP SDL init failed: %s\n", err[0] ? err : "?");
        g_skipped++;
        return;
    }

    /* init() opens exactly one tab. */
    CHECK(masa_gui_tab_count(gui) == 1);
    CHECK(masa_gui_tab_active(gui) == 0);
    CHECK(strcmp(masa_gui_tab_title(gui, 0), "") == 0);

    /* Each new tab becomes active and gets its own screen. */
    CHECK(masa_gui_tab_new(gui) == 1);
    CHECK(masa_gui_tab_count(gui) == 2);
    CHECK(masa_gui_tab_active(gui) == 1);
    CHECK(masa_gui_tab_new(gui) == 2);
    CHECK(masa_gui_tab_count(gui) == 3);

    /* Feed each tab distinct text and confirm the screens are independent. */
    {
        char text[2048];

        masa_gui_tab_select(gui, 0);
        masa_gui_tab_feed(gui, 0, "tab zero\r\n", 10);
        masa_gui_tab_feed(gui, 1, "tab one\r\n", 9);
        masa_gui_tab_select(gui, 0);
        masa_gui_screen_text(gui, text, sizeof(text));
        CHECK_CONTAINS(text, "tab zero");
        masa_gui_tab_select(gui, 1);
        masa_gui_screen_text(gui, text, sizeof(text));
        CHECK_CONTAINS(text, "tab one");
        CHECK(strstr(text, "tab zero") == NULL);
    }

    /* Selecting a bad index is refused. */
    CHECK(masa_gui_tab_select(gui, 9) == -1);

    /* Closing reindexes and keeps the active tab valid. */
    CHECK(masa_gui_tab_close(gui, 0) == 2);
    CHECK(masa_gui_tab_count(gui) == 2);
    CHECK(masa_gui_tab_active(gui) >= 0);

    /* Hit testing maps pixels to tabs. */
    CHECK(masa_gui_tab_at(gui, 10, 30 + 13) == 0);
    CHECK(masa_gui_tab_at(gui, 150 + 10, 30 + 13) == 1);
    CHECK(masa_gui_tab_at(gui, 10, 2) == -1);      /* header, not tabs */
    CHECK(masa_gui_tab_at(gui, 900, 30 + 13) == -1); /* past the last tab */

    masa_gui_shutdown(gui);
}

static void test_gui_scrollback(void)
{
    struct masa_gui *gui;
    char err[256];
    char line[512];
    int i;
    int count;

    printf("[ RUN  ] test_gui_scrollback\n");

    setenv("SDL_VIDEODRIVER", "dummy", 1);
    err[0] = '\0';
    gui = masa_gui_init("masa", 640, 400, err, sizeof(err));
    if (gui == NULL) {
        printf("  SKIP SDL init failed: %s\n", err[0] ? err : "?");
        g_skipped++;
        return;
    }

    CHECK(masa_gui_scrollback_count(gui) == 0);

    /* Push far more lines than the screen holds so history accumulates. */
    for (i = 0; i < 200; i++) {
        char buf[64];

        snprintf(buf, sizeof(buf), "history line %d\r\n", i);
        masa_gui_feed(gui, buf, strlen(buf));
    }

    count = masa_gui_scrollback_count(gui);
    CHECK(count > 0);

    /*
     * Lines still on the live screen have not scrolled off yet, so the
     * oldest retained line is the very first one fed and the newest
     * retained line is (fed - rows).  Index 0 is always the oldest.
     */
    if (count > 0) {
        CHECK(masa_gui_scrollback_line(gui, 0, line, sizeof(line)) > 0);
        CHECK_CONTAINS(line, "history line 0");
        CHECK(masa_gui_scrollback_line(gui, count - 1, line,
                                       sizeof(line)) > 0);
        CHECK_CONTAINS(line, "history line ");
    }

    /* Scrolling clamps to the retained range and reset returns to live. */
    CHECK(masa_gui_scroll(gui, 5) == 5);
    CHECK(masa_gui_scroll_offset(gui) == 5);
    CHECK(masa_gui_scroll(gui, -100) == 0);
    masa_gui_scroll(gui, 10);
    masa_gui_scroll_reset(gui);
    CHECK(masa_gui_scroll_offset(gui) == 0);

    /* The bounded ring never exceeds the cap. */
    CHECK(count <= MASA_GUI_SCROLLBACK);

    masa_gui_shutdown(gui);
}

static void test_gui_clipboard(void)
{
    struct masa_gui *gui;
    char err[256];
    char out[256];
    char sel[512];
    size_t n;

    printf("[ RUN  ] test_gui_clipboard\n");

    setenv("SDL_VIDEODRIVER", "dummy", 1);
    err[0] = '\0';
    gui = masa_gui_init("masa", 640, 400, err, sizeof(err));
    if (gui == NULL) {
        printf("  SKIP SDL init failed: %s\n", err[0] ? err : "?");
        g_skipped++;
        return;
    }

    masa_gui_feed(gui, "copy this text\r\n", 16);

    /* No selection yet -> nothing to copy. */
    CHECK(masa_gui_copy_selection(gui, sel, sizeof(sel)) == 0);

    /* Select the first row and copy it. */
    masa_gui_set_selection(gui, 0, 0, 0, 13);
    n = masa_gui_copy_selection(gui, sel, sizeof(sel));
    CHECK(n > 0);
    CHECK_CONTAINS(sel, "copy this text");

    masa_gui_clear_selection(gui);
    CHECK(masa_gui_copy_selection(gui, sel, sizeof(sel)) == 0);

    /* Paste translates text (and a newline) into child input. */
    n = masa_gui_paste(gui, "pasted\r\n", out, sizeof(out));
    CHECK(n > 0);
    CHECK_CONTAINS(out, "pasted");
    CHECK(strchr(out, '\r') != NULL);

    masa_gui_shutdown(gui);
}

/* ------------------------------------------------------------------ */
/* Danroot AppKit/CoreGraphics SDL2 backend                            */
/* ------------------------------------------------------------------ */

/* The shim provider entry points live in the loader's private header. */
#include "../src/danroot_internal.h"

static int have_clang(void)
{
    const char *path = getenv("PATH");
    char *copy;
    char *dir;
    char cand[4096];

    if (path == NULL) {
        return 0;
    }
    copy = strdup(path);
    if (copy == NULL) {
        return 0;
    }
    for (dir = strtok(copy, ":"); dir != NULL; dir = strtok(NULL, ":")) {
        snprintf(cand, sizeof(cand), "%s/clang", dir);
        if (access(cand, X_OK) == 0) {
            free(copy);
            return 1;
        }
    }
    free(copy);
    return 0;
}

static int write_text(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");

    if (fp == NULL) {
        return -1;
    }
    fputs(text, fp);
    fclose(fp);
    return 0;
}

/* The clean-room AppKit surface must be present in this build, and the
 * SDL2-backed CoreGraphics/AppKit symbols must resolve. */
static void test_appkit_symbols(void)
{
    printf("[ RUN  ] test_appkit_symbols\n");

    CHECK(dr_gui_available() == 1);
    CHECK(dr_gui_class("NSApplication") != NULL);
    CHECK(dr_gui_class("NSWindow") != NULL);
    CHECK(dr_gui_class("NSView") != NULL);
    CHECK(dr_gui_class("NotAClass") == NULL);

    CHECK(dr_gui_lookup("_CGContextFillRect") != NULL);
    CHECK(dr_gui_lookup("_CGContextSetRGBFillColor") != NULL);
    CHECK(dr_gui_lookup("_CGRectMake") != NULL);
    CHECK(dr_gui_lookup("_NSMakeRect") != NULL);
    CHECK(dr_gui_lookup("_NSApplication") != NULL);
    CHECK(dr_gui_lookup("_definitely_not_a_symbol") == NULL);

    CHECK(dr_gui_is_gui_symbol("_CGContextFillRect") == 1);
    CHECK(dr_gui_is_gui_symbol("_NSWindow") == 1);
    CHECK(dr_gui_is_gui_symbol("_malloc") == 0);
}

/*
 * End to end: build a Darwin GUI program that opens an NSWindow, fills it
 * with CoreGraphics and runs the AppKit event loop, load it through the
 * Danroot loader with the SDL2 backend, and verify that the frame the run
 * loop presented actually contains the colour the program painted.
 */
static void test_appkit_run_paints(void)
{
    char dir[] = "/tmp/masa_appkit_XXXXXX";
    char sdk[4096];
    char src[4096];
    char exe[4096];
    char *sdk_argv[2];
    char *cc_argv[8];
    char *run_argv[2];
    unsigned char px[4];
    static const char *source =
        "#include <CoreGraphics/CoreGraphics.h>\n"
        "#include <AppKit/AppKit.h>\n"
        "#include <objc/objc.h>\n"
        "#include <objc/runtime.h>\n"
        "#include <objc/message.h>\n"
        "int main(void) {\n"
        "    id cls = (id)objc_getClass(\"NSWindow\");\n"
        "    id win = objc_msgSend(\n"
        "        objc_msgSend(cls, sel_registerName(\"alloc\")),\n"
        "        sel_registerName(\"initWithContentRect:styleMask:backing:"
        "defer:\"),\n"
        "        (double)0, (double)0, (double)120, (double)120,\n"
        "        (unsigned long)1, (unsigned long)2, (unsigned long)0);\n"
        "    id ctx = objc_msgSend(win, sel_registerName(\"CGContext\"));\n"
        "    CGContextSetRGBFillColor((CGContextRef)ctx, 0.0, 0.0, 1.0,"
        " 1.0);\n"
        "    CGContextFillRect((CGContextRef)ctx, CGRectMake(0, 0, 120,"
        " 120));\n"
        "    id app = objc_msgSend((id)objc_getClass(\"NSApplication\"),\n"
        "                          sel_registerName(\"sharedApplication\"));\n"
        "    objc_msgSend(app, sel_registerName(\"run\"));\n"
        "    return 0;\n"
        "}\n";
    int rc;

    printf("[ RUN  ] test_appkit_run_paints\n");

    if (!have_clang()) {
        printf("  SKIP needs clang to build the GUI program\n");
        g_skipped++;
        return;
    }
    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "  FAIL cannot create a temporary directory\n");
        g_failed++;
        return;
    }

    snprintf(sdk, sizeof(sdk), "%s/sdk", dir);
    snprintf(src, sizeof(src), "%s/app.c", dir);
    snprintf(exe, sizeof(exe), "%s/app", dir);

    sdk_argv[0] = (char *)"init-stub";
    sdk_argv[1] = sdk;
    CHECK(masa_sdk_main(2, sdk_argv) == 0);

    CHECK(write_text(src, source) == 0);

    setenv("MASA_SDK_PATH", sdk, 1);
    setenv("MASA_TARGET", "x86_64-apple-darwin", 1);
    setenv("MASA_GUI_FRAMES", "1", 1);
    setenv("SDL_VIDEODRIVER", "dummy", 1);

    cc_argv[0] = (char *)"-framework";
    cc_argv[1] = (char *)"Cocoa";
    cc_argv[2] = src;
    cc_argv[3] = (char *)"-o";
    cc_argv[4] = exe;
    cc_argv[5] = NULL;
    rc = masa_cc_main(5, cc_argv);
    if (rc != 0) {
        printf("  SKIP cc could not build the GUI program\n");
        g_skipped++;
        return;
    }

    run_argv[0] = exe;
    run_argv[1] = NULL;
    rc = masa_danroot_main(1, run_argv);
    CHECK(rc == 0);

    /* The centre pixel is the blue rectangle the program filled. */
    if (masa_gui_read_pixel(px)) {
        CHECK(px[0] == 0x00); /* R */
        CHECK(px[1] == 0x00); /* G */
        CHECK(px[2] == 0xFF); /* B */
        CHECK(px[3] == 0xFF); /* A */
    } else {
        printf("  FAIL no frame was captured\n");
        g_failed++;
        g_run++;
    }
}

int main(void)
{
    printf("masa-term GUI tests\n");
    test_pty_shell_through_vterm();
    test_pty_resize();
    test_shell_argv_env();
    test_headless_sdl_render();
    test_gui_tabs();
    test_gui_scrollback();
    test_gui_clipboard();
    test_gui_main_headless();
    test_appkit_symbols();
    test_appkit_run_paints();

    printf("\n%d tests run, %d failed, %d skipped\n",
           g_run, g_failed, g_skipped);
    return (g_failed == 0) ? 0 : 1;
}
