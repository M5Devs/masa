/*
 * danroot.c - run a Mach-O MH_EXECUTE image on a POSIX host.
 *
 * `masa run <binary> [args...]` (and its alias `masa danroot`) loads a
 * thin 64-bit Mach-O executable built by `masa cc` straight into the
 * caller's address space and runs it, without a virtual machine, a
 * kernel module or a full Darwin userland.
 *
 * This is the Masa Danroot loader.  It is a clean-room reimplementation
 * of the parts of dyld a self-contained executable needs:
 *
 *   - read the load commands and map every LC_SEGMENT_64 with mmap(),
 *   - apply LC_DYLD_CHAINED_FIXUPS rebases and symbol binds,
 *   - resolve the image's imports against a small clean-room libSystem
 *     shim (the handful of C symbols a Masa-linked program references),
 *   - call the LC_MAIN entry point with (argc, argv, envp, apple) and
 *     return the value it produces as the process exit status.
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

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* The process environment, passed to the loaded image as envp[]. */
extern char **environ;

/* ------------------------------------------------------------------ */
/* Mach-O constants                                                   */
/* ------------------------------------------------------------------ */

#define DR_MH_MAGIC_64              0xfeedfacfu
#define DR_MH_EXECUTE               2u

#define DR_CPU_TYPE_X86_64          0x01000007u
#define DR_CPU_TYPE_ARM64           0x0100000cu

#define DR_LC_SEGMENT_64            0x19u
#define DR_LC_MAIN                  0x80000028u
#define DR_LC_DYLD_CHAINED_FIXUPS   0x80000034u

/* dyld_chained_fixups_header.imports_format values. */
#define DR_CHAINED_IMPORT           1u

/* dyld_chained_starts_in_segment.pointer_format values. */
#define DR_CHAINED_PTR_64           2u
#define DR_CHAINED_PTR_64_OFFSET    6u

/* page_start[] sentinel: no fixups on this page. */
#define DR_CHAINED_PTR_START_NONE   0xffffu

/* VM protection bits, matching the Mach-O segment initial protection. */
#define DR_VM_PROT_READ             0x1u
#define DR_VM_PROT_WRITE            0x2u
#define DR_VM_PROT_EXECUTE          0x4u

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

/* ------------------------------------------------------------------ */
/* Clean-room libSystem shim                                          */
/* ------------------------------------------------------------------ */

/*
 * The symbols a Masa-linked C program imports.  Each one is implemented
 * here on top of the host C library, which is exactly the behaviour the
 * program expects; none of the shim is derived from Apple's libSystem.
 */
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

static char *dr_shim_getenv(const char *name)
{
    return getenv(name);
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
    return open(path, flags, mode);
}

