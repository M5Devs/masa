/*
 * danroot_appkit.c - a clean-room, minimal AppKit and CoreGraphics for the
 * Masa Danroot loader, drawn with SDL2.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This is a small stand-in for the parts of Apple's AppKit and
 * CoreGraphics that a self-contained Darwin GUI program might call.  It is
 * a clean-room implementation from the documented API: nothing here is
 * derived from Apple's source.  A program that creates an NSApplication,
 * opens an NSWindow and fills rectangles with CoreGraphics gets a real
 * SDL2 window with those rectangles painted on screen.
 *
 * CoreGraphics draws into the window's SDL_Renderer.  Because a loaded
 * program draws before it enters the run loop, every fill is also recorded
 * and replayed on each frame, so what the program painted survives the
 * per-frame clear (the way an AppKit view redraws itself).
 *
 * The SDL2 code is compiled only when MASA_DANROOT_SDL2 is defined (the
 * `make gui` / `make test-gui` builds), so the core command line tool and
 * its tests keep building with no SDL2 dependency at all.  Without it the
 * same translation unit provides inert stubs: an image that imports
 * AppKit/CoreGraphics still links and loads, and the loader reports a
 * clear "rebuild with `make gui`" diagnostic for the first GUI symbol it
 * cannot satisfy.
 */

#ifndef _WIN32

#define _POSIX_C_SOURCE 200809L

#endif

#include "danroot_internal.h"

#ifndef _WIN32

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef MASA_DANROOT_SDL2

/* ------------------------------------------------------------------ */
/* Geometry and colour types                                          */
/* ------------------------------------------------------------------ */

typedef struct dr_cg_point {
    double x;
    double y;
} dr_cg_point;

typedef struct dr_cg_size {
    double width;
    double height;
} dr_cg_size;

typedef struct dr_cg_rect {
    dr_cg_point origin;
    dr_cg_size size;
} dr_cg_rect;

typedef struct dr_cg_color {
    double r;
    double g;
    double b;
    double a;
} dr_cg_color;

static const dr_cg_rect dr_cg_rect_zero = {{0.0, 0.0}, {0.0, 0.0}};
static const dr_cg_point dr_cg_point_zero = {0.0, 0.0};
static const dr_cg_size dr_cg_size_zero = {0.0, 0.0};

static dr_cg_point dr_cg_CGPointMake(double x, double y)
{
    dr_cg_point p;

    p.x = x;
    p.y = y;
    return p;
}

static dr_cg_size dr_cg_CGSizeMake(double w, double h)
{
    dr_cg_size s;

    s.width = w;
    s.height = h;
    return s;
}

static dr_cg_rect dr_cg_CGRectMake(double x, double y, double w, double h)
{
    dr_cg_rect r;

    r.origin.x = x;
    r.origin.y = y;
    r.size.width = w;
    r.size.height = h;
    return r;
}

static dr_cg_rect dr_cg_NSMakeRect(double x, double y, double w, double h)
{
    return dr_cg_CGRectMake(x, y, w, h);
}

static dr_cg_point dr_cg_NSMakePoint(double x, double y)
{
    return dr_cg_CGPointMake(x, y);
}

static dr_cg_size dr_cg_NSMakeSize(double w, double h)
{
    return dr_cg_CGSizeMake(w, h);
}

/*
 * The AppKit class handles.  A program reaches them through
 * objc_getClass("NSWindow") and friends; dr_gui_class() returns the
 * matching handle and dr_gui_msgSend() handles the selectors on it.  The
 * Objective-C runtime knows nothing about these, so the two stay in step
 * through the small class table below.
 */
static struct dr_gui_class {
    const char *name;
    struct dr_gui_class *super;
} dr_gui_class_nsobject = {"NSObject", NULL},
  dr_gui_class_nsstring = {"NSString", &dr_gui_class_nsobject},
  dr_gui_class_nsview = {"NSView", &dr_gui_class_nsobject},
  dr_gui_class_nswindow = {"NSWindow", &dr_gui_class_nsobject},
  dr_gui_class_nscolor = {"NSColor", &dr_gui_class_nsobject},
  dr_gui_class_nsapplication = {"NSApplication", &dr_gui_class_nsobject};

