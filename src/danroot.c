/*
 * danroot.c - run a Mach-O image (and its dylibs) on a POSIX host.
 *
 * `masa run <binary> [args...]` (and its alias `masa danroot`) loads a
 * thin 64-bit Mach-O executable built by `masa cc` straight into the
 * caller's address space and runs it, without a virtual machine, a
 * kernel module or a full Darwin userland.
 *
 * This is the Masa Danroot loader.  It is a clean-room reimplementation
 * of the parts of dyld a self-contained program needs:
 *
 *   - read the load commands and map every LC_SEGMENT_64 with mmap(),
 *   - follow LC_LOAD_DYLIB / LC_LOAD_WEAK_DYLIB / LC_REEXPORT_DYLIB,
 *     resolving @executable_path / @loader_path / @rpath (via LC_RPATH),
 *     absolute and MASA_LIBRARY_PATH / DYLD_LIBRARY_PATH references,
 *     mapping each .dylib and indexing its exports (LC_DYLD_EXPORTS_TRIE
 *     and LC_SYMTAB),
 *   - apply LC_DYLD_CHAINED_FIXUPS and LC_DYLD_INFO_ONLY rebases and
 *     symbol binds, resolving imports against a clean-room libSystem
 *     shim and against the exports of every loaded image,
 *   - call the LC_MAIN entry point with (argc, argv, envp, apple) and
 *     return the value it produces as the process exit status.
 *
 * /usr/lib/libSystem.B.dylib and /usr/lib/libSystem.dylib are never read
 * from disk: the built-in shim satisfies them.
 *
 * A Masa-linked program contains no raw syscall instructions - every
 * libc call goes through the shim - so no trap interception is needed
 * for normal C code.  The loader therefore runs entirely in-process and
 * is architecture-matched: an x86_64 host runs x86_64 images and an
 * arm64 host runs arm64 images.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
/* Exposes struct dirent.d_type, which the directory shim reads. */
#define _DEFAULT_SOURCE 1
#endif

#include "masa.h"
#include "masa_compat.h"

#include <stdio.h>

#ifdef _WIN32

/*
 * The loader is a POSIX-only facility: it maps the image with mmap() and
 * relies on the host's own calling convention matching the target's.
 * Windows keeps the subcommand visible but explains the limitation, so
 * the MinGW cross-build stays warning-free.
 */
int masa_danroot_main(int argc, char *const argv[])
{
    (void)argc;
    (void)argv;
    fprintf(stderr,
            "masa: run is currently supported on POSIX hosts "
            "(Linux, Android/Termux)\n");
    return 2;
}

#else /* !_WIN32 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* The process environment, passed to the loaded image as envp[]. */
extern char **environ;

/* ------------------------------------------------------------------ */
/* Mach-O constants                                                   */
/* ------------------------------------------------------------------ */

#define DR_MH_MAGIC_64              0xfeedfacfu
#define DR_MH_EXECUTE               2u
#define DR_MH_DYLIB                 6u

#define DR_CPU_TYPE_X86_64          0x01000007u
#define DR_CPU_TYPE_ARM64           0x0100000cu

#define DR_LC_SEGMENT_64            0x19u
#define DR_LC_SYMTAB                0x02u
#define DR_LC_LOAD_DYLIB            0x0cu
#define DR_LC_ID_DYLIB              0x0du
#define DR_LC_LOAD_WEAK_DYLIB       0x80000018u
#define DR_LC_RPATH                 0x8000001cu
#define DR_LC_REEXPORT_DYLIB        0x8000001fu
#define DR_LC_DYLD_INFO_ONLY        0x80000022u
#define DR_LC_MAIN                  0x80000028u
#define DR_LC_DYLD_EXPORTS_TRIE     0x80000033u
#define DR_LC_DYLD_CHAINED_FIXUPS   0x80000034u

/* dyld_chained_fixups_header.imports_format values. */
#define DR_CHAINED_IMPORT           1u

/* dyld_chained_starts_in_segment.pointer_format values. */
#define DR_CHAINED_PTR_64           2u
#define DR_CHAINED_PTR_64_OFFSET    6u

/* page_start[] sentinel: no fixups on this page. */
#define DR_CHAINED_PTR_START_NONE   0xffffu

/* Classic (LC_DYLD_INFO) rebase/bind opcodes. */
#define DR_REBASE_OPCODE_MASK       0xf0u
#define DR_REBASE_IMMEDIATE_MASK    0x0fu
#define DR_REBASE_OPCODE_DONE       0x00u
#define DR_REBASE_OPCODE_SET_TYPE_IMM 0x10u
#define DR_REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB 0x20u
#define DR_REBASE_OPCODE_ADD_ADDR_ULEB 0x30u
#define DR_REBASE_OPCODE_ADD_ADDR_IMM_SCALED 0x40u
#define DR_REBASE_OPCODE_DO_REBASE_IMM_TIMES 0x50u
#define DR_REBASE_OPCODE_DO_REBASE_ULEB_TIMES 0x60u
#define DR_REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB 0x70u
#define DR_REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB 0x80u

#define DR_BIND_OPCODE_MASK         0xf0u
#define DR_BIND_IMMEDIATE_MASK      0x0fu
#define DR_BIND_OPCODE_DONE         0x00u
#define DR_BIND_OPCODE_SET_DYLIB_ORDINAL_IMM 0x10u
#define DR_BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB 0x20u
#define DR_BIND_OPCODE_SET_DYLIB_SPECIAL_IMM 0x30u
#define DR_BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM 0x40u
#define DR_BIND_OPCODE_SET_TYPE_IMM 0x50u
#define DR_BIND_OPCODE_SET_ADDEND_SLEB 0x60u
#define DR_BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB 0x70u
#define DR_BIND_OPCODE_ADD_ADDR_ULEB 0x80u
#define DR_BIND_OPCODE_DO_BIND        0x90u
#define DR_BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB 0xa0u
#define DR_BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED 0xb0u
#define DR_BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB 0xc0u
#define DR_BIND_OPCODE_THREADED      0xd0u

/* BIND_SYMBOL_FLAGS_* (the immediate of SET_SYMBOL_TRAILING_FLAGS_IMM). */
#define DR_BIND_SYMBOL_FLAGS_WEAK_IMPORT 0x1u

/* BIND_TYPE_* (the low nibble of SET_TYPE_IMM for binds). */
#define DR_BIND_TYPE_POINTER        1u
#define DR_BIND_TYPE_TEXT_ABSOLUTE32 2u
#define DR_BIND_TYPE_POINTER32      3u

/* VM protection bits, matching the Mach-O segment initial protection. */
#define DR_VM_PROT_READ             0x1u
#define DR_VM_PROT_WRITE            0x2u
#define DR_VM_PROT_EXECUTE          0x4u

/* Export trie node flags. */
#define DR_EXPORT_FLAG_REEXPORT     0x08u
#define DR_EXPORT_FLAG_STUB_RESOLVER 0x10u

/* nlist_64.n_type bits. */
#define DR_N_STAB                   0xe0u
#define DR_N_TYPE                   0x0eu
#define DR_N_UNDF                   0x00u
#define DR_N_EXT                    0x01u

/* Fixup pass selector. */
#define DR_FIX_REBASE               1
#define DR_FIX_BIND                 2

#define DR_MAX_SEGMENTS             32
#define DR_MAX_DYLIBS               32
#define DR_MAX_RPATHS               16
#define DR_PATH_MAX                 4096
#define DR_NAME_MAX                 512

/* ------------------------------------------------------------------ */
/* Little-endian readers (hosts Masa targets are little-endian)       */
/* ------------------------------------------------------------------ */

