/*
 * danroot_objc.c - a clean-room, minimal Objective-C runtime and
 * Foundation shim for the Masa Danroot loader.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This is a small stand-in for the parts of the Objective-C runtime and
 * the Foundation class cluster that a self-contained Darwin program might
 * reach for.  It is a clean-room implementation from the documented C API
 * and the observable message-send ABI: nothing here is derived from
 * Apple's sources.  Only a handful of selectors on NSObject/NSString are
 * modelled, which is enough for the common "make a string, log it" path.
 *
 * Every object is kept on a live list and released by dr_objc_reset() when
 * a run finishes, so a program that forgets to balance its retains still
 * leaves nothing behind for AddressSanitizer to report.
 */

#include "danroot_internal.h"

#ifndef _WIN32

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* --- classes ------------------------------------------------------ */

typedef struct dr_objc_class {
    const char *name;
    struct dr_objc_class *super;
} dr_objc_class;

static dr_objc_class dr_class_nsobject = { "NSObject", NULL };
static dr_objc_class dr_class_nsstring = { "NSString", &dr_class_nsobject };
static dr_objc_class dr_class_nsarray = { "NSArray", &dr_class_nsobject };

static dr_objc_class *const dr_classes[] = {
    &dr_class_nsobject, &dr_class_nsstring, &dr_class_nsarray
};

/* --- selectors ---------------------------------------------------- */

typedef struct dr_objc_selector {
    char *name;
    struct dr_objc_selector *next;
} dr_objc_selector;

static dr_objc_selector *dr_selectors;

static void *dr_objc_sel_registerName(const char *name)
{
    dr_objc_selector *s;

    if (name == NULL) {
        return NULL;
    }
    for (s = dr_selectors; s != NULL; s = s->next) {
        if (strcmp(s->name, name) == 0) {
            return s;
        }
    }
    s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->name = strdup(name);
    if (s->name == NULL) {
        free(s);
        return NULL;
    }
    s->next = dr_selectors;
    dr_selectors = s;
    return s;
}

/* --- objects ------------------------------------------------------ */

typedef struct dr_objc_object {
    dr_objc_class *cls;
    unsigned int rc;
    char *cstr; /* NSString payload */
    struct dr_objc_object *live_next;
    struct dr_objc_object *pool_next;
} dr_objc_object;

static dr_objc_object *dr_objects; /* live objects, newest first */

typedef struct dr_autopool {
    dr_objc_object *objects;
    struct dr_autopool *prev;
} dr_autopool;

static dr_autopool *dr_pool_top;

static int dr_is_class(const void *p)
{
    size_t i;

    for (i = 0; i < sizeof(dr_classes) / sizeof(dr_classes[0]); i++) {
        if (p == (const void *)dr_classes[i]) {
            return 1;
        }
    }
    return 0;
}

static dr_objc_object *dr_object_new(dr_objc_class *cls)
{
    dr_objc_object *o = calloc(1, sizeof(*o));

    if (o == NULL) {
        return NULL;
    }
    o->cls = cls;
    o->rc = 1;
    o->live_next = dr_objects;
    dr_objects = o;
    return o;
}

static void dr_object_free(dr_objc_object *o)
{
    dr_objc_object **p = &dr_objects;

    while (*p != NULL) {
        if (*p == o) {
            *p = o->live_next;
            break;
        }
        p = &(*p)->live_next;
    }
    free(o->cstr);
    free(o);
}

static void dr_objc_retain_obj(dr_objc_object *o)
{
    if (o != NULL) {
        o->rc++;
    }
}

static void dr_objc_release_obj(dr_objc_object *o)
{
    if (o != NULL && o->rc > 0 && --o->rc == 0) {
        dr_object_free(o);
    }
}

static void dr_objc_autorelease_obj(dr_objc_object *o)
{
    if (o == NULL) {
        return;
    }
    if (dr_pool_top != NULL) {
        o->pool_next = dr_pool_top->objects;
        dr_pool_top->objects = o;
    }
    /* With no pool the object stays on the live list and is freed at reset. */
}

/* --- runtime entry points ----------------------------------------- */

static void *dr_objc_getClass(const char *name)
{
    size_t i;

    if (name == NULL) {
        return NULL;
    }
    for (i = 0; i < sizeof(dr_classes) / sizeof(dr_classes[0]); i++) {
        if (strcmp(dr_classes[i]->name, name) == 0) {
            return dr_classes[i];
        }
    }
    return NULL;
}

/*
 * objc_msgSend is variadic: the extra words are the message arguments,
 * read on demand for the one selector that takes a C string.
 */