static struct dr_gui_class *const dr_gui_classes[] = {
    &dr_gui_class_nsobject, &dr_gui_class_nsstring, &dr_gui_class_nsview,
    &dr_gui_class_nswindow, &dr_gui_class_nscolor, &dr_gui_class_nsapplication
};

static int dr_gui_is_class(const void *p)
{
    size_t i;

    for (i = 0; i < sizeof(dr_gui_classes) / sizeof(dr_gui_classes[0]); i++) {
        if (p == (const void *)dr_gui_classes[i]) {
            return 1;
        }
    }
    return 0;
}

/* `_NSApp` is a global id: the program loads the pointer it holds, so the
 * loader binds the address of this storage. */
static void *dr_gui_ns_app;

/* AppKit's application entry points, which simply run the event loop. */
static void dr_gui_NSApplicationLoad(void);
static int dr_gui_NSApplicationMain(int argc, const char **argv);

#include <SDL.h>

/* ------------------------------------------------------------------ */
/* Live window / drawing-context / draw-op registries                 */
/* ------------------------------------------------------------------ */

struct dr_gui_draw {
    dr_cg_color color;
    dr_cg_rect rect;
    int stroke;
};

#define DR_GUI_MAX_DRAWS 4096

static struct dr_gui_draw dr_gui_draws[DR_GUI_MAX_DRAWS];
static size_t dr_gui_draw_count;

struct dr_gui_window {
    SDL_Window *window;
    SDL_Renderer *renderer;
    char title[256];
    dr_cg_color background;
    dr_cg_color fill;
    int opened;
    struct dr_gui_window *next;
};

struct dr_gui_context {
    struct dr_gui_window *win;
    struct dr_gui_context *next;
};

static struct dr_gui_window *dr_gui_windows;   /* live windows, newest first */
static struct dr_gui_context *dr_gui_contexts; /* live contexts, newest first */

/* The last framebuffer pixel captured by the run loop, for tests. */
static unsigned char dr_gui_last_pixel[4];
static int dr_gui_have_pixel;

static int dr_gui_sdl_state = -1; /* -1 unknown, 0 failed, 1 ready */

static int dr_gui_sdl_init(void)
{
    if (dr_gui_sdl_state >= 0) {
        return dr_gui_sdl_state;
    }
    dr_gui_sdl_state = (SDL_Init(SDL_INIT_VIDEO) == 0) ? 1 : 0;
    return dr_gui_sdl_state;
}

/*
 * Number of frames the event loop runs before returning.  MASA_GUI_FRAMES
 * lets an automated test drive a GUI program headlessly and exit cleanly;
 * unset (0) means run until the window closes.
 */
static int dr_gui_frame_limit(void)
{
    const char *env = getenv("MASA_GUI_FRAMES");
    long n;

    if (env == NULL || env[0] == '\0') {
        return 0;
    }
    n = strtol(env, NULL, 10);
    if (n < 0) {
        n = 0;
    }
    if (n > 1000000) {
        n = 1000000;
    }
    return (int)n;
}

static struct dr_gui_window *dr_gui_window_new(void)
{
    struct dr_gui_window *w = calloc(1, sizeof(*w));

    if (w == NULL) {
        return NULL;
    }
    w->background.r = 0.0;
    w->background.g = 0.0;
    w->background.b = 0.0;
    w->background.a = 1.0;
    w->fill.r = 1.0;
    w->fill.g = 1.0;
    w->fill.b = 1.0;
    w->fill.a = 1.0;
    w->next = dr_gui_windows;
    dr_gui_windows = w;
    return w;
}

static struct dr_gui_context *dr_gui_context_for(struct dr_gui_window *win)
{
    struct dr_gui_context *c;