static uint32_t dr_rd32(const unsigned char *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint64_t dr_rd64(const unsigned char *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint16_t dr_rd16(const unsigned char *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint64_t dr_round_up(uint64_t value, uint64_t align)
{
    return (value + align - 1) & ~(align - 1);
}

static void dr_strlcpy(char *dst, const char *src, size_t cap)
{
    if (cap == 0) {
        return;
    }
    while (cap > 1 && *src != '\0') {
        *dst++ = *src++;
        cap--;
    }
    *dst = '\0';
}

/*
 * Bounded path formatting.  Kept in one place so the call sites stay
 * free of -Wformat-truncation noise; vsnprintf always NUL-terminates.
 */
static int dr_pathfmt(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n;
}

/* ULEB128 / SLEB128, used by the classic dyld rebase/bind opcodes. */
static uint64_t dr_uleb128(const unsigned char **p, const unsigned char *end)
{
    uint64_t result = 0;
    unsigned shift = 0;

    while (*p < end) {
        unsigned char byte = *(*p)++;

        result |= (uint64_t)(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0) {
            break;
        }
        shift += 7;
        if (shift >= 64) {
            break;
        }
    }
    return result;
}

static int64_t dr_sleb128(const unsigned char **p, const unsigned char *end)
{
    int64_t result = 0;
    unsigned shift = 0;
    unsigned char byte = 0;

    while (*p < end) {
        byte = *(*p)++;
        result |= (int64_t)(byte & 0x7fu) << shift;
        shift += 7;
        if ((byte & 0x80u) == 0) {
            break;
        }
        if (shift >= 64) {
            break;
        }
    }
    if (shift < 64 && (byte & 0x40u) != 0) {
        result |= -((int64_t)1 << shift);
    }
    return result;
}

/* ------------------------------------------------------------------ */
/* Path virtualization (`--root` / MASA_DANROOT_ROOT)                  */
/* ------------------------------------------------------------------ */

/*
 * A proot-style virtual root.  When active, an absolute path that names a
 * macOS system location - a per-user home directory, an application bundle
 * directory, the shared libraries and the Homebrew prefixes - is rewritten
 * to live beneath the virtual root, so a program that expects to write
 * /Applications/MyApp or read /Users/me/hello.txt only ever touches files
 * under <root>.  Host paths outside those prefixes (and every relative
 * path) are left exactly as they are, so ordinary Linux/Android I/O keeps
 * working.
 */

static char dr_root[DR_PATH_MAX];
static int dr_root_active;

/* macOS locations a Masa-linked program may reference by their Darwin
 * path.  Each entry is matched as one or more whole path components, so
 * "/Users" never captures "/Usersomething". */
static const char *const dr_root_prefixes[] = {
    "/Users",
    "/Applications",
    "/Library",
    "/System",
    "/opt/homebrew",
    "/usr/local",
    NULL
};

static void dr_root_set(const char *dir)
{
    size_t len;

    if (dir == NULL || dir[0] == '\0') {
        dr_root_active = 0;
        dr_root[0] = '\0';
        return;
    }
    dr_strlcpy(dr_root, dir, sizeof(dr_root));
    len = strlen(dr_root);
    while (len > 1 && dr_root[len - 1] == '/') {
        dr_root[--len] = '\0';
    }
    dr_root_active = 1;
}

/*
 * Translate a program-supplied path for the host.  When no virtual root is
 * active, a relative path, or a path outside the macOS prefixes, the input
 * pointer is returned unchanged.  Otherwise the rewritten path is written
 * to buf and returned; NULL (with errno set) means the rewrite overflowed.
 */
static const char *dr_vpath(const char *path, char *buf, size_t cap)
{
    size_t i;

    if (!dr_root_active || path == NULL || path[0] != '/') {
        return path;
    }
    for (i = 0; dr_root_prefixes[i] != NULL; i++) {
        size_t plen = strlen(dr_root_prefixes[i]);

        if (strncmp(path, dr_root_prefixes[i], plen) != 0) {
            continue;
        }
        if (path[plen] != '\0' && path[plen] != '/') {
            continue; /* a different component, e.g. /Usersfoo */
        }
        /* Keep the whole Darwin path beneath the root. */
        if (dr_pathfmt(buf, cap, "%s%s", dr_root, path) >= (int)cap) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        return buf;
    }
    return path;
}

/* ------------------------------------------------------------------ */
/* Darwin ABI views of the host structures the shims translate         */
/* ------------------------------------------------------------------ */

/*
 * The target is a 64-bit Darwin process, so the structures it reads back
 * must use the LP64 Darwin layout, not the host's.  These are written out
 * from the documented XNU ABI (sys/stat.h, sys/dirent.h), not copied from
 * any SDK header.
 */
struct dr_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct dr_darwin_stat {
    int32_t  st_dev;
    uint16_t st_mode;
    uint16_t st_nlink;
    uint64_t st_ino;
    uint32_t st_uid;
    uint32_t st_gid;
    int32_t  st_rdev;
    struct dr_timespec st_atimespec;
    struct dr_timespec st_mtimespec;
    struct dr_timespec st_ctimespec;
    struct dr_timespec st_birthtimespec;
    int64_t  st_size;
    int64_t  st_blocks;
    int32_t  st_blksize;
    uint32_t st_flags;
    uint32_t st_gen;
    int32_t  st_lspare;
    int64_t  st_qspare[2];
};

/* Darwin's 64-bit-inode struct dirent: d_name begins at byte 21. */
struct dr_darwin_dirent {
    uint64_t d_ino;
    uint64_t d_seekoff;
    uint16_t d_reclen;
    uint16_t d_namlen;
    uint8_t  d_type;
    char     d_name[1024];
};

/*
 * These layouts are the whole contract with the loaded program: a wrong
 * size or offset would silently corrupt every stat()/readdir() result.
 * The numbers are the documented 64-bit Darwin ABI (144-byte struct stat,
 * st_size at 96; struct dirent with d_name at 21).
 */
#if defined(__x86_64__) || defined(__aarch64__)
_Static_assert(sizeof(struct dr_darwin_stat) == 144, "Darwin struct stat");
_Static_assert(offsetof(struct dr_darwin_stat, st_size) == 96,
               "Darwin st_size offset");
_Static_assert(offsetof(struct dr_darwin_stat, st_mode) == 4,
               "Darwin st_mode offset");
_Static_assert(offsetof(struct dr_darwin_dirent, d_name) == 21,
               "Darwin d_name offset");
_Static_assert(offsetof(struct dr_darwin_dirent, d_type) == 20,
               "Darwin d_type offset");
#endif

static void dr_stat_to_darwin(const struct stat *h, struct dr_darwin_stat *d)
{
    memset(d, 0, sizeof(*d));
    d->st_dev = (int32_t)h->st_dev;
    d->st_mode = (uint16_t)h->st_mode;
    d->st_nlink = (uint16_t)h->st_nlink;
    d->st_ino = (uint64_t)h->st_ino;
    d->st_uid = (uint32_t)h->st_uid;
    d->st_gid = (uint32_t)h->st_gid;
    d->st_rdev = (int32_t)h->st_rdev;
    d->st_atimespec.tv_sec = (int64_t)h->st_atim.tv_sec;
    d->st_atimespec.tv_nsec = (int64_t)h->st_atim.tv_nsec;
    d->st_mtimespec.tv_sec = (int64_t)h->st_mtim.tv_sec;
    d->st_mtimespec.tv_nsec = (int64_t)h->st_mtim.tv_nsec;
    d->st_ctimespec.tv_sec = (int64_t)h->st_ctim.tv_sec;
    d->st_ctimespec.tv_nsec = (int64_t)h->st_ctim.tv_nsec;
    /* The host has no creation time; reuse the change time. */
    d->st_birthtimespec = d->st_ctimespec;
    d->st_size = (int64_t)h->st_size;
    d->st_blocks = (int64_t)h->st_blocks;
    d->st_blksize = (int32_t)h->st_blksize;
}

/* ------------------------------------------------------------------ */
/* Clean-room libSystem shim                                          */
/* ------------------------------------------------------------------ */

/*
 * The symbols a Masa-linked C program imports.  Each one is implemented
 * here on top of the host C library, which is exactly the behaviour the
 * program expects; none of the shim is derived from Apple's libSystem.
 */

/* --- standard I/O ------------------------------------------------- */

static int dr_shim_printf(const char *format, ...)
{
    va_list ap;
    int n;

    va_start(ap, format);
    n = vprintf(format, ap);
    va_end(ap);
    return n;
}

static int dr_shim_puts(const char *s)
{
    return puts(s);
}

static int dr_shim_putchar(int c)
{
    return putchar(c);
}

static int dr_shim_fprintf(void *stream, const char *format, ...)
{
    va_list ap;
    int n;

    va_start(ap, format);
    n = vfprintf((FILE *)stream, format, ap);
    va_end(ap);
    return n;
}

static void *dr_shim_fopen(const char *path, const char *mode)
{
    char vbuf[DR_PATH_MAX];
    const char *p = dr_vpath(path, vbuf, sizeof(vbuf));

    if (p == NULL) {
        return NULL;
    }
    return fopen(p, mode);
}

static int dr_shim_fclose(void *stream)
{
    return fclose((FILE *)stream);
}

static size_t dr_shim_fread(void *ptr, size_t size, size_t count, void *stream)
{
    return fread(ptr, size, count, (FILE *)stream);
}

static size_t dr_shim_fwrite(const void *ptr, size_t size, size_t count,
                             void *stream)
{
    return fwrite(ptr, size, count, (FILE *)stream);
}

static char *dr_shim_fgets(char *s, int size, void *stream)
{
    return fgets(s, size, (FILE *)stream);
}

static int dr_shim_fputs(const char *s, void *stream)
{
    return fputs(s, (FILE *)stream);
}

static int dr_shim_fflush(void *stream)
{
    return fflush((FILE *)stream);
}

static int dr_shim_fseek(void *stream, long offset, int whence)
{
    return fseek((FILE *)stream, offset, whence);
}

static long dr_shim_ftell(void *stream)
{
    return ftell((FILE *)stream);
}

static int dr_shim_feof(void *stream)
{
    return feof((FILE *)stream);
}

static int dr_shim_ferror(void *stream)
{
    return ferror((FILE *)stream);
}

static int dr_shim_snprintf(char *buf, size_t n, const char *format, ...)
{
    va_list ap;
    int r;

    va_start(ap, format);
    r = vsnprintf(buf, n, format, ap);
    va_end(ap);
    return r;
}

static int dr_shim_vsnprintf(char *buf, size_t n, const char *format,
                             va_list ap)
{
    return vsnprintf(buf, n, format, ap);
}

static int dr_shim_sprintf(char *buf, const char *format, ...)
{
    va_list ap;
    int r;

    va_start(ap, format);
    r = vsprintf(buf, format, ap);
    va_end(ap);
    return r;
}

static int dr_shim_sscanf(const char *s, const char *format, ...)
{
    va_list ap;
    int r;

    va_start(ap, format);
    r = vsscanf(s, format, ap);
    va_end(ap);
    return r;
}

/* --- file descriptors --------------------------------------------- */

static long dr_shim_write(int fd, const void *buf, size_t n)
{
    return (long)write(fd, buf, n);
}

static long dr_shim_read(int fd, void *buf, size_t n)
{
    return (long)read(fd, buf, n);
}

static int dr_shim_open(const char *path, int flags, int mode)
{
    char vbuf[DR_PATH_MAX];
    const char *p = dr_vpath(path, vbuf, sizeof(vbuf));

    if (p == NULL) {
        return -1;
    }
    return open(p, flags, mode);
}

static int dr_shim_close(int fd)
{
    return close(fd);
}

static int64_t dr_shim_lseek(int fd, int64_t offset, int whence)
{
    return (int64_t)lseek(fd, (off_t)offset, whence);
}

static int dr_shim_access(const char *path, int mode)
{
    char vbuf[DR_PATH_MAX];
    const char *p = dr_vpath(path, vbuf, sizeof(vbuf));

    if (p == NULL) {
        return -1;
    }
    return access(p, mode);
}

static int dr_shim_unlink(const char *path)
{
    char vbuf[DR_PATH_MAX];
    const char *p = dr_vpath(path, vbuf, sizeof(vbuf));

    if (p == NULL) {
        return -1;
    }
    return unlink(p);
}

static int dr_shim_mkdir(const char *path, int mode)
{
    char vbuf[DR_PATH_MAX];
    const char *p = dr_vpath(path, vbuf, sizeof(vbuf));

    if (p == NULL) {
        return -1;
    }
    return mkdir(p, (mode_t)mode);
}

static int dr_shim_rmdir(const char *path)
{
    char vbuf[DR_PATH_MAX];
    const char *p = dr_vpath(path, vbuf, sizeof(vbuf));

    if (p == NULL) {
        return -1;
    }
    return rmdir(p);
}

static char *dr_shim_getcwd(char *buf, size_t size)
{
    return getcwd(buf, size);
}

static int dr_shim_chdir(const char *path)
{
    char vbuf[DR_PATH_MAX];
    const char *p = dr_vpath(path, vbuf, sizeof(vbuf));

    if (p == NULL) {
        return -1;
    }
    return chdir(p);
}

/* --- directories and stat ----------------------------------------- */

static void *dr_shim_opendir(const char *path)
{
    char vbuf[DR_PATH_MAX];
    const char *p = dr_vpath(path, vbuf, sizeof(vbuf));

    if (p == NULL) {
        return NULL;
    }
    return opendir(p);
}

/*
 * readdir hands the program a Darwin struct dirent.  The host entry is
 * copied into an internal buffer with the fields laid out for a 64-bit
 * Darwin process, so the program reads the correct inode, type and name.
 */
static void *dr_shim_readdir(void *dirp)
{
    static struct dr_darwin_dirent out;
    struct dirent *ent = readdir((DIR *)dirp);
    size_t namlen;

    if (ent == NULL) {
        return NULL;
    }
    namlen = strlen(ent->d_name);
    if (namlen > sizeof(out.d_name) - 1) {
        namlen = sizeof(out.d_name) - 1;
    }
    memset(&out, 0, sizeof(out));
    out.d_ino = (uint64_t)ent->d_ino;
    out.d_seekoff = 0;
    out.d_namlen = (uint16_t)namlen;
    out.d_reclen = (uint16_t)(sizeof(out) - sizeof(out.d_name) + namlen + 1);
    out.d_type = (uint8_t)ent->d_type;
    memcpy(out.d_name, ent->d_name, namlen);
    out.d_name[namlen] = '\0';
    return &out;
}

static int dr_shim_closedir(void *dirp)
{
    return closedir((DIR *)dirp);
}

static int dr_shim_stat(const char *path, void *buf)
{
    char vbuf[DR_PATH_MAX];
    const char *p = dr_vpath(path, vbuf, sizeof(vbuf));
    struct stat h;

    if (p == NULL) {
        return -1;
    }
    if (stat(p, &h) != 0) {
        return -1;
    }
    dr_stat_to_darwin(&h, (struct dr_darwin_stat *)buf);
    return 0;
}

static int dr_shim_lstat(const char *path, void *buf)
{
    char vbuf[DR_PATH_MAX];
    const char *p = dr_vpath(path, vbuf, sizeof(vbuf));
    struct stat h;

    if (p == NULL) {
        return -1;
    }
    if (lstat(p, &h) != 0) {
        return -1;
    }
    dr_stat_to_darwin(&h, (struct dr_darwin_stat *)buf);
    return 0;
}

static int dr_shim_fstat(int fd, void *buf)
{
    struct stat h;

    if (fstat(fd, &h) != 0) {
        return -1;
    }
    dr_stat_to_darwin(&h, (struct dr_darwin_stat *)buf);
    return 0;
}

/* --- strings, memory and formatting ------------------------------- */

static void *dr_shim_malloc(size_t size)
{
    return malloc(size);
}

static void *dr_shim_calloc(size_t count, size_t size)
{
    return calloc(count, size);
}

static void *dr_shim_realloc(void *ptr, size_t size)
{
    return realloc(ptr, size);
}

static void dr_shim_free(void *ptr)
{
    free(ptr);
}

static size_t dr_shim_strlen(const char *s)
{
    return strlen(s);
}

static int dr_shim_strcmp(const char *a, const char *b)
{
    return strcmp(a, b);
}

static int dr_shim_strncmp(const char *a, const char *b, size_t n)
{
    return strncmp(a, b, n);
}

static char *dr_shim_strcpy(char *dst, const char *src)
{
    return strcpy(dst, src);
}

static char *dr_shim_strncpy(char *dst, const char *src, size_t n)
{
    return strncpy(dst, src, n);
}

static char *dr_shim_strcat(char *dst, const char *src)
{
    return strcat(dst, src);
}

static char *dr_shim_strncat(char *dst, const char *src, size_t n)
{
    return strncat(dst, src, n);
}

static char *dr_shim_strchr(const char *s, int c)
{
    return strchr(s, c);
}

static char *dr_shim_strrchr(const char *s, int c)
{
    return strrchr(s, c);
}

static char *dr_shim_strstr(const char *haystack, const char *needle)
{
    return strstr(haystack, needle);
}

static char *dr_shim_strdup(const char *s)
{
    return strdup(s);
}

static char *dr_shim_strndup(const char *s, size_t n)
{
    return strndup(s, n);
}

static char *dr_shim_strerror(int errnum)
{
    return strerror(errnum);
}

static void *dr_shim_memcpy(void *dst, const void *src, size_t n)
{
    return memcpy(dst, src, n);
}

static void *dr_shim_memset(void *dst, int c, size_t n)
{
    return memset(dst, c, n);
}

static void *dr_shim_memmove(void *dst, const void *src, size_t n)
{
    return memmove(dst, src, n);
}

static int dr_shim_atoi(const char *s)
{
    return atoi(s);
}

/* --- process and environment -------------------------------------- */

static char *dr_shim_getenv(const char *name)
{
    return getenv(name);
}

static int dr_shim_setenv(const char *name, const char *value, int overwrite)
{
    return setenv(name, value, overwrite);
}

static int dr_shim_unsetenv(const char *name)
{
    return unsetenv(name);
}

static int dr_shim_getpid(void)
{
    return (int)getpid();
}

static unsigned int dr_shim_getuid(void)
{
    return (unsigned int)getuid();
}

static int dr_shim_isatty(int fd)
{
    return isatty(fd);
}

static int dr_shim_usleep(unsigned int usec)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(usec / 1000000u);
    ts.tv_nsec = (long)(usec % 1000000u) * 1000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
        /* finish the remaining interval */
    }
    return 0;
}

static unsigned int dr_shim_sleep(unsigned int seconds)
{
    return sleep(seconds);
}

static void dr_shim_abort(void)
{
    abort();
}

/*
 * exit()/longjmp: rather than tear the process down (which would skip
 * the loader's own cleanup and leave AddressSanitizer reporting leaks),
 * an exit jumps back to the loader, which unmaps everything and returns
 * the requested status.
 */
static jmp_buf dr_exit_jmp;
static volatile int dr_exit_armed;
static volatile int dr_exit_status;

static void dr_shim_exit(int status)
{
    dr_exit_status = status;
    if (dr_exit_armed) {
        longjmp(dr_exit_jmp, 1);
    }
    /* Called outside a loaded program: nothing sensible left to do. */
    exit(status);
}

/* A no-op stand-in so a stray reference never breaks the load. */
static void dr_shim_stub_binder(void)
{
}

/* --- Darwin system information ------------------------------------ */

static const char *dr_host_machine(void)
{
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(__arm64__)
    return "arm64";
#else
    return "unknown";
#endif
}

/* Darwin's struct utsname uses 256-byte fields, unlike glibc's 65. */
struct dr_utsname {
    char sysname[256];
    char nodename[256];
    char release[256];
    char version[256];
    char machine[256];
};

static int dr_shim_uname(void *buf)
{
    struct dr_utsname *u = (struct dr_utsname *)buf;

    if (u == NULL) {
        errno = EFAULT;
        return -1;
    }
    memset(u, 0, sizeof(*u));
    dr_strlcpy(u->sysname, "Darwin", sizeof(u->sysname));
    dr_strlcpy(u->nodename, "masa.local", sizeof(u->nodename));
    dr_strlcpy(u->release, "24.0.0", sizeof(u->release));
    dr_strlcpy(u->version, "Darwin Kernel Version 24.0.0: Masa Danroot",
               sizeof(u->version));
    dr_strlcpy(u->machine, dr_host_machine(), sizeof(u->machine));
    return 0;
}

/*
 * A sysctl-style copy: with no destination the caller only learns the
 * size; with a short buffer the size is reported and ENOMEM returned.
 */
static int dr_sysctl_copy(const void *src, size_t len, void *oldp,
                          size_t *oldlenp)
{
    if (oldlenp == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (oldp == NULL) {
        *oldlenp = len;
        return 0;
    }
    if (*oldlenp < len) {
        *oldlenp = len;
        errno = ENOMEM;
        return -1;
    }
    memcpy(oldp, src, len);
    *oldlenp = len;
    return 0;
}

static int dr_shim_sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
                                void *newp, size_t newlen)
{
    (void)newp;
    (void)newlen;

    if (name == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (strcmp(name, "kern.ostype") == 0) {
        return dr_sysctl_copy("Darwin", sizeof("Darwin"), oldp, oldlenp);
    }
    if (strcmp(name, "kern.osrelease") == 0) {
        return dr_sysctl_copy("24.0.0", sizeof("24.0.0"), oldp, oldlenp);
    }
    if (strcmp(name, "kern.osversion") == 0) {
        return dr_sysctl_copy("24A335", sizeof("24A335"), oldp, oldlenp);
    }
    if (strcmp(name, "kern.hostname") == 0) {
        return dr_sysctl_copy("masa.local", sizeof("masa.local"), oldp,
                              oldlenp);
    }
    if (strcmp(name, "hw.machine") == 0) {
        const char *m = dr_host_machine();
        return dr_sysctl_copy(m, strlen(m) + 1, oldp, oldlenp);
    }
    if (strcmp(name, "hw.ncpu") == 0 ||
        strcmp(name, "hw.logicalcpu") == 0 ||
        strcmp(name, "hw.physicalcpu") == 0 ||
        strcmp(name, "hw.availcpu") == 0) {
        int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (n < 1) {
            n = 1;
        }
        return dr_sysctl_copy(&n, sizeof(n), oldp, oldlenp);
    }
    if (strcmp(name, "hw.memsize") == 0) {
        uint64_t mem = (uint64_t)sysconf(_SC_PHYS_PAGES) *
                       (uint64_t)sysconf(_SC_PAGESIZE);
        return dr_sysctl_copy(&mem, sizeof(mem), oldp, oldlenp);
    }
    errno = ENOENT;
    return -1;
}

/* --- Mach time and task ------------------------------------------- */

/*
 * A minimal clean-room subset of <mach/mach_time.h> and <mach/mach.h>.
 * The host monotonic clock is the natural source for mach_absolute_time;
 * the timebase is 1:1 (nanoseconds), which is what Apple silicon reports.
 * mach_task_self_ only has to be a stable, non-zero task port handle.
 */
static uint32_t dr_mach_task_self = 0x103u;

static uint64_t dr_shim_mach_absolute_time(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int dr_shim_mach_timebase_info(void *info)
{
    struct {
        uint32_t numer;
        uint32_t denom;
    } *tb = (void *)info;

    if (tb == NULL) {
        return -1;
    }
    tb->numer = 1;
    tb->denom = 1;
    return 0;
}

/* --- dynamic linker (wired into the loader below) ----------------- */

static void *dr_dlopen(const char *path, int mode);
static void *dr_dlsym(void *handle, const char *name);
static int dr_dlclose(void *handle);
static char *dr_dlerror(void);

/* --- the shim table ----------------------------------------------- */

struct dr_symbol {
    const char *name;
    void *addr;
};

static const struct dr_symbol dr_symbols[] = {
    /* standard I/O */
    { "_printf",            (void *)dr_shim_printf },
    { "_puts",              (void *)dr_shim_puts },
    { "_putchar",           (void *)dr_shim_putchar },
    { "_fprintf",           (void *)dr_shim_fprintf },
    { "_fopen",             (void *)dr_shim_fopen },
    { "_fclose",            (void *)dr_shim_fclose },
    { "_fread",             (void *)dr_shim_fread },
    { "_fwrite",            (void *)dr_shim_fwrite },
    { "_fgets",             (void *)dr_shim_fgets },
    { "_fputs",             (void *)dr_shim_fputs },
    { "_fflush",            (void *)dr_shim_fflush },
    { "_fseek",             (void *)dr_shim_fseek },
    { "_ftell",             (void *)dr_shim_ftell },
    { "_feof",              (void *)dr_shim_feof },
    { "_ferror",            (void *)dr_shim_ferror },
    { "_snprintf",          (void *)dr_shim_snprintf },
    { "_vsnprintf",         (void *)dr_shim_vsnprintf },
    { "_sprintf",           (void *)dr_shim_sprintf },
    { "_sscanf",            (void *)dr_shim_sscanf },
    /* file descriptors */
    { "_write",             (void *)dr_shim_write },
    { "_read",              (void *)dr_shim_read },
    { "_open",              (void *)dr_shim_open },
    { "_close",             (void *)dr_shim_close },
    { "_lseek",             (void *)dr_shim_lseek },
    { "_access",            (void *)dr_shim_access },
    { "_unlink",            (void *)dr_shim_unlink },
    { "_mkdir",             (void *)dr_shim_mkdir },
    { "_rmdir",             (void *)dr_shim_rmdir },
    { "_getcwd",            (void *)dr_shim_getcwd },
    { "_chdir",             (void *)dr_shim_chdir },
    /* directories and stat */
    { "_opendir",           (void *)dr_shim_opendir },
    { "_readdir",           (void *)dr_shim_readdir },
    { "_closedir",          (void *)dr_shim_closedir },
    { "_stat",              (void *)dr_shim_stat },
    { "_lstat",             (void *)dr_shim_lstat },
    { "_fstat",             (void *)dr_shim_fstat },
    /* strings and memory */
    { "_malloc",            (void *)dr_shim_malloc },
    { "_calloc",            (void *)dr_shim_calloc },
    { "_realloc",           (void *)dr_shim_realloc },
    { "_free",              (void *)dr_shim_free },
    { "_strlen",            (void *)dr_shim_strlen },
    { "_strcmp",            (void *)dr_shim_strcmp },
    { "_strncmp",           (void *)dr_shim_strncmp },
    { "_strcpy",            (void *)dr_shim_strcpy },
    { "_strncpy",           (void *)dr_shim_strncpy },
    { "_strcat",            (void *)dr_shim_strcat },
    { "_strncat",           (void *)dr_shim_strncat },
    { "_strchr",            (void *)dr_shim_strchr },
    { "_strrchr",           (void *)dr_shim_strrchr },
    { "_strstr",            (void *)dr_shim_strstr },
    { "_strdup",            (void *)dr_shim_strdup },
    { "_strndup",           (void *)dr_shim_strndup },
    { "_strerror",          (void *)dr_shim_strerror },
    { "_memcpy",            (void *)dr_shim_memcpy },
    { "_memset",            (void *)dr_shim_memset },
    { "_memmove",           (void *)dr_shim_memmove },
    { "_atoi",              (void *)dr_shim_atoi },
    /* process and environment */
    { "_getenv",            (void *)dr_shim_getenv },
    { "_setenv",            (void *)dr_shim_setenv },
    { "_unsetenv",          (void *)dr_shim_unsetenv },
    { "_getpid",            (void *)dr_shim_getpid },
    { "_getuid",            (void *)dr_shim_getuid },
    { "_isatty",            (void *)dr_shim_isatty },
    { "_usleep",            (void *)dr_shim_usleep },
    { "_sleep",             (void *)dr_shim_sleep },
    { "_abort",             (void *)dr_shim_abort },
    { "_exit",              (void *)dr_shim_exit },
    /* Darwin system information */
    { "_uname",             (void *)dr_shim_uname },
    { "_sysctlbyname",      (void *)dr_shim_sysctlbyname },
    /* Mach time and task */
    { "_mach_absolute_time",  (void *)dr_shim_mach_absolute_time },
    { "_mach_timebase_info",  (void *)dr_shim_mach_timebase_info },
    /* dynamic linker */
    { "_dlopen",            (void *)dr_dlopen },
    { "_dlsym",             (void *)dr_dlsym },
    { "_dlclose",           (void *)dr_dlclose },
    { "_dlerror",           (void *)dr_dlerror },
    /* standard streams (data symbols: &stdout etc.) */
    { "___stdinp",          (void *)&stdin },
    { "___stdoutp",         (void *)&stdout },
    { "___stderrp",         (void *)&stderr },
    /* mach_task_self_ is a data symbol: the program reads its value. */
    { "_mach_task_self_",   (void *)&dr_mach_task_self },
    { "dyld_stub_binder",   (void *)dr_shim_stub_binder },
    { NULL, NULL }
};

static void *dr_resolve_shim(const char *name)
{
    const struct dr_symbol *s;

    for (s = dr_symbols; s->name != NULL; s++) {
        if (strcmp(s->name, name) == 0) {
            return s->addr;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Load command view                                                  */
/* ------------------------------------------------------------------ */

struct dr_segment {
    char name[17];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t initprot;
};

struct dr_dylib_ref {
    char name[DR_NAME_MAX];
    int weak;      /* LC_LOAD_WEAK_DYLIB */
    int reexport;  /* LC_REEXPORT_DYLIB */
};

struct dr_image {
    struct dr_segment segs[DR_MAX_SEGMENTS];
    int seg_count;
    uint64_t text_vmaddr;      /* preferred vmaddr of the lowest segment */
    uint64_t text_fileoff;     /* file offset that maps to text_vmaddr */
    uint64_t entryoff;         /* LC_MAIN file offset of the entry point */
    int is_dylib;              /* MH_DYLIB rather than MH_EXECUTE */

    /* LC_DYLD_INFO_ONLY (classic rebase/bind opcodes); 0 when absent. */
    int has_dyld_info;
    uint32_t rebase_off;
    uint32_t rebase_size;
    uint32_t bind_off;
    uint32_t bind_size;
    uint32_t lazy_bind_off;
    uint32_t lazy_bind_size;

    /* dependencies */
    struct dr_dylib_ref dylibs[DR_MAX_DYLIBS];
    int dylib_count;

    /* LC_RPATH search paths */
    char rpaths[DR_MAX_RPATHS][DR_NAME_MAX];
    int rpath_count;

    /* LC_ID_DYLIB install name */
    char install_name[DR_NAME_MAX];

    /* exports */
    uint32_t exports_trie_off;
    uint32_t exports_trie_size;
    uint32_t symtab_off;
    uint32_t symtab_count;
    uint32_t strtab_off;
    uint32_t strtab_size;
};

/*
 * Walk the load commands.  Returns 0 on success and -1 on a truncated or
 * malformed command list, writing a reason into err.
 */
static int dr_read_commands(const unsigned char *buf, size_t size,
                            struct dr_image *img, char *err, size_t errcap)
{
    uint32_t ncmds;
    uint32_t i;
    size_t off;

    memset(img, 0, sizeof(*img));
    if (size < 32) {
        snprintf(err, errcap, "truncated Mach-O header");
        return -1;
    }
    ncmds = dr_rd32(buf + 16);
    img->is_dylib = (dr_rd32(buf + 12) == DR_MH_DYLIB);
    off = 32; /* sizeof(struct mach_header_64) */

    for (i = 0; i < ncmds; i++) {
        uint32_t cmd;
        uint32_t cmdsize;

        if (off + 8 > size) {
            snprintf(err, errcap, "truncated load commands");
            return -1;
        }
        cmd = dr_rd32(buf + off);
        cmdsize = dr_rd32(buf + off + 4);
        if (cmdsize < 8 || off + cmdsize > size) {
            snprintf(err, errcap, "malformed load command %u", i);
            return -1;
        }

        if (cmd == DR_LC_SEGMENT_64) {
            struct dr_segment *seg;

            if (img->seg_count >= DR_MAX_SEGMENTS) {
                snprintf(err, errcap, "too many segments");
                return -1;
            }
            if (cmdsize < 72) {
                snprintf(err, errcap, "short LC_SEGMENT_64");
                return -1;
            }
            seg = &img->segs[img->seg_count++];
            memcpy(seg->name, buf + off + 8, 16);
            seg->name[16] = '\0';
            seg->vmaddr   = dr_rd64(buf + off + 24);
            seg->vmsize   = dr_rd64(buf + off + 32);
            seg->fileoff  = dr_rd64(buf + off + 40);
            seg->filesize = dr_rd64(buf + off + 48);
            seg->initprot = dr_rd32(buf + off + 60);
        } else if (cmd == DR_LC_MAIN) {
            if (cmdsize < 24) {
                snprintf(err, errcap, "short LC_MAIN");
                return -1;
            }
            img->entryoff = dr_rd64(buf + off + 8);
        } else if (cmd == DR_LC_DYLD_INFO_ONLY) {
            if (cmdsize < 48) {
                snprintf(err, errcap, "short LC_DYLD_INFO_ONLY");
                return -1;
            }
            img->has_dyld_info  = 1;
            img->rebase_off     = dr_rd32(buf + off + 8);
            img->rebase_size    = dr_rd32(buf + off + 12);
            img->bind_off       = dr_rd32(buf + off + 16);
            img->bind_size      = dr_rd32(buf + off + 20);
            img->lazy_bind_off  = dr_rd32(buf + off + 32);
            img->lazy_bind_size = dr_rd32(buf + off + 36);
        } else if (cmd == DR_LC_DYLD_EXPORTS_TRIE) {
            if (cmdsize < 16) {
                snprintf(err, errcap, "short LC_DYLD_EXPORTS_TRIE");
                return -1;
            }
            img->exports_trie_off  = dr_rd32(buf + off + 8);
            img->exports_trie_size = dr_rd32(buf + off + 12);
        } else if (cmd == DR_LC_SYMTAB) {
            if (cmdsize < 24) {
                snprintf(err, errcap, "short LC_SYMTAB");
                return -1;
            }
            img->symtab_off   = dr_rd32(buf + off + 8);
            img->symtab_count = dr_rd32(buf + off + 12);
            img->strtab_off   = dr_rd32(buf + off + 16);
            img->strtab_size  = dr_rd32(buf + off + 20);
        } else if (cmd == DR_LC_ID_DYLIB || cmd == DR_LC_LOAD_DYLIB ||
                   cmd == DR_LC_LOAD_WEAK_DYLIB || cmd == DR_LC_REEXPORT_DYLIB) {
            uint32_t name_off;

            if (cmdsize < 24) {
                snprintf(err, errcap, "short dylib load command");
                return -1;
            }
            name_off = dr_rd32(buf + off + 8);
            if (name_off < 24 || name_off >= cmdsize) {
                snprintf(err, errcap, "dylib name offset out of range");
                return -1;
            }
            if (cmd == DR_LC_ID_DYLIB) {
                dr_strlcpy(img->install_name, (const char *)(buf + off + name_off),
                           sizeof(img->install_name));
            } else {
                struct dr_dylib_ref *ref;
                if (img->dylib_count >= DR_MAX_DYLIBS) {
                    snprintf(err, errcap, "too many dylib dependencies");
                    return -1;
                }
                ref = &img->dylibs[img->dylib_count++];
                dr_strlcpy(ref->name, (const char *)(buf + off + name_off),
                           sizeof(ref->name));
                ref->weak = (cmd == DR_LC_LOAD_WEAK_DYLIB);
                ref->reexport = (cmd == DR_LC_REEXPORT_DYLIB);
            }
        } else if (cmd == DR_LC_RPATH) {
            uint32_t path_off;

            if (cmdsize < 12) {
                snprintf(err, errcap, "short LC_RPATH");
                return -1;
            }
            path_off = dr_rd32(buf + off + 8);
            if (path_off < 12 || path_off >= cmdsize) {
                snprintf(err, errcap, "rpath offset out of range");
                return -1;
            }
            if (img->rpath_count < DR_MAX_RPATHS) {
                dr_strlcpy(img->rpaths[img->rpath_count],
                           (const char *)(buf + off + path_off), DR_NAME_MAX);
                img->rpath_count++;
            }
        }

        off += cmdsize;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Module registry and export index                                   */
/* ------------------------------------------------------------------ */

struct dr_export {
    char *name;
    void *addr;
};

struct dr_module {
    char path[DR_PATH_MAX];       /* resolved on-disk path */
    unsigned char *file;          /* file mapping (fixup source) */
    size_t filesize;
    unsigned char *loaded;        /* runtime mapping of the segments */
    uint64_t span;
    int64_t slide;
    int is_main;
    int weak;                     /* loaded to satisfy a weak reference */
    struct dr_image img;
    struct dr_export *exports;
    int export_count;
    int export_cap;
};

struct dr_loader {
    struct dr_module **mods;
    int mod_count;
    int mod_cap;
    char exe_dir[DR_PATH_MAX];
    char main_path[DR_PATH_MAX];
};

static struct dr_loader *dr_active_loader;

static struct dr_module *dr_find_loaded(struct dr_loader *ld, const char *path)
{
    int i;

    for (i = 0; i < ld->mod_count; i++) {
        struct dr_module *m = ld->mods[i];

        if (strcmp(m->path, path) == 0) {
            return m;
        }
        if (m->img.install_name[0] != '\0' &&
            strcmp(m->img.install_name, path) == 0) {
            return m;
        }
    }
    return NULL;
}

static int dr_add_export(struct dr_module *m, const char *name, void *addr)
{
    int i;

    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    for (i = 0; i < m->export_count; i++) {
        if (strcmp(m->exports[i].name, name) == 0) {
            return 0; /* already indexed */
        }
    }
    if (m->export_count == m->export_cap) {
        int cap = m->export_cap ? m->export_cap * 2 : 16;
        struct dr_export *grown = realloc(m->exports,
                                          (size_t)cap * sizeof(*grown));
        if (grown == NULL) {
            return -1;
        }
        m->exports = grown;
        m->export_cap = cap;
    }
    m->exports[m->export_count].name = strdup(name);
    if (m->exports[m->export_count].name == NULL) {
        return -1;
    }
    m->exports[m->export_count].addr = addr;
    m->export_count++;
    return 0;
}

static void *dr_module_export(const struct dr_module *m, const char *name)
{
    int i;

    for (i = 0; i < m->export_count; i++) {
        if (strcmp(m->exports[i].name, name) == 0) {
            return m->exports[i].addr;
        }
    }
    return NULL;
}

static void *dr_lookup_export(struct dr_loader *ld, const char *name)
{
    int i;

    for (i = 0; i < ld->mod_count; i++) {
        void *p = dr_module_export(ld->mods[i], name);

        if (p != NULL) {
            return p;
        }
    }
    return NULL;
}

/* Resolve a symbol against the shim table, then every loaded image. */
static void *dr_resolve_symbol(struct dr_loader *ld, const char *name)
{
    void *p = dr_resolve_shim(name);

    if (p != NULL) {
        return p;
    }
    return dr_lookup_export(ld, name);
}

/*
 * Walk the LC_DYLD_EXPORTS_TRIE and index every regular (non-reexport)
 * exported symbol.  The encoding (as emitted by ld64.lld and read by
 * dyld) is one node of:
 *
 *   terminalSize : uleb128        byte length of the terminal payload
 *   terminal     : terminalSize bytes (flags, then the address, or the
 *                  reexport ordinal + import name)
 *   childCount   : uleb128
 *   childCount x { edgeString : cstring, childOffset : uleb128 }
 *
 * The stored address is an absolute vmaddr, so the runtime address is
 * loaded + (address - text_vmaddr).
 */
static int dr_parse_export_trie(struct dr_module *m, char *err, size_t errcap)
{
    const unsigned char *trie;
    size_t trie_size = m->img.exports_trie_size;
    /* Explicit stack of (node offset, name prefix) for a bounded walk. */
    struct frame {
        size_t off;
        char name[512];
    } stack[64];
    int sp = 0;

    if (trie_size == 0) {
        return 0;
    }
    if ((size_t)m->img.exports_trie_off + trie_size > m->filesize) {
        snprintf(err, errcap, "exports trie out of range");
        return -1;
    }
    trie = m->file + m->img.exports_trie_off;

    stack[sp].off = 0;
    stack[sp].name[0] = '\0';
    sp++;

    while (sp > 0) {
        struct frame f = stack[--sp];
        const unsigned char *p = trie + f.off;
        const unsigned char *end = trie + trie_size;
        const unsigned char *payload_end;
        uint64_t terminal_size;
        uint64_t child_count;
        uint64_t c;

        if (f.off >= trie_size) {
            snprintf(err, errcap, "exports trie node out of range");
            return -1;
        }
        terminal_size = dr_uleb128(&p, end);
        payload_end = p + terminal_size;
        if (payload_end > end) {
            snprintf(err, errcap, "exports trie terminal out of range");
            return -1;
        }
        if (terminal_size != 0) {
            uint64_t flags = dr_uleb128(&p, end);

            if ((flags & DR_EXPORT_FLAG_REEXPORT) != 0) {
                /* Resolved through the reexported image; skip here. */
                (void)dr_uleb128(&p, end); /* ordinal */
            } else {
                uint64_t addr = dr_uleb128(&p, end);
                void *runtime = (void *)(uintptr_t)(m->loaded +
                                       (addr - m->img.text_vmaddr));

                if (dr_add_export(m, f.name, runtime) != 0) {
                    snprintf(err, errcap, "out of memory");
                    return -1;
                }
            }
        }
        p = payload_end;

        child_count = dr_uleb128(&p, end);
        for (c = 0; c < child_count; c++) {
            char edge[256];
            size_t n = 0;
            uint64_t child;

            if (p >= end) {
                snprintf(err, errcap, "exports trie edge out of range");
                return -1;
            }
            while (p < end && *p != '\0' && n + 1 < sizeof(edge)) {
                edge[n++] = (char)*p++;
            }
            edge[n] = '\0';
            if (p < end && *p == '\0') {
                p++; /* consume the terminating NUL */
            }
            child = dr_uleb128(&p, end);
            if (sp >= (int)(sizeof(stack) / sizeof(stack[0]))) {
                snprintf(err, errcap, "exports trie too deep");
                return -1;
            }
            if (strlen(f.name) + strlen(edge) + 1 < sizeof(stack[0].name)) {
                (void)dr_pathfmt(stack[sp].name, sizeof(stack[sp].name),
                                 "%s%s", f.name, edge);
            } else {
                dr_strlcpy(stack[sp].name, f.name, sizeof(stack[sp].name));
            }
            stack[sp].off = (size_t)child;
            sp++;
        }
    }
    return 0;
}

/*
 * Index LC_SYMTAB external defined symbols as a fallback (and to cover
 * images without an export trie).  n_value is an absolute vmaddr, so the
 * runtime address is loaded + (n_value - text_vmaddr).
 */
static int dr_parse_symtab(struct dr_module *m, char *err, size_t errcap)
{
    uint32_t i;
    const unsigned char *symtab;
    const unsigned char *strtab;

    if (m->img.symtab_count == 0 || m->img.symtab_off == 0) {
        return 0;
    }
    if ((size_t)m->img.symtab_off +
            (size_t)m->img.symtab_count * 16 > m->filesize ||
        (size_t)m->img.strtab_off + m->img.strtab_size > m->filesize) {
        snprintf(err, errcap, "symbol table out of range");
        return -1;
    }
    symtab = m->file + m->img.symtab_off;
    strtab = m->file + m->img.strtab_off;

    for (i = 0; i < m->img.symtab_count; i++) {
        const unsigned char *nl = symtab + (size_t)i * 16;
        uint32_t n_strx = dr_rd32(nl);
        uint8_t n_type = nl[4];
        uint64_t n_value = dr_rd64(nl + 8);

        if ((n_type & DR_N_STAB) != 0) {
            continue;
        }
        if ((n_type & DR_N_EXT) == 0) {
            continue;
        }
        if ((n_type & DR_N_TYPE) == DR_N_UNDF) {
            continue; /* undefined: an import, not an export */
        }
        if (n_value == 0 || n_strx >= m->img.strtab_size) {
            continue;
        }
        if (dr_add_export(m, (const char *)(strtab + n_strx),
                          (void *)(uintptr_t)(m->loaded +
                                              (n_value - m->img.text_vmaddr)))
            != 0) {
            snprintf(err, errcap, "out of memory");
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Chained fixups                                                     */
/* ------------------------------------------------------------------ */

/*
 * Locate LC_DYLD_CHAINED_FIXUPS in the raw load commands and return its
 * (dataoff, datasize) within the file.  Returns 0 when present.
 */
static int dr_find_chained_fixups(const unsigned char *buf, size_t size,
                                  uint64_t *dataoff, uint64_t *datasize)
{
    uint32_t ncmds = dr_rd32(buf + 16);
    uint32_t i;
    size_t off = 32;

    for (i = 0; i < ncmds; i++) {
        uint32_t cmd;
        uint32_t cmdsize;

        if (off + 8 > size) {
            return -1;
        }
        cmd = dr_rd32(buf + off);
        cmdsize = dr_rd32(buf + off + 4);
        if (cmdsize < 8 || off + cmdsize > size) {
            return -1;
        }
        if (cmd == DR_LC_DYLD_CHAINED_FIXUPS) {
            if (cmdsize < 16) {
                return -1;
            }
            *dataoff = dr_rd32(buf + off + 8);
            *datasize = dr_rd32(buf + off + 12);
            return 0;
        }
        off += cmdsize;
    }
    return -1;
}

/*
 * Apply the chained fixups for one module.  In DR_FIX_REBASE mode the
 * rebase slots are rewritten to the module's real load address; in
 * DR_FIX_BIND mode the bind slots are resolved against the shim and the
 * loaded images.  The chain is always walked so the other kind is
 * skipped without losing the walk.
 */
static int dr_apply_chained(struct dr_loader *ld, struct dr_module *m,
                            uint64_t dataoff, uint64_t datasize, int mode,
                            char *err, size_t errcap)
{
    const unsigned char *file = m->file;
    size_t filesize = m->filesize;
    unsigned char *loaded = m->loaded;
    int64_t slide = m->slide;
    const unsigned char *hdr;
    uint32_t starts_off;
    uint32_t imports_off;
    uint32_t symbols_off;
    uint32_t imports_count;
    uint32_t imports_format;
    uint32_t symbols_format;
    const unsigned char *symbols;
    const char **names;
    unsigned char *weak;
    const unsigned char *starts;
    uint32_t seg_count;
    uint32_t s;
    int rc = 0;

    if (dataoff + datasize > filesize || datasize < 28) {
        snprintf(err, errcap, "chained fixups out of range");
        return -1;
    }
    hdr = file + dataoff;

    starts_off     = dr_rd32(hdr + 4);
    imports_off    = dr_rd32(hdr + 8);
    symbols_off    = dr_rd32(hdr + 12);
    imports_count  = dr_rd32(hdr + 16);
    imports_format = dr_rd32(hdr + 20);
    symbols_format = dr_rd32(hdr + 24);

    if (symbols_format != 0) {
        snprintf(err, errcap,
                 "compressed chained-fixups symbol table is not supported");
        return -1;
    }
    if (imports_format != DR_CHAINED_IMPORT) {
        snprintf(err, errcap, "unsupported chained import format %u",
                 imports_format);
        return -1;
    }
    if (symbols_off > datasize ||
        imports_off + (size_t)imports_count * 4 > datasize) {
        snprintf(err, errcap, "chained fixups table out of range");
        return -1;
    }

    symbols = hdr + symbols_off;

    /* Build the import name table from the symbol string pool. */
    names = calloc(imports_count ? imports_count : 1, sizeof(*names));
    weak = calloc(imports_count ? imports_count : 1, sizeof(*weak));
    if (names == NULL || weak == NULL) {
        snprintf(err, errcap, "out of memory");
        free(names);
        free(weak);
        return -1;
    }
    for (s = 0; s < imports_count; s++) {
        uint32_t word = dr_rd32(hdr + imports_off + s * 4);
        uint32_t name_off = (word >> 9) & 0x7fffffu; /* 23-bit name offset */

        if (symbols_off + name_off >= datasize) {
            snprintf(err, errcap, "import %u name out of range", s);
            free(names);
            free(weak);
            return -1;
        }
        names[s] = (const char *)(symbols + name_off);
        weak[s] = (unsigned char)((word >> 8) & 1u); /* weak_import */
    }

    if (starts_off + 4 > datasize) {
        snprintf(err, errcap, "chained starts out of range");
        free(names);
        free(weak);
        return -1;
    }
    starts = hdr + starts_off;
    seg_count = dr_rd32(starts);

    for (s = 0; s < seg_count; s++) {
        uint32_t info_off;
        const unsigned char *si;
        uint16_t page_size;
        uint16_t pointer_format;
        uint64_t segment_offset;
        uint16_t page_count;
        uint16_t page;

        if (starts_off + 4 + (size_t)s * 4 + 4 > datasize) {
            break;
        }
        info_off = dr_rd32(starts + 4 + s * 4);
        if (info_off == 0) {
            continue; /* segment carries no fixups */
        }
        if (starts_off + info_off + 22 > datasize) {
            snprintf(err, errcap, "chained segment %u out of range", s);
            rc = -1;
            break;
        }
        si = starts + info_off;
        page_size       = dr_rd16(si + 4);
        pointer_format  = dr_rd16(si + 6);
        segment_offset  = dr_rd64(si + 8);
        page_count      = dr_rd16(si + 20);

        if (pointer_format != DR_CHAINED_PTR_64 &&
            pointer_format != DR_CHAINED_PTR_64_OFFSET) {
            snprintf(err, errcap, "unsupported pointer format %u",
                     pointer_format);
            rc = -1;
            break;
        }

        for (page = 0; page < page_count; page++) {
            uint16_t page_start;
            uint64_t loc;

            if (starts_off + info_off + 22 + (size_t)page * 2 + 2 > datasize) {
                break;
            }
            page_start = dr_rd16(si + 22 + page * 2);
            if (page_start == DR_CHAINED_PTR_START_NONE) {
                continue;
            }
            loc = (uint64_t)(uintptr_t)loaded + segment_offset +
                  (uint64_t)page * page_size + page_start;

            for (;;) {
                uint64_t *slot = (uint64_t *)(uintptr_t)loc;
                uint64_t word = *slot;
                uint64_t bind = (word >> 63) & 1u;
                uint64_t next = (word >> 51) & 0xfffu;

                if (bind) {
                    if (mode == DR_FIX_BIND) {
                        uint32_t ordinal = (uint32_t)(word & 0xffffffu);
                        uint32_t addend = (uint32_t)((word >> 24) & 0xffu);
                        void *addr;

                        if (ordinal >= imports_count) {
                            snprintf(err, errcap,
                                     "bind ordinal %u out of range", ordinal);
                            rc = -1;
                            break;
                        }
                        addr = dr_resolve_symbol(ld, names[ordinal]);
                        if (addr == NULL) {
                            if (weak[ordinal]) {
                                *slot = addend; /* weak: NULL + addend */
                                goto next_slot;
                            }
                            snprintf(err, errcap,
                                     "unresolved symbol '%s'", names[ordinal]);
                            rc = -1;
                            break;
                        }
                        *slot = (uint64_t)(uintptr_t)addr + addend;
                    }
                } else if (mode == DR_FIX_REBASE) {
                    uint64_t target = word & 0xfffffffffULL; /* 36 bits */
                    uint64_t high8 = (word >> 36) & 0xffu;
                    uint64_t addr;

                    if (pointer_format == DR_CHAINED_PTR_64) {
                        addr = target + (uint64_t)slide;
                    } else {
                        addr = (uint64_t)(uintptr_t)loaded + target;
                    }
                    addr |= high8 << 56;
                    *slot = addr;
                }

            next_slot:
                if (next == 0 || rc != 0) {
                    break;
                }
                loc += next * 4;
            }
            if (rc != 0) {
                break;
            }
        }
        if (rc != 0) {
            break;
        }
    }

    free(names);
    free(weak);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Classic dyld info (LC_DYLD_INFO_ONLY)                              */
/* ------------------------------------------------------------------ */

/*
 * Runtime address of a (segment index, segment-relative offset) location.
 * The opcodes carry offsets relative to a segment's preferred vmaddr, so
 * the runtime address is loaded + (seg.vmaddr - image_base) + offset.
 */
static uint64_t dr_seg_addr(const struct dr_image *img, unsigned char *loaded,
                            int seg_index, uint64_t seg_offset, int *ok)
{
    if (seg_index < 0 || seg_index >= img->seg_count) {
        *ok = 0;
        return 0;
    }
    *ok = 1;
    return (uint64_t)(uintptr_t)loaded +
           (img->segs[seg_index].vmaddr - img->text_vmaddr) + seg_offset;
}

/* Rebase one pointer slot: add the slide to the value already there. */
static int dr_apply_one_rebase(const struct dr_image *img,
                               unsigned char *loaded, int64_t slide,
                               int seg_index, uint64_t seg_offset,
                               char *err, size_t errcap)
{
    int ok;
    uint64_t addr = dr_seg_addr(img, loaded, seg_index, seg_offset, &ok);
    uint64_t *slot;

    if (!ok) {
        snprintf(err, errcap, "rebase segment %d out of range", seg_index);
        return -1;
    }
    slot = (uint64_t *)(uintptr_t)addr;
    *slot += (uint64_t)slide;
    return 0;
}

/* 1 when ordinal names a weak (LC_LOAD_WEAK_DYLIB) dependency. */
static int dr_ordinal_weak(const struct dr_image *img, int ordinal)
{
    return ordinal >= 1 && ordinal <= img->dylib_count &&
           img->dylibs[ordinal - 1].weak;
}

static int dr_apply_one_bind(struct dr_loader *ld, const struct dr_image *img,
                             unsigned char *loaded, int seg_index,
                             uint64_t seg_offset, const char *name,
                             int bind_type, int64_t addend, int weak,
                             char *err, size_t errcap)
{
    int ok;
    uint64_t addr = dr_seg_addr(img, loaded, seg_index, seg_offset, &ok);
    void *target;

    if (!ok) {
        snprintf(err, errcap, "bind segment %d out of range", seg_index);
        return -1;
    }
    if (bind_type != DR_BIND_TYPE_POINTER) {
        snprintf(err, errcap, "unsupported bind type %d", bind_type);
        return -1;
    }
    target = dr_resolve_symbol(ld, name);
    if (target == NULL) {
        if (weak) {
            /* A weak import that is absent binds to NULL plus the addend. */
            *(uint64_t *)(uintptr_t)addr = (uint64_t)addend;
            return 0;
        }
        snprintf(err, errcap, "unresolved symbol '%s'", name);
        return -1;
    }
    *(uint64_t *)(uintptr_t)addr = (uint64_t)(uintptr_t)target + (uint64_t)addend;
    return 0;
}

/* Apply the classic rebase opcodes from LC_DYLD_INFO_ONLY. */
static int dr_apply_rebases(const struct dr_module *m, char *err, size_t errcap)
{
    const struct dr_image *img = &m->img;
    const unsigned char *p;
    const unsigned char *end;
    uint64_t seg_offset = 0;
    int seg_index = 0;

    if (img->rebase_size == 0) {
        return 0;
    }
    if ((uint64_t)img->rebase_off + img->rebase_size > m->filesize) {
        snprintf(err, errcap, "rebase opcodes out of range");
        return -1;
    }
    p = m->file + img->rebase_off;
    end = p + img->rebase_size;

    while (p < end) {
        uint8_t byte = *p++;
        uint8_t opcode = byte & DR_REBASE_OPCODE_MASK;
        uint8_t imm = byte & DR_REBASE_IMMEDIATE_MASK;
        uint64_t count;
        uint64_t i;
        int rc = 0;

        switch (opcode) {
        case DR_REBASE_OPCODE_DONE:
            return 0;
        case DR_REBASE_OPCODE_SET_TYPE_IMM:
            break; /* only pointer rebases are emitted for these images */
        case DR_REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
            seg_index = imm;
            seg_offset = dr_uleb128(&p, end);
            break;
        case DR_REBASE_OPCODE_ADD_ADDR_ULEB:
            seg_offset += dr_uleb128(&p, end);
            break;
        case DR_REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
            seg_offset += (uint64_t)imm * sizeof(void *);
            break;
        case DR_REBASE_OPCODE_DO_REBASE_IMM_TIMES:
            count = imm;
            for (i = 0; i < count; i++) {
                rc = dr_apply_one_rebase(img, m->loaded, m->slide, seg_index,
                                         seg_offset, err, errcap);
                if (rc != 0) {
                    return rc;
                }
                seg_offset += sizeof(void *);
            }
            break;
        case DR_REBASE_OPCODE_DO_REBASE_ULEB_TIMES:
            count = dr_uleb128(&p, end);
            for (i = 0; i < count; i++) {
                rc = dr_apply_one_rebase(img, m->loaded, m->slide, seg_index,
                                         seg_offset, err, errcap);
                if (rc != 0) {
                    return rc;
                }
                seg_offset += sizeof(void *);
            }
            break;
        case DR_REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB:
            rc = dr_apply_one_rebase(img, m->loaded, m->slide, seg_index,
                                     seg_offset, err, errcap);
            if (rc != 0) {
                return rc;
            }
            seg_offset += dr_uleb128(&p, end) + sizeof(void *);
            break;
        case DR_REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB:
            count = dr_uleb128(&p, end);
            {
                uint64_t skip = dr_uleb128(&p, end);
                for (i = 0; i < count; i++) {
                    rc = dr_apply_one_rebase(img, m->loaded, m->slide,
                                             seg_index, seg_offset, err, errcap);
                    if (rc != 0) {
                        return rc;
                    }
                    seg_offset += skip + sizeof(void *);
                }
            }
            break;
        default:
            snprintf(err, errcap, "unsupported rebase opcode %#x", opcode);
            return -1;
        }
    }
    return 0;
}

/*
 * Apply a classic bind opcode stream (bind or lazy_bind).  Lazy binds are
 * resolved eagerly: each entry ends with DONE, so DONE starts the next
 * entry instead of ending the stream.
 */
static int dr_apply_binds(struct dr_loader *ld, const struct dr_module *m,
                          uint32_t off, uint32_t size, int lazy,
                          char *err, size_t errcap)
{
    const struct dr_image *img = &m->img;
    const unsigned char *p;
    const unsigned char *end;
    uint64_t seg_offset = 0;
    int seg_index = 0;
    int bind_type = DR_BIND_TYPE_POINTER;
    int64_t addend = 0;
    int ordinal = 0;
    int weak = 0;
    char symbol[512];

    if (size == 0) {
        return 0;
    }
    if ((uint64_t)off + size > m->filesize) {
        snprintf(err, errcap, "bind opcodes out of range");
        return -1;
    }
    p = m->file + off;
    end = p + size;
    symbol[0] = '\0';

    while (p < end) {
        uint8_t byte = *p++;
        uint8_t opcode = byte & DR_BIND_OPCODE_MASK;
        uint8_t imm = byte & DR_BIND_IMMEDIATE_MASK;
        uint64_t count;
        uint64_t i;
        int rc = 0;

        switch (opcode) {
        case DR_BIND_OPCODE_DONE:
            if (lazy) {
                break; /* end of this entry; the next entry follows */
            }
            return 0;
        case DR_BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
            ordinal = imm;
            break;
        case DR_BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
            ordinal = (int)dr_uleb128(&p, end);
            break;
        case DR_BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
            ordinal = 0; /* self/flat/weak-lookup/main: no ordinal */
            break;
        case DR_BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM: {
            size_t n = 0;
            while (p < end && *p != '\0') {
                if (n + 1 < sizeof(symbol)) {
                    symbol[n++] = (char)*p;
                }
                p++;
            }
            symbol[n] = '\0';
            if (p < end) {
                p++; /* skip the trailing NUL */
            }
            weak = (imm & DR_BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0;
            break;
        }
        case DR_BIND_OPCODE_SET_TYPE_IMM:
            bind_type = imm;
            break;
        case DR_BIND_OPCODE_SET_ADDEND_SLEB:
            addend = dr_sleb128(&p, end);
            break;
        case DR_BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
            seg_index = imm;
            seg_offset = dr_uleb128(&p, end);
            break;
        case DR_BIND_OPCODE_ADD_ADDR_ULEB:
            seg_offset += dr_uleb128(&p, end);
            break;
        case DR_BIND_OPCODE_DO_BIND:
            rc = dr_apply_one_bind(ld, img, m->loaded, seg_index, seg_offset,
                                   symbol, bind_type, addend,
                                   weak || dr_ordinal_weak(img, ordinal),
                                   err, errcap);
            seg_offset += sizeof(void *);
            break;
        case DR_BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
            rc = dr_apply_one_bind(ld, img, m->loaded, seg_index, seg_offset,
                                   symbol, bind_type, addend,
                                   weak || dr_ordinal_weak(img, ordinal),
                                   err, errcap);
            seg_offset += dr_uleb128(&p, end) + sizeof(void *);
            break;
        case DR_BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
            rc = dr_apply_one_bind(ld, img, m->loaded, seg_index, seg_offset,
                                   symbol, bind_type, addend,
                                   weak || dr_ordinal_weak(img, ordinal),
                                   err, errcap);
            seg_offset += (uint64_t)imm * sizeof(void *) + sizeof(void *);
            break;
        case DR_BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
            count = dr_uleb128(&p, end);
            {
                uint64_t skip = dr_uleb128(&p, end);
                for (i = 0; i < count; i++) {
                    rc = dr_apply_one_bind(ld, img, m->loaded, seg_index,
                                           seg_offset, symbol, bind_type,
                                           addend,
                                           weak || dr_ordinal_weak(img, ordinal),
                                           err, errcap);
                    if (rc != 0) {
                        break;
                    }
                    seg_offset += skip + sizeof(void *);
                }
            }
            break;
        case DR_BIND_OPCODE_THREADED:
            snprintf(err, errcap, "threaded binds are not supported");
            return -1;
        default:
            snprintf(err, errcap, "unsupported bind opcode %#x", opcode);
            return -1;
        }
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/* Run one fixup pass (rebase or bind) over a module's relocations. */
static int dr_module_fixups(struct dr_loader *ld, struct dr_module *m, int mode,
                            char *err, size_t errcap)
{
    uint64_t dataoff = 0, datasize = 0;

    if (dr_find_chained_fixups(m->file, m->filesize, &dataoff, &datasize) == 0) {
        return dr_apply_chained(ld, m, dataoff, datasize, mode, err, errcap);
    }
    if (m->img.has_dyld_info) {
        if (mode == DR_FIX_REBASE) {
            return dr_apply_rebases(m, err, errcap);
        }
        if (dr_apply_binds(ld, m, m->img.bind_off, m->img.bind_size, 0,
                           err, errcap) != 0) {
            return -1;
        }
        return dr_apply_binds(ld, m, m->img.lazy_bind_off,
                              m->img.lazy_bind_size, 1, err, errcap);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Path resolution                                                    */
/* ------------------------------------------------------------------ */

static void dr_dirname(const char *path, char *out, size_t cap)
{
    const char *slash = strrchr(path, '/');

    if (slash == NULL) {
        dr_strlcpy(out, ".", cap);
        return;
    }
    if (slash == path) {
        dr_strlcpy(out, "/", cap);
        return;
    }
    {
        size_t len = (size_t)(slash - path);
        if (len >= cap) {
            len = cap - 1;
        }
        memcpy(out, path, len);
        out[len] = '\0';
    }
}

static int dr_file_readable(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Expand an @executable_path / @loader_path prefix inside a search path. */
static void dr_expand_path(const struct dr_loader *ld,
                           const struct dr_module *from,
                           const char *in, char *out, size_t cap)
{
    if (strncmp(in, "@executable_path", 16) == 0) {
        (void)dr_pathfmt(out, cap, "%s%s", ld->exe_dir, in + 16);
    } else if (strncmp(in, "@loader_path", 12) == 0) {
        char dir[DR_PATH_MAX];
        dr_dirname(from != NULL ? from->path : ld->main_path, dir, sizeof(dir));
        (void)dr_pathfmt(out, cap, "%s%s", dir, in + 12);
    } else {
        dr_strlcpy(out, in, cap);
    }
}

/* Search one colon-separated directory list for a bare library name. */
static int dr_search_dirlist(const char *list, const char *name, char *out,
                             size_t cap)
{
    const char *p = list;

    while (p != NULL && *p != '\0') {
        const char *colon = strchr(p, ':');
        size_t len = (colon != NULL) ? (size_t)(colon - p) : strlen(p);
        char candidate[DR_PATH_MAX];

        if (len > 0) {
            (void)dr_pathfmt(candidate, sizeof(candidate), "%.*s/%s", (int)len, p, name);
            if (dr_file_readable(candidate)) {
                dr_strlcpy(out, candidate, cap);
                return 0;
            }
        }
        p = (colon != NULL) ? colon + 1 : NULL;
    }
    return -1;
}

/*
 * Resolve a dylib reference from a loading module.  Supports absolute
 * paths, @executable_path, @loader_path, @rpath (via the loader's and the
 * main image's LC_RPATH entries), and bare names searched on
 * MASA_LIBRARY_PATH, DYLD_LIBRARY_PATH and the loading image's directory.
 * Returns 0 and fills out on success.
 */
static int dr_resolve_dylib_path(const struct dr_loader *ld,
                                 const struct dr_module *from, const char *name,
                                 char *out, size_t cap)
{
    char dir[DR_PATH_MAX];

    if (strncmp(name, "@executable_path/", 17) == 0) {
        (void)dr_pathfmt(out, cap, "%s/%s", ld->exe_dir, name + 17);
        return 0;
    }
    if (strncmp(name, "@loader_path/", 13) == 0) {
        dr_dirname(from != NULL ? from->path : ld->main_path, dir, sizeof(dir));
        (void)dr_pathfmt(out, cap, "%s/%s", dir, name + 13);
        return 0;
    }
    if (strncmp(name, "@rpath/", 7) == 0) {
        const char *suffix = name + 7;
        int i;
        const struct dr_module *sources[2];
        int nsources = 0;

        if (from != NULL) {
            sources[nsources++] = from;
        }
        if (ld->mod_count > 0 && ld->mods[0] != from) {
            sources[nsources++] = ld->mods[0];
        }
        for (i = 0; i < nsources; i++) {
            int r;
            for (r = 0; r < sources[i]->img.rpath_count; r++) {
                char expanded[DR_PATH_MAX];
                char candidate[DR_PATH_MAX];

                dr_expand_path(ld, from, sources[i]->img.rpaths[r],
                               expanded, sizeof(expanded));
                (void)dr_pathfmt(candidate, sizeof(candidate), "%s/%s", expanded,
                                 suffix);
                if (dr_file_readable(candidate)) {
                    dr_strlcpy(out, candidate, cap);
                    return 0;
                }
            }
        }
        /* The environment library paths are searched as extra rpaths. */
        {
            const char *env = getenv(MASA_LIBRARY_PATH_ENV);

            if (env != NULL && dr_search_dirlist(env, suffix, out, cap) == 0) {
                return 0;
            }
            env = getenv("DYLD_LIBRARY_PATH");
            if (env != NULL && dr_search_dirlist(env, suffix, out, cap) == 0) {
                return 0;
            }
        }
        return -1;
    }
    if (name[0] == '/') {
        dr_strlcpy(out, name, cap);
        return dr_file_readable(out) ? 0 : -1;
    }

    /* A bare name: search the environment paths, then local directories. */
    {
        const char *env;
        char candidate[DR_PATH_MAX];

        env = getenv(MASA_LIBRARY_PATH_ENV);
        if (env != NULL && dr_search_dirlist(env, name, out, cap) == 0) {
            return 0;
        }
        env = getenv("DYLD_LIBRARY_PATH");
        if (env != NULL && dr_search_dirlist(env, name, out, cap) == 0) {
            return 0;
        }
        dr_dirname(from != NULL ? from->path : ld->main_path, dir, sizeof(dir));
        (void)dr_pathfmt(candidate, sizeof(candidate), "%s/%s", dir, name);
        if (dr_file_readable(candidate)) {
            dr_strlcpy(out, candidate, cap);
            return 0;
        }
        (void)dr_pathfmt(candidate, sizeof(candidate), "%s/%s", ld->exe_dir, name);
        if (dr_file_readable(candidate)) {
            dr_strlcpy(out, candidate, cap);
            return 0;
        }
        (void)dr_pathfmt(candidate, sizeof(candidate), "./%s", name);
        if (dr_file_readable(candidate)) {
            dr_strlcpy(out, candidate, cap);
            return 0;
        }
    }
    return -1;
}

/*
 * True for the system libraries the built-in shim already satisfies:
 * libSystem (the C library) plus the stub SDK's crt1.o / dylib1.o, which
 * exist only so ld64.lld produces an executable - the loader supplies
 * their runtime behaviour (the entry call and dyld_stub_binder) itself.
 */
static int dr_is_system_stub(const char *name)
{
    return strcmp(name, "/usr/lib/libSystem.B.dylib") == 0 ||
           strcmp(name, "/usr/lib/libSystem.dylib") == 0 ||
           strcmp(name, "libSystem.B.dylib") == 0 ||
           strcmp(name, "libSystem.dylib") == 0 ||
           strcmp(name, "/usr/lib/crt1.o") == 0 ||
           strcmp(name, "crt1.o") == 0 ||
           strcmp(name, "/usr/lib/dylib1.o") == 0 ||
           strcmp(name, "dylib1.o") == 0 ||
           strcmp(name, "/usr/lib/libgcc_s.1.dylib") == 0 ||
           strcmp(name, "libgcc_s.1.dylib") == 0;
}

/* ------------------------------------------------------------------ */
/* Module loading                                                     */
/* ------------------------------------------------------------------ */

static int dr_cpu_type(void)
{
#if defined(__x86_64__) || defined(_M_X64)
    return (int)DR_CPU_TYPE_X86_64;
#elif defined(__aarch64__) || defined(__arm64__)
    return (int)DR_CPU_TYPE_ARM64;
#else
    return 0; /* unsupported host architecture */
#endif
}

static const char *dr_cpu_name(int cpu)
{
    if (cpu == (int)DR_CPU_TYPE_X86_64) {
        return "x86_64";
    }
    if (cpu == (int)DR_CPU_TYPE_ARM64) {
        return "arm64";
    }
    return "unknown";
}

static void dr_free_module(struct dr_module *m)
{
    int i;

    if (m == NULL) {
        return;
    }
    if (m->loaded != NULL && m->loaded != MAP_FAILED) {
        munmap(m->loaded, m->span);
    }
    if (m->file != NULL && m->file != MAP_FAILED) {
        munmap(m->file, m->filesize);
    }
    for (i = 0; i < m->export_count; i++) {
        free(m->exports[i].name);
    }
    free(m->exports);
    free(m);
}

static void dr_free_loader(struct dr_loader *ld)
{
    int i;

    for (i = 0; i < ld->mod_count; i++) {
        dr_free_module(ld->mods[i]);
    }
    free(ld->mods);
    ld->mods = NULL;
    ld->mod_count = 0;
    ld->mod_cap = 0;
}

static int dr_register_module(struct dr_loader *ld, struct dr_module *m)
{
    if (ld->mod_count == ld->mod_cap) {
        int cap = ld->mod_cap ? ld->mod_cap * 2 : 8;
        struct dr_module **grown = realloc(ld->mods,
                                           (size_t)cap * sizeof(*grown));
        if (grown == NULL) {
            return -1;
        }
        ld->mods = grown;
        ld->mod_cap = cap;
    }
    ld->mods[ld->mod_count++] = m;
    return 0;
}

static struct dr_module *dr_load_module(struct dr_loader *ld,
                                        const char *path, int is_main,
                                        char *err, size_t errcap);

/*
 * Load one dependency of from (or of the main image when from is NULL),
 * honouring the weak/reexport flags.  Returns the module, or NULL when a
 * weak reference is missing (not an error) or on failure (err set).
 */
static int dr_load_dependency(struct dr_loader *ld, struct dr_module *from,
                             const char *name, int weak,
                             char *err, size_t errcap)
{
    char resolved[DR_PATH_MAX];

    if (dr_is_system_stub(name)) {
        return 0; /* satisfied by the built-in shim */
    }
    if (dr_resolve_dylib_path(ld, from, name, resolved, sizeof(resolved)) != 0) {
        if (weak) {
            return 0; /* a missing weak dependency is not fatal */
        }
        snprintf(err, errcap, "cannot find dependency '%s'", name);
        return -1;
    }
    if (dr_find_loaded(ld, resolved) != NULL) {
        return 0; /* already mapped (also breaks cycles) */
    }
    if (dr_load_module(ld, resolved, 0, err, errcap) == NULL) {
        return weak ? 0 : -1;
    }
    return 0;
}

/*
 * Map a Mach-O image (executable or dylib), copy its segments, index its
 * exports and recursively load its dependencies.  The module is
 * registered before recursion so a cyclic reference resolves to the
 * in-progress image rather than looping.
 */
static struct dr_module *dr_load_module(struct dr_loader *ld, const char *path,
                                        int is_main, char *err, size_t errcap)
{
    struct dr_module *m;
    int fd;
    struct stat st;
    uint64_t min_vm = UINT64_MAX;
    uint64_t max_end = 0;
    uint64_t span;
    long pagesize;
    int i;
    struct dr_module *existing = dr_find_loaded(ld, path);

    if (existing != NULL) {
        return existing;
    }

    m = calloc(1, sizeof(*m));
    if (m == NULL) {
        snprintf(err, errcap, "out of memory");
        return NULL;
    }
    m->file = MAP_FAILED;
    m->loaded = MAP_FAILED;
    m->is_main = is_main;
    dr_strlcpy(m->path, path, sizeof(m->path));

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(err, errcap, "cannot open '%s': %s", path, strerror(errno));
        dr_free_module(m);
        return NULL;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(err, errcap, "'%s' is not a regular file", path);
        close(fd);
        dr_free_module(m);
        return NULL;
    }
    m->filesize = (size_t)st.st_size;
    if (m->filesize < 32) {
        snprintf(err, errcap, "'%s' is too small to be a Mach-O image", path);
        close(fd);
        dr_free_module(m);
        return NULL;
    }
    m->file = mmap(NULL, m->filesize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m->file == MAP_FAILED) {
        snprintf(err, errcap, "cannot map '%s': %s", path, strerror(errno));
        m->file = NULL;
        dr_free_module(m);
        return NULL;
    }

    if (dr_rd32(m->file) != DR_MH_MAGIC_64) {
        snprintf(err, errcap, "'%s' is not a 64-bit Mach-O image", path);
        goto fail;
    }
    {
        uint32_t filetype = dr_rd32(m->file + 12);

        if (filetype != DR_MH_EXECUTE && filetype != DR_MH_DYLIB) {
            snprintf(err, errcap, "'%s' is not an executable or dylib", path);
            goto fail;
        }
        if (is_main && filetype != DR_MH_EXECUTE) {
            snprintf(err, errcap, "'%s' is not an executable image", path);
            goto fail;
        }
    }
    {
        int want = dr_cpu_type();
        uint32_t have = dr_rd32(m->file + 4);

        if (want == 0) {
            snprintf(err, errcap, "unsupported host architecture for %s", path);
            goto fail;
        }
        if (have != (uint32_t)want) {
            snprintf(err, errcap, "architecture mismatch: '%s' is %s, host is %s",
                     path, dr_cpu_name((int)have), dr_cpu_name(want));
            goto fail;
        }
    }

    if (dr_read_commands(m->file, m->filesize, &m->img, err, errcap) != 0) {
        goto fail;
    }
    if (is_main && m->img.entryoff == 0) {
        snprintf(err, errcap, "'%s' has no LC_MAIN entry point", path);
        goto fail;
    }

    /* Preferred base and total span across the mapped segments. */
    for (i = 0; i < m->img.seg_count; i++) {
        if (strcmp(m->img.segs[i].name, "__PAGEZERO") == 0) {
            continue;
        }
        if (m->img.segs[i].vmaddr < min_vm) {
            min_vm = m->img.segs[i].vmaddr;
        }
        if (m->img.segs[i].vmaddr + m->img.segs[i].vmsize > max_end) {
            max_end = m->img.segs[i].vmaddr + m->img.segs[i].vmsize;
        }
    }
    if (min_vm == UINT64_MAX || max_end <= min_vm) {
        snprintf(err, errcap, "'%s' has no loadable segments", path);
        goto fail;
    }
    m->img.text_vmaddr = min_vm;
    m->img.text_fileoff = 0;
    for (i = 0; i < m->img.seg_count; i++) {
        if (m->img.segs[i].vmaddr == min_vm) {
            m->img.text_fileoff = m->img.segs[i].fileoff;
            break;
        }
    }
    if (is_main && m->img.entryoff < m->img.text_fileoff) {
        snprintf(err, errcap, "'%s' entry point precedes the image base", path);
        goto fail;
    }

    pagesize = sysconf(_SC_PAGESIZE);
    if (pagesize <= 0) {
        pagesize = 4096;
    }
    span = dr_round_up(max_end - m->img.text_vmaddr, (uint64_t)pagesize);
    m->span = span;

    /*
     * Map the image dynamically (no fixed address): under AddressSanitizer
     * the preferred Mach-O base 0x100000000 can overlap the sanitizer's
     * shadow, so the loader takes whatever the kernel hands it and slides
     * every rebase by (loaded - preferred).
     */
    m->loaded = mmap(NULL, span, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m->loaded == MAP_FAILED) {
        snprintf(err, errcap, "cannot map %llu bytes of image: %s",
                 (unsigned long long)span, strerror(errno));
        m->loaded = NULL;
        goto fail;
    }
    m->slide = (int64_t)((uint64_t)(uintptr_t)m->loaded - m->img.text_vmaddr);

    /* Copy each segment's file contents to its slid runtime address. */
    for (i = 0; i < m->img.seg_count; i++) {
        const struct dr_segment *seg = &m->img.segs[i];

        if (strcmp(seg->name, "__PAGEZERO") == 0 || seg->filesize == 0) {
            continue;
        }
        if (seg->fileoff + seg->filesize > m->filesize ||
            seg->vmaddr + seg->filesize > max_end) {
            snprintf(err, errcap, "'%s' segment '%s' is out of range", path,
                     seg->name);
            goto fail;
        }
        memcpy(m->loaded + (seg->vmaddr - m->img.text_vmaddr),
               m->file + seg->fileoff, (size_t)seg->filesize);
    }

    /* Index the exports now that the segments are mapped. */
    if (dr_parse_export_trie(m, err, errcap) != 0 ||
        dr_parse_symtab(m, err, errcap) != 0) {
        goto fail;
    }

    /* Register before recursing so a cycle resolves to this module. */
    if (dr_register_module(ld, m) != 0) {
        snprintf(err, errcap, "out of memory");
        goto fail;
    }

    /* Load the dependencies (skipping libSystem, which the shim covers). */
    for (i = 0; i < m->img.dylib_count; i++) {
        struct dr_dylib_ref *ref = &m->img.dylibs[i];

        if (dr_load_dependency(ld, m, ref->name, ref->weak, err, errcap) != 0) {
            goto fail_unregistered;
        }
    }

    return m;

fail_unregistered:
    /* The module is already registered; drop it from the registry. */
    ld->mod_count--;
    fail:
    dr_free_module(m);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* The loader                                                         */
/* ------------------------------------------------------------------ */

static int dr_protect_module(struct dr_module *m, char *err, size_t errcap)
{
    long pagesize = sysconf(_SC_PAGESIZE);
    int i;

    if (pagesize <= 0) {
        pagesize = 4096;
    }
    for (i = 0; i < m->img.seg_count; i++) {
        const struct dr_segment *seg = &m->img.segs[i];
        int prot = 0;
        uint64_t seg_vm;

        if (strcmp(seg->name, "__PAGEZERO") == 0) {
            continue;
        }
        if (seg->initprot & DR_VM_PROT_READ) {
            prot |= PROT_READ;
        }
        if (seg->initprot & DR_VM_PROT_WRITE) {
            prot |= PROT_WRITE;
        }
        if (seg->initprot & DR_VM_PROT_EXECUTE) {
            prot |= PROT_EXEC;
        }
        seg_vm = (uint64_t)(uintptr_t)m->loaded +
                 (seg->vmaddr - m->img.text_vmaddr);
        if (mprotect((void *)(uintptr_t)seg_vm,
                     dr_round_up(seg->vmsize, (uint64_t)pagesize), prot) != 0) {
            snprintf(err, errcap, "cannot protect segment '%s': %s",
                     seg->name, strerror(errno));
            return -1;
        }
    }
    return 0;
}

/*
 * Run the Mach-O image at path with the given program arguments.  prog_argv
 * is the caller's argument vector with prog_argv[0] naming the image, so
 * the loaded program receives the same argv[0] a native run would.
 * Returns the image's exit status, or a non-zero loader error code.
 */
static int dr_run(const char *path, int prog_argc, char *const prog_argv[])
{
    char err[256];
    struct dr_loader ld;
    struct dr_module *main_mod;
    int i;
    int rc = 1;
    char **argv = NULL;
    char *apple[3];
    char exe_path[DR_PATH_MAX];
    int status;

    memset(&ld, 0, sizeof(ld));
    err[0] = '\0';

    dr_strlcpy(ld.main_path, path, sizeof(ld.main_path));
    dr_dirname(path, ld.exe_dir, sizeof(ld.exe_dir));

    main_mod = dr_load_module(&ld, path, 1, err, sizeof(err));
    if (main_mod == NULL) {
        fprintf(stderr, "masa: run: '%s': %s\n", path, err);
        dr_free_loader(&ld);
        return 1;
    }

    /* Relocations: every module is mapped, so rebase then bind. */
    for (i = 0; i < ld.mod_count; i++) {
        if (dr_module_fixups(&ld, ld.mods[i], DR_FIX_REBASE, err,
                             sizeof(err)) != 0 ||
            dr_module_fixups(&ld, ld.mods[i], DR_FIX_BIND, err,
                             sizeof(err)) != 0) {
            fprintf(stderr, "masa: run: '%s': %s\n", path, err);
            goto done;
        }
    }
    for (i = 0; i < ld.mod_count; i++) {
        if (dr_protect_module(ld.mods[i], err, sizeof(err)) != 0) {
            fprintf(stderr, "masa: run: '%s': %s\n", path, err);
            goto done;
        }
    }

    /*
     * argv: the caller's vector verbatim.  Its argv[0] names the image, so
     * the loaded program sees the same argv[0] a native run would; the
     * image path only feeds the apple[] metadata.
     */
    argv = calloc((size_t)prog_argc + 1, sizeof(*argv));
    if (argv == NULL) {
        fprintf(stderr, "masa: run: out of memory\n");
        goto done;
    }
    for (i = 0; i < prog_argc; i++) {
        argv[i] = prog_argv[i];
    }
    argv[prog_argc] = NULL;

    if (dr_pathfmt(exe_path, sizeof(exe_path), "executable_path=%s", path) >=
        (int)sizeof(exe_path)) {
        exe_path[0] = '\0';
    }
    apple[0] = exe_path;
    apple[1] = (char *)"dyld_file=danroot";
    apple[2] = NULL;

    /* Run it.  A clean exit longjmps back here so cleanup still happens. */
    dr_active_loader = &ld;
    dr_exit_status = 0;
    dr_exit_armed = 1;
    if (setjmp(dr_exit_jmp) == 0) {
        typedef int (*dr_entry_fn)(int, char **, char **, char **);
        uint64_t entry_vm = (uint64_t)(uintptr_t)main_mod->loaded +
                            (main_mod->img.entryoff -
                             main_mod->img.text_fileoff);
        dr_entry_fn entry = (dr_entry_fn)(uintptr_t)entry_vm;

        status = entry(prog_argc, argv, environ, apple);
    } else {
        status = dr_exit_status;
    }
    dr_exit_armed = 0;
    dr_active_loader = NULL;
    rc = status;

done:
    free(argv);
    dr_free_loader(&ld);
    return rc;
}

int masa_danroot_main(int argc, char *const argv[])
{
    const char *root = NULL;
    int i = 0;

    /*
     * Options come before the image: `masa run --root <dir> <binary> ...`.
     * A `--root` on the command line wins over MASA_DANROOT_ROOT.  The
     * image path and everything after it are the program's own arguments.
     */
    while (i < argc && argv[i] != NULL) {
        if (strcmp(argv[i], "--root") == 0) {
            if (i + 1 >= argc || argv[i + 1] == NULL) {
                fprintf(stderr, "masa: run: --root requires a directory\n");
                return 2;
            }
            root = argv[i + 1];
            i += 2;
            continue;
        }
        if (strncmp(argv[i], "--root=", 7) == 0) {
            root = argv[i] + 7;
            i += 1;
            continue;
        }
        break;
    }

    if (root == NULL) {
        root = getenv(MASA_DANROOT_ROOT_ENV);
    }
    dr_root_set(root);

    if (i >= argc || argv[i] == NULL || argv[i][0] == '\0') {
        fprintf(stderr, "masa: run: missing Mach-O executable\n");
        return 2;
    }
    /* argv[i] is the image path; the loaded program gets the rest verbatim. */
    return dr_run(argv[i], argc - i, &argv[i]);
}

/* ------------------------------------------------------------------ */
/* Dynamic linker shims (need the loader above)                       */
/* ------------------------------------------------------------------ */

static void *dr_dlopen(const char *path, int mode)
{
    struct dr_loader *ld = dr_active_loader;
    char err[256];

    (void)mode;
    if (ld == NULL) {
        return NULL;
    }
    if (path == NULL) {
        /* The main executable's handle. */
        return ld->mod_count > 0 ? (void *)ld->mods[0] : NULL;
    }
    err[0] = '\0';
    {
        char vbuf[DR_PATH_MAX];
        char resolved[DR_PATH_MAX];
        const char *vpath;
        struct dr_module *m;
        int before;
        int i;

        vpath = dr_vpath(path, vbuf, sizeof(vbuf));
        if (vpath == NULL) {
            return NULL;
        }
        if (dr_resolve_dylib_path(ld, NULL, vpath, resolved, sizeof(resolved))
            != 0) {
            return NULL;
        }
        before = ld->mod_count;
        m = dr_find_loaded(ld, resolved);
        if (m == NULL) {
            m = dr_load_module(ld, resolved, 0, err, sizeof(err));
            if (m == NULL) {
                return NULL;
            }
        }
        /*
         * A module pulled in here (with its dependencies) is mapped but not
         * yet relocated: dr_run only fixes and protects the images known at
         * startup.  Finish every newly registered module so a function
         * returned by dlsym is callable.
         */
        for (i = before; i < ld->mod_count; i++) {
            if (dr_module_fixups(ld, ld->mods[i], DR_FIX_REBASE, err,
                                 sizeof(err)) != 0 ||
                dr_module_fixups(ld, ld->mods[i], DR_FIX_BIND, err,
                                 sizeof(err)) != 0 ||
                dr_protect_module(ld->mods[i], err, sizeof(err)) != 0) {
                return NULL;
            }
        }
        return (void *)m;
    }
}

/*
 * Resolve a dlsym name the way dyld does.  Mach-O C symbols carry a
 * leading underscore, so dlsym(handle, "malloc") must find "_malloc":
 * try the name as given, then with a leading underscore prepended (and
 * the other way round when the caller already supplied one).
 */
static void *dr_dlsym_lookup(struct dr_loader *ld, struct dr_module *m,
                             const char *name)
{
    char underscored[1024];
    const char *candidates[2];
    int i;

    if (name[0] == '_') {
        candidates[0] = name;
        candidates[1] = name + 1;
    } else {
        (void)dr_pathfmt(underscored, sizeof(underscored), "_%s", name);
        candidates[0] = underscored;
        candidates[1] = name;
    }
    for (i = 0; i < 2; i++) {
        void *p;

        if (candidates[i] == NULL || candidates[i][0] == '\0') {
            continue;
        }
        if (m != NULL) {
            p = dr_module_export(m, candidates[i]);
            if (p != NULL) {
                return p;
            }
        }
        p = dr_resolve_symbol(ld, candidates[i]);
        if (p != NULL) {
            return p;
        }
    }
    return NULL;
}

static void *dr_dlsym(void *handle, const char *name)
{
    struct dr_loader *ld = dr_active_loader;
    struct dr_module *m = (struct dr_module *)handle;

    if (name == NULL || ld == NULL) {
        return NULL;
    }
    return dr_dlsym_lookup(ld, m, name);
}

static int dr_dlclose(void *handle)
{
    (void)handle;
    return 0; /* images stay mapped until the program exits */
}

static char *dr_dlerror(void)
{
    return NULL;
}

#endif /* _WIN32 */
