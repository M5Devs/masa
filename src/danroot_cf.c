/*
 * danroot_cf.c - a clean-room, minimal CoreFoundation for the Masa Danroot
 * loader.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This is a small, self-contained stand-in for the parts of Apple's
 * CoreFoundation that a self-contained Darwin program might call.  It is a
 * clean-room implementation from the documented C API: nothing here is
 * derived from Apple's source.  Only two reference-counted types are
 * modelled, CFString and CFArray, plus the retain/release pair.
 *
 * The runtime keeps every live object on a single list.  CFRetain bumps the
 * count and CFRelease drops it, freeing at zero; when a run finishes the
 * loader calls dr_cf_reset() to free whatever a program left behind, so the
 * process stays leak-free even if the program never balanced its retains.
 */

#ifndef _WIN32

#define _POSIX_C_SOURCE 200809L

#endif

#include "danroot_internal.h"

#ifndef _WIN32

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * A CFTypeRef is an opaque pointer.  Every object starts with a small
 * header: the retain count, the concrete kind and the link in the live
 * list.  The layouts are private to this file.
 */
typedef struct dr_cf_base {
    unsigned int rc;
    int kind; /* 1 = string, 2 = array */
    struct dr_cf_base *next;
} dr_cf_base;

typedef struct {
    dr_cf_base base;
    char *utf8;    /* NUL-terminated copy of the text */
    size_t length; /* number of UTF-8 code units, excluding the NUL */
} dr_cf_string;

typedef struct {
    dr_cf_base base;
    const void **items; /* borrowed references to the elements */
    size_t count;
} dr_cf_array;

static dr_cf_base *dr_cf_objects; /* live-object list, newest first */

/*
 * kCFAllocatorDefault is a const pointer variable: a program that names it
 * loads the pointer stored at the symbol, so the symbol must hold a stable
 * non-NULL handle.  The allocator is otherwise ignored here.
 */
static int dr_cf_allocator_storage;
static void *dr_cf_allocator_default = &dr_cf_allocator_storage;

static dr_cf_base *dr_cf_link(dr_cf_base *b)
{
    b->next = dr_cf_objects;
    dr_cf_objects = b;
    return b;
}

static void dr_cf_unlink(dr_cf_base *b)
{
    dr_cf_base **p = &dr_cf_objects;

    while (*p != NULL) {
        if (*p == b) {
            *p = b->next;
            return;
        }
        p = &(*p)->next;
    }
}

static void dr_cf_destroy(dr_cf_base *b)
{
    if (b->kind == 1) {
        free(((dr_cf_string *)b)->utf8);
    } else if (b->kind == 2) {
        free(((dr_cf_array *)b)->items);
    }
    free(b);
}

/* --- CFString ----------------------------------------------------- */

static void *dr_cf_string_new(const char *cstr)
{
    dr_cf_string *s;
    size_t len;

    if (cstr == NULL) {
        return NULL;
    }
    len = strlen(cstr);
    s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    /* Own a copy, so the loader frees it regardless of the source's life. */
    s->utf8 = malloc(len + 1);
    if (s->utf8 == NULL) {
        free(s);
        return NULL;
    }
    memcpy(s->utf8, cstr, len + 1);
    s->length = len;
    s->base.rc = 1;
    s->base.kind = 1;
    dr_cf_link(&s->base);
    return s;
}

static void *dr_cf_CFStringCreateWithCString(void *alloc, const char *cstr,
                                             unsigned int encoding)
{
    (void)alloc;
    (void)encoding; /* the stub SDK only models the UTF-8 encoding */
    return dr_cf_string_new(cstr);
}

static const char *dr_cf_CFStringGetCStringPtr(void *ref, unsigned int encoding)
{
    (void)encoding;
    if (ref == NULL || ((dr_cf_base *)ref)->kind != 1) {
        return NULL;
    }
    return ((dr_cf_string *)ref)->utf8;
}

static int dr_cf_CFStringGetCString(void *ref, char *buf, long bufsize,
                                    unsigned int encoding)
{
    dr_cf_string *s;

    (void)encoding;
    if (ref == NULL || ((dr_cf_base *)ref)->kind != 1) {
        return 0;
    }
    s = (dr_cf_string *)ref;
    if (buf == NULL || bufsize < (long)s->length + 1) {
        return 0;
    }
    memcpy(buf, s->utf8, s->length + 1);
    return 1;
}

static long dr_cf_CFStringGetLength(void *ref)
{
    if (ref == NULL || ((dr_cf_base *)ref)->kind != 1) {
        return 0;
    }
    return (long)((dr_cf_string *)ref)->length;
}