    for (c = dr_gui_contexts; c != NULL; c = c->next) {
        if (c->win == win) {
            return c;
        }
    }
    c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    c->win = win;
    c->next = dr_gui_contexts;
    dr_gui_contexts = c;
    return c;
}

static void dr_gui_set_draw_color(SDL_Renderer *ren, const dr_cg_color *c)
{
    SDL_SetRenderDrawColor(ren, (Uint8)(c->r * 255.0 + 0.5),
                           (Uint8)(c->g * 255.0 + 0.5),
                           (Uint8)(c->b * 255.0 + 0.5),
                           (Uint8)(c->a * 255.0 + 0.5));
}

static int dr_gui_open_window(struct dr_gui_window *w, double cw, double ch)
{
    int win_w;
    int win_h;
    const char *title;

    if (w == NULL || w->opened || !dr_gui_sdl_init()) {
        return w != NULL && w->opened;
    }
    win_w = (cw >= 16.0 && cw <= 8192.0) ? (int)(cw + 0.5) : 640;
    win_h = (ch >= 16.0 && ch <= 8192.0) ? (int)(ch + 0.5) : 480;
    title = (w->title[0] != '\0') ? w->title : "Masa AppKit";

    w->window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED,
                                 SDL_WINDOWPOS_CENTERED, win_w, win_h,
                                 SDL_WINDOW_SHOWN);
    if (w->window == NULL) {
        return 0;
    }
    w->renderer = SDL_CreateRenderer(w->window, -1, SDL_RENDERER_SOFTWARE);
    if (w->renderer == NULL) {
        SDL_DestroyWindow(w->window);
        w->window = NULL;
        return 0;
    }
    dr_gui_set_draw_color(w->renderer, &w->background);
    SDL_RenderClear(w->renderer);
    SDL_RenderPresent(w->renderer);
    w->opened = 1;
    return 1;
}

/* Replay every recorded draw op for the window's frame. */
static void dr_gui_draw_frame(struct dr_gui_window *w)
{
    size_t i;

    if (w == NULL || w->renderer == NULL) {
        return;
    }
    dr_gui_set_draw_color(w->renderer, &w->background);
    SDL_RenderClear(w->renderer);
    for (i = 0; i < dr_gui_draw_count; i++) {
        const struct dr_gui_draw *d = &dr_gui_draws[i];
        SDL_Rect r;

        r.x = (int)(d->rect.origin.x + 0.5);
        r.y = (int)(d->rect.origin.y + 0.5);
        r.w = (int)(d->rect.size.width + 0.5);
        r.h = (int)(d->rect.size.height + 0.5);
        dr_gui_set_draw_color(w->renderer, &d->color);
        if (d->stroke) {
            SDL_RenderDrawRect(w->renderer, &r);
        } else {
            SDL_RenderFillRect(w->renderer, &r);
        }
    }
}

/* Draw, present, and read back the centre pixel so a test can verify the
 * result of the program's CoreGraphics calls. */
static void dr_gui_present_and_read(struct dr_gui_window *w)
{
    SDL_Rect pixel;
    Uint32 px = 0;
    int win_w = 0;
    int win_h = 0;

    if (w == NULL || w->renderer == NULL) {
        return;
    }
    dr_gui_draw_frame(w);
    SDL_GetWindowSize(w->window, &win_w, &win_h);
    pixel.x = win_w / 2;
    pixel.y = win_h / 2;
    pixel.w = 1;
    pixel.h = 1;
    if (SDL_RenderReadPixels(w->renderer, &pixel, SDL_PIXELFORMAT_RGBA8888,
                             &px, 4) == 0) {
        dr_gui_last_pixel[0] = (unsigned char)((px >> 24) & 0xff);
        dr_gui_last_pixel[1] = (unsigned char)((px >> 16) & 0xff);
        dr_gui_last_pixel[2] = (unsigned char)((px >> 8) & 0xff);
        dr_gui_last_pixel[3] = (unsigned char)(px & 0xff);
        dr_gui_have_pixel = 1;
    }
    SDL_RenderPresent(w->renderer);
}

