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
        " _opendir, _readdir, _closedir, _stat, _lstat, _fstat,"
        " _mach_absolute_time, _mach_timebase_info, _mach_task_self_,"
        " ___stack_chk_fail, ___stack_chk_guard,"
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
        "/* File-type bits, as on Darwin. */\n"
        "#define S_IFMT  0170000\n"
        "#define S_IFIFO 0010000\n"
        "#define S_IFCHR 0020000\n"
        "#define S_IFDIR 0040000\n"
        "#define S_IFBLK 0060000\n"
        "#define S_IFREG 0100000\n"
        "#define S_IFLNK 0120000\n"
        "#define S_IFSOCK 0140000\n"
        "\n"
        "#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)\n"
        "#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)\n"
        "#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)\n"
        "\n"
        "/*\n"
        " * The 64-bit-inode Darwin struct stat (LP64).  The shim fills this\n"
        " * layout in directly, so a program can read st_mode, st_size and the\n"
        " * times back without a translation layer.\n"
        " */\n"
        "struct timespec {\n"
        "    long tv_sec;\n"
        "    long tv_nsec;\n"
        "};\n"
        "\n"
        "struct stat {\n"
        "    int      st_dev;\n"
        "    unsigned short st_mode;\n"
        "    unsigned short st_nlink;\n"
        "    unsigned long long st_ino;\n"
        "    unsigned int st_uid;\n"
        "    unsigned int st_gid;\n"
        "    int      st_rdev;\n"
        "    struct timespec st_atimespec;\n"
        "    struct timespec st_mtimespec;\n"
        "    struct timespec st_ctimespec;\n"
        "    struct timespec st_birthtimespec;\n"
        "    long long st_size;\n"
        "    long long st_blocks;\n"
        "    int      st_blksize;\n"
        "    unsigned int st_flags;\n"
        "    unsigned int st_gen;\n"
        "    int      st_lspare;\n"
        "    long long st_qspare[2];\n"
        "};\n"
        "\n"
        "#define st_atime st_atimespec.tv_sec\n"
        "#define st_mtime st_mtimespec.tv_sec\n"
        "#define st_ctime st_ctimespec.tv_sec\n"
        "#define st_birthtime st_birthtimespec.tv_sec\n"
        "\n"
        "int mkdir(const char *path, int mode);\n"
        "int stat(const char *path, struct stat *buf);\n"
        "int lstat(const char *path, struct stat *buf);\n"
        "int fstat(int fd, struct stat *buf);\n"
        "\n"
        "#endif /* MASA_STUB_SYS_STAT_H */\n";
}

