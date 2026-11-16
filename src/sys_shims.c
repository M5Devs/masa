/*
 * sys_shims.c - lightweight macOS environment shims.
 *
 * Build systems routinely shell out to uname, sw_vers, xcode-select and
 * xcrun to discover the platform and the SDK.  `masa sys <command>`
 * answers those queries on a non-macOS host.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _POSIX_C_SOURCE 200809L

#include "masa.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *env_or(const char *name, const char *fallback)
{
    const char *value = getenv(name);

    if (value != NULL && value[0] != '\0') {
        return value;
    }
    return fallback;
}

static const char *shim_sdk_path(void)
{
    return env_or(MASA_SDK_PATH_ENV, MASA_DEFAULT_SDK_PATH);
}

static const char *shim_machine(void)
{
    const char *target = env_or(MASA_TARGET_ENV, MASA_DEFAULT_TARGET);

    if (strncmp(target, "x86_64", 6) == 0) {
        return "x86_64";
    }
    return "arm64";
}

static int shim_uname(int argc, char *const argv[])
{
    const char *flag = (argc > 1) ? argv[1] : NULL;

    if (flag == NULL || strcmp(flag, "-s") == 0) {
        puts("Darwin");
        return 0;
    }
    if (strcmp(flag, "-m") == 0) {
        puts(shim_machine());
        return 0;
    }

    fprintf(stderr, "masa: uname: unsupported option '%s'\n", flag);
    return 1;
}

static int shim_sw_vers(int argc, char *const argv[])
{
    const char *flag = (argc > 1) ? argv[1] : NULL;

    if (flag == NULL) {
        printf("ProductName:\t%s\n", MASA_SW_PRODUCT_NAME);
        printf("ProductVersion:\t%s\n", MASA_SW_PRODUCT_VERSION);
        printf("BuildVersion:\t%s\n", MASA_SW_BUILD_VERSION);
        return 0;
    }
    if (strcmp(flag, "-productName") == 0) {
        puts(MASA_SW_PRODUCT_NAME);
        return 0;
    }
    if (strcmp(flag, "-productVersion") == 0) {
        puts(MASA_SW_PRODUCT_VERSION);
        return 0;
    }
    if (strcmp(flag, "-buildVersion") == 0) {
        puts(MASA_SW_BUILD_VERSION);
        return 0;
    }

    fprintf(stderr, "masa: sw_vers: unsupported option '%s'\n", flag);
    return 1;
}

static int shim_xcode_select(int argc, char *const argv[])
{
    const char *flag = (argc > 1) ? argv[1] : NULL;

    if (flag != NULL && strcmp(flag, "-p") == 0) {
        puts(shim_sdk_path());
        return 0;
    }

    fprintf(stderr, "masa: xcode-select: unsupported option '%s'\n",
            (flag != NULL) ? flag : "(none)");
    return 1;
}

static int shim_xcrun(int argc, char *const argv[])
{
    const char *flag = (argc > 1) ? argv[1] : NULL;

    if (flag != NULL && strcmp(flag, "--show-sdk-path") == 0) {
        puts(shim_sdk_path());
        return 0;
    }

    fprintf(stderr, "masa: xcrun: unsupported option '%s'\n",
            (flag != NULL) ? flag : "(none)");
    return 1;
}

int masa_sys_dispatch(int argc, char *const argv[])
{
    const char *command;

    if (argv == NULL || argc < 1 || argv[0] == NULL) {
        fprintf(stderr, "masa: sys: missing command\n");
        return 1;
    }

    command = argv[0];
    if (strcmp(command, "uname") == 0) {
        return shim_uname(argc, argv);
    }
    if (strcmp(command, "sw_vers") == 0) {
        return shim_sw_vers(argc, argv);
    }
    if (strcmp(command, "xcode-select") == 0) {
        return shim_xcode_select(argc, argv);
    }
    if (strcmp(command, "xcrun") == 0) {
        return shim_xcrun(argc, argv);
    }

    fprintf(stderr, "masa: sys: unsupported command '%s'\n", command);
    return 1;
}