static void *dr_objc_msgSend(void *recv, void *sel, ...)
{
    dr_objc_selector *s = (dr_objc_selector *)sel;
    const char *name = (s != NULL) ? s->name : NULL;

    if (recv == NULL || name == NULL) {
        return NULL; /* messaging nil yields nil */
    }

    if (dr_is_class(recv)) {
        dr_objc_class *cls = recv;

        if (strcmp(name, "alloc") == 0) {
            return dr_object_new(cls);
        }
        if (strcmp(name, "stringWithUTF8String:") == 0) {
            const char *cstr;
            dr_objc_object *o;
            va_list ap;

            va_start(ap, sel);
            cstr = va_arg(ap, const char *);
            va_end(ap);
            o = dr_object_new(&dr_class_nsstring);
            if (o == NULL) {
                return NULL;
            }
            o->cstr = strdup(cstr != NULL ? cstr : "");
            if (o->cstr == NULL) {
                dr_object_free(o);
                return NULL;
            }
            return o;
        }
        if (strcmp(name, "retain") == 0 || strcmp(name, "autorelease") == 0) {
            return recv;
        }
        return NULL;
    }

    {
        dr_objc_object *o = recv;

        if (strcmp(name, "init") == 0) {
            return o;
        }
        if (strcmp(name, "retain") == 0) {
            dr_objc_retain_obj(o);
            return o;
        }
        if (strcmp(name, "release") == 0) {
            dr_objc_release_obj(o);
            return NULL;
        }
        if (strcmp(name, "autorelease") == 0) {
            dr_objc_autorelease_obj(o);
            return o;
        }
        if (strcmp(name, "UTF8String") == 0) {
            return o->cstr;
        }
        if (strcmp(name, "length") == 0) {
            return (void *)(uintptr_t)(o->cstr != NULL ? strlen(o->cstr) : 0);
        }
        if (strcmp(name, "description") == 0) {
            return o->cstr != NULL ? (void *)o->cstr : (void *)o->cls->name;
        }
    }
    return NULL;
}

static void *dr_objc_autoreleasePoolPush(void)
{
    dr_autopool *pool = calloc(1, sizeof(*pool));

    if (pool == NULL) {
        return NULL;
    }
    pool->prev = dr_pool_top;
    dr_pool_top = pool;
    return pool;
}

static void dr_objc_autoreleasePoolPop(void *token)
{
    while (dr_pool_top != NULL) {
        dr_autopool *pool = dr_pool_top;
        dr_objc_object *o = pool->objects;

        while (o != NULL) {
            dr_objc_object *next = o->pool_next;

            o->pool_next = NULL;
            dr_objc_release_obj(o);
            o = next;
        }
        dr_pool_top = pool->prev;
        free(pool);
        if (pool == token) {
            break;
        }
    }
}

/*
 * The format argument to NSLog is normally an NSString, but a plain C
 * string is accepted too.  A pointer that names a live object is treated
 * as an NSString and its payload used; anything else is taken as a C
 * string directly.
 */
static const char *dr_objc_cstr_of(const void *p)
{
    dr_objc_object *o;

    for (o = dr_objects; o != NULL; o = o->live_next) {
        if ((const void *)o == p) {
            return o->cstr;
        }
    }
    return (const char *)p;
}

static void dr_objc_NSLog(void *format, ...)
{
    const char *fmt = dr_objc_cstr_of(format);
    time_t now;
    struct tm tmv;
    char ts[32];
    va_list ap;

    now = time(NULL);
    if (localtime_r(&now, &tmv) != NULL) {
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    } else {
        ts[0] = '\0';
    }
    fprintf(stderr, "%s Masa[%d] ", ts, (int)getpid());
    if (fmt != NULL) {
        va_start(ap, format);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    fputc('\n', stderr);
    fflush(stderr);
}

/* --- provider plumbing -------------------------------------------- */

static const struct {
    const char *name;
    void *addr;
} dr_objc_symbols[] = {
    { "_objc_getClass",              (void *)dr_objc_getClass },
    { "_sel_registerName",           (void *)dr_objc_sel_registerName },
    { "_objc_msgSend",               (void *)dr_objc_msgSend },
    { "_objc_autoreleasePoolPush",   (void *)dr_objc_autoreleasePoolPush },
    { "_objc_autoreleasePoolPop",    (void *)dr_objc_autoreleasePoolPop },
    { "_NSLog",                      (void *)dr_objc_NSLog },
    { NULL, NULL }
};

void *dr_objc_lookup(const char *name)
{
    size_t i;

    for (i = 0; dr_objc_symbols[i].name != NULL; i++) {
        if (strcmp(dr_objc_symbols[i].name, name) == 0) {
            return dr_objc_symbols[i].addr;
        }
    }
    return NULL;
}

void dr_objc_reset(void)
{
    dr_objc_selector *s;

    while (dr_pool_top != NULL) {
        dr_autopool *pool = dr_pool_top;

        dr_pool_top = pool->prev;
        free(pool);
    }
    while (dr_objects != NULL) {
        dr_objc_object *o = dr_objects;

        dr_objects = o->live_next;
        free(o->cstr);
        free(o);
    }
    while (dr_selectors != NULL) {
        s = dr_selectors;
        dr_selectors = s->next;
        free(s->name);
        free(s);
    }
}

#else /* _WIN32 */

void *dr_objc_lookup(const char *name)
{
    (void)name;
    return NULL;
}

void dr_objc_reset(void)
{
}

#endif /* _WIN32 */
