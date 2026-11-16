/*
 * pty_win.c - pseudo-terminal backend for masa-term on Windows.
 *
 * Windows 10 (1809) and later provide the Console Pseudo Terminal
 * ("ConPTY") API, which is the equivalent of a Unix pty: CreatePseudoConsole
 * builds a terminal object, and a child process started with the
 * PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE thread attribute talks to it as if it
 * had a real console.  This file implements the masa_pty interface on top of
 * it and is selected by `make gui` on Windows/MinGW.
 *
 * MinGW-w64's <windows.h> predates ConPTY, so the handful of declarations
 * it is missing (HPCON and the three entry points) are provided here and the
 * functions are resolved at load time from kernel32.  The file only uses the
 * Win32 API and libc, never SDL or libvterm, so it cross-compiles cleanly
 * from a Linux host with `x86_64-w64-mingw32-gcc` even when SDL is absent.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef _WIN32

/* Ask for the Windows 10 API surface before <windows.h> is read. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "masa.h"
#include "masa_compat.h"
#include "masa_pty.h"

/* ------------------------------------------------------------------ */
/* ConPTY declarations missing from older MinGW-w64 headers            */
/* ------------------------------------------------------------------ */

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE \
    ProcThreadAttributeValue(22, FALSE, TRUE, FALSE)
#endif

typedef HANDLE HPCON;

typedef HRESULT (WINAPI *masa_create_pc_fn)(COORD size, HANDLE hInput,
                                            HANDLE hOutput, DWORD dwFlags,
                                            HPCON *phPC);
typedef HRESULT (WINAPI *masa_resize_pc_fn)(HPCON hPC, COORD size);
typedef void (WINAPI *masa_close_pc_fn)(HPCON hPC);

static masa_create_pc_fn g_create_pc;
static masa_resize_pc_fn g_resize_pc;
static masa_close_pc_fn g_close_pc;