/*
 * The NSApplication run loop.  It opens the first window (if the program
 * has not already shown one), then pumps events and draws frames until the
 * window is closed or MASA_GUI_FRAMES frames have been drawn.
 */
static void dr_gui_run_loop(void)
{
    int limit = dr_gui_frame_limit();
    int frame = 0;
    int running = 1;

    if (dr_gui_windows != NULL) {
        dr_gui_open_window(dr_gui_windows, 0.0, 0.0);
    }
    if (dr_gui_windows == NULL || dr_gui_windows->renderer == NULL) {
        return; /* no window to draw into */
    }
    while (running) {
        SDL_Event ev;

        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT ||
                (ev.type == SDL_KEYDOWN &&
                 ev.key.keysym.sym == SDLK_ESCAPE)) {
                running = 0;
            }
        }
        dr_gui_present_and_read(dr_gui_windows);
        frame++;
        if (!running) {
            break;
        }
        if (limit > 0 && frame >= limit) {
            break;
        }
        SDL_Delay(16);
    }
    if (limit > 0) {
        fprintf(stderr, "masa-gui: %d frames, pixel=%02X%02X%02X%02X\n", frame,
                dr_gui_have_pixel ? dr_gui_last_pixel[0] : 0,
                dr_gui_have_pixel ? dr_gui_last_pixel[1] : 0,
                dr_gui_have_pixel ? dr_gui_last_pixel[2] : 0,
                dr_gui_have_pixel ? dr_gui_last_pixel[3] : 0);
        fflush(stderr);
    }
}

/* --- CoreGraphics drawing ----------------------------------------- */

static void dr_gui_record(struct dr_gui_window *w, dr_cg_rect rect, int stroke)
{
    struct dr_gui_draw *d;

    if (dr_gui_draw_count >= DR_GUI_MAX_DRAWS) {
        dr_gui_draw_count = 0; /* drop the oldest frame and start over */
    }
    d = &dr_gui_draws[dr_gui_draw_count++];
    d->rect = rect;
    d->stroke = stroke;
    if (w != NULL) {
        d->color = w->fill;
    } else {
        d->color.r = 1.0;
        d->color.g = 1.0;
        d->color.b = 1.0;
        d->color.a = 1.0;
    }
}

static void dr_gui_ctx_set_fill(void *ctx, double r, double g, double b,
                                double a)
{
    struct dr_gui_context *c = ctx;

    if (c != NULL && c->win != NULL) {
        c->win->fill.r = r;
        c->win->fill.g = g;
        c->win->fill.b = b;
        c->win->fill.a = a;
    }
}

static void dr_gui_ctx_fill_rect(void *ctx, dr_cg_rect rect)
{
    struct dr_gui_context *c = ctx;

    if (c == NULL || c->win == NULL) {
        return;
    }
    dr_gui_record(c->win, rect, 0);
    if (c->win->renderer != NULL) {
        SDL_Rect r;

        dr_gui_set_draw_color(c->win->renderer, &c->win->fill);
        r.x = (int)(rect.origin.x + 0.5);
        r.y = (int)(rect.origin.y + 0.5);
        r.w = (int)(rect.size.width + 0.5);
        r.h = (int)(rect.size.height + 0.5);
        SDL_RenderFillRect(c->win->renderer, &r);
    }
}

static void dr_gui_ctx_set_stroke(void *ctx, double r, double g, double b,
                                  double a)
{
    struct dr_gui_context *c = ctx;

    if (c != NULL && c->win != NULL) {
        c->win->fill.r = r;
        c->win->fill.g = g;
        c->win->fill.b = b;
        c->win->fill.a = a;
    }
}

