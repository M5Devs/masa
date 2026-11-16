/*
 * platform_win.c - process spawning and host services for Windows.
 *
 * Masa targets POSIX hosts first, but a Windows build lets the shim
 * environment run under MSYS2 / MinGW-w64 without an MSYS shell.  All
 * child-process creation goes through CreateProcess(); the shims are
 * materialised as copies of masa.exe (or .cmd wrappers) because Windows
 * symlinks need administrator privileges.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef _WIN32

#include "masa.h"
#include "masa_compat.h"

#include <ctype.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* ------------------------------------------------------------------ */
/* Command-line construction                                          */
/* ------------------------------------------------------------------ */

/* Bytes needed to quote one argument for the CreateProcess command line. */
static size_t quoted_len(const char *arg)
{
    size_t len = strlen(arg);
    size_t extra = 2; /* surrounding quotes, worst case */
    size_t i;

    for (i = 0; i < len; i++) {
        if (arg[i] == '"' || arg[i] == '\\') {
            extra++;
        }
    }
    return len + extra + 1;
}

/*
 * Append arg to *dst (advancing it) using the quoting rules the C runtime
 * parses: wrap in quotes and backslash-escape embedded quotes.
 */
static void append_quoted(char **dst, const char *arg)
{
    char *out = *dst;
    size_t len = strlen(arg);
    size_t i;
    int need_quotes = (arg[0] == '\0');

    for (i = 0; i < len; i++) {
        if (arg[i] == ' ' || arg[i] == '\t' || arg[i] == '"') {
            need_quotes = 1;
            break;
        }
    }

    if (!need_quotes) {
        memcpy(out, arg, len);
        *dst = out + len;
        return;
    }

    *out++ = '"';
    for (i = 0; i < len; i++) {
        size_t backslashes = 0;

        while (i < len && arg[i] == '\\') {
            backslashes++;
            i++;
        }
        if (i == len) {
            /* Trailing backslashes must be doubled before the closing quote. */
            while (backslashes-- > 0) {
                *out++ = '\\';
                *out++ = '\\';
            }
            break;
        }
        if (arg[i] == '"') {
            while (backslashes-- > 0) {
                *out++ = '\\';
                *out++ = '\\';
            }
            *out++ = '\\';
            *out++ = '"';
        } else {
            while (backslashes-- > 0) {
                *out++ = '\\';
            }
            *out++ = arg[i];
        }
    }
    *out++ = '"';
    *dst = out;
}

/* Join argv into a single CreateProcess command line (malloc'd). */
static char *build_command_line(const char *const argv[])
{
    size_t total = 0;
    size_t i;
    char *line;
    char *p;

    for (i = 0; argv[i] != NULL; i++) {
        total += quoted_len(argv[i]) + 1;
    }
    line = malloc(total + 1);
    if (line == NULL) {
        return NULL;
    }

    p = line;
    for (i = 0; argv[i] != NULL; i++) {
        if (i > 0) {
            *p++ = ' ';
        }
        append_quoted(&p, argv[i]);
    }
    *p = '\0';
    return line;
}

/* ------------------------------------------------------------------ */
/* Spawning                                                           */
/* ------------------------------------------------------------------ */

