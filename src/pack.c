/*
 * pack.c - `masa pack`: package a built .app bundle into an iOS .ipa or a
 * macOS UDIF .dmg, using nothing but the C standard library.
 *
 * The point is distribution without external tools: no `zip`, no
 * `hdiutil`, no `codesign`.  Two formats are produced:
 *
 *   --ipa  A ZIP archive (PKZIP, STORE method, CRC32) with the app under
 *          `Payload/<App>.app/`, which is exactly what an .ipa is.
 *
 *   --dmg  An Apple UDIF disk image: a clean image payload holding the
 *          .app directory tree, then the UDIF XML plist (the `blkx`
 *          resource fork, base64-encoded) and finally the genuine
 *          512-byte `koly` trailer at EOF.
 *
 * Everything is little-endian on disk, as both formats require.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
/* Large-file offsets (fseeko/ftello) on 32-bit hosts too. */
#define _FILE_OFFSET_BITS 64
#endif

#include "masa.h"
#include "masa_compat.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
typedef __int64 pack_off_t;
#define pack_fseek _fseeki64
#define pack_ftell _ftelli64
#else
typedef off_t pack_off_t;
#define pack_fseek fseeko
#define pack_ftell ftello
#endif

/* A 512-byte disk-image sector: the UDIF image and its trailer align to it. */
#define DMG_SECTOR 512u

/* ------------------------------------------------------------------ */
/* Little-endian field writers                                        */
/* ------------------------------------------------------------------ */

static void put_u16le(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
}

static void put_u32le(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
    p[2] = (unsigned char)((v >> 16) & 0xffu);
    p[3] = (unsigned char)((v >> 24) & 0xffu);
}

static void put_u64le(unsigned char *p, uint64_t v)
{
    put_u32le(p, (uint32_t)(v & 0xffffffffu));
    put_u32le(p + 4, (uint32_t)(v >> 32));
}

/* ------------------------------------------------------------------ */
/* CRC32 (the IEEE 802.3 polynomial, as used by ZIP and UDIF)         */
/* ------------------------------------------------------------------ */

#define CRC32_INIT 0xFFFFFFFFu

static uint32_t crc_table[256];