/* --- CFArray ------------------------------------------------------ */

static void *dr_cf_CFArrayCreate(void *alloc, const void **values,
                                 long num_values, const void *callbacks)
{
    dr_cf_array *a;
    long n = num_values;

    (void)alloc;
    (void)callbacks; /* the stub keeps only borrowed references */
    if (n < 0) {
        n = 0;
    }
    a = calloc(1, sizeof(*a));
    if (a == NULL) {
        return NULL;
    }
    a->count = (size_t)n;
    if (n > 0) {
        a->items = malloc((size_t)n * sizeof(*a->items));
        if (a->items == NULL) {
            free(a);
            return NULL;
        }
        memcpy(a->items, values, (size_t)n * sizeof(*a->items));
    }
    a->base.rc = 1;
    a->base.kind = 2;
    dr_cf_link(&a->base);
    return a;
}

static long dr_cf_CFArrayGetCount(void *ref)
{
    if (ref == NULL || ((dr_cf_base *)ref)->kind != 2) {
        return 0;
    }
    return (long)((dr_cf_array *)ref)->count;
}

static const void *dr_cf_CFArrayGetValueAtIndex(void *ref, long idx)
{
    dr_cf_array *a;

    if (ref == NULL || ((dr_cf_base *)ref)->kind != 2) {
        return NULL;
    }
    a = (dr_cf_array *)ref;
    if (idx < 0 || (size_t)idx >= a->count) {
        return NULL;
    }
    return a->items[idx];
}

/* --- reference counting and diagnostics --------------------------- */

static void *dr_cf_retain(void *ref)
{
    if (ref != NULL) {
        ((dr_cf_base *)ref)->rc++;
    }
    return ref;
}

static void dr_cf_release(void *ref)
{
    dr_cf_base *b = (dr_cf_base *)ref;

    if (b == NULL) {
        return;
    }
    if (b->rc > 1) {
        b->rc--;
        return;
    }
    dr_cf_unlink(b);
    dr_cf_destroy(b);
}

/*
 * CFShow prints a short, human-readable description of an object to
 * stderr, the way Apple's does: a string shows its text, an array shows
 * its elements one per line.
 */
static void dr_cf_show(void *ref)
{
    if (ref == NULL) {
        fputs("(null)\n", stderr);
        return;
    }
    if (((dr_cf_base *)ref)->kind == 1) {
        fprintf(stderr, "%s\n", ((dr_cf_string *)ref)->utf8);
    } else if (((dr_cf_base *)ref)->kind == 2) {
        dr_cf_array *a = (dr_cf_array *)ref;
        size_t i;

        for (i = 0; i < a->count; i++) {
            dr_cf_show((void *)a->items[i]);
        }
    }
}

/* --- provider plumbing -------------------------------------------- */

static const struct {
    const char *name;
    void *addr;
} dr_cf_symbols[] = {
    { "_CFStringCreateWithCString", (void *)dr_cf_CFStringCreateWithCString },
    { "_CFStringGetCStringPtr",     (void *)dr_cf_CFStringGetCStringPtr },
    { "_CFStringGetCString",        (void *)dr_cf_CFStringGetCString },
    { "_CFStringGetLength",         (void *)dr_cf_CFStringGetLength },
    { "_CFShow",                    (void *)dr_cf_show },
    { "_CFRetain",                  (void *)dr_cf_retain },
    { "_CFRelease",                 (void *)dr_cf_release },
    { "_kCFAllocatorDefault",       (void *)&dr_cf_allocator_default },
    { "_CFArrayCreate",             (void *)dr_cf_CFArrayCreate },
    { "_CFArrayGetCount",           (void *)dr_cf_CFArrayGetCount },
    { "_CFArrayGetValueAtIndex",    (void *)dr_cf_CFArrayGetValueAtIndex },
    { NULL, NULL }
};

void *dr_cf_lookup(const char *name)
{
    size_t i;

    for (i = 0; dr_cf_symbols[i].name != NULL; i++) {
        if (strcmp(dr_cf_symbols[i].name, name) == 0) {
            return dr_cf_symbols[i].addr;
        }
    }
    return NULL;
}

void dr_cf_reset(void)
{
    while (dr_cf_objects != NULL) {
        dr_cf_base *b = dr_cf_objects;

        dr_cf_objects = b->next;
        dr_cf_destroy(b);
    }
}

#else /* _WIN32 */

void *dr_cf_lookup(const char *name)
{
    (void)name;
    return NULL;
}

void dr_cf_reset(void)
{
}

#endif /* _WIN32 */