static int run_child(const char *dir, const char *const argv[],
                     int capture, char *out, size_t outcap)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    HANDLE read_pipe = NULL;
    HANDLE write_pipe = NULL;
    char *cmdline;
    DWORD status = 0;
    int rc;

    if (argv == NULL || argv[0] == NULL || argv[0][0] == '\0') {
        fprintf(stderr, "masa: no command to run\n");
        return -1;
    }
    if (out != NULL && outcap > 0) {
        out[0] = '\0';
    }

    cmdline = build_command_line(argv);
    if (cmdline == NULL) {
        return -1;
    }

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    if (capture) {
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = NULL;
        sa.bInheritHandle = TRUE;
        if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
            free(cmdline);
            return -1;
        }
        SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = write_pipe;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }

    rc = CreateProcessA(NULL, cmdline, NULL, NULL, capture ? TRUE : FALSE,
                        0, NULL, dir, &si, &pi);
    free(cmdline);
    if (!rc) {
        fprintf(stderr, "masa: cannot execute '%s' (error %lu)\n",
                argv[0], (unsigned long)GetLastError());
        if (capture) {
            CloseHandle(read_pipe);
            CloseHandle(write_pipe);
        }
        return -1;
    }

    if (capture) {
        char buf[4096];
        DWORD got;
        size_t used = 0;

        CloseHandle(write_pipe);
        while (ReadFile(read_pipe, buf, (DWORD)sizeof(buf), &got, NULL) &&
               got > 0) {
            if (out != NULL && used + got < outcap) {
                memcpy(out + used, buf, got);
                used += got;
            }
        }
        if (out != NULL && outcap > 0) {
            out[(used < outcap) ? used : outcap - 1] = '\0';
        }
        CloseHandle(read_pipe);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    if (!GetExitCodeProcess(pi.hProcess, &status)) {
        status = (DWORD)-1;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return (int)status;
}

int masa_spawn_wait_in(const char *dir, const char *const argv[])
{
    return run_child((dir != NULL && dir[0] != '\0') ? dir : NULL, argv,
                     0, NULL, 0);
}

int masa_spawn_wait(const char *const argv[])
{
    return run_child(NULL, argv, 0, NULL, 0);
}

int masa_capture(const char *const argv[], char *out, size_t outcap)
{
    return run_child(NULL, argv, 1, out, outcap);
}

/* ------------------------------------------------------------------ */
/* Temporary directories                                              */
/* ------------------------------------------------------------------ */

int masa_make_temp_dir(const char *prefix, char *buf, size_t bufcap)
{
    char base[MAX_PATH];
    const char *tmpdir = getenv(MASA_TMPDIR_ENV);
    unsigned attempt;
    DWORD len;

    if (buf == NULL || bufcap == 0) {
        return -1;
    }
    if (prefix == NULL) {
        prefix = "masa-";
    }

    if (tmpdir != NULL && tmpdir[0] != '\0') {
        if (snprintf(base, sizeof(base), "%s", tmpdir) >= (int)sizeof(base)) {
            return -1;
        }
    } else {
        len = GetTempPathA((DWORD)sizeof(base), base);
        if (len == 0 || len >= sizeof(base)) {
            if (snprintf(base, sizeof(base), ".") >= (int)sizeof(base)) {
                return -1;
            }
        }
    }

    for (attempt = 0; attempt < 1000; attempt++) {
        if (snprintf(buf, bufcap, "%s\\%s%lu-%u", base, prefix,
                     (unsigned long)GetCurrentProcessId(), attempt) >=
            (int)bufcap) {
            return -1;
        }
        if (CreateDirectoryA(buf, NULL)) {
            return 0;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            break;
        }
    }

    fprintf(stderr, "masa: cannot create a temporary directory under '%s'\n",
            base);
    return -1;
}

char masa_path_separator(void)
{
    return ';';
}

const char *masa_shell_path(void)
{
    const char *comspec = getenv("COMSPEC");

    return (comspec != NULL && comspec[0] != '\0') ? comspec : "cmd.exe";
}

char *masa_self_path(void)
{
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof(buf));

    if (n == 0 || n >= sizeof(buf)) {
        return NULL;
    }
    return strdup(buf);
}

/* ------------------------------------------------------------------ */
/* PATH lookup                                                        */
/* ------------------------------------------------------------------ */

/* 1 when path names an existing regular file. */
static int is_file(const char *path)
{
    struct _stat st;

    return (path != NULL && path[0] != '\0' && _stat(path, &st) == 0 &&
            (st.st_mode & _S_IFMT) == _S_IFREG);
}

char *masa_which(const char *name)
{
    static const char *exts[] = {"", ".exe", ".cmd", ".bat", NULL};
    const char *path_env;
    char *copy;
    char *dir;
    size_t e;

    if (name == NULL || name[0] == '\0') {
        return NULL;
    }

    path_env = getenv("PATH");
    if (path_env == NULL) {
        return NULL;
    }
    copy = strdup(path_env);
    if (copy == NULL) {
        return NULL;
    }

    for (dir = strtok(copy, ";"); dir != NULL; dir = strtok(NULL, ";")) {
        const char *d = (dir[0] == '\0') ? "." : dir;

        for (e = 0; exts[e] != NULL; e++) {
            char candidate[MASA_PATH_MAX];

            if (snprintf(candidate, sizeof(candidate), "%s\\%s%s", d, name,
                         exts[e]) >= (int)sizeof(candidate)) {
                continue;
            }
            if (is_file(candidate)) {
                char *result = strdup(candidate);

                free(copy);
                return result;
            }
        }
    }

    free(copy);
    return NULL;
}