static void dr_gui_ctx_stroke_rect(void *ctx, dr_cg_rect rect)
{
    struct dr_gui_context *c = ctx;

    if (c == NULL || c->win == NULL) {
        return;
    }
    dr_gui_record(c->win, rect, 1);
    if (c->win->renderer != NULL) {
        SDL_Rect r;

        dr_gui_set_draw_color(c->win->renderer, &c->win->fill);
        r.x = (int)(rect.origin.x + 0.5);
        r.y = (int)(rect.origin.y + 0.5);
        r.w = (int)(rect.size.width + 0.5);
        r.h = (int)(rect.size.height + 0.5);
        SDL_RenderDrawRect(c->win->renderer, &r);
    }
}

static void dr_gui_ctx_clear_rect(void *ctx, dr_cg_rect rect)
{
    struct dr_gui_context *c = ctx;

    if (c == NULL || c->win == NULL) {
        return;
    }
    /* Clearing paints the window background over the rectangle. */
    {
        dr_cg_color save = c->win->fill;

        c->win->fill = c->win->background;
        dr_gui_record(c->win, rect, 0);
        c->win->fill = save;
    }
}

static void dr_gui_ctx_flush(void *ctx)
{
    struct dr_gui_context *c = ctx;

    if (c != NULL && c->win != NULL && c->win->renderer != NULL) {
        dr_gui_draw_frame(c->win);
        SDL_RenderPresent(c->win->renderer);
    }
}

/* A CGColorRef is only ever handed back to setBackgroundColor:, so a small
 * heap object carrying the four components is enough. */
struct dr_gui_color {
    dr_cg_color c;
};

static void *dr_gui_CGColorCreateGenericRGB(double r, double g, double b,
                                            double a)
{
    struct dr_gui_color *col = calloc(1, sizeof(*col));

    if (col == NULL) {
        return NULL;
    }
    col->c.r = r;
    col->c.g = g;
    col->c.b = b;
    col->c.a = a;
    return col;
}

static void dr_gui_CGColorRelease(void *color)
{
    free(color);
}

/* --- AppKit selectors --------------------------------------------- */

static void dr_gui_set_title(struct dr_gui_window *w, const char *title)
{
    if (w == NULL || title == NULL) {
        return;
    }
    snprintf(w->title, sizeof(w->title), "%s", title);
    if (w->window != NULL) {
        SDL_SetWindowTitle(w->window, w->title);
    }
}

static int dr_gui_handle_application(const char *name, va_list *ap,
                                     void **out);

static int dr_gui_handle_class(struct dr_gui_class *cls, const char *name,
                               va_list *ap, void **out)
{
    if (cls == &dr_gui_class_nsapplication) {
        if (strcmp(name, "sharedApplication") == 0) {
            dr_gui_ns_app = cls;
            *out = cls;
            return 1;
        }
        /* The application object doubles as its class handle here, so the
         * instance selectors arrive on the class. */
        if (dr_gui_handle_application(name, ap, out)) {
            return 1;
        }
    }
    if (cls == &dr_gui_class_nswindow && strcmp(name, "alloc") == 0) {
        *out = dr_gui_window_new();
        return 1;
    }
    if (strcmp(name, "alloc") == 0) {
        *out = cls; /* NSApplication / NSColor are singletons here */
        return 1;
    }
    if (strcmp(name, "retain") == 0 || strcmp(name, "autorelease") == 0) {
        *out = cls;
        return 1;
    }
    return 0;
}