static int dr_shim_close(int fd)
{
    return close(fd);
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

struct dr_symbol {
    const char *name;
    void *addr;
};

static const struct dr_symbol dr_symbols[] = {
    { "_printf",            (void *)dr_shim_printf },
    { "_puts",              (void *)dr_shim_puts },
    { "_putchar",           (void *)dr_shim_putchar },
    { "_malloc",            (void *)dr_shim_malloc },
    { "_calloc",            (void *)dr_shim_calloc },
    { "_realloc",           (void *)dr_shim_realloc },
    { "_free",              (void *)dr_shim_free },
    { "_strlen",            (void *)dr_shim_strlen },
    { "_strcmp",            (void *)dr_shim_strcmp },
    { "_strncmp",           (void *)dr_shim_strncmp },
    { "_strcpy",            (void *)dr_shim_strcpy },
    { "_memcpy",            (void *)dr_shim_memcpy },
    { "_memset",            (void *)dr_shim_memset },
    { "_memmove",           (void *)dr_shim_memmove },
    { "_atoi",              (void *)dr_shim_atoi },
    { "_getenv",            (void *)dr_shim_getenv },
    { "_fprintf",           (void *)dr_shim_fprintf },
    { "_write",             (void *)dr_shim_write },
    { "_read",              (void *)dr_shim_read },
    { "_open",              (void *)dr_shim_open },
    { "_close",             (void *)dr_shim_close },
    { "_abort",             (void *)dr_shim_abort },
    { "_exit",              (void *)dr_shim_exit },
    { "dyld_stub_binder",   (void *)dr_shim_stub_binder },
    { NULL, NULL }
};

static void *dr_resolve(const char *name)
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

#define DR_MAX_SEGMENTS 32

struct dr_segment {
    char name[17];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t initprot;
};

struct dr_image {
    struct dr_segment segs[DR_MAX_SEGMENTS];
    int seg_count;
    uint64_t text_vmaddr;      /* preferred vmaddr of the lowest segment */
    uint64_t text_fileoff;     /* file offset that maps to text_vmaddr */
    uint64_t entryoff;         /* LC_MAIN file offset of the entry point */
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
    ncmds = dr_rd32(buf + 16);
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
        }

        off += cmdsize;
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
 * Apply the chained fixups: rebases are rewritten to the image's real
 * load address and binds are resolved against the shim table.  loaded is
 * the runtime address the image's preferred base was mapped at, and
 * slide = loaded - preferred_base.
 */
static int dr_apply_fixups(const unsigned char *file, size_t filesize,
                           unsigned char *loaded, int64_t slide,
                           uint64_t dataoff, uint64_t datasize,
                           char *err, size_t errcap)
{
    const unsigned char *hdr;
    uint32_t starts_off;
    uint32_t imports_off;
    uint32_t symbols_off;
    uint32_t imports_count;
    uint32_t imports_format;
    uint32_t symbols_format;
    const unsigned char *symbols;
    const char **names;
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
    if (symbols_off > datasize || imports_off + (size_t)imports_count * 4 > datasize) {
        snprintf(err, errcap, "chained fixups table out of range");
        return -1;
    }

    symbols = hdr + symbols_off;

    /* Build the import name table from the symbol string pool. */
    names = calloc(imports_count ? imports_count : 1, sizeof(*names));
    if (names == NULL) {
        snprintf(err, errcap, "out of memory");
        return -1;
    }
    for (s = 0; s < imports_count; s++) {
        uint32_t word = dr_rd32(hdr + imports_off + s * 4);
        uint32_t name_off = (word >> 9) & 0x7fffffu; /* 23-bit name offset */

        if (symbols_off + name_off >= datasize) {
            snprintf(err, errcap, "import %u name out of range", s);
            free(names);
            return -1;
        }
        names[s] = (const char *)(symbols + name_off);
    }

    if (starts_off + 4 > datasize) {
        snprintf(err, errcap, "chained starts out of range");
        free(names);
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
                    uint32_t ordinal = (uint32_t)(word & 0xffffffu);
                    uint32_t addend = (uint32_t)((word >> 24) & 0xffu);
                    void *addr;

                    if (ordinal >= imports_count) {
                        snprintf(err, errcap, "bind ordinal %u out of range",
                                 ordinal);
                        rc = -1;
                        break;
                    }
                    addr = dr_resolve(names[ordinal]);
                    if (addr == NULL) {
                        snprintf(err, errcap,
                                 "unresolved symbol '%s'", names[ordinal]);
                        rc = -1;
                        break;
                    }
                    *slot = (uint64_t)(uintptr_t)addr + addend;
                } else {
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
    return rc;
}

/* ------------------------------------------------------------------ */
/* The loader                                                         */
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

/*
 * Run the Mach-O image at path with the given program arguments.  prog_argv
 * is the caller's argument vector with prog_argv[0] naming the image, so
 * the loaded program receives the same argv[0] a native run would.
 * Returns the image's exit status, or a non-zero loader error code.
 */
static int dr_run(const char *path, int prog_argc, char *const prog_argv[])
{
    char err[256];
    int fd;
    struct stat st;
    unsigned char *file = NULL;
    size_t filesize = 0;
    struct dr_image img;
    uint64_t dataoff = 0, datasize = 0;
    uint64_t min_vm = UINT64_MAX;
    uint64_t max_end = 0;
    uint64_t span;
    unsigned char *loaded = MAP_FAILED;
    int64_t slide;
    int i;
    int rc = 1;
    char **argv = NULL;
    char *apple[3];
    char exe_path[4096];
    int status;
    long pagesize;

    err[0] = '\0';

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "masa: run: cannot open '%s': %s\n",
                path, strerror(errno));
        return 1;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "masa: run: '%s' is not a regular file\n", path);
        close(fd);
        return 1;
    }
    filesize = (size_t)st.st_size;
    if (filesize < 32) {
        fprintf(stderr, "masa: run: '%s' is too small to be a Mach-O image\n",
                path);
        close(fd);
        return 1;
    }
    file = mmap(NULL, filesize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED) {
        fprintf(stderr, "masa: run: cannot map '%s': %s\n",
                path, strerror(errno));
        return 1;
    }

    /* Header checks: 64-bit Mach-O executable for the host architecture. */
    if (dr_rd32(file) != DR_MH_MAGIC_64) {
        fprintf(stderr, "masa: run: '%s' is not a 64-bit Mach-O image\n",
                path);
        goto done_file;
    }
    if (dr_rd32(file + 12) != DR_MH_EXECUTE) {
        fprintf(stderr, "masa: run: '%s' is not an executable image\n", path);
        goto done_file;
    }
    {
        int want = dr_cpu_type();
        uint32_t have = dr_rd32(file + 4);

        if (want == 0) {
            fprintf(stderr,
                    "masa: run: unsupported host architecture for %s\n", path);
            goto done_file;
        }
        if (have != (uint32_t)want) {
            fprintf(stderr,
                    "masa: run: architecture mismatch: '%s' is %s, host is %s\n",
                    path, dr_cpu_name((int)have), dr_cpu_name(want));
            goto done_file;
        }
    }

    if (dr_read_commands(file, filesize, &img, err, sizeof(err)) != 0) {
        fprintf(stderr, "masa: run: '%s': %s\n", path, err);
        goto done_file;
    }
    if (img.entryoff == 0) {
        fprintf(stderr, "masa: run: '%s' has no LC_MAIN entry point\n", path);
        goto done_file;
    }

    /* Preferred base and total span across the mapped segments. */
    min_vm = UINT64_MAX;
    for (i = 0; i < img.seg_count; i++) {
        if (strcmp(img.segs[i].name, "__PAGEZERO") == 0) {
            continue;
        }
        if (img.segs[i].vmaddr < min_vm) {
            min_vm = img.segs[i].vmaddr;
        }
        if (img.segs[i].vmaddr + img.segs[i].vmsize > max_end) {
            max_end = img.segs[i].vmaddr + img.segs[i].vmsize;
        }
    }
    if (min_vm == UINT64_MAX || max_end <= min_vm) {
        fprintf(stderr, "masa: run: '%s' has no loadable segments\n", path);
        goto done_file;
    }

    /*
     * The image base is the lowest segment's vmaddr; the file offset that
     * maps to it comes from whichever segment sits there (__TEXT, whose
     * fileoff is 0 in a Masa-linked image).  Both the segment copy and the
     * LC_MAIN entry translation are relative to this pair.
     */
    img.text_vmaddr = min_vm;
    img.text_fileoff = 0;
    for (i = 0; i < img.seg_count; i++) {
        if (img.segs[i].vmaddr == min_vm) {
            img.text_fileoff = img.segs[i].fileoff;
            break;
        }
    }
    if (img.entryoff < img.text_fileoff) {
        fprintf(stderr, "masa: run: '%s' entry point precedes the image base\n",
                path);
        goto done_file;
    }

    pagesize = sysconf(_SC_PAGESIZE);
    if (pagesize <= 0) {
        pagesize = 4096;
    }
    span = dr_round_up(max_end - img.text_vmaddr, (uint64_t)pagesize);

    /*
     * Map the image dynamically (no fixed address): under AddressSanitizer
     * the preferred Mach-O base 0x100000000 can overlap the sanitizer's
     * shadow, so the loader takes whatever the kernel hands it and slides
     * every rebase by (loaded - preferred).
     */
    loaded = mmap(NULL, span, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (loaded == MAP_FAILED) {
        fprintf(stderr, "masa: run: cannot map %llu bytes of image: %s\n",
                (unsigned long long)span, strerror(errno));
        goto done_file;
    }
    slide = (int64_t)((uint64_t)(uintptr_t)loaded - img.text_vmaddr);

    /* Copy each segment's file contents to its slid runtime address. */
    for (i = 0; i < img.seg_count; i++) {
        const struct dr_segment *seg = &img.segs[i];

        if (strcmp(seg->name, "__PAGEZERO") == 0) {
            continue;
        }
        if (seg->filesize == 0) {
            continue;
        }
        if (seg->fileoff + seg->filesize > filesize ||
            seg->vmaddr + seg->filesize > max_end) {
            fprintf(stderr, "masa: run: '%s' segment '%s' is out of range\n",
                    path, seg->name);
            goto done_image;
        }
        memcpy(loaded + (seg->vmaddr - img.text_vmaddr),
               file + seg->fileoff, (size_t)seg->filesize);
    }

    /* Relocations, then the final per-segment protections. */
    if (dr_find_chained_fixups(file, filesize, &dataoff, &datasize) == 0) {
        if (dr_apply_fixups(file, filesize, loaded, slide, dataoff, datasize,
                            err, sizeof(err)) != 0) {
            fprintf(stderr, "masa: run: '%s': %s\n", path, err);
            goto done_image;
        }
    }
    for (i = 0; i < img.seg_count; i++) {
        const struct dr_segment *seg = &img.segs[i];
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
        seg_vm = (uint64_t)(uintptr_t)loaded + (seg->vmaddr - img.text_vmaddr);
        if (mprotect((void *)(uintptr_t)seg_vm,
                     dr_round_up(seg->vmsize, (uint64_t)pagesize), prot) != 0) {
            fprintf(stderr, "masa: run: cannot protect segment '%s': %s\n",
                    seg->name, strerror(errno));
            goto done_image;
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
        goto done_image;
    }
    for (i = 0; i < prog_argc; i++) {
        argv[i] = prog_argv[i];
    }
    argv[prog_argc] = NULL;

    if (snprintf(exe_path, sizeof(exe_path), "executable_path=%s", path) >=
        (int)sizeof(exe_path)) {
        exe_path[0] = '\0';
    }
    apple[0] = exe_path;
    apple[1] = (char *)"dyld_file=danroot";
    apple[2] = NULL;

    /* Run it.  A clean exit longjmps back here so cleanup still happens. */
    dr_exit_status = 0;
    dr_exit_armed = 1;
    if (setjmp(dr_exit_jmp) == 0) {
        typedef int (*dr_entry_fn)(int, char **, char **, char **);
        uint64_t entry_vm = (uint64_t)(uintptr_t)loaded +
                            (img.entryoff - img.text_fileoff);
        dr_entry_fn entry = (dr_entry_fn)(uintptr_t)entry_vm;

        status = entry(prog_argc, argv, environ, apple);
    } else {
        status = dr_exit_status;
    }
    dr_exit_armed = 0;
    rc = status;

done_image:
    munmap(loaded, span);
done_file:
    free(argv);
    munmap(file, filesize);
    return rc;
}

int masa_danroot_main(int argc, char *const argv[])
{
    if (argc < 1 || argv[0] == NULL || argv[0][0] == '\0') {
        fprintf(stderr, "masa: run: missing Mach-O executable\n");
        return 2;
    }
    /* argv[0] is the image path; the loaded program gets argv verbatim. */
    return dr_run(argv[0], argc, argv);
}

#endif /* _WIN32 */