static const char *dirent_h(void)
{
    return
        "#ifndef MASA_STUB_DIRENT_H\n"
        "#define MASA_STUB_DIRENT_H\n"
        "\n"
        "#ifndef NULL\n"
        "#define NULL ((void *)0)\n"
        "#endif\n"
        "\n"
        "typedef struct __masa_DIR DIR;\n"
        "\n"
        "/* Directory entry types, as on Darwin. */\n"
        "#define DT_UNKNOWN 0\n"
        "#define DT_FIFO    1\n"
        "#define DT_CHR     2\n"
        "#define DT_DIR     4\n"
        "#define DT_BLK     6\n"
        "#define DT_REG     8\n"
        "#define DT_LNK    10\n"
        "#define DT_SOCK   12\n"
        "#define DT_WHT    14\n"
        "\n"
        "/* The 64-bit-inode Darwin struct dirent. */\n"
        "struct dirent {\n"
        "    unsigned long long d_ino;\n"
        "    unsigned long long d_seekoff;\n"
        "    unsigned short d_reclen;\n"
        "    unsigned short d_namlen;\n"
        "    unsigned char d_type;\n"
        "    char d_name[1024];\n"
        "};\n"
        "\n"
        "DIR *opendir(const char *path);\n"
        "struct dirent *readdir(DIR *dirp);\n"
        "int closedir(DIR *dirp);\n"
        "\n"
        "#endif /* MASA_STUB_DIRENT_H */\n";
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

static const char *stdint_h(void)
{
    return
        "#ifndef MASA_STUB_STDINT_H\n"
        "#define MASA_STUB_STDINT_H\n"
        "\n"
        "/* Clean-room fixed-width integer types for a 64-bit Darwin target. */\n"
        "typedef signed char        int8_t;\n"
        "typedef unsigned char      uint8_t;\n"
        "typedef short              int16_t;\n"
        "typedef unsigned short     uint16_t;\n"
        "typedef int                int32_t;\n"
        "typedef unsigned int       uint32_t;\n"
        "typedef long long          int64_t;\n"
        "typedef unsigned long long uint64_t;\n"
        "\n"
        "#endif /* MASA_STUB_STDINT_H */\n";
}

static const char *mach_mach_time_h(void)
{
    return
        "#ifndef MASA_STUB_MACH_MACH_TIME_H\n"
        "#define MASA_STUB_MACH_MACH_TIME_H\n"
        "\n"
        "#include <stdint.h>\n"
        "\n"
        "/*\n"
        " * Clean-room subset of <mach/mach_time.h>.  The Danroot shim reports\n"
        " * a 1:1 timebase (numer == denom == 1) over the host monotonic clock,\n"
        " * so mach_absolute_time() already counts nanoseconds.\n"
        " */\n"
        "struct mach_timebase_info {\n"
        "    uint32_t numer;\n"
        "    uint32_t denom;\n"
        "};\n"
        "typedef struct mach_timebase_info mach_timebase_info_data_t;\n"
        "\n"
        "uint64_t mach_absolute_time(void);\n"
        "int mach_timebase_info(mach_timebase_info_data_t *info);\n"
        "\n"
        "#endif /* MASA_STUB_MACH_MACH_TIME_H */\n";
}

static const char *mach_mach_h(void)
{
    return
        "#ifndef MASA_STUB_MACH_MACH_H\n"
        "#define MASA_STUB_MACH_MACH_H\n"
        "\n"
        "#include <stdint.h>\n"
        "\n"
        "/*\n"
        " * Clean-room subset of <mach/mach.h>: just enough for a program that\n"
        " * passes mach_task_self() around.  The port is a stable, non-zero\n"
        " * handle the shim hands out; no Mach IPC is performed.\n"
        " */\n"
        "typedef uint32_t mach_port_t;\n"
        "typedef mach_port_t task_t;\n"
        "typedef int kern_return_t;\n"
        "\n"
        "#define KERN_SUCCESS 0\n"
        "\n"
        "extern mach_port_t mach_task_self_;\n"
        "#define mach_task_self() mach_task_self_\n"
        "\n"
        "#endif /* MASA_STUB_MACH_MACH_H */\n";
}

/* ------------------------------------------------------------------ */
/* Clean-room CoreFoundation / Objective-C framework stubs            */
/* ------------------------------------------------------------------ */

/*
 * The CoreFoundation and Foundation stubs mirror the loader's clean-room
 * shims (src/danroot_cf.c, src/danroot_objc.c): the symbol lists are
 * exactly what those tables can resolve at run time, so an image linked
 * against them runs immediately under `masa run`.
 */
static const char *corefoundation_tbd(void)
{
    return
        "--- !tapi-tbd\n"
        "tbd-version:     4\n"
        "targets:         [ arm64-macos, x86_64-macos ]\n"
        "install-name:    '/System/Library/Frameworks/CoreFoundation.framework"
        "/CoreFoundation'\n"
        "current-version: 1.0\n"
        "compatibility-version: 150.0\n"
        "exports:\n"
        "  - targets:         [ arm64-macos, x86_64-macos ]\n"
        "    symbols:         [ _CFStringCreateWithCString,"
        " _CFStringGetCStringPtr, _CFStringGetCString, _CFStringGetLength,"
        " _CFShow, _CFRetain, _CFRelease, _kCFAllocatorDefault,"
        " _CFArrayCreate, _CFArrayGetCount, _CFArrayGetValueAtIndex ]\n"
        "...\n";
}

static const char *foundation_tbd(void)
{
    return
        "--- !tapi-tbd\n"
        "tbd-version:     4\n"
        "targets:         [ arm64-macos, x86_64-macos ]\n"
        "install-name:    '/System/Library/Frameworks/Foundation.framework"
        "/Foundation'\n"
        "current-version: 1.0\n"
        "compatibility-version: 300.0\n"
        "exports:\n"
        "  - targets:         [ arm64-macos, x86_64-macos ]\n"
        "    symbols:         [ _NSLog, _objc_getClass, _sel_registerName,"
        " _objc_msgSend, _objc_autoreleasePoolPush,"
        " _objc_autoreleasePoolPop ]\n"
        "...\n";
}

static const char *libobjc_tbd(void)
{
    return
        "--- !tapi-tbd\n"
        "tbd-version:     4\n"
        "targets:         [ arm64-macos, x86_64-macos ]\n"
        "install-name:    '/usr/lib/libobjc.A.dylib'\n"
        "current-version: 1.0\n"
        "compatibility-version: 1.0\n"
        "exports:\n"
        "  - targets:         [ arm64-macos, x86_64-macos ]\n"
        "    symbols:         [ _objc_getClass, _sel_registerName,"
        " _objc_msgSend, _objc_autoreleasePoolPush,"
        " _objc_autoreleasePoolPop, _NSLog ]\n"
        "...\n";
}

static const char *corefoundation_h(void)
{
    return
        "#ifndef MASA_STUB_COREFOUNDATION_H\n"
        "#define MASA_STUB_COREFOUNDATION_H\n"
        "\n"
        "/*\n"
        " * Clean-room subset of Apple's CoreFoundation.  Only the two\n"
        " * reference-counted types the Danroot shim models (CFString and\n"
        " * CFArray) plus retain/release are declared.\n"
        " */\n"
        "#include <stddef.h>\n"
        "\n"
        "typedef const void *CFTypeRef;\n"
        "typedef const struct __CFString *CFStringRef;\n"
        "typedef const struct __CFArray *CFArrayRef;\n"
        "typedef const struct __CFAllocator *CFAllocatorRef;\n"
        "typedef unsigned long CFIndex;\n"
        "typedef unsigned int CFStringEncoding;\n"
        "typedef unsigned char Boolean;\n"
        "\n"
        "/* kCFStringEncodingUTF8 */\n"
        "#define kCFStringEncodingUTF8 0x08000100\n"
        "\n"
        "extern const CFAllocatorRef kCFAllocatorDefault;\n"
        "\n"
        "CFStringRef CFStringCreateWithCString(CFAllocatorRef alloc,\n"
        "                                      const char *cStr,\n"
        "                                      CFStringEncoding encoding);\n"
        "const char *CFStringGetCStringPtr(CFStringRef s,\n"
        "                                  CFStringEncoding encoding);\n"
        "Boolean CFStringGetCString(CFStringRef s, char *buffer,\n"
        "                           CFIndex bufferSize,\n"
        "                           CFStringEncoding encoding);\n"
        "CFIndex CFStringGetLength(CFStringRef s);\n"
        "\n"
        "CFArrayRef CFArrayCreate(CFAllocatorRef alloc, const void **values,\n"
        "                         CFIndex numValues, const void *callBacks);\n"
        "CFIndex CFArrayGetCount(CFArrayRef a);\n"
        "const void *CFArrayGetValueAtIndex(CFArrayRef a, CFIndex idx);\n"
        "\n"
        "CFTypeRef CFRetain(CFTypeRef cf);\n"
        "void CFRelease(CFTypeRef cf);\n"
        "void CFShow(CFTypeRef obj);\n"
        "\n"
        "#endif /* MASA_STUB_COREFOUNDATION_H */\n";
}

static const char *foundation_h(void)
{
    return
        "#ifndef MASA_STUB_FOUNDATION_H\n"
        "#define MASA_STUB_FOUNDATION_H\n"
        "\n"
        "/*\n"
        " * Clean-room subset of Apple's Foundation.  NSLog is declared for\n"
        " * plain C as well, so a C program can drive the runtime directly;\n"
        " * the class interface is only visible to Objective-C.\n"
        " */\n"
        "#include <objc/objc.h>\n"
        "#include <objc/message.h>\n"
        "\n"
        "void NSLog(id format, ...);\n"
        "\n"
        "#ifdef __OBJC__\n"
        "@interface NSString : NSObject\n"
        "+ (id)stringWithUTF8String:(const char *)cStr;\n"
        "- (const char *)UTF8String;\n"
        "- (unsigned long)length;\n"
        "@end\n"
        "#endif\n"
        "\n"
        "#endif /* MASA_STUB_FOUNDATION_H */\n";
}

static const char *objc_objc_h(void)
{
    return
        "#ifndef MASA_STUB_OBJC_OBJC_H\n"
        "#define MASA_STUB_OBJC_OBJC_H\n"
        "\n"
        "/*\n"
        " * Clean-room subset of <objc/objc.h>: the object and class handle\n"
        " * types a program needs to name a class or send it a message.  The\n"
        " * types are usable from plain C; the class interface is guarded so a\n"
        " * C translation unit can include this header too.\n"
        " */\n"
        "#include <stddef.h>\n"
        "\n"
        "typedef struct objc_class *Class;\n"
        "typedef struct objc_object {\n"
        "    Class isa;\n"
        "} *id;\n"
        "typedef struct objc_selector *SEL;\n"
        "\n"
        "typedef signed char BOOL;\n"
        "#ifndef YES\n"
        "#define YES ((BOOL)1)\n"
        "#define NO  ((BOOL)0)\n"
        "#endif\n"
        "\n"
        "typedef long NSInteger;\n"
        "typedef unsigned long NSUInteger;\n"
        "\n"
        "#ifdef __OBJC__\n"
        "@class NSString;\n"
        "@interface NSObject\n"
        "- (id)init;\n"
        "- (id)retain;\n"
        "- (void)release;\n"
        "- (id)autorelease;\n"
        "@end\n"
        "#endif\n"
        "\n"
        "#endif /* MASA_STUB_OBJC_OBJC_H */\n";
}

static const char *objc_runtime_h(void)
{
    return
        "#ifndef MASA_STUB_OBJC_RUNTIME_H\n"
        "#define MASA_STUB_OBJC_RUNTIME_H\n"
        "\n"
        "/*\n"
        " * Clean-room subset of <objc/runtime.h>: class lookup, selector\n"
        " * registration and the autorelease-pool entry points.\n"
        " */\n"
        "#include <objc/objc.h>\n"
        "\n"
        "Class objc_getClass(const char *name);\n"
        "SEL sel_registerName(const char *name);\n"
        "\n"
        "void *objc_autoreleasePoolPush(void);\n"
        "void objc_autoreleasePoolPop(void *pool);\n"
        "\n"
        "#endif /* MASA_STUB_OBJC_RUNTIME_H */\n";
}

static const char *objc_message_h(void)
{
    return
        "#ifndef MASA_STUB_OBJC_MESSAGE_H\n"
        "#define MASA_STUB_OBJC_MESSAGE_H\n"
        "\n"
        "/*\n"
        " * Clean-room subset of <objc/message.h>.  objc_msgSend is variadic:\n"
        " * the message arguments follow the selector, and the shim reads the\n"
        " * ones the selector needs.\n"
        " */\n"
        "#include <objc/objc.h>\n"
        "\n"
        "id objc_msgSend(id self, SEL op, ...);\n"
        "\n"
        "#endif /* MASA_STUB_OBJC_MESSAGE_H */\n";
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
    { "usr/include/stdint.h", stdint_h },
    { "usr/include/unistd.h", unistd_h },
    { "usr/include/fcntl.h", fcntl_h },
    { "usr/include/dlfcn.h", dlfcn_h },
    { "usr/include/dirent.h", dirent_h },
    { "usr/include/sys/stat.h", sys_stat_h },
    { "usr/include/sys/utsname.h", sys_utsname_h },
    { "usr/include/sys/sysctl.h", sys_sysctl_h },
    { "usr/include/mach/mach_time.h", mach_mach_time_h },
    { "usr/include/mach/mach.h", mach_mach_h },
    /* Objective-C runtime: a stub library plus its headers. */
    { "usr/lib/libobjc.A.tbd", libobjc_tbd },
    { "usr/lib/libobjc.dylib.tbd", libobjc_tbd },
    { "usr/include/objc/objc.h", objc_objc_h },
    { "usr/include/objc/runtime.h", objc_runtime_h },
    { "usr/include/objc/message.h", objc_message_h },
    /* Frameworks the clean-room loader satisfies at run time. */
    { "System/Library/Frameworks/CoreFoundation.framework/CoreFoundation.tbd",
      corefoundation_tbd },
    { "System/Library/Frameworks/CoreFoundation.framework/Headers"
      "/CoreFoundation.h", corefoundation_h },
    { "System/Library/Frameworks/Foundation.framework/Foundation.tbd",
      foundation_tbd },
    { "System/Library/Frameworks/Foundation.framework/Headers/Foundation.h",
      foundation_h }
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