static int dr_gui_handle_window(struct dr_gui_window *w, const char *name,
                                va_list *ap, void **out)
{
    if (strcmp(name, "initWithContentRect:styleMask:backing:defer:") == 0) {
        double cw;
        double ch;

        (void)va_arg(*ap, double); /* x */
        (void)va_arg(*ap, double); /* y */
        cw = va_arg(*ap, double);
        ch = va_arg(*ap, double);
        (void)va_arg(*ap, long); /* styleMask */
        (void)va_arg(*ap, long); /* backing */
        (void)va_arg(*ap, long); /* defer */
        dr_gui_open_window(w, cw, ch);
        *out = w;
        return 1;
    }
    if (strcmp(name, "setTitle:") == 0) {
        void *title = va_arg(*ap, void *);

        dr_gui_set_title(w, dr_objc_cstr_of(title));
        *out = NULL;
        return 1;
    }
    if (strcmp(name, "makeKeyAndOrderFront:") == 0) {
        (void)va_arg(*ap, void *);
        if (!w->opened) {
            dr_gui_open_window(w, 0.0, 0.0);
        } else if (w->window != NULL) {
            SDL_ShowWindow(w->window);
        }
        *out = NULL;
        return 1;
    }
    if (strcmp(name, "contentView") == 0) {
        *out = w; /* the window doubles as its own content view */
        return 1;
    }
    if (strcmp(name, "setBackgroundColor:") == 0) {
        void *color = va_arg(*ap, void *);

        if (color != NULL) {
            w->background = ((struct dr_gui_color *)color)->c;
        }
        *out = NULL;
        return 1;
    }
    if (strcmp(name, "CGContext") == 0 ||
        strcmp(name, "graphicsContext") == 0) {
        *out = dr_gui_context_for(w);
        return 1;
    }
    if (strcmp(name, "run") == 0) {
        dr_gui_run_loop();
        *out = NULL;
        return 1;
    }
    if (strcmp(name, "stop:") == 0 || strcmp(name, "terminate:") == 0) {
        (void)va_arg(*ap, void *);
        *out = NULL;
        return 1;
    }
    if (strcmp(name, "display") == 0 || strcmp(name, "flush") == 0) {
        dr_gui_ctx_flush(dr_gui_context_for(w));
        *out = NULL;
        return 1;
    }
    if (strcmp(name, "setNeedsDisplay:") == 0) {
        (void)va_arg(*ap, long);
        dr_gui_ctx_flush(dr_gui_context_for(w));
        *out = NULL;
        return 1;
    }
    return 0;
}

static int dr_gui_handle_application(const char *name, va_list *ap, void **out)
{
    if (strcmp(name, "run") == 0) {
        dr_gui_run_loop();
        *out = NULL;
        return 1;
    }
    if (strcmp(name, "stop:") == 0 || strcmp(name, "terminate:") == 0) {
        (void)va_arg(*ap, void *);
        *out = NULL;
        return 1;
    }
    if (strcmp(name, "activateIgnoringOtherApps:") == 0) {
        (void)va_arg(*ap, long);
        *out = NULL;
        return 1;
    }
    return 0;
}

/* --- provider plumbing -------------------------------------------- */

int dr_gui_available(void)
{
    return 1;
}

void *dr_gui_class(const char *name)
{
    size_t i;

    if (name == NULL) {
        return NULL;
    }
    for (i = 0; i < sizeof(dr_gui_classes) / sizeof(dr_gui_classes[0]); i++) {
        if (strcmp(dr_gui_classes[i]->name, name) == 0) {
            return dr_gui_classes[i];
        }
    }
    return NULL;
}

int dr_gui_msgSend(void *recv, const char *name, va_list *ap, void **out)
{
    struct dr_gui_window *w;
    struct dr_gui_context *c;

    if (recv == NULL || name == NULL) {
        return 0;
    }
    if (dr_gui_is_class(recv)) {
        return dr_gui_handle_class(recv, name, ap, out);
    }
    for (w = dr_gui_windows; w != NULL; w = w->next) {
        if ((void *)w == recv) {
            return dr_gui_handle_window(w, name, ap, out);
        }
    }
    /* A drawing context reached through `contentView`/`CGContext`. */
    for (c = dr_gui_contexts; c != NULL; c = c->next) {
        if ((void *)c == recv) {
            if (strcmp(name, "setRGBFillColor") == 0) {
                double r = va_arg(*ap, double);
                double g = va_arg(*ap, double);
                double b = va_arg(*ap, double);
                double a = va_arg(*ap, double);

                dr_gui_ctx_set_fill(c, r, g, b, a);
                *out = NULL;
                return 1;
            }
            if (strcmp(name, "fillRect") == 0) {
                dr_cg_rect rect;

                rect.origin.x = va_arg(*ap, double);
                rect.origin.y = va_arg(*ap, double);
                rect.size.width = va_arg(*ap, double);
                rect.size.height = va_arg(*ap, double);
                dr_gui_ctx_fill_rect(c, rect);
                *out = NULL;
                return 1;
            }
            if (strcmp(name, "flush") == 0) {
                dr_gui_ctx_flush(c);
                *out = NULL;
                return 1;
            }
        }
    }
    if (recv == dr_gui_ns_app ||
        recv == (void *)&dr_gui_class_nsapplication) {
        return dr_gui_handle_application(name, ap, out);
    }
    return 0;
}