static int load_conpty(void)
{
    HMODULE k32;

    if (g_create_pc != NULL) {
        return 0;
    }

    k32 = GetModuleHandleA("kernel32.dll");
    if (k32 == NULL) {
        k32 = LoadLibraryA("kernel32.dll");
    }
    if (k32 == NULL) {
        return -1;
    }

    g_create_pc = (masa_create_pc_fn)(void *)GetProcAddress(k32,
                                                            "CreatePseudoConsole");
    g_resize_pc = (masa_resize_pc_fn)(void *)GetProcAddress(k32,
                                                            "ResizePseudoConsole");
    g_close_pc = (masa_close_pc_fn)(void *)GetProcAddress(k32,
                                                          "ClosePseudoConsole");
    if (g_create_pc == NULL || g_resize_pc == NULL || g_close_pc == NULL) {
        g_create_pc = NULL;
        g_resize_pc = NULL;
        g_close_pc = NULL;
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Command line and environment block                                 */
/* ------------------------------------------------------------------ */

/* Append arg to the command line using the quoting the C runtime parses. */
static char *build_command_line(char *const argv[])
{
    size_t total = 1;
    size_t i;
    char *line;
    char *p;

    for (i = 0; argv[i] != NULL; i++) {
        total += strlen(argv[i]) * 2 + 3;
    }
    line = malloc(total);
    if (line == NULL) {
        return NULL;
    }

    p = line;
    for (i = 0; argv[i] != NULL; i++) {
        const char *arg = argv[i];
        size_t len = strlen(arg);
        size_t j;
        int need_quotes = (arg[0] == '\0');

        for (j = 0; j < len; j++) {
            if (arg[j] == ' ' || arg[j] == '\t' || arg[j] == '"') {
                need_quotes = 1;
                break;
            }
        }
        if (i > 0) {
            *p++ = ' ';
        }
        if (!need_quotes) {
            memcpy(p, arg, len);
            p += len;
            continue;
        }
        *p++ = '"';
        for (j = 0; j < len; j++) {
            size_t backslashes = 0;

            while (j < len && arg[j] == '\\') {
                backslashes++;
                j++;
            }
            if (j == len) {
                while (backslashes-- > 0) {
                    *p++ = '\\';
                    *p++ = '\\';
                }
                break;
            }
            if (arg[j] == '"') {
                while (backslashes-- > 0) {
                    *p++ = '\\';
                    *p++ = '\\';
                }
                *p++ = '\\';
                *p++ = '"';
            } else {
                while (backslashes-- > 0) {
                    *p++ = '\\';
                }
                *p++ = arg[j];
            }
        }
        *p++ = '"';
    }
    *p = '\0';
    return line;
}

/* Build a CreateProcess environment block ("K=V\0K=V\0\0") from envp. */
static char *build_env_block(char *const envp[])
{
    size_t total = 1;
    size_t i;
    char *block;
    char *p;

    if (envp == NULL) {
        return NULL; /* Inherit the parent environment. */
    }

    for (i = 0; envp[i] != NULL; i++) {
        total += strlen(envp[i]) + 1;
    }
    block = malloc(total);
    if (block == NULL) {
        return NULL;
    }

    p = block;
    for (i = 0; envp[i] != NULL; i++) {
        size_t len = strlen(envp[i]);

        memcpy(p, envp[i], len + 1);
        p += len + 1;
    }
    *p = '\0';
    return block;
}

/* ------------------------------------------------------------------ */
/* Open                                                               */
/* ------------------------------------------------------------------ */

int masa_pty_open(struct masa_pty *pty, int rows, int cols,
                  char *const argv[], char *const envp[])
{
    SECURITY_ATTRIBUTES sa;
    HANDLE in_read = NULL;
    HANDLE in_write = NULL;
    HANDLE out_read = NULL;
    HANDLE out_write = NULL;
    HPCON hpc = NULL;
    COORD size;
    STARTUPINFOEXA si;
    PROCESS_INFORMATION pi;
    SIZE_T attr_size = 0;
    char *cmdline = NULL;
    char *env_block = NULL;
    int ok;

    if (pty == NULL || argv == NULL || argv[0] == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    memset(pty, 0, sizeof(*pty));
    pty->closed = 0;

    if (load_conpty() != 0) {
        fprintf(stderr, "masa-term: ConPTY is unavailable (needs Windows 10 "
                        "1809 or later)\n");
        return -1;
    }

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&in_read, &in_write, &sa, 0)) {
        return -1;
    }
    if (!CreatePipe(&out_read, &out_write, &sa, 0)) {
        CloseHandle(in_read);
        CloseHandle(in_write);
        return -1;
    }

    /* The parent keeps the write end of stdin and the read end of stdout. */
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

    size.X = (SHORT)((cols > 0) ? cols : 80);
    size.Y = (SHORT)((rows > 0) ? rows : 24);

    if (FAILED(g_create_pc(size, in_read, out_write, 0, &hpc))) {
        fprintf(stderr, "masa-term: CreatePseudoConsole failed (error %lu)\n",
                (unsigned long)GetLastError());
        CloseHandle(in_read);
        CloseHandle(in_write);
        CloseHandle(out_read);
        CloseHandle(out_write);
        return -1;
    }

    /* ConPTY duplicated these; the parent no longer needs them. */
    CloseHandle(in_read);
    CloseHandle(out_write);
    in_read = NULL;
    out_write = NULL;

    cmdline = build_command_line(argv);
    env_block = build_env_block(envp);
    if (cmdline == NULL || (envp != NULL && env_block == NULL)) {
        free(cmdline);
        free(env_block);
        g_close_pc(hpc);
        CloseHandle(in_write);
        CloseHandle(out_read);
        return -1;
    }

    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_size);
    if (si.lpAttributeList == NULL ||
        !InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0,
                                           &attr_size)) {
        free(si.lpAttributeList);
        free(cmdline);
        free(env_block);
        g_close_pc(hpc);
        CloseHandle(in_write);
        CloseHandle(out_read);
        return -1;
    }

    if (!UpdateProcThreadAttribute(si.lpAttributeList, 0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   hpc, sizeof(hpc), NULL, NULL)) {
        DeleteProcThreadAttributeList(si.lpAttributeList);
        free(si.lpAttributeList);
        free(cmdline);
        free(env_block);
        g_close_pc(hpc);
        CloseHandle(in_write);
        CloseHandle(out_read);
        return -1;
    }

    ok = CreateProcessA(NULL, cmdline, NULL, NULL, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT, env_block, NULL,
                        &si.StartupInfo, &pi);

    DeleteProcThreadAttributeList(si.lpAttributeList);
    free(si.lpAttributeList);
    free(cmdline);
    free(env_block);

    if (!ok) {
        fprintf(stderr, "masa-term: cannot start '%s' (error %lu)\n",
                argv[0], (unsigned long)GetLastError());
        g_close_pc(hpc);
        CloseHandle(in_write);
        CloseHandle(out_read);
        return -1;
    }

    CloseHandle(pi.hThread);

    pty->input_write = in_write;
    pty->output_read = out_read;
    pty->process = pi.hProcess;
    pty->pseudo_console = hpc;
    pty->exited = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* I/O                                                                */
