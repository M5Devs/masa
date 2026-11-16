/*
 * masa_compat.h - small portability helpers shared by the sources.
 *
 * Masa builds for POSIX hosts (Linux, Android/Termux) and for Windows
 * (MinGW-w64).  Only the handful of C library calls whose names or
 * availability differ are dealt with here; process creation and the rest
 * of the host services live in the platform backends
 * (src/platform_posix.c, src/platform_win.c).
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MASA_COMPAT_H
#define MASA_COMPAT_H

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MASA_PATH_MAX
#define MASA_PATH_MAX 4096
#endif

#ifdef _WIN32

#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef S_IFMT
#define S_IFMT _S_IFMT
#endif
#ifndef S_IFDIR
#define S_IFDIR _S_IFDIR
#endif
#ifndef S_IFREG
#define S_IFREG _S_IFREG
#endif

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif
/* Windows has no POSIX symlinks; nothing reports S_ISLNK. */
#ifndef S_ISLNK
#define S_ISLNK(mode) 0
#endif

/* R_OK/W_OK/F_OK/X_OK live in <unistd.h> on POSIX. */
#ifndef F_OK
#define F_OK 0
#endif
#ifndef X_OK
#define X_OK 1
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef R_OK
#define R_OK 4
#endif

#ifndef access
#define access(path, mode) _access((path), (mode))
#endif

/*
 * A tiny <dirent.h> stand-in so the sources can walk a directory the
 * same way on both hosts.  Only the entry name is exposed.
 */
typedef struct masa_dir MASA_DIR;

struct masa_dirent {
    char d_name[260];
};

MASA_DIR *masa_opendir(const char *path);
struct masa_dirent *masa_readdir(MASA_DIR *dir);
int masa_closedir(MASA_DIR *dir);

#define MASA_IS_SEP(c) ((c) == '/' || (c) == '\\')

#else /* !_WIN32 */

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef DIR MASA_DIR;
/* Let the callers spell the entry type uniformly: struct masa_dirent. */
#define masa_dirent dirent

#define masa_opendir(path) opendir(path)
#define masa_closedir(dir) closedir(dir)
#define masa_readdir(dir) readdir(dir)

#define MASA_IS_SEP(c) ((c) == '/')

#endif /* _WIN32 */

/* ------------------------------------------------------------------ */
/* Shared filesystem helpers (implemented in the platform backends)    */
/* ------------------------------------------------------------------ */

/*
 * Create every directory level in path (like `mkdir -p`).  Returns 0 on
 * success and -1 on failure; an existing directory is not an error.
 */
int masa_mkdir_p(const char *path);

/*
 * Resolve path to an absolute, symlink-free form.  Returns a malloc'd
 * string the caller frees, or NULL on failure.
 */
char *masa_realpath(const char *path);

/*
 * Create a symlink (POSIX) or, on Windows, a copy of the target so the
 * link can be run without administrator privileges.  Returns 0 on
 * success.
 */
int masa_symlink_or_copy(const char *target, const char *linkpath);

#endif /* MASA_COMPAT_H */
