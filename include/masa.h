/*
 * masa.h - public interface for Masa, a cross-compilation wrapper and
 * Darwin environment shim for building macOS Mach-O binaries on systems
 * without Xcode.
 *
 * Copyright (C) 2026 Masa contributors
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef MASA_H
#define MASA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MASA_VERSION "0.2.0"

/* Target triple used when MASA_TARGET is not set in the environment. */
#define MASA_DEFAULT_TARGET "arm64-apple-darwin"

/* SDK root reported by the shims when MASA_SDK_PATH is not set. */
#define MASA_DEFAULT_SDK_PATH "/opt/masa/sdk"

/* ProductName/ProductVersion/BuildVersion reported by the sw_vers shim. */
#define MASA_SW_PRODUCT_NAME "macOS"
#define MASA_SW_PRODUCT_VERSION "15.0"
#define MASA_SW_BUILD_VERSION "24A335"

/* Value of MASA_DRY_RUN that disables command execution. */
#define MASA_DRY_RUN_ENV "MASA_DRY_RUN"

/* Environment variables understood by Masa. */
#define MASA_TARGET_ENV "MASA_TARGET"
#define MASA_SDK_PATH_ENV "MASA_SDK_PATH"
#define MASA_CC_ENV "MASA_CC"
/* Compiler the shims must forward to; set by `masa exec` before it edits PATH. */
#define MASA_REAL_CC_ENV "MASA_REAL_CC"
/* Directory holding the argv[0] shims; reused by `masa exec` when set. */
#define MASA_SHIM_DIR_ENV "MASA_SHIM_DIR"
/* Overrides the symlink target of `masa init-shims` (defaults to the running
 * Masa executable).  Intended for packaging and tests. */
#define MASA_SHIM_TARGET_ENV "MASA_SHIM_TARGET"

/* Exit status returned when the wrapper cannot spawn a tool at all. */
#define MASA_EXIT_SPAWN_ERROR 127

/* ------------------------------------------------------------------ */
/* Process spawning (src/platform_linux.c)                            */
/* ------------------------------------------------------------------ */

/*
 * Spawn argv[0] as a child process, wait for it and return its exit
 * status.  A negative result means Masa itself failed (the tool could
 * not be found or started); a non-negative result is the child's exit
 * code, or 128 + signal if the child was killed by a signal.
 */
int masa_spawn_wait(const char *const argv[]);

/* ------------------------------------------------------------------ */
/* cc wrapper (src/cc_wrapper.c)                                      */
/* ------------------------------------------------------------------ */

/*
 * Append the macOS cross-compilation arguments to argv: the target
 * triple, the linker selection and, when an SDK is configured, the
 * sysroot.  argv must have room for at least 5 more entries.
 *
 * Returns 0 on success and -1 when an argument is missing or the array
 * would overflow.
 */
int masa_build_cc_args(const char *target, const char *sdk_path,
                       const char *args[], int *count, int max_args);

/*
 * Resolve the real host compiler, taking care never to select Masa
 * itself.  MASA_REAL_CC wins when set, then MASA_CC, otherwise PATH is
 * searched for clang, cc and gcc.  Directories that would recurse back
 * into Masa (its own directory, MASA_SHIM_DIR and any candidate whose
 * real path is Masa) are skipped.
 *
 * Returns a malloc'd path that the caller must free, or NULL when no
 * compiler outside Masa could be found.
 */
char *masa_resolve_cc(void);

/*
 * Run the cc wrapper for the given compiler arguments (argc may be 0).
 * Reads MASA_TARGET, MASA_SDK_PATH and MASA_DRY_RUN from the
 * environment.  Returns the process exit status, or 1 on failure.
 */
int masa_cc_main(int argc, char *const argv[]);

/* ------------------------------------------------------------------ */
/* Darwin environment shims (src/sys_shims.c)                         */
/* ------------------------------------------------------------------ */

/*
 * Dispatch a shimmed Darwin command.  Only the leading command name is
 * interpreted; trailing arguments are accepted and ignored so callers
 * may pass through the original argv verbatim.
 *
 * Returns the exit status to report to the shell.
 */
int masa_sys_dispatch(int argc, char *const argv[]);

/* ------------------------------------------------------------------ */
/* Environment runner (src/exec.c)                                    */
/* ------------------------------------------------------------------ */

/* Shim names created by `masa init-shims` / `masa exec`. */
extern const char *const masa_shim_tools[];

/*
 * Path of the running Masa executable, resolved through symlinks where
 * the platform allows it.  Returns a malloc'd string the caller frees,
 * or NULL on failure.
 */
char *masa_self_path(void);

/*
 * Create (or refresh) the argv[0] shims inside dir, each a symlink to
 * target_exe.  Returns 0 on success and -1 on failure.
 */
int masa_make_shims(const char *dir, const char *target_exe);

/*
 * `masa init-shims <dir>` - create the shim symlinks in dir.
 * Returns the process exit status.
 */
int masa_init_shims_main(int argc, char *const argv[]);

/*
 * `masa exec <command> [args...]` - build a shim directory, prepend it
 * to PATH, export MASA_REAL_CC and run the command.
 * Returns the process exit status.
 */
int masa_exec_main(int argc, char *const argv[]);

/* ------------------------------------------------------------------ */
/* CLI router (src/main.c)                                            */
/* ------------------------------------------------------------------ */

/*
 * Entry point shared by the binary and the unit tests.  argv[0] is the
 * program name: when its basename is a shim name (cc, clang, gcc, uname,
 * sw_vers, xcode-select, xcrun) the matching behaviour is selected,
 * otherwise the normal `masa <subcommand>` router runs.
 */
int masa_main(int argc, char *const argv[]);

#ifdef __cplusplus
}
#endif

#endif /* MASA_H */