/* ------------------------------------------------------------------ */

ssize_t masa_pty_read(struct masa_pty *pty, char *buf, size_t len)
{
    DWORD avail = 0;
    DWORD got = 0;

    if (pty == NULL || pty->output_read == NULL || buf == NULL || len == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    if (!PeekNamedPipe((HANDLE)pty->output_read, NULL, 0, NULL, &avail, NULL)) {
        return 0; /* Pipe closed: end of input, not an error. */
    }
    if (avail == 0) {
        return 0; /* Nothing pending; the caller polls again later. */
    }
    if (len > (size_t)avail) {
        len = (size_t)avail;
    }

    if (!ReadFile((HANDLE)pty->output_read, buf, (DWORD)len, &got, NULL)) {
        return 0;
    }
    return (ssize_t)got;
}

ssize_t masa_pty_write(struct masa_pty *pty, const char *buf, size_t len)
{
    size_t done = 0;

    if (pty == NULL || pty->input_write == NULL || buf == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    while (done < len) {
        DWORD wrote = 0;

        if (!WriteFile((HANDLE)pty->input_write, buf + done,
                       (DWORD)(len - done), &wrote, NULL)) {
            return -1;
        }
        if (wrote == 0) {
            break;
        }
        done += wrote;
    }
    return (ssize_t)done;
}

int masa_pty_set_size(struct masa_pty *pty, int rows, int cols)
{
    COORD size;

    if (pty == NULL || pty->pseudo_console == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    size.X = (SHORT)((cols > 0) ? cols : 80);
    size.Y = (SHORT)((rows > 0) ? rows : 24);
    return SUCCEEDED(g_resize_pc((HPCON)pty->pseudo_console, size)) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Child lifetime                                                     */
/* ------------------------------------------------------------------ */

int masa_pty_child_status(struct masa_pty *pty, int *status)
{
    DWORD code = 0;

    if (pty == NULL || pty->process == NULL) {
        return -1;
    }
    if (!GetExitCodeProcess((HANDLE)pty->process, &code)) {
        return -1;
    }
    if (code == STILL_ACTIVE) {
        return 0;
    }
    if (status != NULL) {
        *status = (int)code;
    }
    pty->exited = 1;
    pty->exit_code = code;
    return 1;
}

int masa_pty_close(struct masa_pty *pty)
{
    int status = -1;

    if (pty == NULL || pty->closed) {
        return -1;
    }
    pty->closed = 1;

    if (pty->process != NULL) {
        DWORD code = 0;

        if (GetExitCodeProcess((HANDLE)pty->process, &code) &&
            code == STILL_ACTIVE) {
            TerminateProcess((HANDLE)pty->process, 0);
            WaitForSingleObject((HANDLE)pty->process, INFINITE);
        }
        if (GetExitCodeProcess((HANDLE)pty->process, &code)) {
            status = (int)code;
        }
        CloseHandle((HANDLE)pty->process);
        pty->process = NULL;
    }

    if (pty->input_write != NULL) {
        CloseHandle((HANDLE)pty->input_write);
        pty->input_write = NULL;
    }
    if (pty->output_read != NULL) {
        CloseHandle((HANDLE)pty->output_read);
        pty->output_read = NULL;
    }
    if (pty->pseudo_console != NULL) {
        g_close_pc((HPCON)pty->pseudo_console);
        pty->pseudo_console = NULL;
    }
    return status;
}

#endif /* _WIN32 */