int masa_setenv(const char *name, const char *value, int overwrite)
{
    if (!overwrite && getenv(name) != NULL) {
        return 0;
    }
    return _putenv_s(name, (value != NULL) ? value : "");
}

int masa_unsetenv(const char *name)
{
    return _putenv_s(name, "");
}

int masa_mkdir(const char *path)
{
    return _mkdir(path);
}

/* ------------------------------------------------------------------ */
/* Shim creation                                                      */
/* ------------------------------------------------------------------ */

/*
 * A shim is a copy of the target next to a .cmd wrapper, so `cc` resolves
 * as `cc.exe` (a byte copy of masa.exe) and `cc.cmd` forwards to it.  No
 * symlink privileges are required.
 */
int masa_make_shim(const char *dir, const char *name, const char *target)
{
    char exe_path[MASA_PATH_MAX];
    char cmd_path[MASA_PATH_MAX];
    FILE *fp;

    if (snprintf(exe_path, sizeof(exe_path), "%s\\%s.exe", dir, name) >=
        (int)sizeof(exe_path)) {
        return -1;
    }
    masa_unlink(exe_path);
    if (!CopyFileA(target, exe_path, FALSE)) {
        return -1;
    }

    if (snprintf(cmd_path, sizeof(cmd_path), "%s\\%s.cmd", dir, name) >=
        (int)sizeof(cmd_path)) {
        return -1;
    }
    fp = fopen(cmd_path, "w");
    if (fp == NULL) {
        return -1;
    }
    fprintf(fp, "@echo off\r\n\"%%~dp0%s.exe\" %%*\r\n", name);
    if (fclose(fp) != 0) {
        return -1;
    }
    return 0;
}

int masa_unlink(const char *path)
{
    return (remove(path) == 0) ? 0 : -1;
}

int masa_rmdir(const char *path)
{
    return (_rmdir(path) == 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Shared filesystem helpers                                          */
/* ------------------------------------------------------------------ */

int masa_mkdir_p(const char *path)
{
    char tmp[MASA_PATH_MAX];
    char *p;
    size_t len = strlen(path);

    if (len == 0 || len >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, path, len + 1);

    for (p = tmp + 1; *p != '\0'; p++) {
        if (MASA_IS_SEP(*p)) {
            *p = '\0';
            if (masa_mkdir(tmp) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (masa_mkdir(tmp) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

char *masa_realpath(const char *path)
{
    char buf[MASA_PATH_MAX];

    if (_fullpath(buf, path, sizeof(buf)) == NULL) {
        return NULL;
    }
    return strdup(buf);
}

int masa_symlink_or_copy(const char *target, const char *linkpath)
{
    masa_unlink(linkpath);
    return CopyFileA(target, linkpath, FALSE) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Directory walking (the <dirent.h> stand-in)                        */
/* ------------------------------------------------------------------ */

struct masa_dir {
    HANDLE handle;
    WIN32_FIND_DATAA data;
    struct masa_dirent entry;
    int first;
    int done;
};

MASA_DIR *masa_opendir(const char *path)
{
    char pattern[MASA_PATH_MAX];
    MASA_DIR *dir = calloc(1, sizeof(*dir));

    if (dir == NULL) {
        return NULL;
    }
    if (snprintf(pattern, sizeof(pattern), "%s\\*", path) >=
        (int)sizeof(pattern)) {
        free(dir);
        return NULL;
    }
    dir->handle = FindFirstFileA(pattern, &dir->data);
    if (dir->handle == INVALID_HANDLE_VALUE) {
        free(dir);
        return NULL;
    }
    dir->first = 1;
    return dir;
}

struct masa_dirent *masa_readdir(MASA_DIR *dir)
{
    if (dir == NULL || dir->done) {
        return NULL;
    }
    if (!dir->first) {
        if (!FindNextFileA(dir->handle, &dir->data)) {
            dir->done = 1;
            return NULL;
        }
    }
    dir->first = 0;
    snprintf(dir->entry.d_name, sizeof(dir->entry.d_name), "%s",
             dir->data.cFileName);
    return &dir->entry;
}

int masa_closedir(MASA_DIR *dir)
{
    if (dir == NULL) {
        return -1;
    }
    FindClose(dir->handle);
    free(dir);
    return 0;
}

#endif /* _WIN32 */
