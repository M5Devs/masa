/*
 * gui_term.c - masa-term, the SDL2 terminal emulator window.
 *
 * A standalone GUI front end for `masa shell`: PTY bytes are fed into a
 * libvterm screen and drawn as a sleek dark macOS-style window with the
 * classic red/yellow/green traffic lights, full 256/RGB colour and a
 * blinking block cursor.
 *
 * The window holds up to MASA_GUI_MAX_TABS tabs, each owning its own
 * pseudo-terminal and libvterm screen.  History scrolled off the top is
 * retained per tab in a bounded scrollback ring and can be browsed with the
 * mouse wheel or Shift-PageUp/PageDown; the mouse can also select text to
 * copy, and Ctrl-Shift-V pastes the system clipboard into the child.  This
 * file is compiled only by `make gui`, so the core command line tool keeps
 * building without SDL or libvterm.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <SDL_ttf.h>
#include <vterm.h>

#include "masa.h"
#include "masa_compat.h"
#include "masa_gui.h"

/* ------------------------------------------------------------------ */
/* Look and feel                                                       */
/* ------------------------------------------------------------------ */

#define MASA_HEADER_H      30
#define MASA_TABBAR_H      26
#define MASA_FONT_POINT    16
#define MASA_TITLE_POINT   13
#define MASA_TAB_POINT     12
#define MASA_PAD_X         10
#define MASA_PAD_Y         8
#define MASA_TITLE         "masa \xe2\x80\x94 Darwin" /* masa — Darwin */

/* Default dark palette. */
#define MASA_BG_R 30
#define MASA_BG_G 30
#define MASA_BG_B 34
#define MASA_FG_R 220
#define MASA_FG_G 220
#define MASA_FG_B 214

/* Glyph cache: bounded, cleared wholesale when it fills up. */
#define GLYPH_CAP 2048
#define GLYPH_STYLE_BOLD   1
#define GLYPH_STYLE_ITALIC 2

/* Scrollback ring: MASA_GUI_SCROLLBACK lines per tab, as UTF-32 text so a
 * history line survives independent of the screen's own cell buffer. */
struct sb_line {
    uint32_t *cp;
    int len;
};

/* A cached glyph texture. */
struct glyph {
    uint32_t cp;
    Uint32 fg;
    uint8_t style;
    int top; /* offset from the cell top to the surface top */
    int w, h;
    SDL_Texture *tex;
};

struct masa_tab {
    VTerm *vt;
    VTermScreen *screen;

    struct masa_pty pty;
    int pty_open;

    struct sb_line *sb;
    int sb_cap;
    int sb_len;   /* retained lines */
    int sb_head;  /* next write slot in the ring */
    int sb_held;  /* line sb_popline() handed back, to be returned on the
                     next push */

    char title[128];

    int cursor_visible;
    int cursor_blink;
    int cursor_row, cursor_col;
};

struct masa_gui {
    SDL_Window *window;
    SDL_Renderer *renderer;
    TTF_Font *font;
    TTF_Font *title_font;
    TTF_Font *tab_font;

    int win_w, win_h;
    int rows, cols;
    int cell_w, cell_h;
    int ascent;

    struct masa_tab tabs[MASA_GUI_MAX_TABS];
    int tab_count;
    int active;

    int scroll_offset; /* active tab, 0 = live */
    int sb_total;      /* active tab's retained line count */

    /* Mouse selection in active-tab screen coordinates. */
    int sel_active;
    int sel_r0, sel_c0, sel_r1, sel_c1;
    int selecting;

    struct glyph slots[GLYPH_CAP];
    int glyph_count;

    int have_sdl;
    int have_ttf;
    int have_window;
    int have_renderer;
};

/* xterm 256-colour palette, built once. */
static SDL_Color g_palette[256];
static int g_palette_ready;

