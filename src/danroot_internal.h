/*
 * danroot_internal.h - private interface between the Danroot loader
 * (src/danroot.c) and its clean-room runtime shims.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DANROOT_INTERNAL_H
#define DANROOT_INTERNAL_H

#include <stddef.h>

/*
 * Shim providers for the clean-room CoreFoundation and Objective-C
 * runtimes.  Each returns the address to bind for a symbol name (as it
 * appears in a Mach-O image, with the leading underscore) or NULL when it
 * does not own that name.  The loader consults them after its own table.
 */
void *dr_cf_lookup(const char *name);
void *dr_objc_lookup(const char *name);

/*
 * Free every object a shimmed runtime still holds.  The loader calls these
 * when a run finishes so a program that never released its objects still
 * leaves the process with nothing allocated (AddressSanitizer-clean).
 */
void dr_cf_reset(void);
void dr_objc_reset(void);

#endif /* DANROOT_INTERNAL_H */
