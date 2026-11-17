/*
 * sdk.c - inspect an SDK root and generate a clean-room stub SDK.
 *
 * The stub SDK is written from scratch here: a TAPI v4 text-based
 * definition (.tbd) that names the symbols a Mach-O image needs, plus a
 * few standard headers.  It contains no Apple code, so it is safe to
 * ship and enough for ld64.lld to produce real MH_EXECUTE images.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "masa.h"
#include "masa_compat.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MASA_PATH_MAX 4096

static const char *sdk_root(void)
{
    const char *value = getenv(MASA_SDK_PATH_ENV);

    if (value != NULL && value[0] != '\0') {
        return value;
    }
    return MASA_DEFAULT_SDK_PATH;
}

/* Create every directory in path, ignoring those that already exist. */
static int write_text_file(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");

    if (fp == NULL) {
        fprintf(stderr, "masa: cannot write '%s': %s\n", path, strerror(errno));
        return -1;
    }
    if (fputs(text, fp) == EOF || fclose(fp) != 0) {
        fprintf(stderr, "masa: cannot write '%s': %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Stub contents                                                      */
/* ------------------------------------------------------------------ */

/*
 * The .tbd files below are clean-room stubs: the symbol lists are the
 * names a normal C program references, written out here rather than
 * copied from any SDK.  install-name follows the convention a real
 * libSystem advertises so linked images record a plausible load command.
 *
 * The exported set is exactly what the Danroot loader (src/danroot.c)
 * can resolve at run time, so a program linked against this stub can be
 * executed immediately with `masa run`.
 */
static const char *libsystem_tbd(void)
{
    return
        "--- !tapi-tbd\n"
        "tbd-version:     4\n"
        "targets:         [ arm64-macos, x86_64-macos ]\n"
        "install-name:    '/usr/lib/libSystem.B.dylib'\n"
        "current-version: 1311.100.3\n"
        "compatibility-version: 1.0\n"
        "exports:\n"
        "  - targets:         [ arm64-macos, x86_64-macos ]\n"
        "    symbols:         [ _printf, _puts, _putchar, _fprintf, _fopen,"
        " _fclose, _fread, _fwrite, _fgets, _fputs, _fflush, _fseek, _ftell,"
        " _feof, _ferror, _snprintf, _vsnprintf, _sprintf, _sscanf,"
        " _write, _read, _open, _close, _lseek, _access, _unlink, _mkdir,"
        " _rmdir, _getcwd, _chdir,"
        " _malloc, _calloc, _realloc, _free, _strlen, _strcmp, _strncmp,"
        " _strcpy, _strncpy, _strcat, _strncat, _strchr, _strrchr, _strstr,"
        " _strdup, _strndup, _strerror, _memcpy, _memset, _memmove, _atoi,"
        " _getenv, _setenv, _unsetenv, _getpid, _getuid, _isatty, _usleep,"
        " _sleep, _abort, _exit, _uname, _sysctlbyname,"
        " _dlopen, _dlsym, _dlclose, _dlerror,"
        " ___stdinp, ___stdoutp, ___stderrp, dyld_stub_binder ]\n"
        "...\n";
}

static const char *libgcc_s_tbd(void)
{
    return
        "--- !tapi-tbd\n"
        "tbd-version:     4\n"
        "targets:         [ arm64-macos, x86_64-macos ]\n"
        "install-name:    '/usr/lib/libgcc_s.1.dylib'\n"
        "current-version: 1.0\n"
        "compatibility-version: 1.0\n"
        "exports:\n"
        "  - targets:         [ arm64-macos, x86_64-macos ]\n"
        "    symbols:         [ ]\n"
        "...\n";
}

static const char *crt1_tbd(void)
{
    return
        "--- !tapi-tbd\n"
        "tbd-version:     4\n"
        "targets:         [ arm64-macos, x86_64-macos ]\n"
        "install-name:    '/usr/lib/crt1.o'\n"
        "current-version: 1.0\n"
        "compatibility-version: 1.0\n"
        "exports:\n"
        "  - targets:         [ arm64-macos, x86_64-macos ]\n"
        "    symbols:         [ _main ]\n"
        "...\n";
}

/*
 * ld64.lld links a -dynamiclib against -ldylib1.o; the stub only has to
 * name the symbol so the link succeeds, because the Danroot loader does
 * not call __dyld_private itself.
 */
static const char *dylib1_tbd(void)
{
    return
        "--- !tapi-tbd\n"
        "tbd-version:     4\n"
        "targets:         [ arm64-macos, x86_64-macos ]\n"
        "install-name:    '/usr/lib/dylib1.o'\n"
        "current-version: 1.0\n"
        "compatibility-version: 1.0\n"
        "exports:\n"
        "  - targets:         [ arm64-macos, x86_64-macos ]\n"
        "    symbols:         [ __dyld_private ]\n"
        "...\n";
}

static const char *stdlib_h(void)
{
    return
        "#ifndef MASA_STUB_STDLIB_H\n"
        "#define MASA_STUB_STDLIB_H\n"
        "\n"
        "/* Clean-room stub of the standard declarations used by tests. */\n"
        "#define EXIT_SUCCESS 0\n"
        "#define EXIT_FAILURE 1\n"
        "#define NULL ((void *)0)\n"
        "\n"
        "typedef unsigned long size_t;\n"
        "\n"
        "void exit(int status);\n"
        "void abort(void);\n"
        "void *malloc(size_t size);\n"
        "void *calloc(size_t count, size_t size);\n"
        "void *realloc(void *ptr, size_t size);\n"
        "void free(void *ptr);\n"
        "int atoi(const char *s);\n"
        "char *getenv(const char *name);\n"
        "int setenv(const char *name, const char *value, int overwrite);\n"
        "int unsetenv(const char *name);\n"
        "int getpid(void);\n"
        "unsigned int getuid(void);\n"
        "\n"
        "#endif /* MASA_STUB_STDLIB_H */\n";
}

static const char *string_h(void)
{
    return
        "#ifndef MASA_STUB_STRING_H\n"
        "#define MASA_STUB_STRING_H\n"
        "\n"
        "#ifndef NULL\n"
        "#define NULL ((void *)0)\n"
        "#endif\n"
        "\n"
        "typedef unsigned long size_t;\n"
        "\n"
        "void *memcpy(void *dst, const void *src, size_t n);\n"
        "void *memset(void *dst, int c, size_t n);\n"
        "void *memmove(void *dst, const void *src, size_t n);\n"
        "size_t strlen(const char *s);\n"
        "int strcmp(const char *a, const char *b);\n"
        "int strncmp(const char *a, const char *b, size_t n);\n"
        "char *strcpy(char *dst, const char *src);\n"
        "char *strncpy(char *dst, const char *src, size_t n);\n"
        "char *strcat(char *dst, const char *src);\n"
        "char *strncat(char *dst, const char *src, size_t n);\n"
        "char *strchr(const char *s, int c);\n"
        "char *strrchr(const char *s, int c);\n"
        "char *strstr(const char *haystack, const char *needle);\n"
        "char *strdup(const char *s);\n"
        "char *strndup(const char *s, size_t n);\n"
        "char *strerror(int errnum);\n"
        "\n"
        "#endif /* MASA_STUB_STRING_H */\n";
}

static const char *stdio_h(void)
{
    return
        "#ifndef MASA_STUB_STDIO_H\n"
        "#define MASA_STUB_STDIO_H\n"
        "\n"
        "#ifndef NULL\n"
        "#define NULL ((void *)0)\n"
        "#endif\n"
        "\n"
        "typedef unsigned long size_t;\n"
        "typedef struct __masa_FILE FILE;\n"
        "\n"
        "#define EOF (-1)\n"
        "#define SEEK_SET 0\n"
        "#define SEEK_CUR 1\n"
        "#define SEEK_END 2\n"
        "\n"
        "extern FILE *stdin;\n"
        "extern FILE *stdout;\n"
        "extern FILE *stderr;\n"
        "\n"
        "int printf(const char *format, ...);\n"
        "int fprintf(FILE *stream, const char *format, ...);\n"
        "int sprintf(char *buf, const char *format, ...);\n"
        "int snprintf(char *buf, size_t n, const char *format, ...);\n"
        "int sscanf(const char *s, const char *format, ...);\n"
        "int puts(const char *s);\n"
        "int putchar(int c);\n"
        "FILE *fopen(const char *path, const char *mode);\n"
        "int fclose(FILE *stream);\n"
        "size_t fread(void *ptr, size_t size, size_t count, FILE *stream);\n"
        "size_t fwrite(const void *ptr, size_t size, size_t count,"
        " FILE *stream);\n"
        "char *fgets(char *s, int size, FILE *stream);\n"
        "int fputs(const char *s, FILE *stream);\n"
        "int fflush(FILE *stream);\n"
        "int fseek(FILE *stream, long offset, int whence);\n"
        "long ftell(FILE *stream);\n"
        "int feof(FILE *stream);\n"
        "int ferror(FILE *stream);\n"
        "\n"
        "#endif /* MASA_STUB_STDIO_H */\n";
}

static const char *unistd_h(void)
{
    return
        "#ifndef MASA_STUB_UNISTD_H\n"
        "#define MASA_STUB_UNISTD_H\n"
        "\n"
        "#ifndef NULL\n"
        "#define NULL ((void *)0)\n"
        "#endif\n"
        "\n"
        "typedef unsigned long size_t;\n"
        "typedef long ssize_t;\n"
        "typedef long off_t;\n"
        "\n"
        "int close(int fd);\n"
        "ssize_t read(int fd, void *buf, size_t n);\n"
        "ssize_t write(int fd, const void *buf, size_t n);\n"
        "off_t lseek(int fd, off_t offset, int whence);\n"
        "int access(const char *path, int mode);\n"
        "int unlink(const char *path);\n"
        "int rmdir(const char *path);\n"
        "char *getcwd(char *buf, size_t size);\n"
        "int chdir(const char *path);\n"
        "int isatty(int fd);\n"
        "int usleep(unsigned int usec);\n"
        "unsigned int sleep(unsigned int seconds);\n"
        "\n"
        "#endif /* MASA_STUB_UNISTD_H */\n";
}

static const char *fcntl_h(void)
{
    return
        "#ifndef MASA_STUB_FCNTL_H\n"
        "#define MASA_STUB_FCNTL_H\n"
        "\n"
        "#ifndef NULL\n"
        "#define NULL ((void *)0)\n"
        "#endif\n"
        "\n"
        "#define O_RDONLY 0\n"
        "#define O_WRONLY 1\n"
        "#define O_RDWR   2\n"
        "#define O_CREAT  0x0200\n"
        "#define O_TRUNC  0x0400\n"
        "#define O_APPEND 0x0008\n"
        "\n"
        "int open(const char *path, int flags, ...);\n"
        "\n"
        "#endif /* MASA_STUB_FCNTL_H */\n";
}

static const char *sys_stat_h(void)
{
    return
        "#ifndef MASA_STUB_SYS_STAT_H\n"
        "#define MASA_STUB_SYS_STAT_H\n"
        "\n"
        "#ifndef NULL\n"
        "#define NULL ((void *)0)\n"
        "#endif\n"
        "\n"
        "#define S_IRWXU 0700\n"
        "#define S_IRUSR 0400\n"
        "#define S_IWUSR 0200\n"
        "\n"
        "int mkdir(const char *path, int mode);\n"
        "\n"
        "#endif /* MASA_STUB_SYS_STAT_H */\n";
}

static const char *sys_utsname_h(void)
{
    return
        "#ifndef MASA_STUB_SYS_UTSNAME_H\n"
        "#define MASA_STUB_SYS_UTSNAME_H\n"
        "\n"
        "#ifndef NULL\n"
        "#define NULL ((void *)0)\n"
        "#endif\n"
        "\n"
        "struct utsname {\n"
        "    char sysname[256];\n"
        "    char nodename[256];\n"
        "    char release[256];\n"
        "    char version[256];\n"
        "    char machine[256];\n"
        "};\n"
        "\n"
        "int uname(struct utsname *buf);\n"
        "\n"
        "#endif /* MASA_STUB_SYS_UTSNAME_H */\n";
}

static const char *sys_sysctl_h(void)
{
    return
        "#ifndef MASA_STUB_SYS_SYSCTL_H\n"
        "#define MASA_STUB_SYS_SYSCTL_H\n"
        "\n"
        "#ifndef NULL\n"
        "#define NULL ((void *)0)\n"
        "#endif\n"
        "\n"
        "typedef unsigned long size_t;\n"
        "\n"
        "int sysctlbyname(const char *name, void *oldp, size_t *oldlenp,\n"
        "                 void *newp, size_t newlen);\n"
        "\n"
        "#endif /* MASA_STUB_SYS_SYSCTL_H */\n";
}

static const char *dlfcn_h(void)
{
    return
        "#ifndef MASA_STUB_DLFCN_H\n"
        "#define MASA_STUB_DLFCN_H\n"
        "\n"
        "#ifndef NULL\n"
        "#define NULL ((void *)0)\n"
        "#endif\n"
        "\n"
        "#define RTLD_LAZY   0x1\n"
        "#define RTLD_NOW    0x2\n"
        "#define RTLD_LOCAL  0x4\n"
        "#define RTLD_GLOBAL 0x8\n"
        "#define RTLD_DEFAULT ((void *)0)\n"
        "#define RTLD_NEXT    ((void *)-1)\n"
        "\n"
        "void *dlopen(const char *path, int mode);\n"
        "void *dlsym(void *handle, const char *symbol);\n"
        "int dlclose(void *handle);\n"
        "char *dlerror(void);\n"
        "\n"
        "#endif /* MASA_STUB_DLFCN_H */\n";
}

/* ------------------------------------------------------------------ */
/* sdk init-stub                                                      */
/* ------------------------------------------------------------------ */

struct stub_file {
    const char *relpath;
    const char *(*contents)(void);
};

static const struct stub_file stub_files[] = {
    { "usr/lib/libSystem.tbd", libsystem_tbd },
    { "usr/lib/libSystem.B.tbd", libsystem_tbd },
    { "usr/lib/libgcc_s.1.tbd", libgcc_s_tbd },
    { "usr/lib/dylib1.o", dylib1_tbd },
    { "usr/lib/crt1.o", crt1_tbd },
    { "usr/include/stdlib.h", stdlib_h },
    { "usr/include/string.h", string_h },
    { "usr/include/stdio.h", stdio_h },
    { "usr/include/unistd.h", unistd_h },
    { "usr/include/fcntl.h", fcntl_h },
    { "usr/include/dlfcn.h", dlfcn_h },
    { "usr/include/sys/stat.h", sys_stat_h },
    { "usr/include/sys/utsname.h", sys_utsname_h },
    { "usr/include/sys/sysctl.h", sys_sysctl_h }
};

static int sdk_init_stub(const char *dir)
{
    size_t i;

    if (dir == NULL || dir[0] == '\0') {
        fprintf(stderr, "masa: sdk init-stub: missing directory\n");
        return 2;
    }

    for (i = 0; i < sizeof(stub_files) / sizeof(stub_files[0]); i++) {
        char path[MASA_PATH_MAX];
        char parent[MASA_PATH_MAX];
        const char *slash;

        if (snprintf(path, sizeof(path), "%s/%s", dir,
                     stub_files[i].relpath) >= (int)sizeof(path)) {
            fprintf(stderr, "masa: sdk init-stub: path too long\n");
            return 1;
        }

        /* Create the containing directory before writing the file. */
        slash = strrchr(path, '/');
        if (slash != NULL) {
            size_t len = (size_t)(slash - path);
            memcpy(parent, path, len);
            parent[len] = '\0';
            if (masa_mkdir_p(parent) != 0) {
                fprintf(stderr, "masa: sdk init-stub: cannot create '%s': %s\n",
                        parent, strerror(errno));
                return 1;
            }
        }

        if (write_text_file(path, stub_files[i].contents()) != 0) {
            return 1;
        }
    }

    printf("masa: wrote stub SDK to %s\n", dir);
    printf("  %s/%s\n", dir, MASA_SDK_LIBSYSTEM_RELPATH);
    printf("Use it with: MASA_SDK_PATH=%s masa exec make\n", dir);
    return 0;
}

/* ------------------------------------------------------------------ */
/* sdk info                                                           */
/* ------------------------------------------------------------------ */

static int sdk_info(void)
{
    const char *root = sdk_root();
    char libpath[MASA_PATH_MAX];
    struct stat st;
    int ready;

    if (snprintf(libpath, sizeof(libpath), "%s/%s", root,
                 MASA_SDK_LIBSYSTEM_RELPATH) >= (int)sizeof(libpath)) {
        fprintf(stderr, "masa: sdk info: path too long\n");
        return 1;
    }

    ready = (stat(libpath, &st) == 0 && S_ISREG(st.st_mode));

    printf("SDK root:      %s\n", root);
    printf("libSystem.tbd: %s\n", libpath);
    printf("status:        %s\n", ready ? "ready" : "missing libSystem.tbd");
    if (!ready) {
        printf("hint:          run `masa sdk init-stub %s` to create a stub\n",
               root);
    }
    return ready ? 0 : 1;
}

int masa_sdk_main(int argc, char *const argv[])
{
    if (argc < 1 || argv[0] == NULL || argv[0][0] == '\0') {
        fprintf(stderr, "masa: sdk: missing subcommand (info | init-stub)\n");
        return 2;
    }

    if (strcmp(argv[0], "info") == 0) {
        return sdk_info();
    }
    if (strcmp(argv[0], "init-stub") == 0) {
        return sdk_init_stub(argc > 1 ? argv[1] : NULL);
    }

    fprintf(stderr, "masa: sdk: unknown subcommand '%s'\n", argv[0]);
    return 2;
}
