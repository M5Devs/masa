/*
 * doctor.c - `masa doctor`: report whether this machine can build macOS
 * binaries locally.
 *
 * Masa is meant to be self-sufficient: given a host clang and LLVM's
 * ld64.lld, a developer can produce Mach-O images without GitHub Actions,
 * a Mac, or Xcode.  This command inspects the local toolchain, the SDK
 * and the target and prints a `[OK]` / `[WARN]` / `[MISSING]` line for
 * each, then exits 0 when the core requirements (a C compiler and a
 * Mach-O-capable linker) are met.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "masa.h"
#include "masa_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Status tags, kept short so a column of them stays readable. */
#define MASA_DOCTOR_OK      "[OK]     "
#define MASA_DOCTOR_WARN    "[WARN]   "
#define MASA_DOCTOR_MISSING "[MISSING]"

static int regular_file(const char *path)
{
    struct stat st;

    return (path != NULL && path[0] != '\0' && stat(path, &st) == 0 &&
            S_ISREG(st.st_mode));
}

/* Print one "<tag> <label>: <value>" line, with an optional trailing hint. */
static void report(const char *tag, const char *label, const char *value)
{
    printf("%s %-14s %s\n", tag, label, (value != NULL) ? value : "");
}

/*
 * Report a tool located by masa_locate_tool().  `required` selects the
 * status used when it is absent: [MISSING] for the core tools (which make
 * doctor fail) and [WARN] for the optional ones.
 */
static int report_tool(const char *label, const char *prog_name,
                       const char *const fallbacks[], int required)
{
    char *path = masa_locate_tool(prog_name, fallbacks);

    if (path != NULL) {
        report(MASA_DOCTOR_OK, label, path);
        free(path);
        return 1;
    }
    report(required ? MASA_DOCTOR_MISSING : MASA_DOCTOR_WARN, label,
           "not found");
    return 0;
}

static const char *const ld_fallbacks[] = {"ld64.lld", "lld", NULL};
static const char *const ar_fallbacks[] = {"llvm-ar", "ar", NULL};
static const char *const ranlib_fallbacks[] = {"llvm-ranlib", "ranlib", NULL};
static const char *const nm_fallbacks[] = {"llvm-nm", "nm", NULL};
static const char *const objdump_fallbacks[] = {"llvm-objdump", "objdump", NULL};
static const char *const qemu_fallbacks[] = {"qemu-aarch64",
                                             "qemu-aarch64-static", NULL};
static const char *const aarch64_cc_fallbacks[] = {"aarch64-linux-gnu-gcc",
                                                   NULL};

/*
 * Cross-architecture Apple Silicon readiness: an arm64 Mach-O image can run
 * on an x86_64 host through qemu-aarch64 driving an aarch64 build of Masa.
 * Both pieces are optional, so an absent one is a [WARN], not a failure.
 */
static void check_cross_arch(void)
{
    report_tool("qemu-aarch64", "qemu-aarch64", qemu_fallbacks, 0);
    report_tool("aarch64-gcc", "aarch64-linux-gnu-gcc", aarch64_cc_fallbacks,
                0);
}

static int check_compiler(void)
{
    char *cc = masa_resolve_cc();

    if (cc != NULL) {
        report(MASA_DOCTOR_OK, "C compiler", cc);
        free(cc);
        return 1;
    }
    report(MASA_DOCTOR_MISSING, "C compiler",
           "no clang/cc/gcc found (set MASA_REAL_CC)");
    return 0;
}

static int check_sdk(void)
{
    const char *root = getenv(MASA_SDK_PATH_ENV);
    char libsystem[MASA_PATH_MAX];
    char crt1[MASA_PATH_MAX];
    int have_lib;
    int have_crt1;

    if (root == NULL || root[0] == '\0') {
        root = MASA_DEFAULT_SDK_PATH;
    }

    if (snprintf(libsystem, sizeof(libsystem), "%s/%s", root,
                 MASA_SDK_LIBSYSTEM_RELPATH) >= (int)sizeof(libsystem) ||
        snprintf(crt1, sizeof(crt1), "%s/usr/lib/crt1.o", root) >=
            (int)sizeof(crt1)) {
        report(MASA_DOCTOR_WARN, "SDK", "path too long");
        return 0;
    }

    have_lib = regular_file(libsystem);
    have_crt1 = regular_file(crt1);

    if (have_lib && have_crt1) {
        char detail[MASA_PATH_MAX * 2];

        snprintf(detail, sizeof(detail), "%s (libSystem.tbd + crt1.o)", root);
        report(MASA_DOCTOR_OK, "SDK", detail);
        return 1;
    }

    {
        char detail[MASA_PATH_MAX * 2];
        const char *missing = have_lib ? "usr/lib/crt1.o"
                                       : (have_crt1 ? "usr/lib/libSystem.tbd"
                                                    : "usr/lib/libSystem.tbd "
                                                      "and usr/lib/crt1.o");

        snprintf(detail, sizeof(detail), "%s missing %s", root, missing);
        report(MASA_DOCTOR_WARN, "SDK", detail);
    }
    printf("         hint: run `masa sdk init-stub %s`\n", root);
    return 0;
}

int masa_doctor_main(int argc, char *const argv[])
{
    const char *target = getenv(MASA_TARGET_ENV);
    int have_cc;
    int have_ld;

    (void)argc;
    (void)argv;

    printf("masa %s doctor - local macOS cross-build readiness\n\n",
           MASA_VERSION);

    have_cc = check_compiler();
    have_ld = report_tool("linker", "ld64.lld", ld_fallbacks, 1);

    report_tool("archiver", "llvm-ar", ar_fallbacks, 0);
    report_tool("ranlib", "llvm-ranlib", ranlib_fallbacks, 0);
    report_tool("nm", "llvm-nm", nm_fallbacks, 0);
    report_tool("objdump", "llvm-objdump", objdump_fallbacks, 0);

    {
        char *swiftc = masa_resolve_swiftc();

        if (swiftc != NULL) {
            report(MASA_DOCTOR_OK, "swiftc", swiftc);
            free(swiftc);
        } else {
            report(MASA_DOCTOR_WARN, "swiftc",
                   "not found (optional; only for Swift targets)");
        }
    }

    check_sdk();
    check_cross_arch();

    report(MASA_DOCTOR_OK, "target",
           (target != NULL && target[0] != '\0') ? target
                                                 : MASA_DEFAULT_TARGET);

    printf("\n");
    if (have_cc && have_ld) {
        printf("result: core C cross-build ready (clang + ld64.lld)\n");
        return 0;
    }
    printf("result: cannot build Mach-O images locally; install clang and "
           "LLVM's ld64.lld\n");
    return 1;
}