static void build_palette(void)
{
    static const unsigned char base[16][3] = {
        {0, 0, 0},       {205, 0, 0},     {0, 205, 0},     {205, 205, 0},
        {0, 0, 238},     {205, 0, 205},   {0, 205, 205},   {229, 229, 229},
        {127, 127, 127}, {255, 0, 0},     {0, 255, 0},     {255, 255, 0},
        {92, 92, 255},   {255, 0, 255},   {0, 255, 255},   {255, 255, 255},
    };
    static const unsigned char levels[6] = {0, 95, 135, 175, 215, 255};
    int i;

    for (i = 0; i < 16; i++) {
        g_palette[i].r = base[i][0];
        g_palette[i].g = base[i][1];
        g_palette[i].b = base[i][2];
        g_palette[i].a = 255;
    }
    for (i = 0; i < 216; i++) {
        g_palette[16 + i].r = levels[(i / 36) % 6];
        g_palette[16 + i].g = levels[(i / 6) % 6];
        g_palette[16 + i].b = levels[i % 6];
        g_palette[16 + i].a = 255;
    }
    for (i = 0; i < 24; i++) {
        unsigned char v = (unsigned char)(8 + i * 10);
        g_palette[232 + i].r = v;
        g_palette[232 + i].g = v;
        g_palette[232 + i].b = v;
        g_palette[232 + i].a = 255;
    }
    g_palette_ready = 1;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void set_err(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;

    if (err == NULL || cap == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static int utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Decode one UTF-8 codepoint; returns bytes consumed (>=1). */
static int utf8_decode(const char *s, uint32_t *cp)
{
    unsigned char c = (unsigned char)s[0];

    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if ((c & 0xE0) == 0xC0 && s[1] != '\0') {
        *cp = ((uint32_t)(c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && s[1] != '\0' && s[2] != '\0') {
        *cp = ((uint32_t)(c & 0x0F) << 12) |
              (((unsigned char)s[1] & 0x3F) << 6) |
              ((unsigned char)s[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && s[1] != '\0' && s[2] != '\0' && s[3] != '\0') {
        *cp = ((uint32_t)(c & 0x07) << 18) |
              (((unsigned char)s[1] & 0x3F) << 12) |
              (((unsigned char)s[2] & 0x3F) << 6) |
              ((unsigned char)s[3] & 0x3F);
        return 4;
    }
    *cp = '?';
    return 1;
}

/* ------------------------------------------------------------------ */
/* Font discovery                                                      */
/* ------------------------------------------------------------------ */

static const char *const g_font_candidates[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/liberation2/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
    "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
    "/Library/Fonts/Menlo.ttc",
    "/System/Library/Fonts/Menlo.ttc",
    "/System/Library/Fonts/Supplemental/Menlo.ttc",
    NULL,
};

static TTF_Font *open_mono_font(int point, char *err, size_t errcap)
{
    const char *env = getenv("MASA_FONT");
    size_t i;

    if (env != NULL && env[0] != '\0') {
        TTF_Font *f = TTF_OpenFont(env, point);

        if (f != NULL) {
            return f;
        }
        set_err(err, errcap, "MASA_FONT=%s could not be opened: %s", env,
                TTF_GetError());
    }

    for (i = 0; g_font_candidates[i] != NULL; i++) {
        TTF_Font *f = TTF_OpenFont(g_font_candidates[i], point);

        if (f != NULL) {
            return f;
        }
    }

    set_err(err, errcap, "no monospace font found (set MASA_FONT)");
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Glyph cache                                                         */
/* ------------------------------------------------------------------ */

static void glyph_cache_clear(struct masa_gui *gui)
{
    int i;

    for (i = 0; i < GLYPH_CAP; i++) {
        if (gui->slots[i].tex != NULL) {
            SDL_DestroyTexture(gui->slots[i].tex);
        }
        memset(&gui->slots[i], 0, sizeof(gui->slots[i]));
    }
    gui->glyph_count = 0;
}

static unsigned glyph_hash(uint32_t cp, Uint32 fg, uint8_t style)
{
    unsigned h = cp * 2654435761u;

    h ^= fg * 40503u;
    h ^= (unsigned)style * 97u;
    return h;
}

/*
 * Return a texture for cp drawn in fg with the given style, creating and
 * caching it on first use.  Returns NULL when the glyph cannot be drawn
 * (spaces and unsupported codepoints simply have no texture).
 */
static struct glyph *glyph_get(struct masa_gui *gui, uint32_t cp, Uint32 fg,
                               uint8_t style)
{
    unsigned idx;
    unsigned probe;
    char utf8[8];
    int len;
    SDL_Color color;
    SDL_Surface *surf;
    struct glyph *slot;

    if (cp == 0 || cp == ' ') {
        return NULL;
    }

    if (gui->glyph_count * 4 >= GLYPH_CAP * 3) {
        glyph_cache_clear(gui);
    }

    idx = glyph_hash(cp, fg, style) % GLYPH_CAP;
    for (probe = 0; probe < GLYPH_CAP; probe++) {
        slot = &gui->slots[(idx + probe) % GLYPH_CAP];
        if (slot->tex == NULL && slot->cp == 0) {
            break;
        }
        if (slot->cp == cp && slot->fg == fg && slot->style == style) {
            return slot;
        }
    }
    if (probe == GLYPH_CAP) {
        return NULL;
    }

    len = utf8_encode(cp, utf8);
    utf8[len] = '\0';

    TTF_SetFontStyle(gui->font,
                     ((style & GLYPH_STYLE_BOLD) ? TTF_STYLE_BOLD : 0) |
                     ((style & GLYPH_STYLE_ITALIC) ? TTF_STYLE_ITALIC : 0));

    color.r = (Uint8)((fg >> 16) & 0xFF);
    color.g = (Uint8)((fg >> 8) & 0xFF);
    color.b = (Uint8)(fg & 0xFF);
    color.a = 255;

    surf = TTF_RenderUTF8_Blended(gui->font, utf8, color);
    TTF_SetFontStyle(gui->font, TTF_STYLE_NORMAL);
    if (surf == NULL) {
        return NULL;
    }

    slot->cp = cp;
    slot->fg = fg;
    slot->style = style;
    slot->w = surf->w;
    slot->h = surf->h;
    slot->top = gui->ascent - surf->h;
    if (slot->top < 0) {
        slot->top = 0;
    }
    slot->tex = SDL_CreateTextureFromSurface(gui->renderer, surf);
    SDL_FreeSurface(surf);
    if (slot->tex == NULL) {
        memset(slot, 0, sizeof(*slot));
        return NULL;
    }
    SDL_SetTextureBlendMode(slot->tex, SDL_BLENDMODE_BLEND);
    gui->glyph_count++;
    return slot;
}

/* ------------------------------------------------------------------ */
/* Cell colour resolution                                              */
/* ------------------------------------------------------------------ */

static SDL_Color indexed_color(int idx, Uint32 *packed)
{
    SDL_Color c;

    if (idx < 0 || idx > 255) {
        idx = 7;
    }
    c = g_palette[idx];
    if (packed != NULL) {
        *packed = ((Uint32)c.r << 16) | ((Uint32)c.g << 8) | (Uint32)c.b;
    }
    return c;
}

static void resolve_colors(const VTermScreenCell *cell, SDL_Color *fg,
                           SDL_Color *bg, Uint32 *fg_packed)
{
    int bold = cell->attrs.bold;

    if (VTERM_COLOR_IS_DEFAULT_FG(&cell->fg)) {
        fg->r = MASA_FG_R;
        fg->g = MASA_FG_G;
        fg->b = MASA_FG_B;
    } else if (VTERM_COLOR_IS_INDEXED(&cell->fg)) {
        int idx = cell->fg.indexed.idx;

        if (bold && idx < 8) {
            idx += 8;
        }
        *fg = indexed_color(idx, NULL);
    } else {
        fg->r = cell->fg.rgb.red;
        fg->g = cell->fg.rgb.green;
        fg->b = cell->fg.rgb.blue;
    }
    fg->a = 255;

    if (VTERM_COLOR_IS_DEFAULT_BG(&cell->bg)) {
        bg->r = MASA_BG_R;
        bg->g = MASA_BG_G;
        bg->b = MASA_BG_B;
    } else if (VTERM_COLOR_IS_INDEXED(&cell->bg)) {
        *bg = indexed_color(cell->bg.indexed.idx, NULL);
    } else {
        bg->r = cell->bg.rgb.red;
        bg->g = cell->bg.rgb.green;
        bg->b = cell->bg.rgb.blue;
    }
    bg->a = 255;

    if (cell->attrs.reverse) {
        SDL_Color t = *fg;

        *fg = *bg;
        *bg = t;
    }

    if (fg_packed != NULL) {
        *fg_packed = ((Uint32)fg->r << 16) | ((Uint32)fg->g << 8) | (Uint32)fg->b;
    }
}

/* ------------------------------------------------------------------ */
/* libvterm plumbing (per tab)                                         */
/* ------------------------------------------------------------------ */

/*
 * The screen callbacks are registered with the tab's own libvterm screen,
 * which passes the owning struct masa_tab back as `user`.
 */

static int cb_settermprop(VTermProp prop, VTermValue *val, void *user)
{
    struct masa_tab *tab = user;

    if (prop == VTERM_PROP_CURSORVISIBLE) {
        tab->cursor_visible = val->boolean;
    } else if (prop == VTERM_PROP_CURSORBLINK) {
        tab->cursor_blink = val->boolean;
    } else if (prop == VTERM_PROP_TITLE) {
        size_t n = val->string.len;

        if (n >= sizeof(tab->title)) {
            n = sizeof(tab->title) - 1;
        }
        memcpy(tab->title, val->string.str, n);
        tab->title[n] = '\0';
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Scrollback ring                                                     */
/* ------------------------------------------------------------------ */

static void sb_line_free(struct sb_line *line)
{
    free(line->cp);
    line->cp = NULL;
    line->len = 0;
}

static void sb_free_all(struct masa_tab *tab)
{
    int i;

    for (i = 0; i < tab->sb_cap; i++) {
        sb_line_free(&tab->sb[i]);
    }
    free(tab->sb);
    tab->sb = NULL;
    tab->sb_cap = 0;
    tab->sb_len = 0;
    tab->sb_head = 0;
    tab->sb_held = 0;
}

/*
 * Copy one libvterm row (cols cells) into a fresh UTF-32 buffer, trimming
 * trailing blanks.  Returns a malloc'd buffer (len written into *out_len),
 * or NULL when the row is entirely blank.
 */
static uint32_t *sb_row_copy(int cols, const VTermScreenCell *cells, int *out_len)
{
    uint32_t *cp;
    int last = -1;
    int i;
    int len = 0;

    for (i = 0; i < cols; i++) {
        if (cells[i].chars[0] != 0 && cells[i].chars[0] != ' ') {
            last = i;
        }
    }
    *out_len = 0;
    if (last < 0) {
        return NULL;
    }
    cp = malloc(((size_t)last + 1) * sizeof(uint32_t));
    if (cp == NULL) {
        return NULL;
    }
    for (i = 0; i <= last; i++) {
        cp[len++] = (cells[i].chars[0] != 0) ? cells[i].chars[0] : ' ';
    }
    *out_len = len;
    return cp;
}

static int cb_sb_pushline(int cols, const VTermScreenCell *cells, void *user)
{
    struct masa_tab *tab = user;
    uint32_t *cp;
    int len = 0;

    if (tab->sb_cap == 0) {
        return 0; /* scrollback disabled */
    }

    /*
     * sb_popline() handed a line back for reuse; reclaim it now, before the
     * new line takes its slot, so a pop/push cycle does not leak.
     */
    if (tab->sb_held) {
        sb_line_free(&tab->sb[tab->sb_head]);
        tab->sb_held = 0;
    }

    cp = sb_row_copy(cols, cells, &len);
    if (cp == NULL) {
        return 0; /* a blank line still counts as scrolled-off */
    }

    sb_line_free(&tab->sb[tab->sb_head]);
    tab->sb[tab->sb_head].cp = cp;
    tab->sb[tab->sb_head].len = len;
    tab->sb_head = (tab->sb_head + 1) % tab->sb_cap;
    if (tab->sb_len < tab->sb_cap) {
        tab->sb_len++;
    }
    return 1;
}

static int cb_sb_popline(int cols, VTermScreenCell *cells, void *user)
{
    struct masa_tab *tab = user;
    struct sb_line *line;
    int idx;

    if (tab->sb_len == 0) {
        return 0;
    }
    idx = (tab->sb_head - tab->sb_len + tab->sb_cap) % tab->sb_cap;
    line = &tab->sb[idx];
    if (line->cp == NULL) {
        tab->sb_len--;
        return 0;
    }

    {
        int i;

        for (i = 0; i < cols; i++) {
            memset(&cells[i], 0, sizeof(cells[i]));
            cells[i].chars[0] = (i < line->len) ? line->cp[i] : ' ';
            cells[i].width = 1;
        }
    }

    /*
     * The line is now on the screen again; remember that its slot must be
     * freed when the next line is pushed, and shrink the ring.
     */
    tab->sb_held = 1;
    tab->sb_len--;
    if (tab->sb_len == 0) {
        tab->sb_head = 0;
        tab->sb_held = 0;
        sb_line_free(line);
    }
    return 1;
}

static int cb_sb_clear(void *user)
{
    struct masa_tab *tab = user;
    int i;

    for (i = 0; i < tab->sb_cap; i++) {
        sb_line_free(&tab->sb[i]);
    }
    tab->sb_len = 0;
    tab->sb_head = 0;
    tab->sb_held = 0;
    return 1;
}

static void install_callbacks(struct masa_tab *tab)
{
    static VTermScreenCallbacks cbs;

    memset(&cbs, 0, sizeof(cbs));
    cbs.settermprop = cb_settermprop;
    cbs.sb_pushline = cb_sb_pushline;
    cbs.sb_popline = cb_sb_popline;
    cbs.sb_clear = cb_sb_clear;
    vterm_screen_set_callbacks(tab->screen, &cbs, tab);
}

/* ------------------------------------------------------------------ */
/* Tab lifecycle                                                       */
/* ------------------------------------------------------------------ */

static void tab_reset_view(struct masa_gui *gui)
{
    gui->scroll_offset = 0;
    gui->sb_total = 0;
    gui->sel_active = 0;
    gui->selecting = 0;
}

/*
 * Allocate a tab's scrollback ring.  Called once per tab; the ring itself
 * is bounded by MASA_GUI_SCROLLBACK so a chatty child cannot grow it
 * without limit.
 */
static int tab_init_scrollback(struct masa_tab *tab)
{
    tab->sb_cap = MASA_GUI_SCROLLBACK;
    tab->sb = calloc((size_t)tab->sb_cap, sizeof(struct sb_line));
    if (tab->sb == NULL) {
        tab->sb_cap = 0;
        return -1;
    }
    tab->sb_len = 0;
    tab->sb_head = 0;
    tab->sb_held = 0;
    return 0;
}

/*
 * Create a fresh libvterm screen for the tab and size it to the window's
 * grid.  Returns 0 on success.
 */
static int tab_create_vterm(struct masa_gui *gui, struct masa_tab *tab)
{
    tab->vt = vterm_new(gui->rows > 0 ? gui->rows : 24,
                        gui->cols > 0 ? gui->cols : 80);
    if (tab->vt == NULL) {
        return -1;
    }
    vterm_set_utf8(tab->vt, 1);
    tab->screen = vterm_obtain_screen(tab->vt);
    vterm_screen_reset(tab->screen, 1);
    install_callbacks(tab);
    if (tab_init_scrollback(tab) != 0) {
        vterm_free(tab->vt);
        tab->vt = NULL;
        tab->screen = NULL;
        return -1;
    }
    tab->cursor_visible = 1;
    tab->cursor_blink = 0;
    return 0;
}

/* Release one tab's resources: PTY, vterm and scrollback. */
static void tab_destroy(struct masa_tab *tab)
{
    if (tab->pty_open) {
        masa_pty_close(&tab->pty);
        tab->pty_open = 0;
    }
    if (tab->vt != NULL) {
        vterm_free(tab->vt);
        tab->vt = NULL;
        tab->screen = NULL;
    }
    sb_free_all(tab);
    tab->title[0] = '\0';
}

int masa_gui_tab_count(const struct masa_gui *gui)
{
    return (gui != NULL) ? gui->tab_count : 0;
}

int masa_gui_tab_active(const struct masa_gui *gui)
{
    return (gui != NULL) ? gui->active : -1;
}

int masa_gui_tab_new(struct masa_gui *gui)
{
    int idx;

    if (gui == NULL || gui->tab_count >= MASA_GUI_MAX_TABS) {
        return -1;
    }
    idx = gui->tab_count;
    memset(&gui->tabs[idx], 0, sizeof(gui->tabs[idx]));
    if (tab_create_vterm(gui, &gui->tabs[idx]) != 0) {
        return -1;
    }
    gui->tab_count++;
    gui->active = idx;
    tab_reset_view(gui);
    return idx;
}

int masa_gui_tab_select(struct masa_gui *gui, int index)
{
    if (gui == NULL || index < 0 || index >= gui->tab_count) {
        return -1;
    }
    gui->active = index;
    tab_reset_view(gui);
    return 0;
}

int masa_gui_tab_close(struct masa_gui *gui, int index)
{
    int i;

    if (gui == NULL || index < 0 || index >= gui->tab_count) {
        return -1;
    }
    tab_destroy(&gui->tabs[index]);

    for (i = index; i < gui->tab_count - 1; i++) {
        gui->tabs[i] = gui->tabs[i + 1];
    }
    gui->tab_count--;
    memset(&gui->tabs[gui->tab_count], 0, sizeof(gui->tabs[gui->tab_count]));

    if (gui->tab_count == 0) {
        gui->active = -1;
    } else if (gui->active >= gui->tab_count) {
        gui->active = gui->tab_count - 1;
    }
    tab_reset_view(gui);
    return gui->tab_count;
}

struct masa_pty *masa_gui_tab_pty(struct masa_gui *gui, int index)
{
    if (gui == NULL || index < 0 || index >= gui->tab_count) {
        return NULL;
    }
    return &gui->tabs[index].pty;
}

int masa_gui_tab_open_pty(struct masa_gui *gui, int index,
                          char *const argv[], char *const envp[])
{
    struct masa_tab *tab;

    if (gui == NULL || index < 0 || index >= gui->tab_count) {
        return -1;
    }
    tab = &gui->tabs[index];
    if (masa_pty_open(&tab->pty, gui->rows, gui->cols, argv, envp) != 0) {
        return -1;
    }
    tab->pty_open = 1;
    return 0;
}

const char *masa_gui_tab_title(const struct masa_gui *gui, int index)
{
    if (gui == NULL || index < 0 || index >= gui->tab_count) {
        return "";
    }
    return gui->tabs[index].title;
}

void masa_gui_tab_feed(struct masa_gui *gui, int index, const char *bytes,
                       size_t len)
{
    if (gui == NULL || index < 0 || index >= gui->tab_count ||
        gui->tabs[index].vt == NULL || bytes == NULL) {
        return;
    }
    vterm_input_write(gui->tabs[index].vt, bytes, len);
    if (index == gui->active) {
        gui->sb_total = gui->tabs[index].sb_len;
    }
}

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

static void measure_cells(struct masa_gui *gui)
{
    int w = 0;
    int h = 0;

    if (TTF_SizeText(gui->font, "M", &w, &h) != 0 || w <= 0) {
        w = gui->font != NULL ? TTF_FontHeight(gui->font) / 2 : 8;
    }
    if (w <= 0) {
        w = 8;
    }
    gui->cell_w = w;
    gui->cell_h = TTF_FontHeight(gui->font);
    if (gui->cell_h <= 0) {
        gui->cell_h = 16;
    }
    gui->ascent = TTF_FontAscent(gui->font);
}

/* The y offset where the terminal body starts, below the header + tab bar. */
static int body_top(void)
{
    return MASA_HEADER_H + MASA_TABBAR_H;
}

static void recompute_grid(struct masa_gui *gui)
{
    int avail_w = gui->win_w - 2 * MASA_PAD_X;
    int avail_h = gui->win_h - body_top() - 2 * MASA_PAD_Y;
    int i;

    gui->cols = avail_w / gui->cell_w;
    gui->rows = avail_h / gui->cell_h;
    if (gui->cols < 1) {
        gui->cols = 1;
    }
    if (gui->rows < 1) {
        gui->rows = 1;
    }

    for (i = 0; i < gui->tab_count; i++) {
        if (gui->tabs[i].vt != NULL) {
            vterm_set_size(gui->tabs[i].vt, gui->rows, gui->cols);
        }
        if (gui->tabs[i].pty_open) {
            masa_pty_set_size(&gui->tabs[i].pty, gui->rows, gui->cols);
        }
    }
}

void masa_gui_size(const struct masa_gui *gui, int *rows, int *cols)
{
    if (gui == NULL) {
        return;
    }
    if (rows != NULL) {
        *rows = gui->rows;
    }
    if (cols != NULL) {
        *cols = gui->cols;
    }
}

void masa_gui_resize(struct masa_gui *gui, int win_w, int win_h)
{
    if (gui == NULL) {
        return;
    }
    if (win_w > 0) {
        gui->win_w = win_w;
    }
    if (win_h > 0) {
        gui->win_h = win_h;
    }
    recompute_grid(gui);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

struct masa_gui *masa_gui_init(const char *title, int win_w, int win_h,
                               char *err, size_t errcap)
{
    struct masa_gui *gui;

    if (!g_palette_ready) {
        build_palette();
    }

    gui = calloc(1, sizeof(*gui));
    if (gui == NULL) {
        set_err(err, errcap, "out of memory");
        return NULL;
    }

    gui->win_w = (win_w > 0) ? win_w : 900;
    gui->win_h = (win_h > 0) ? win_h : 600;
    gui->active = -1;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        set_err(err, errcap, "SDL_Init failed: %s", SDL_GetError());
        free(gui);
        return NULL;
    }
    gui->have_sdl = 1;

    if (TTF_Init() != 0) {
        set_err(err, errcap, "TTF_Init failed: %s", TTF_GetError());
        masa_gui_shutdown(gui);
        return NULL;
    }
    gui->have_ttf = 1;

    gui->font = open_mono_font(MASA_FONT_POINT, err, errcap);
    if (gui->font == NULL) {
        masa_gui_shutdown(gui);
        return NULL;
    }
    gui->title_font = TTF_OpenFont(
        (getenv("MASA_FONT") != NULL && getenv("MASA_FONT")[0] != '\0')
            ? getenv("MASA_FONT")
            : g_font_candidates[0],
        MASA_TITLE_POINT);
    if (gui->title_font == NULL) {
        gui->title_font = gui->font; /* Fall back to the main font. */
    }
    gui->tab_font = TTF_OpenFont(
        (getenv("MASA_FONT") != NULL && getenv("MASA_FONT")[0] != '\0')
            ? getenv("MASA_FONT")
            : g_font_candidates[0],
        MASA_TAB_POINT);
    if (gui->tab_font == NULL) {
        gui->tab_font = gui->title_font;
    }

    measure_cells(gui);

    gui->window = SDL_CreateWindow((title != NULL) ? title : MASA_TITLE,
                                   SDL_WINDOWPOS_UNDEFINED,
                                   SDL_WINDOWPOS_UNDEFINED,
                                   gui->win_w, gui->win_h,
                                   SDL_WINDOW_RESIZABLE);
    if (gui->window == NULL) {
        set_err(err, errcap, "SDL_CreateWindow failed: %s", SDL_GetError());
        masa_gui_shutdown(gui);
        return NULL;
    }
    gui->have_window = 1;

    gui->renderer = SDL_CreateRenderer(gui->window, -1,
                                       SDL_RENDERER_ACCELERATED |
                                       SDL_RENDERER_PRESENTVSYNC);
    if (gui->renderer == NULL) {
        gui->renderer = SDL_CreateRenderer(gui->window, -1,
                                           SDL_RENDERER_SOFTWARE);
    }
    if (gui->renderer == NULL) {
        set_err(err, errcap, "SDL_CreateRenderer failed: %s", SDL_GetError());
        masa_gui_shutdown(gui);
        return NULL;
    }
    gui->have_renderer = 1;

    /* Route printable characters through SDL_TEXTINPUT so dead keys and
     * composed sequences arrive intact; keysym handling covers the rest. */
    SDL_StartTextInput();

    /* Work out the grid before creating the first tab, which sizes to it. */
    recompute_grid(gui);

    if (masa_gui_tab_new(gui) < 0) {
        set_err(err, errcap, "vterm_new failed");
        masa_gui_shutdown(gui);
        return NULL;
    }
    return gui;
}

void masa_gui_shutdown(struct masa_gui *gui)
{
    int i;

    if (gui == NULL) {
        return;
    }

    glyph_cache_clear(gui);

    for (i = 0; i < gui->tab_count; i++) {
        tab_destroy(&gui->tabs[i]);
    }
    gui->tab_count = 0;

    if (gui->tab_font != NULL && gui->tab_font != gui->font &&
        gui->tab_font != gui->title_font) {
        TTF_CloseFont(gui->tab_font);
    }
    gui->tab_font = NULL;
    if (gui->title_font != NULL && gui->title_font != gui->font) {
        TTF_CloseFont(gui->title_font);
    }
    gui->title_font = NULL;
    if (gui->font != NULL) {
        TTF_CloseFont(gui->font);
        gui->font = NULL;
    }
    if (gui->have_renderer && gui->renderer != NULL) {
        SDL_DestroyRenderer(gui->renderer);
        gui->renderer = NULL;
    }
    if (gui->have_window && gui->window != NULL) {
        SDL_DestroyWindow(gui->window);
        gui->window = NULL;
    }
    if (gui->have_ttf) {
        TTF_Quit();
        gui->have_ttf = 0;
    }
    if (gui->have_sdl) {
        SDL_Quit();
        gui->have_sdl = 0;
    }

    free(gui);
}

/* The active tab, or NULL when none is open. */
static struct masa_tab *active_tab(struct masa_gui *gui)
{
    if (gui == NULL || gui->active < 0 || gui->active >= gui->tab_count) {
        return NULL;
    }
    return &gui->tabs[gui->active];
}

void masa_gui_feed(struct masa_gui *gui, const char *bytes, size_t len)
{
    if (gui == NULL || gui->active < 0) {
        return;
    }
    masa_gui_tab_feed(gui, gui->active, bytes, len);
}

/* ------------------------------------------------------------------ */
/* Scrollback and clipboard API                                        */
/* ------------------------------------------------------------------ */

int masa_gui_scrollback_count(const struct masa_gui *gui)
{
    return (gui != NULL) ? gui->sb_total : 0;
}

size_t masa_gui_scrollback_line(const struct masa_gui *gui, int index,
                                char *out, size_t cap)
{
    const struct masa_tab *tab;
    struct sb_line *line;
    int pos;
    int i;
    size_t used = 0;

    if (gui == NULL || out == NULL || cap == 0 || gui->active < 0 ||
        index < 0 || index >= gui->sb_total) {
        return 0;
    }
    out[0] = '\0';
    tab = &gui->tabs[gui->active];
    pos = (tab->sb_head - tab->sb_len + index + tab->sb_cap) % tab->sb_cap;
    line = &tab->sb[pos];

    for (i = 0; i < line->len && used + 4 < cap; i++) {
        used += (size_t)utf8_encode(line->cp[i], out + used);
    }
    out[used] = '\0';
    return used;
}

int masa_gui_scroll(struct masa_gui *gui, int lines)
{
    if (gui == NULL) {
        return 0;
    }
    gui->scroll_offset += lines;
    if (gui->scroll_offset < 0) {
        gui->scroll_offset = 0;
    }
    if (gui->scroll_offset > gui->sb_total) {
        gui->scroll_offset = gui->sb_total;
    }
    return gui->scroll_offset;
}

int masa_gui_scroll_offset(const struct masa_gui *gui)
{
    return (gui != NULL) ? gui->scroll_offset : 0;
}

void masa_gui_scroll_reset(struct masa_gui *gui)
{
    if (gui != NULL) {
        gui->scroll_offset = 0;
    }
}

size_t masa_gui_paste(struct masa_gui *gui, const char *text, char *out,
                      size_t outcap)
{
    struct masa_tab *tab = active_tab(gui);
    const char *p;
    size_t used = 0;

    if (tab == NULL || tab->vt == NULL || text == NULL || out == NULL ||
        outcap == 0) {
        return 0;
    }
    out[0] = '\0';

    /*
     * Newlines are the interesting case: a terminal expects a bare CR for
     * Return, so map CR/LF to VTERM_KEY_ENTER and send everything else as
     * typed text.  This mirrors masa_gui_input_text().
     */
    for (p = text; *p != '\0' && used + 4 < outcap;) {
        uint32_t cp;
        int n = utf8_decode(p, &cp);

        p += n;
        if (cp == '\n' || cp == '\r') {
            vterm_keyboard_key(tab->vt, VTERM_KEY_ENTER, VTERM_MOD_NONE);
        } else if (cp >= 0x20) {
            vterm_keyboard_unichar(tab->vt, cp, VTERM_MOD_NONE);
        }
        used += vterm_output_read(tab->vt, out + used, outcap - used);
    }
    return used;
}

void masa_gui_set_selection(struct masa_gui *gui, int r0, int c0, int r1,
                            int c1)
{
    if (gui == NULL) {
        return;
    }
    gui->sel_r0 = r0;
    gui->sel_c0 = c0;
    gui->sel_r1 = r1;
    gui->sel_c1 = c1;
    gui->sel_active = 1;
}

void masa_gui_clear_selection(struct masa_gui *gui)
{
    if (gui != NULL) {
        gui->sel_active = 0;
    }
}

size_t masa_gui_copy_selection(const struct masa_gui *gui, char *out,
                               size_t cap)
{
    struct masa_tab *tab;
    int r0, c0, r1, c1;
    int row;
    size_t used = 0;

    if (gui == NULL || out == NULL || cap == 0 || !gui->sel_active ||
        gui->active < 0) {
        return 0;
    }
    out[0] = '\0';
    tab = &((struct masa_gui *)gui)->tabs[gui->active];
    if (tab->screen == NULL) {
        return 0;
    }

    r0 = gui->sel_r0;
    c0 = gui->sel_c0;
    r1 = gui->sel_r1;
    c1 = gui->sel_c1;
    if (r1 < r0 || (r1 == r0 && c1 < c0)) {
        int t;
        t = r0; r0 = r1; r1 = t;
        t = c0; c0 = c1; c1 = t;
    }

    for (row = r0; row <= r1 && row < gui->rows; row++) {
        int from = (row == r0) ? c0 : 0;
        int to = (row == r1) ? c1 : gui->cols - 1;
        int last = -1;
        int col;

        if (from < 0) {
            from = 0;
        }
        if (to >= gui->cols) {
            to = gui->cols - 1;
        }
        for (col = from; col <= to; col++) {
            VTermPos pos;
            VTermScreenCell cell;

            memset(&cell, 0, sizeof(cell));
            pos.row = row;
            pos.col = col;
            if (vterm_screen_get_cell(tab->screen, pos, &cell) &&
                cell.chars[0] != 0 && cell.chars[0] != ' ') {
                last = col;
            }
        }
        for (col = from; col <= last && used + 4 < cap; col++) {
            VTermPos pos;
            VTermScreenCell cell;

            memset(&cell, 0, sizeof(cell));
            pos.row = row;
            pos.col = col;
            if (vterm_screen_get_cell(tab->screen, pos, &cell) &&
                cell.width != 0) {
                used += (size_t)utf8_encode(
                    cell.chars[0] != 0 ? cell.chars[0] : ' ', out + used);
            }
        }
        if (row < r1 && used + 1 < cap) {
            out[used++] = '\n';
        }
    }
    out[(used < cap) ? used : cap - 1] = '\0';
    return used;
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static VTermKey map_special(SDL_Keycode sym)
{
    switch (sym) {
    case SDLK_RETURN:    return VTERM_KEY_ENTER;
    case SDLK_KP_ENTER:  return VTERM_KEY_ENTER;
    case SDLK_TAB:       return VTERM_KEY_TAB;
    case SDLK_BACKSPACE: return VTERM_KEY_BACKSPACE;
    case SDLK_ESCAPE:    return VTERM_KEY_ESCAPE;
    case SDLK_UP:        return VTERM_KEY_UP;
    case SDLK_DOWN:      return VTERM_KEY_DOWN;
    case SDLK_LEFT:      return VTERM_KEY_LEFT;
    case SDLK_RIGHT:     return VTERM_KEY_RIGHT;
    case SDLK_INSERT:    return VTERM_KEY_INS;
    case SDLK_DELETE:    return VTERM_KEY_DEL;
    case SDLK_HOME:      return VTERM_KEY_HOME;
    case SDLK_END:       return VTERM_KEY_END;
    case SDLK_PAGEUP:    return VTERM_KEY_PAGEUP;
    case SDLK_PAGEDOWN:  return VTERM_KEY_PAGEDOWN;
    default:
        if (sym >= SDLK_F1 && sym <= SDLK_F12) {
            return VTERM_KEY_FUNCTION((int)(sym - SDLK_F1) + 1);
        }
        return VTERM_KEY_NONE;
    }
}

static VTermModifier map_mods(Uint16 mod)
{
    VTermModifier m = VTERM_MOD_NONE;

    if (mod & KMOD_SHIFT) {
        m |= VTERM_MOD_SHIFT;
    }
    if (mod & KMOD_ALT) {
        m |= VTERM_MOD_ALT;
    }
    if (mod & KMOD_CTRL) {
        m |= VTERM_MOD_CTRL;
    }
    return m;
}

static size_t drain_output(struct masa_gui *gui, char *out, size_t outcap)
{
    struct masa_tab *tab = active_tab(gui);

    if (tab == NULL || tab->vt == NULL) {
        return 0;
    }
    return vterm_output_read(tab->vt, out, outcap);
}

size_t masa_gui_input(struct masa_gui *gui, const SDL_Keycode key,
                      uint16_t mod, char *out, size_t outcap)
{
    struct masa_tab *tab = active_tab(gui);
    VTermKey special;
    VTermModifier vm;

    if (tab == NULL || tab->vt == NULL || out == NULL || outcap == 0) {
        return 0;
    }

    vm = map_mods(mod);
    special = map_special(key);

    if (special != VTERM_KEY_NONE) {
        vterm_keyboard_key(tab->vt, special, vm);
        return drain_output(gui, out, outcap);
    }

    if ((mod & KMOD_CTRL) && key >= SDLK_a && key <= SDLK_z) {
        vterm_keyboard_unichar(tab->vt, (uint32_t)key, VTERM_MOD_CTRL);
        return drain_output(gui, out, outcap);
    }

    /* Plain printable characters arrive through SDL_TEXTINPUT, which
     * masa_gui_input_text() handles; emitting them here would double
     * every keystroke. */
    return 0;
}

size_t masa_gui_input_text(struct masa_gui *gui, const char *utf8,
                           char *out, size_t outcap)
{
    struct masa_tab *tab = active_tab(gui);
    const char *p;

    if (tab == NULL || tab->vt == NULL || utf8 == NULL ||
        out == NULL || outcap == 0) {
        return 0;
    }

    for (p = utf8; *p != '\0';) {
        uint32_t cp;
        int n = utf8_decode(p, &cp);

        p += n;
        if (cp == '\n' || cp == '\r') {
            vterm_keyboard_key(tab->vt, VTERM_KEY_ENTER, VTERM_MOD_NONE);
        } else if (cp == 0x7f || cp == 0x08) {
            vterm_keyboard_key(tab->vt, VTERM_KEY_BACKSPACE, VTERM_MOD_NONE);
        } else if (cp >= 0x20) {
            vterm_keyboard_unichar(tab->vt, cp, VTERM_MOD_NONE);
        }
    }

    return drain_output(gui, out, outcap);
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static void draw_header(struct masa_gui *gui)
{
    SDL_Rect bar = {0, 0, gui->win_w, MASA_HEADER_H};
    int cy = MASA_HEADER_H / 2;
    int r = 6;
    int cx = 20;
    SDL_Color lights[3] = {
        {255, 95, 86, 255},
        {255, 189, 46, 255},
        {39, 201, 63, 255},
    };
    int i;
    SDL_Color sep = {48, 48, 52, 255};

    SDL_SetRenderDrawColor(gui->renderer, 45, 45, 48, 255);
    SDL_RenderFillRect(gui->renderer, &bar);

    for (i = 0; i < 3; i++) {
        SDL_Rect dot = {cx - r, cy - r, 2 * r, 2 * r};

        SDL_SetRenderDrawColor(gui->renderer, lights[i].r, lights[i].g,
                               lights[i].b, 255);
        SDL_RenderFillRect(gui->renderer, &dot);
        cx += 20;
    }

    /* Window title, centred. */
    {
        const char *title = MASA_TITLE;
        SDL_Color fg = {190, 190, 195, 255};
        SDL_Surface *surf = TTF_RenderUTF8_Blended(gui->title_font, title, fg);

        if (surf != NULL) {
            SDL_Texture *tex =
                SDL_CreateTextureFromSurface(gui->renderer, surf);
            SDL_Rect dst;

            if (tex != NULL) {
                dst.w = surf->w;
                dst.h = surf->h;
                dst.x = (gui->win_w - surf->w) / 2;
                dst.y = (MASA_HEADER_H - surf->h) / 2;
                SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                SDL_RenderCopy(gui->renderer, tex, NULL, &dst);
                SDL_DestroyTexture(tex);
            }
            SDL_FreeSurface(surf);
        }
    }

    SDL_SetRenderDrawColor(gui->renderer, sep.r, sep.g, sep.b, 255);
    SDL_RenderDrawLine(gui->renderer, 0, MASA_HEADER_H - 1, gui->win_w,
                       MASA_HEADER_H - 1);
}

/* One tab's pixel width in the tab bar. */
#define MASA_TAB_W 150

int masa_gui_tab_at(const struct masa_gui *gui, int x, int y)
{
    int idx;

    if (gui == NULL || y < MASA_HEADER_H || y >= MASA_HEADER_H + MASA_TABBAR_H) {
        return -1;
    }
    if (x < 0) {
        return -1;
    }
    idx = x / MASA_TAB_W;
    return (idx < gui->tab_count) ? idx : -1;
}

static void draw_tabbar(struct masa_gui *gui)
{
    SDL_Rect bar = {0, MASA_HEADER_H, gui->win_w, MASA_TABBAR_H};
    int i;

    SDL_SetRenderDrawColor(gui->renderer, 38, 38, 42, 255);
    SDL_RenderFillRect(gui->renderer, &bar);

    for (i = 0; i < gui->tab_count; i++) {
        SDL_Rect tab = {i * MASA_TAB_W, MASA_HEADER_H, MASA_TAB_W,
                        MASA_TABBAR_H};
        const char *label = masa_gui_tab_title(gui, i);
        char text[64];
        SDL_Color fg;
        SDL_Surface *surf;
        int tx;
        int ty;

        if (i == gui->active) {
            SDL_SetRenderDrawColor(gui->renderer, MASA_BG_R, MASA_BG_G,
                                   MASA_BG_B, 255);
        } else {
            SDL_SetRenderDrawColor(gui->renderer, 45, 45, 48, 255);
        }
        SDL_RenderFillRect(gui->renderer, &tab);

        /* Separator between tabs. */
        SDL_SetRenderDrawColor(gui->renderer, 25, 25, 28, 255);
        SDL_RenderDrawLine(gui->renderer, i * MASA_TAB_W, MASA_HEADER_H,
                           i * MASA_TAB_W, MASA_HEADER_H + MASA_TABBAR_H - 1);

        if (label[0] == '\0') {
            snprintf(text, sizeof(text), "shell %d", i + 1);
        } else {
            snprintf(text, sizeof(text), "%s", label);
        }
        if (strlen(text) > 20) {
            text[20] = '\0';
        }

        fg.r = (i == gui->active) ? 235 : 160;
        fg.g = (i == gui->active) ? 235 : 160;
        fg.b = (i == gui->active) ? 240 : 165;
        fg.a = 255;
        surf = TTF_RenderUTF8_Blended(gui->tab_font, text, fg);
        if (surf != NULL) {
            SDL_Texture *tex =
                SDL_CreateTextureFromSurface(gui->renderer, surf);

            if (tex != NULL) {
                SDL_Rect dst;

                tx = i * MASA_TAB_W + 10;
                ty = MASA_HEADER_H + (MASA_TABBAR_H - surf->h) / 2;
                dst.w = surf->w;
                dst.h = surf->h;
                dst.x = tx;
                dst.y = ty;
                SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                SDL_RenderCopy(gui->renderer, tex, NULL, &dst);
                SDL_DestroyTexture(tex);
            }
            SDL_FreeSurface(surf);
        }
    }

    SDL_SetRenderDrawColor(gui->renderer, 48, 48, 52, 255);
    SDL_RenderDrawLine(gui->renderer, 0, MASA_HEADER_H + MASA_TABBAR_H - 1,
                       gui->win_w, MASA_HEADER_H + MASA_TABBAR_H - 1);
}

static void draw_cursor(struct masa_gui *gui, int row, int col)
{
    int x = MASA_PAD_X + col * gui->cell_w;
    int y = body_top() + MASA_PAD_Y + row * gui->cell_h;
    SDL_Rect cell = {x, y, gui->cell_w, gui->cell_h};

    SDL_SetRenderDrawBlendMode(gui->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(gui->renderer, MASA_FG_R, MASA_FG_G, MASA_FG_B, 140);
    SDL_RenderFillRect(gui->renderer, &cell);
    SDL_SetRenderDrawBlendMode(gui->renderer, SDL_BLENDMODE_NONE);
}

/* Highlight the selected cell range (active tab, live view). */
static void draw_selection(struct masa_gui *gui)
{
    int r0, c0, r1, c1;
    int row;

    if (!gui->sel_active) {
        return;
    }
    r0 = gui->sel_r0; c0 = gui->sel_c0;
    r1 = gui->sel_r1; c1 = gui->sel_c1;
    if (r1 < r0 || (r1 == r0 && c1 < c0)) {
        int t;
        t = r0; r0 = r1; r1 = t;
        t = c0; c0 = c1; c1 = t;
    }

    SDL_SetRenderDrawBlendMode(gui->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(gui->renderer, 90, 120, 200, 110);
    for (row = r0; row <= r1 && row < gui->rows; row++) {
        int from = (row == r0) ? c0 : 0;
        int to = (row == r1) ? c1 : gui->cols - 1;
        SDL_Rect rect;

        if (from < 0) {
            from = 0;
        }
        if (to >= gui->cols) {
            to = gui->cols - 1;
        }
        rect.x = MASA_PAD_X + from * gui->cell_w;
        rect.y = body_top() + MASA_PAD_Y + row * gui->cell_h;
        rect.w = (to - from + 1) * gui->cell_w;
        rect.h = gui->cell_h;
        SDL_RenderFillRect(gui->renderer, &rect);
    }
    SDL_SetRenderDrawBlendMode(gui->renderer, SDL_BLENDMODE_NONE);
}

/* Draw one screen row from the live buffer. */
static void draw_live_row(struct masa_gui *gui, struct masa_tab *tab, int row,
                          int screen_row)
{
    int col;

    for (col = 0; col < gui->cols; col++) {
        VTermPos pos;
        VTermScreenCell cell;
        SDL_Color fg, bg;
        Uint32 fg_packed;
        int x = MASA_PAD_X + col * gui->cell_w;
        int y = body_top() + MASA_PAD_Y + row * gui->cell_h;
        SDL_Rect rect = {x, y, gui->cell_w, gui->cell_h};
        uint32_t cp;

        memset(&cell, 0, sizeof(cell));
        pos.row = screen_row;
        pos.col = col;
        if (!vterm_screen_get_cell(tab->screen, pos, &cell)) {
            continue;
        }
        if (cell.width == 0) {
            continue;
        }
        resolve_colors(&cell, &fg, &bg, &fg_packed);

        if (bg.r != MASA_BG_R || bg.g != MASA_BG_G || bg.b != MASA_BG_B) {
            SDL_SetRenderDrawColor(gui->renderer, bg.r, bg.g, bg.b, 255);
            SDL_RenderFillRect(gui->renderer, &rect);
        }

        cp = cell.chars[0];
        if (cp != 0 && cp != ' ') {
            struct glyph *g = glyph_get(gui, cp, fg_packed,
                                        (cell.attrs.bold ? GLYPH_STYLE_BOLD : 0) |
                                        (cell.attrs.italic ? GLYPH_STYLE_ITALIC : 0));

            if (g != NULL) {
                SDL_Rect dst = {x, y + g->top, g->w, g->h};

                SDL_RenderCopy(gui->renderer, g->tex, NULL, &dst);
            }
        }

        if (cell.attrs.underline && cell.attrs.underline != VTERM_UNDERLINE_OFF) {
            SDL_SetRenderDrawColor(gui->renderer, fg.r, fg.g, fg.b, 255);
            SDL_RenderDrawLine(gui->renderer, x, y + gui->cell_h - 1,
                               x + gui->cell_w - 1, y + gui->cell_h - 1);
        }
    }
}

/* Draw one scrollback row (index into the retained ring). */
static void draw_sb_row(struct masa_gui *gui, struct masa_tab *tab, int row,
                        int sb_index)
{
    char utf8[2048];
    int col;
    const char *p;

    masa_gui_scrollback_line(gui, sb_index, utf8, sizeof(utf8));
    p = utf8;
    for (col = 0; col < gui->cols && *p != '\0'; col++) {
        uint32_t cp;
        int n = utf8_decode(p, &cp);
        int x = MASA_PAD_X + col * gui->cell_w;
        int y = body_top() + MASA_PAD_Y + row * gui->cell_h;

        p += n;
        if (cp != 0 && cp != ' ') {
            struct glyph *g = glyph_get(gui, cp,
                                        (MASA_FG_R << 16) | (MASA_FG_G << 8) |
                                            MASA_FG_B, 0);

            if (g != NULL) {
                SDL_Rect dst = {x, y + g->top, g->w, g->h};

                SDL_RenderCopy(gui->renderer, g->tex, NULL, &dst);
            }
        }
    }
    (void)tab;
}

void masa_gui_render(struct masa_gui *gui)
{
    struct masa_tab *tab;
    int row;
    int blink_on = 1;

    if (gui == NULL || gui->renderer == NULL) {
        return;
    }
    tab = active_tab(gui);

    if (tab != NULL && tab->cursor_blink) {
        blink_on = (SDL_GetTicks() / 500) % 2 == 0;
    }

    /* Track the terminal's real cursor position. */
    if (tab != NULL && tab->vt != NULL) {
        VTermState *state = vterm_obtain_state(tab->vt);
        VTermPos pos;

        if (state != NULL) {
            vterm_state_get_cursorpos(state, &pos);
            tab->cursor_row = pos.row;
            tab->cursor_col = pos.col;
        }
    }

    SDL_SetRenderDrawColor(gui->renderer, MASA_BG_R, MASA_BG_G, MASA_BG_B, 255);
    SDL_RenderClear(gui->renderer);

    /* Terminal body background. */
    {
        SDL_Rect body = {MASA_PAD_X, body_top() + MASA_PAD_Y,
                         gui->cols * gui->cell_w, gui->rows * gui->cell_h};

        SDL_SetRenderDrawColor(gui->renderer, MASA_BG_R, MASA_BG_G,
                               MASA_BG_B, 255);
        SDL_RenderFillRect(gui->renderer, &body);
    }

    if (tab != NULL && tab->screen != NULL) {
        int offset = gui->scroll_offset;

        for (row = 0; row < gui->rows; row++) {
            /*
             * With the view scrolled back by `offset` lines, the top rows
             * come from history and the rest from the live screen shifted
             * down.  sb_index 0 is the oldest retained line.
             */
            int sb_index = gui->sb_total - offset + row;

            if (sb_index < 0) {
                continue;
            }
            if (sb_index < gui->sb_total) {
                draw_sb_row(gui, tab, row, sb_index);
            } else {
                int screen_row = sb_index - gui->sb_total;

                if (screen_row < gui->rows) {
                    draw_live_row(gui, tab, row, screen_row);
                }
            }
        }

        if (offset == 0) {
            draw_selection(gui);
        }
        if (offset == 0 && tab->cursor_visible && blink_on) {
            draw_cursor(gui, tab->cursor_row, tab->cursor_col);
        }
    }

    draw_header(gui);
    draw_tabbar(gui);
    SDL_RenderPresent(gui->renderer);
}

/* ------------------------------------------------------------------ */
/* Screen text extraction (used by the test suite)                     */
/* ------------------------------------------------------------------ */

size_t masa_gui_screen_text(const struct masa_gui *gui, char *out, size_t cap)
{
    struct masa_tab *tab;
    int row;
    int col;
    size_t used = 0;

    if (gui == NULL || out == NULL || cap == 0 || gui->active < 0) {
        return 0;
    }
    tab = &((struct masa_gui *)gui)->tabs[gui->active];
    if (tab->screen == NULL) {
        return 0;
    }
    out[0] = '\0';

    for (row = 0; row < gui->rows; row++) {
        int last = -1;

        for (col = 0; col < gui->cols; col++) {
            VTermPos pos;
            VTermScreenCell cell;

            memset(&cell, 0, sizeof(cell));
            pos.row = row;
            pos.col = col;
            if (!vterm_screen_get_cell(tab->screen, pos, &cell)) {
                continue;
            }
            if (cell.chars[0] != 0 && cell.chars[0] != ' ') {
                last = col;
            }
        }

        for (col = 0; col <= last; col++) {
            VTermPos pos;
            VTermScreenCell cell;
            char utf8[8];
            int n;
            int k;

            memset(&cell, 0, sizeof(cell));
            pos.row = row;
            pos.col = col;
            if (!vterm_screen_get_cell(tab->screen, pos, &cell)) {
                continue;
            }
            if (cell.width == 0) {
                continue;
            }
            n = utf8_encode(cell.chars[0] != 0 ? cell.chars[0] : ' ', utf8);
            for (k = 0; k < n && used + 1 < cap; k++) {
                out[used++] = utf8[k];
            }
        }

        if (used + 1 < cap) {
            out[used++] = '\n';
        }
    }

    if (used < cap) {
        out[used] = '\0';
    } else {
        out[cap - 1] = '\0';
        used = cap - 1;
    }
    return used;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

/* Drain every tab's PTY into its own screen. */
static void pump_tabs(struct masa_gui *gui)
{
    int i;

    for (i = 0; i < gui->tab_count; i++) {
        struct masa_tab *tab = &gui->tabs[i];
        char buf[4096];
        ssize_t n;

        if (!tab->pty_open) {
            continue;
        }
        while ((n = masa_pty_read(&tab->pty, buf, sizeof(buf))) > 0) {
            masa_gui_tab_feed(gui, i, buf, (size_t)n);
        }
    }
}

/* Write `out` to the active tab's PTY. */
static void write_active(struct masa_gui *gui, const char *out, size_t n)
{
    struct masa_tab *tab = active_tab(gui);

    if (n > 0 && tab != NULL && tab->pty_open) {
        masa_pty_write(&tab->pty, out, n);
    }
}

/*
 * Open a new tab and start `masa shell` on its own PTY.  Returns the new
 * tab index, or -1 on failure.
 */
static int open_shell_tab(struct masa_gui *gui, char *const argv[],
                          char *const envp[])
{
    int idx = masa_gui_tab_new(gui);

    if (idx < 0) {
        return -1;
    }
    if (masa_gui_tab_open_pty(gui, idx, argv, envp) != 0) {
        masa_gui_tab_close(gui, idx);
        return -1;
    }
    return idx;
}

static int run_headless(struct masa_gui *gui, int frames)
{
    int i;

    for (i = 0; i < frames; i++) {
        pump_tabs(gui);
        masa_gui_render(gui);
        SDL_Delay(20);
    }
    return 0;
}

int masa_gui_main(int argc, char *const argv[])
{
    struct masa_gui *gui = NULL;
    char **shell_argv = NULL;
    char **shell_env = NULL;
    char err[256];
    int headless = 0;
    int frames = 30;
    int status = 0;
    int running = 1;
    int i;

    for (i = 1; i < argc; i++) {
        if (argv[i] == NULL) {
            break;
        }
        if (strcmp(argv[i], "--headless") == 0) {
            headless = 1;
            setenv("SDL_VIDEODRIVER", "dummy", 1);
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = atoi(argv[++i]);
        }
    }

    err[0] = '\0';
    gui = masa_gui_init(MASA_TITLE, 900, 600, err, sizeof(err));
    if (gui == NULL) {
        fprintf(stderr, "masa-term: %s\n", err[0] ? err : "initialisation failed");
        return 1;
    }

    shell_argv = masa_gui_shell_argv();
    shell_env = masa_gui_shell_env();
    if (shell_argv == NULL) {
        fprintf(stderr, "masa-term: cannot locate the masa executable "
                        "(set MASA_BIN)\n");
        masa_gui_free_argv(shell_env);
        masa_gui_shutdown(gui);
        return 1;
    }

    /*
     * masa_gui_init() created an empty first tab; give it the shell PTY.
     * A failure here means no PTY is available at all.
     */
    if (open_shell_tab(gui, shell_argv, shell_env) < 0) {
        fprintf(stderr, "masa-term: cannot open a pseudo-terminal: %s\n",
                strerror(errno));
        masa_gui_free_argv(shell_argv);
        masa_gui_free_argv(shell_env);
        masa_gui_shutdown(gui);
        return 1;
    }

    if (headless) {
        run_headless(gui, frames);
        masa_gui_free_argv(shell_argv);
        masa_gui_free_argv(shell_env);
        masa_gui_shutdown(gui);
        return 0;
    }

    while (running) {
        SDL_Event ev;

        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = 0;
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                    ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    masa_gui_resize(gui, ev.window.data1, ev.window.data2);
                }
                break;
            case SDL_KEYDOWN: {
                Uint16 mod = ev.key.keysym.mod;
                SDL_Keycode sym = ev.key.keysym.sym;

                /* Tab management and clipboard shortcuts win over input. */
                if ((mod & KMOD_CTRL) && (mod & KMOD_SHIFT) &&
                    sym == SDLK_t) {
                    if (open_shell_tab(gui, shell_argv, shell_env) < 0) {
                        fprintf(stderr, "masa-term: tab limit reached\n");
                    }
                    break;
                }
                if ((mod & KMOD_CTRL) && (mod & KMOD_SHIFT) &&
                    sym == SDLK_w) {
                    int left = masa_gui_tab_close(gui, masa_gui_tab_active(gui));

                    if (left <= 0) {
                        running = 0;
                    }
                    break;
                }
                if ((mod & KMOD_CTRL) && sym == SDLK_TAB) {
                    int n = masa_gui_tab_count(gui);

                    if (n > 0) {
                        int next = (masa_gui_tab_active(gui) + 1) % n;

                        masa_gui_tab_select(gui, next);
                    }
                    break;
                }
                if ((mod & KMOD_CTRL) && (mod & KMOD_SHIFT) &&
                    sym == SDLK_v) {
                    char *clip = SDL_GetClipboardText();
                    char out[4096];

                    if (clip != NULL) {
                        size_t n = masa_gui_paste(gui, clip, out, sizeof(out));

                        write_active(gui, out, n);
                        SDL_free(clip);
                    }
                    break;
                }
                if ((mod & KMOD_CTRL) && (mod & KMOD_SHIFT) &&
                    sym == SDLK_c) {
                    char sel[8192];

                    if (masa_gui_copy_selection(gui, sel, sizeof(sel)) > 0) {
                        SDL_SetClipboardText(sel);
                    }
                    break;
                }
                if ((mod & KMOD_SHIFT) &&
                    (sym == SDLK_PAGEUP || sym == SDLK_PAGEDOWN)) {
                    masa_gui_scroll(gui,
                                    (sym == SDLK_PAGEUP) ? 5 : -5);
                    break;
                }

                /* Any other key snaps the view back to live. */
                masa_gui_scroll_reset(gui);
                masa_gui_clear_selection(gui);

                {
                    char out[512];
                    size_t n = masa_gui_input(gui, sym, mod, out, sizeof(out));

                    write_active(gui, out, n);
                }
                break;
            }
            case SDL_TEXTINPUT: {
                char out[512];
                size_t n = masa_gui_input_text(gui, ev.text.text, out,
                                               sizeof(out));

                masa_gui_scroll_reset(gui);
                write_active(gui, out, n);
                break;
            }
            case SDL_MOUSEWHEEL:
                masa_gui_scroll(gui, ev.wheel.y * 3);
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    int tab = masa_gui_tab_at(gui, ev.button.x, ev.button.y);

                    if (tab >= 0) {
                        masa_gui_tab_select(gui, tab);
                    } else if (ev.button.y >= body_top()) {
                        int row = (ev.button.y - body_top() - MASA_PAD_Y) /
                                  gui->cell_h;
                        int col = (ev.button.x - MASA_PAD_X) / gui->cell_w;

                        gui->selecting = 1;
                        masa_gui_set_selection(gui, row, col, row, col);
                    }
                }
                break;
            case SDL_MOUSEMOTION:
                if (gui->selecting) {
                    int row = (ev.motion.y - body_top() - MASA_PAD_Y) /
                              gui->cell_h;
                    int col = (ev.motion.x - MASA_PAD_X) / gui->cell_w;

                    masa_gui_set_selection(gui, gui->sel_r0, gui->sel_c0, row,
                                           col);
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    gui->selecting = 0;
                }
                break;
            default:
                break;
            }
        }

        pump_tabs(gui);
        masa_gui_render(gui);
        SDL_Delay(10);
    }

    /* Close every tab (each reaps its PTY) and report the first tab's exit. */
    if (masa_gui_tab_count(gui) > 0) {
        struct masa_pty *pty = masa_gui_tab_pty(gui, 0);

        if (pty != NULL) {
            status = masa_pty_child_status(pty, &status) == 1 ? status : 0;
        }
    }
    masa_gui_free_argv(shell_argv);
    masa_gui_free_argv(shell_env);
    masa_gui_shutdown(gui);
    return (status < 0) ? 0 : status;
}

#if !defined(MASA_GUI_NO_MAIN) && !defined(MASA_NO_MAIN)
int main(int argc, char *argv[])
{
    return masa_gui_main(argc, argv);
}
#endif
