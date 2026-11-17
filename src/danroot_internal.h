/*
 * danroot_internal.h - private interface between the Danroot loader
 * (src/danroot.c) and its clean-room runtime shims.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DANROOT_INTERNAL_H
#define DANROOT_INTERNAL_H

#include <stdarg.h>
#include <stddef.h>
#include <string.h>

/*
 * Darwin's arm64 varargs ABI passes every variadic argument on the stack,
 * whereas the host AArch64 ABI reads them from the general-purpose and
 * vector registers.  A shim reached from a Darwin image is therefore entered
 * with the arguments on the stack, where a plain C `...` shim would read
 * stale registers and print garbage.  When Masa is built for aarch64 - the
 * ARM64 side of the cross-architecture bridge - a variadic shim is emitted
 * as a small assembly trampoline that captures the caller's stack pointer
 * and rebuilds the argument list from the stack, where Darwin left it.  On
 * any other host the native `...` shim is used unchanged.
 */
#if defined(__aarch64__)
#define DR_DARWIN_VARARGS 1
#else
#define DR_DARWIN_VARARGS 0
#endif

#if DR_DARWIN_VARARGS
/*
 * The host AArch64 va_list: the first field is the pointer to the stack area
 * holding arguments that did not fit in registers; the remaining fields
 * describe the register save areas.  Zeroing the register offsets makes
 * every argument be read from the stack, which is exactly where the Darwin
 * caller left it.  `ap` is a pointer because the host va_list is a struct
 * that would otherwise be copied by value.
 */
struct dr_host_va {
    void *stack;
    void *gr_top;
    void *vr_top;
    int gr_offs;
    int vr_offs;
};

static inline void dr_va_from_stack(va_list *ap, void *stack)
{
    struct dr_host_va v;

    v.stack = stack;
    v.gr_top = NULL;
    v.vr_top = NULL;
    v.gr_offs = 0;
    v.vr_offs = 0;
    memcpy((void *)ap, &v, sizeof(v));
}

/*
 * Emit the trampoline for one variadic shim.  `sym` is the symbol the
 * loader binds, `target` the C helper that receives the caller's stack
 * pointer, and `reg` the argument register that helper reads it from.
 */
#define DR_VARARG_TRAMPOLINE(sym, target, reg)                          \
    __asm__(".pushsection .text\n"                                      \
            ".globl " #sym "\n"                                         \
            ".p2align 2\n"                                              \
            ".type " #sym ", %function\n"                               \
            #sym ":\n"                                                  \
            "  mov " #reg ", sp\n"                                      \
            "  b " #target "\n"                                         \
            ".size " #sym ", .-" #sym "\n"                              \
            ".popsection\n")
#endif

/*
 * Shim providers for the clean-room CoreFoundation and Objective-C
 * runtimes.  Each returns the address to bind for a symbol name (as it
 * appears in a Mach-O image, with the leading underscore) or NULL when it
 * does not own that name.  The loader consults them after its own table.
 */
void *dr_cf_lookup(const char *name);
void *dr_objc_lookup(const char *name);

/*
 * Describe a shimmed object as a C string when one can be recovered.
 *
 *   dr_cf_string_utf8(p)  - the UTF-8 text of p when p is a live CFString,
 *                           otherwise NULL (pointer identity is checked
 *                           against the CF live list, so a C string that
 *                           merely looks like an object is never dereferenced).
 *   dr_objc_cstr_of(p)    - the text of an NSString/CFString object, or p
 *                           itself when it is a plain C string; used by
 *                           NSLog and the AppKit title/selector shims.
 */
const char *dr_cf_string_utf8(const void *p);
const char *dr_objc_cstr_of(const void *p);

/*
 * GUI backend (src/danroot_appkit.c): a clean-room AppKit/CoreGraphics
 * built over SDL2, so a self-contained Darwin GUI program can open a real
 * window and draw into it.  The SDL2 code is compiled only when
 * MASA_DANROOT_SDL2 is defined (the `make gui` / `make test-gui` builds);
 * otherwise the same translation unit provides these as inert stubs so the
 * core loader keeps building with no SDL2 dependency at all.
 *
 *   dr_gui_available()  - 1 when the SDL2 backend is compiled in.
 *   dr_gui_class(name)  - the class handle for an AppKit class, or NULL.
 *   dr_gui_msgSend()    - handle an AppKit selector on recv; returns 1 and
 *                         stores the result in *out when it owns recv.
 *   dr_gui_lookup(name) - the address of a CoreGraphics/AppKit symbol.
 *   dr_gui_is_gui_symbol(name) - 1 when name belongs to the GUI surface,
 *                         used to explain an unresolved import when the
 *                         SDL2 backend is absent.
 *   dr_gui_reset()      - release every window/context a run left behind.
 */
int dr_gui_available(void);
void *dr_gui_class(const char *name);
int dr_gui_msgSend(void *recv, const char *name, va_list *ap, void **out);
void *dr_gui_lookup(const char *name);
int dr_gui_is_gui_symbol(const char *name);
void dr_gui_reset(void);

/*
 * Free every object a shimmed runtime still holds.  The loader calls these
 * when a run finishes so a program that never released its objects still
 * leaves the process with nothing allocated (AddressSanitizer-clean).
 */
void dr_cf_reset(void);
void dr_objc_reset(void);

#endif /* DANROOT_INTERNAL_H */