void *dr_gui_lookup(const char *name)
{
    static const struct {
        const char *name;
        void *addr;
    } syms[] = {
        {"_CGContextSetRGBFillColor", (void *)dr_gui_ctx_set_fill},
        {"_CGContextSetRGBStrokeColor", (void *)dr_gui_ctx_set_stroke},
        {"_CGContextFillRect", (void *)dr_gui_ctx_fill_rect},
        {"_CGContextStrokeRect", (void *)dr_gui_ctx_stroke_rect},
        {"_CGContextClearRect", (void *)dr_gui_ctx_clear_rect},
        {"_CGContextFlush", (void *)dr_gui_ctx_flush},
        {"_CGRectMake", (void *)dr_cg_CGRectMake},
        {"_CGPointMake", (void *)dr_cg_CGPointMake},
        {"_CGSizeMake", (void *)dr_cg_CGSizeMake},
        {"_NSMakeRect", (void *)dr_cg_NSMakeRect},
        {"_NSMakePoint", (void *)dr_cg_NSMakePoint},
        {"_NSMakeSize", (void *)dr_cg_NSMakeSize},
        {"_CGColorCreateGenericRGB", (void *)dr_gui_CGColorCreateGenericRGB},
        {"_CGColorRelease", (void *)dr_gui_CGColorRelease},
        {"_CGRectZero", (void *)&dr_cg_rect_zero},
        {"_CGPointZero", (void *)&dr_cg_point_zero},
        {"_CGSizeZero", (void *)&dr_cg_size_zero},
        {"_NSApplication", (void *)&dr_gui_class_nsapplication},
        {"_NSWindow", (void *)&dr_gui_class_nswindow},
        {"_NSView", (void *)&dr_gui_class_nsview},
        {"_NSColor", (void *)&dr_gui_class_nscolor},
        {"_NSObject", (void *)&dr_gui_class_nsobject},
        {"_NSString", (void *)&dr_gui_class_nsstring},
        {"_NSApp", (void *)&dr_gui_ns_app},
        {"_NSApplicationLoad", (void *)dr_gui_NSApplicationLoad},
        {"_NSApplicationMain", (void *)dr_gui_NSApplicationMain},
        {NULL, NULL}
    };
    size_t i;

    for (i = 0; syms[i].name != NULL; i++) {
        if (strcmp(syms[i].name, name) == 0) {
            return syms[i].addr;
        }
    }
    return NULL;
}

int dr_gui_is_gui_symbol(const char *name)
{
    static const char *const names[] = {
        "NSApplication", "NSWindow", "NSView", "NSColor", "NSObject",
        "NSString", "NSApp", "CoreGraphics", "AppKit", "Cocoa", NULL
    };
    size_t i;

    if (name == NULL) {
        return 0;
    }
    if (name[0] == '_') {
        name++;
    }
    for (i = 0; names[i] != NULL; i++) {
        if (strcmp(name, names[i]) == 0) {
            return 1;
        }
    }
    return strncmp(name, "CGContext", 9) == 0 ||
           strncmp(name, "CGRect", 6) == 0 ||
           strncmp(name, "CGPoint", 7) == 0 ||
           strncmp(name, "CGSize", 6) == 0 ||
           strncmp(name, "CGColor", 7) == 0 ||
           strncmp(name, "NSMake", 6) == 0;
}