static void crc_table_init(void)
{
    int i;
    int k;

    for (i = 0; i < 256; i++) {
        uint32_t c = (uint32_t)i;

        for (k = 0; k < 8; k++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc_table[i] = c;
    }
}

static uint32_t crc32_update(uint32_t crc, const void *buf, size_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    size_t i;

    for (i = 0; i < len; i++) {
        crc = crc_table[(crc ^ p[i]) & 0xffu] ^ (crc >> 8);
    }
    return crc;
}

static uint32_t crc32_final(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ */
/* base64 (for the UDIF `blkx` <data> element)                        */
/* ------------------------------------------------------------------ */

static size_t base64_size(size_t len)
{
    return 4 * ((len + 2) / 3);
}

static void base64_encode(const unsigned char *in, size_t len, char *out)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0;
    size_t o = 0;

    while (i + 3 <= len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) |
                     (uint32_t)in[i + 2];

        out[o++] = tbl[(v >> 18) & 0x3fu];
        out[o++] = tbl[(v >> 12) & 0x3fu];
        out[o++] = tbl[(v >> 6) & 0x3fu];
        out[o++] = tbl[v & 0x3fu];
        i += 3;
    }
    if (len - i == 1) {
        uint32_t v = (uint32_t)in[i] << 16;

        out[o++] = tbl[(v >> 18) & 0x3fu];
        out[o++] = tbl[(v >> 12) & 0x3fu];
        out[o++] = '=';
        out[o++] = '=';
    } else if (len - i == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);

        out[o++] = tbl[(v >> 18) & 0x3fu];
        out[o++] = tbl[(v >> 12) & 0x3fu];
        out[o++] = tbl[(v >> 6) & 0x3fu];
        out[o++] = '=';
    }
    out[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* Directory walk                                                     */
/* ------------------------------------------------------------------ */

struct path_list {
    char **items;
    size_t count;
    size_t cap;
};

static void path_list_free(struct path_list *l)
{
    size_t i;

    for (i = 0; i < l->count; i++) {
        free(l->items[i]);
    }
    free(l->items);
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

static int path_list_push(struct path_list *l, const char *s)
{
    if (l->count + 1 > l->cap) {
        size_t next = (l->cap == 0) ? 8 : l->cap * 2;
        char **grown = realloc(l->items, next * sizeof(*grown));

        if (grown == NULL) {
            return -1;
        }
        l->items = grown;
        l->cap = next;
    }
    l->items[l->count] = strdup(s);
    if (l->items[l->count] == NULL) {
        return -1;
    }
    l->count++;
    return 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/*
 * Collect every regular file under `root` into `out`, as paths relative
 * to `root` (using '/' as the separator on every host).  `rel` is the
 * prefix for the current level; `depth` guards against symlink cycles.
 */
static int walk_collect(const char *root, const char *rel, int depth,
                        struct path_list *out)
{
    MASA_DIR *d;
    struct masa_dirent *ent;

    if (depth > 64) {
        return 0;
    }

    d = masa_opendir(root);
    if (d == NULL) {
        fprintf(stderr, "masa: pack: cannot read '%s': %s\n", root,
                strerror(errno));
        return -1;
    }

    while ((ent = masa_readdir(d)) != NULL) {
        char full[MASA_PATH_MAX];
        char child[MASA_PATH_MAX];
        struct stat st;
        int n;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        n = snprintf(full, sizeof(full), "%s/%s", root, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(full)) {
            masa_closedir(d);
            fprintf(stderr, "masa: pack: path too long under '%s'\n", root);
            return -1;
        }
        if (rel[0] == '\0') {
            n = snprintf(child, sizeof(child), "%s", ent->d_name);
        } else {
            n = snprintf(child, sizeof(child), "%s/%s", rel, ent->d_name);
        }
        if (n < 0 || (size_t)n >= sizeof(child)) {
            masa_closedir(d);
            fprintf(stderr, "masa: pack: path too long under '%s'\n", root);
            return -1;
        }

        if (stat(full, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (walk_collect(full, child, depth + 1, out) != 0) {
                masa_closedir(d);
                return -1;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (path_list_push(out, child) != 0) {
                masa_closedir(d);
                return -1;
            }
        }
    }

    masa_closedir(d);
    return 0;
}

/* The final path component, for deriving the application name. */
static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');

#ifdef _WIN32
    {
        const char *bslash = strrchr(path, '\\');

        if (bslash != NULL && (slash == NULL || bslash > slash)) {
            slash = bslash;
        }
    }
#endif
    return (slash != NULL) ? slash + 1 : path;
}

static void dos_time(time_t t, uint16_t *out_time, uint16_t *out_date)
{
    struct tm *tm = localtime(&t);
    int year;

    if (tm == NULL) {
        *out_time = 0;
        *out_date = 0x0021; /* 1980-01-01 */
        return;
    }
    year = tm->tm_year + 1900;
    if (year < 1980) {
        year = 1980;
    }
    if (year > 2107) {
        year = 2107;
    }
    *out_date = (uint16_t)(((year - 1980) << 9) | ((tm->tm_mon + 1) << 5) |
                           tm->tm_mday);
    *out_time = (uint16_t)((tm->tm_hour << 11) | (tm->tm_min << 5) |
                           (tm->tm_sec / 2));
}

/* ------------------------------------------------------------------ */
/* ZIP writer (STORE, CRC32) - the .ipa container                     */
/* ------------------------------------------------------------------ */

struct zip_entry {
    char *name;
    uint32_t crc;
    uint64_t size;
    uint64_t offset;
    uint16_t dostime;
    uint16_t dosdate;
};

/*
 * Write one file as a ZIP local header + stored data.  The CRC and sizes
 * are patched into the header afterwards, so the file is read only once.
 */
static int zip_write_file(FILE *fp, const char *disk_path, const char *zip_name,
                          uint16_t dostime, uint16_t dosdate,
                          struct zip_entry *e)
{
    unsigned char hdr[30];
    unsigned char buf[65536];
    unsigned char patch[12];
    uint32_t crc = CRC32_INIT;
    uint64_t total = 0;
    pack_off_t offset;
    pack_off_t after;
    size_t nlen = strlen(zip_name);
    FILE *in;
    size_t got;

    in = fopen(disk_path, "rb");
    if (in == NULL) {
        fprintf(stderr, "masa: pack: cannot read '%s': %s\n", disk_path,
                strerror(errno));
        return -1;
    }

    offset = pack_ftell(fp);
    if (offset < 0) {
        fclose(in);
        return -1;
    }

    memset(hdr, 0, sizeof(hdr));
    put_u32le(hdr + 0, 0x04034b50u); /* local file header */
    put_u16le(hdr + 4, 20);          /* version needed (2.0) */
    put_u16le(hdr + 6, 0);           /* flags */
    put_u16le(hdr + 8, 0);           /* compression: store */
    put_u16le(hdr + 10, dostime);
    put_u16le(hdr + 12, dosdate);
    put_u32le(hdr + 14, 0);          /* crc (patched below) */
    put_u32le(hdr + 18, 0);          /* compressed size (patched) */
    put_u32le(hdr + 22, 0);          /* uncompressed size (patched) */
    put_u16le(hdr + 26, (uint16_t)nlen);
    put_u16le(hdr + 28, 0);          /* extra length */

    if (fwrite(hdr, 1, sizeof(hdr), fp) != sizeof(hdr) ||
        fwrite(zip_name, 1, nlen, fp) != nlen) {
        fclose(in);
        fprintf(stderr, "masa: pack: write failed: %s\n", strerror(errno));
        return -1;
    }

    while ((got = fread(buf, 1, sizeof(buf), in)) > 0) {
        crc = crc32_update(crc, buf, got);
        if (fwrite(buf, 1, got, fp) != got) {
            fclose(in);
            fprintf(stderr, "masa: pack: write failed: %s\n", strerror(errno));
            return -1;
        }
        total += got;
    }
    if (ferror(in)) {
        fclose(in);
        fprintf(stderr, "masa: pack: read failed: %s\n", strerror(errno));
        return -1;
    }
    fclose(in);

    if (total > 0xFFFFFFFFu) {
        fprintf(stderr,
                "masa: pack: '%s' is too large for a ZIP32 archive\n",
                disk_path);
        return -1;
    }

    crc = crc32_final(crc);
    after = pack_ftell(fp);
    if (after < 0) {
        return -1;
    }
    put_u32le(patch + 0, crc);
    put_u32le(patch + 4, (uint32_t)total);
    put_u32le(patch + 8, (uint32_t)total);
    if (pack_fseek(fp, offset + 14, SEEK_SET) != 0 ||
        fwrite(patch, 1, sizeof(patch), fp) != sizeof(patch) ||
        pack_fseek(fp, after, SEEK_SET) != 0) {
        fprintf(stderr, "masa: pack: seek/write failed: %s\n", strerror(errno));
        return -1;
    }

    e->name = strdup(zip_name);
    if (e->name == NULL) {
        return -1;
    }
    e->crc = crc;
    e->size = total;
    e->offset = (uint64_t)offset;
    e->dostime = dostime;
    e->dosdate = dosdate;
    return 0;
}

static int zip_write_central(FILE *fp, const struct zip_entry *entries,
                             size_t count, uint64_t cd_offset)
{
    unsigned char hdr[46];
    unsigned char eocd[22];
    uint64_t cd_size = 0;
    size_t i;

    for (i = 0; i < count; i++) {
        size_t nlen = strlen(entries[i].name);

        memset(hdr, 0, sizeof(hdr));
        put_u32le(hdr + 0, 0x02014b50u); /* central directory header */
        put_u16le(hdr + 4, 20);          /* version made by */
        put_u16le(hdr + 6, 20);          /* version needed */
        put_u16le(hdr + 8, 0);           /* flags */
        put_u16le(hdr + 10, 0);          /* store */
        put_u16le(hdr + 12, entries[i].dostime);
        put_u16le(hdr + 14, entries[i].dosdate);
        put_u32le(hdr + 16, entries[i].crc);
        put_u32le(hdr + 20, (uint32_t)entries[i].size);
        put_u32le(hdr + 24, (uint32_t)entries[i].size);
        put_u16le(hdr + 28, (uint16_t)nlen);
        put_u16le(hdr + 30, 0); /* extra */
        put_u16le(hdr + 32, 0); /* comment */
        put_u16le(hdr + 34, 0); /* disk number */
        put_u16le(hdr + 36, 0); /* internal attributes */
        put_u32le(hdr + 38, 0); /* external attributes */
        put_u32le(hdr + 42, (uint32_t)entries[i].offset);

        if (fwrite(hdr, 1, sizeof(hdr), fp) != sizeof(hdr) ||
            fwrite(entries[i].name, 1, nlen, fp) != nlen) {
            return -1;
        }
        cd_size += sizeof(hdr) + nlen;
    }

    memset(eocd, 0, sizeof(eocd));
    put_u32le(eocd + 0, 0x06054b50u); /* end of central directory */
    put_u16le(eocd + 4, 0);           /* this disk */
    put_u16le(eocd + 6, 0);           /* disk with the directory */
    put_u16le(eocd + 8, (uint16_t)count);
    put_u16le(eocd + 10, (uint16_t)count);
    put_u32le(eocd + 12, (uint32_t)cd_size);
    put_u32le(eocd + 16, (uint32_t)cd_offset);
    put_u16le(eocd + 20, 0);          /* comment length */
    return (fwrite(eocd, 1, sizeof(eocd), fp) == sizeof(eocd)) ? 0 : -1;
}

static int pack_ipa(const char *app_dir, const char *app_name,
                    const char *out_path)
{
    struct path_list files;
    struct zip_entry *entries = NULL;
    char prefix[MASA_PATH_MAX];
    FILE *fp;
    size_t i;
    int rc = 0;

    memset(&files, 0, sizeof(files));

    if (walk_collect(app_dir, "", 0, &files) != 0) {
        path_list_free(&files);
        return 1;
    }
    if (files.count > 0xFFFFu) {
        fprintf(stderr, "masa: pack: too many files for a ZIP32 archive\n");
        path_list_free(&files);
        return 1;
    }
    if (files.count > 1) {
        qsort(files.items, files.count, sizeof(files.items[0]), cmp_str);
    }

    if (snprintf(prefix, sizeof(prefix), "Payload/%s.app/", app_name) >=
        (int)sizeof(prefix)) {
        fprintf(stderr, "masa: pack: application name too long\n");
        path_list_free(&files);
        return 1;
    }

    entries = calloc(files.count + 1, sizeof(*entries));
    if (entries == NULL) {
        path_list_free(&files);
        return 1;
    }

    fp = fopen(out_path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "masa: pack: cannot create '%s': %s\n", out_path,
                strerror(errno));
        free(entries);
        path_list_free(&files);
        return 1;
    }

    for (i = 0; i < files.count; i++) {
        char disk[MASA_PATH_MAX];
        char zipname[MASA_PATH_MAX];
        struct stat st;
        uint16_t dtime = 0;
        uint16_t ddate = 0x0021;

        if (snprintf(disk, sizeof(disk), "%s/%s", app_dir, files.items[i]) >=
                (int)sizeof(disk) ||
            snprintf(zipname, sizeof(zipname), "%s%s", prefix,
                     files.items[i]) >= (int)sizeof(zipname)) {
            fprintf(stderr, "masa: pack: path too long\n");
            rc = 1;
            break;
        }
        if (stat(disk, &st) == 0) {
            dos_time(st.st_mtime, &dtime, &ddate);
        }
        if (zip_write_file(fp, disk, zipname, dtime, ddate, &entries[i]) != 0) {
            rc = 1;
            break;
        }
    }

    if (rc == 0) {
        pack_off_t cd = pack_ftell(fp);

        if (cd < 0 ||
            zip_write_central(fp, entries, files.count, (uint64_t)cd) != 0) {
            fprintf(stderr, "masa: pack: write failed: %s\n", strerror(errno));
            rc = 1;
        }
    }

    fclose(fp);

    for (i = 0; i < files.count; i++) {
        free(entries[i].name);
    }
    free(entries);
    path_list_free(&files);

    if (rc != 0) {
        remove(out_path);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* UDIF (.dmg) writer                                                 */
/* ------------------------------------------------------------------ */

/*
 * The UDIF trailer (`koly`) is a fixed 512-byte structure at EOF.  Field
 * offsets, in order:
 *   0 magic "koly", 4 version, 8 headerSize, 12 flags,
 *   16 runningDataForkOffset, 24 dataForkOffset, 32 dataForkLength,
 *   40 rsrcForkOffset, 48 rsrcForkLength, 56 segmentNumber, 60 segmentCount,
 *   64 segmentID[16], 80 dataChecksumType, 84 dataChecksumSize,
 *   88 dataChecksum[128], 216 xmlOffset, 224 xmlLength, 232 reserved[120],
 *   352 checksumType, 356 checksumSize, 360 checksum[128], 488 imageVariant,
 *   492 sectorCount, 500 reserved2, 504 reserved3, 508 reserved4.
 */
struct dmg_writer {
    FILE *fp;
    uint32_t data_crc; /* CRC32 of the data fork */
    uint32_t file_crc; /* CRC32 of everything before the trailer */
    uint64_t data_len;
    uint64_t file_len;
};

static int dmg_write(struct dmg_writer *w, const void *buf, size_t len,
                     int is_data)
{
    if (len > 0 && fwrite(buf, 1, len, w->fp) != len) {
        fprintf(stderr, "masa: pack: write failed: %s\n", strerror(errno));
        return -1;
    }
    if (is_data) {
        w->data_crc = crc32_update(w->data_crc, buf, len);
        w->data_len += len;
    }
    w->file_crc = crc32_update(w->file_crc, buf, len);
    w->file_len += len;
    return 0;
}

static int dmg_write_file_data(struct dmg_writer *w, const char *path)
{
    unsigned char buf[65536];
    FILE *in = fopen(path, "rb");
    size_t got;

    if (in == NULL) {
        fprintf(stderr, "masa: pack: cannot read '%s': %s\n", path,
                strerror(errno));
        return -1;
    }
    while ((got = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (dmg_write(w, buf, got, 1) != 0) {
            fclose(in);
            return -1;
        }
    }
    if (ferror(in)) {
        fprintf(stderr, "masa: pack: read failed: %s\n", strerror(errno));
        fclose(in);
        return -1;
    }
    fclose(in);
    return 0;
}

/*
 * Serialise the .app tree into the data fork as a self-describing image:
 *   "MASAIMG1" (8), u32 file count, u32 reserved,
 *   then per file: u32 path length, u64 file size, path bytes, file bytes.
 * The fork is zero-padded to a whole number of 512-byte sectors.
 */
static int dmg_write_payload(struct dmg_writer *w, const char *app_dir,
                             const struct path_list *files)
{
    unsigned char hdr[16];
    size_t i;

    memcpy(hdr, "MASAIMG1", 8);
    put_u32le(hdr + 8, (uint32_t)files->count);
    put_u32le(hdr + 12, 0);
    if (dmg_write(w, hdr, sizeof(hdr), 1) != 0) {
        return -1;
    }

    for (i = 0; i < files->count; i++) {
        char disk[MASA_PATH_MAX];
        unsigned char fh[12];
        struct stat st;
        size_t plen = strlen(files->items[i]);

        if (snprintf(disk, sizeof(disk), "%s/%s", app_dir,
                     files->items[i]) >= (int)sizeof(disk)) {
            fprintf(stderr, "masa: pack: path too long\n");
            return -1;
        }
        if (stat(disk, &st) != 0) {
            fprintf(stderr, "masa: pack: cannot stat '%s': %s\n", disk,
                    strerror(errno));
            return -1;
        }
        put_u32le(fh + 0, (uint32_t)plen);
        put_u64le(fh + 4, (uint64_t)st.st_size);
        if (dmg_write(w, fh, sizeof(fh), 1) != 0 ||
            dmg_write(w, files->items[i], plen, 1) != 0 ||
            dmg_write_file_data(w, disk) != 0) {
            return -1;
        }
    }

    while (w->data_len % DMG_SECTOR != 0) {
        unsigned char zero = 0;

        if (dmg_write(w, &zero, 1, 1) != 0) {
            return -1;
        }
    }
    return 0;
}

/*
 * Build the UDIF `blkx` resource: a `mish` table with a single data run
 * covering the whole data fork and a terminating run.  Runs begin at
 * offset 0xCC.
 */
static void dmg_build_blkx(unsigned char *table, uint64_t sector_count,
                           uint64_t data_len)
{
    memset(table, 0, 204 + 2 * 40);
    /* "mish" as the on-disk bytes 6D 69 73 68, written little-endian. */
    put_u32le(table + 0x00, 0x6873696Du); /* 'mish' */
    put_u32le(table + 0x04, 1);           /* version */
    put_u32le(table + 0x08, 0);           /* sector number */
    put_u64le(table + 0x0C, sector_count);
    put_u32le(table + 0x14, 0);           /* data offset */
    put_u32le(table + 0x18, 0);           /* buffers needed */
    put_u32le(table + 0x1C, 1);           /* block descriptors */
    put_u32le(table + 0xC0, 2);           /* run count */
    /* run 0: the image data */
    put_u32le(table + 0xCC, 0x00000002u); /* type: data */
    put_u64le(table + 0xD4, 0);           /* sector start */
    put_u64le(table + 0xDC, sector_count);
    put_u64le(table + 0xE4, 0);           /* compressed offset */
    put_u64le(table + 0xEC, data_len);    /* compressed length */
    /* run 1: terminator */
    put_u32le(table + 0xF4, 0x7FFFFFFEu); /* type: last block */
    put_u64le(table + 0xFC, sector_count);
}

static int pack_dmg(const char *app_dir, const char *app_name,
                    const char *out_path)
{
    struct path_list files;
    struct dmg_writer w;
    unsigned char blkx[204 + 2 * 40];
    char *b64 = NULL;
    char *xml = NULL;
    unsigned char koly[512];
    uint64_t sector_count;
    uint64_t xml_len;
    uint32_t data_crc;
    uint32_t master_crc;
    uint32_t seg_seed;
    int rc = 1;

    memset(&files, 0, sizeof(files));

    if (walk_collect(app_dir, "", 0, &files) != 0) {
        path_list_free(&files);
        return 1;
    }
    if (files.count > 1) {
        qsort(files.items, files.count, sizeof(files.items[0]), cmp_str);
    }

    memset(&w, 0, sizeof(w));
    w.fp = fopen(out_path, "wb");
    if (w.fp == NULL) {
        fprintf(stderr, "masa: pack: cannot create '%s': %s\n", out_path,
                strerror(errno));
        path_list_free(&files);
        return 1;
    }
    w.data_crc = CRC32_INIT;
    w.file_crc = CRC32_INIT;

    if (dmg_write_payload(&w, app_dir, &files) != 0) {
        goto done;
    }
    sector_count = w.data_len / DMG_SECTOR;

    dmg_build_blkx(blkx, sector_count, w.data_len);
    {
        size_t blen = sizeof(blkx);
        size_t need = base64_size(blen) + 1;

        b64 = malloc(need);
        if (b64 == NULL) {
            goto done;
        }
        base64_encode(blkx, blen, b64);
    }

    xml = malloc(8192);
    if (xml == NULL) {
        goto done;
    }
    snprintf(xml, 8192,
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
             "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
             "<plist version=\"1.0\">\n"
             "<dict>\n"
             "\t<key>resource-fork</key>\n"
             "\t<dict>\n"
             "\t\t<key>blkx</key>\n"
             "\t\t<array>\n"
             "\t\t\t<dict>\n"
             "\t\t\t\t<key>Attributes</key>\n"
             "\t\t\t\t<string>0x0050</string>\n"
             "\t\t\t\t<key>CFName</key>\n"
             "\t\t\t\t<string>%s (Apple_HFS : 0)</string>\n"
             "\t\t\t\t<key>Data</key>\n"
             "\t\t\t\t<data>%s</data>\n"
             "\t\t\t\t<key>ID</key>\n"
             "\t\t\t\t<string>0</string>\n"
             "\t\t\t\t<key>Name</key>\n"
             "\t\t\t\t<string>Apple_HFS</string>\n"
             "\t\t\t</dict>\n"
             "\t\t</array>\n"
             "\t</dict>\n"
             "</dict>\n"
             "</plist>\n",
             app_name, b64);

    xml_len = strlen(xml);
    if (dmg_write(&w, xml, xml_len, 0) != 0) {
        goto done;
    }

    data_crc = crc32_final(w.data_crc);
    master_crc = crc32_final(w.file_crc);
    seg_seed = crc32_final(crc32_update(CRC32_INIT, app_name,
                                        strlen(app_name)));

    memset(koly, 0, sizeof(koly));
    memcpy(koly + 0, "koly", 4);
    put_u32le(koly + 4, 4);                    /* version */
    put_u32le(koly + 8, 512);                  /* headerSize */
    put_u32le(koly + 12, 0x00000001u);         /* flags: flattened */
    put_u64le(koly + 16, 0);                   /* runningDataForkOffset */
    put_u64le(koly + 24, 0);                   /* dataForkOffset */
    put_u64le(koly + 32, w.data_len);          /* dataForkLength */
    put_u64le(koly + 40, 0);                   /* rsrcForkOffset */
    put_u64le(koly + 48, 0);                   /* rsrcForkLength */
    put_u32le(koly + 56, 1);                   /* segmentNumber */
    put_u32le(koly + 60, 1);                   /* segmentCount */
    put_u32le(koly + 64, seg_seed);            /* segmentID */
    put_u32le(koly + 68, seg_seed ^ 0x9E3779B9u);
    put_u32le(koly + 72, ~seg_seed);
    put_u32le(koly + 76, seg_seed + 0x85EBCA6Bu);
    put_u32le(koly + 80, 2);                   /* dataChecksumType: CRC32 */
    put_u32le(koly + 84, 32);                  /* dataChecksumSize (bits) */
    put_u32le(koly + 88, data_crc);            /* dataChecksum */
    put_u64le(koly + 216, w.data_len);         /* xmlOffset */
    put_u64le(koly + 224, xml_len);            /* xmlLength */
    put_u32le(koly + 352, 2);                  /* checksumType: CRC32 */
    put_u32le(koly + 356, 32);                 /* checksumSize (bits) */
    put_u32le(koly + 360, master_crc);         /* checksum */
    put_u32le(koly + 488, 1);                  /* imageVariant */
    put_u64le(koly + 492, sector_count);       /* sectorCount */

    if (fwrite(koly, 1, sizeof(koly), w.fp) != sizeof(koly)) {
        fprintf(stderr, "masa: pack: write failed: %s\n", strerror(errno));
        goto done;
    }
    rc = 0;

done:
    if (w.fp != NULL) {
        fclose(w.fp);
    }
    free(b64);
    free(xml);
    path_list_free(&files);
    if (rc != 0) {
        remove(out_path);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* `masa pack`                                                        */
/* ------------------------------------------------------------------ */

int masa_pack_main(int argc, char *const argv[])
{
    const char *input = NULL;
    const char *output = NULL;
    char app_name[MASA_PATH_MAX];
    int want_ipa = 0;
    int want_dmg = 0;
    int i;

    crc_table_init();

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--ipa") == 0) {
            want_ipa = 1;
        } else if (strcmp(argv[i], "--dmg") == 0) {
            want_dmg = 1;
        } else if (strcmp(argv[i], "-o") == 0 ||
                   strcmp(argv[i], "--output") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "masa: pack: -o needs an output path\n");
                return 2;
            }
            output = argv[++i];
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "masa: pack: unknown option '%s'\n", argv[i]);
            return 2;
        } else if (input == NULL) {
            input = argv[i];
        } else {
            fprintf(stderr, "masa: pack: unexpected argument '%s'\n", argv[i]);
            return 2;
        }
    }

    if (want_ipa && want_dmg) {
        fprintf(stderr, "masa: pack: choose one of --ipa or --dmg\n");
        return 2;
    }
    if (!want_ipa && !want_dmg) {
        fprintf(stderr, "masa: pack: missing format (--ipa | --dmg)\n");
        return 2;
    }
    if (input == NULL) {
        fprintf(stderr, "masa: pack: missing input <App.app>\n");
        return 2;
    }
    if (output == NULL) {
        fprintf(stderr, "masa: pack: missing -o <output>\n");
        return 2;
    }

    {
        struct stat st;

        if (stat(input, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "masa: pack: '%s' is not a directory\n", input);
            return 1;
        }
    }

    {
        const char *base;
        size_t blen;
        char trimmed[MASA_PATH_MAX];

        /* Tolerate a trailing separator so `masa pack Demo.app/` works. */
        snprintf(trimmed, sizeof(trimmed), "%s", input);
        blen = strlen(trimmed);
        while (blen > 1 && MASA_IS_SEP(trimmed[blen - 1])) {
            trimmed[--blen] = '\0';
        }

        base = base_name(trimmed);
        blen = strlen(base);
        if (blen > 4 && strcmp(base + blen - 4, ".app") == 0) {
            blen -= 4;
        }
        if (blen == 0 || blen >= sizeof(app_name)) {
            fprintf(stderr, "masa: pack: invalid application name in '%s'\n",
                    input);
            return 1;
        }
        memcpy(app_name, base, blen);
        app_name[blen] = '\0';
    }

    if (want_ipa) {
        int rc = pack_ipa(input, app_name, output);

        if (rc == 0) {
            printf("masa: pack: wrote %s (Payload/%s.app)\n", output,
                   app_name);
        }
        return rc;
    }

    {
        int rc = pack_dmg(input, app_name, output);

        if (rc == 0) {
            printf("masa: pack: wrote %s (UDIF, %s)\n", output, app_name);
        }
        return rc;
    }
}