void dr_gui_reset(void)
{
    struct dr_gui_context *c;

    while (dr_gui_contexts != NULL) {
        c = dr_gui_contexts;
        dr_gui_contexts = c->next;
        free(c);
    }
    while (dr_gui_windows != NULL) {
        struct dr_gui_window *w = dr_gui_windows;

        dr_gui_windows = w->next;
        if (w->renderer != NULL) {
            SDL_DestroyRenderer(w->renderer);
        }
        if (w->window != NULL) {
            SDL_DestroyWindow(w->window);
        }
        free(w);
    }
    dr_gui_ns_app = NULL;
    dr_gui_draw_count = 0;
    if (dr_gui_sdl_state == 1) {
        SDL_Quit();
        dr_gui_sdl_state = -1;
    }
}

static void dr_gui_NSApplicationLoad(void)
{
}

static int dr_gui_NSApplicationMain(int argc, const char **argv)
{
    (void)argc;
    (void)argv;
    dr_gui_run_loop();
    return 0;
}

int masa_gui_read_pixel(unsigned char *rgba)
{
    if (rgba == NULL || !dr_gui_have_pixel) {
        return 0;
    }
    rgba[0] = dr_gui_last_pixel[0];
    rgba[1] = dr_gui_last_pixel[1];
    rgba[2] = dr_gui_last_pixel[2];
    rgba[3] = dr_gui_last_pixel[3];
    return 1;
}

#else /* no MASA_DANROOT_SDL2 */

int dr_gui_available(void)
{
    return 0;
}

void *dr_gui_class(const char *name)
{
    (void)name;
    return NULL;
}

int dr_gui_msgSend(void *recv, const char *name, va_list *ap, void **out)
{
    (void)recv;
    (void)name;
    (void)ap;
    (void)out;
    return 0;
}

void *dr_gui_lookup(const char *name)
{
    (void)name;
    return NULL;
}

int dr_gui_is_gui_symbol(const char *name)
{
    static const char *const names[] = {
        "NSApplication", "NSWindow", "NSView", "NSColor", "NSObject",
        "NSString", "NSApp", NULL
    };
    size_t i;

    if (name == NULL) {
        return 0;
    }
    if (name[0] == '_') {
        name++;
    }
    for (i = 0; names[i] != NULL; i++) {
        if (strcmp(name, names[i]) == 0) {
            return 1;
        }
    }
    return strncmp(name, "CGContext", 9) == 0 ||
           strncmp(name, "CGRect", 6) == 0 ||
           strncmp(name, "CGPoint", 7) == 0 ||
           strncmp(name, "CGSize", 6) == 0 ||
           strncmp(name, "CGColor", 7) == 0 ||
           strncmp(name, "NSMake", 6) == 0;
}

void dr_gui_reset(void)
{
}

int masa_gui_read_pixel(unsigned char *rgba)
{
    (void)rgba;
    return 0;
}

#endif /* MASA_DANROOT_SDL2 */

#else /* _WIN32 */

int dr_gui_available(void)
{
    return 0;
}

void *dr_gui_class(const char *name)
{
    (void)name;
    return NULL;
}

int dr_gui_msgSend(void *recv, const char *name, va_list *ap, void **out)
{
    (void)recv;
    (void)name;
    (void)ap;
    (void)out;
    return 0;
}

void *dr_gui_lookup(const char *name)
{
    (void)name;
    return NULL;
}

int dr_gui_is_gui_symbol(const char *name)
{
    (void)name;
    return 0;
}

void dr_gui_reset(void)
{
}

int masa_gui_read_pixel(unsigned char *rgba)
{
    (void)rgba;
    return 0;
}

#endif /* _WIN32 */
