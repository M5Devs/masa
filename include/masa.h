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

#define MASA_VERSION "1.6.0"

/* Target triple used when MASA_TARGET is not set in the environment. */
#define MASA_DEFAULT_TARGET "arm64-apple-darwin"

/* SDK root reported by the shims when MASA_SDK_PATH is not set. */
#define MASA_DEFAULT_SDK_PATH "/opt/masa/sdk"

/* Platform and version ld64.lld is told about when linking. */
#define MASA_MACOS_PLATFORM "macos"
#define MASA_MACOS_VERSION "15.0"

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
/* When set to 1, `masa shell` leaves the caller's PS1 untouched. */
#define MASA_KEEP_PS1_ENV "MASA_KEEP_PS1"
/* Extra directories the Danroot loader searches for `@rpath`/bare dylib
 * names, ahead of DYLD_LIBRARY_PATH.  Colon-separated. */
#define MASA_LIBRARY_PATH_ENV "MASA_LIBRARY_PATH"
/* Virtual root for the Danroot loader's `--root` path virtualization: when
 * set (or passed as `--root <dir>`), paths under the macOS system prefixes
 * are mapped beneath this directory instead of the host root. */
#define MASA_DANROOT_ROOT_ENV "MASA_DANROOT_ROOT"

/* Cross-architecture bridge.  On an x86_64 host an arm64 Mach-O image is
 * handed to a user-mode ARM64 runner (qemu-aarch64) driving an aarch64
 * build of Masa; these override the PATH / sibling-binary discovery of the
 * runner and the bridge binary. */
#define MASA_QEMU_AARCH64_ENV "MASA_QEMU_AARCH64"
#define MASA_DANROOT_AARCH64_ENV "MASA_DANROOT_AARCH64"
/* Set to 1 to disable the cross-architecture bridge entirely, so an arm64
 * image on an x86_64 host reports the plain architecture mismatch. */
#define MASA_DANROOT_NO_BRIDGE_ENV "MASA_DANROOT_NO_BRIDGE"

/* Number of frames the AppKit SDL2 run loop draws before returning, so an
 * automated test can drive a GUI program headlessly and exit cleanly.
 * Unset means run until the window is closed. */
#define MASA_GUI_FRAMES_ENV "MASA_GUI_FRAMES"

/* Exit status returned when the wrapper cannot spawn a tool at all. */
#define MASA_EXIT_SPAWN_ERROR 127

/* ------------------------------------------------------------------ */
/* Process spawning and host services                                 */
/*   (src/platform_posix.c on Linux/Android, src/platform_win.c on    */
/*    Windows)                                                        */
/* ------------------------------------------------------------------ */

/*
 * Spawn argv[0] as a child process, wait for it and return its exit
 * status.  A negative result means Masa itself failed (the tool could
 * not be found or started); a non-negative result is the child's exit
 * code, or 128 + signal if the child was killed by a signal.
 */
int masa_spawn_wait(const char *const argv[]);

/*
 * Spawn argv[0], wait for it and copy up to outcap - 1 bytes of its
 * standard output into out (NUL-terminated).  Returns the child's exit
 * status, or -1 when Masa itself failed.
 */
int masa_capture(const char *const argv[], char *out, size_t outcap);

/*
 * Like masa_spawn_wait(), but the child first chdir()s into dir (when
 * non-NULL and non-empty).  Used by the Homebrew builder so a formula's
 * `system "make"` runs inside the unpacked source directory.
 */
int masa_spawn_wait_in(const char *dir, const char *const argv[]);

/*
 * Create a fresh, empty temporary directory and copy its path into buf
 * (bufcap bytes).  The directory name is prefix plus a unique suffix.
 * The path is built under $TMPDIR when that is set, so hosts such as
 * Android/Termux can redirect it, and otherwise under the platform's
 * default temporary directory.  Returns 0 on success and -1 on failure.
 */
int masa_make_temp_dir(const char *prefix, char *buf, size_t bufcap);

/* The host's PATH list separator: ':' on POSIX, ';' on Windows. */
char masa_path_separator(void);

/* The path to a POSIX shell, used by `masa shell` as a last resort. */
const char *masa_shell_path(void);

/* Path of the running Masa executable (resolved through symlinks where
 * the platform allows it).  Returns a malloc'd string the caller frees,
 * or NULL on failure. */
char *masa_self_path(void);

/* Locate name on PATH and return a malloc'd path the caller frees, or
 * NULL when it is not an executable file. */
char *masa_which(const char *name);

/* Set or clear an environment variable.  Return 0 on success. */
int masa_setenv(const char *name, const char *value, int overwrite);
int masa_unsetenv(const char *name);

/* Create a single directory level (like mkdir(path, 0755)). */
int masa_mkdir(const char *path);

/* Create one argv[0] shim: a symlink on POSIX, a copy or a .cmd wrapper
 * on Windows so no administrator privileges are needed.  Returns 0 on
 * success. */
int masa_make_shim(const char *dir, const char *name, const char *target);

/* Remove a file or a directory level. */
int masa_unlink(const char *path);
int masa_rmdir(const char *path);

/* ------------------------------------------------------------------ */
/* cc wrapper (src/cc_wrapper.c)                                      */
/* ------------------------------------------------------------------ */

/*
 * Append the macOS cross-compilation arguments to argv: the target
 * triple, the linker selection and, when an SDK is configured, the
 * sysroot.  argv must have room for at least 6 more entries.
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
/* Swift wrapper (src/swift_wrapper.c)                                */
/* ------------------------------------------------------------------ */

/* Default Swift target when MASA_TARGET is not set. */
#define MASA_DEFAULT_SWIFT_TARGET "arm64-apple-macosx15.0"

/* Environment variable naming the host Swift compiler. */
#define MASA_SWIFTC_ENV "MASA_SWIFTC"

/* A parsed Package.swift manifest: the package name and whether a
 * conventional Sources/<name> tree exists next to it. */
struct masa_package {
    char *name;        /* package name, or NULL when none was found */
    char *root;        /* directory holding Package.swift */
    int has_sources;   /* 1 when a Sources/ directory exists */
};

/*
 * Append the cross-compilation arguments for swiftc to argv: the target
 * triple and, when an SDK is configured, the sysroot.  swiftc takes the
 * SDK through `-sdk` (not `-isysroot`), so the two wrappers differ here.
 * argv must have room for at least 4 more entries.
 *
 * Returns 0 on success and -1 when an argument is missing or the array
 * would overflow.
 */
int masa_build_swift_args(const char *target, const char *sdk_path,
                          const char *args[], int *count, int max_args);

/*
 * Resolve the host Swift compiler.  MASA_SWIFTC wins when set; otherwise
 * PATH is searched for swiftc.  Returns a malloc'd path the caller frees,
 * or NULL when none was found.
 */
char *masa_resolve_swiftc(void);

/*
 * Read and lightly parse the Package.swift at path (Swift Package
 * Manager manifest).  Fills *out with the package name and whether a
 * Sources/ directory sits next to the manifest.  Returns 0 on success
 * and -1 when the file is missing or clearly not a manifest, writing a
 * reason into err (when errcap > 0).
 */
int masa_package_parse(const char *path, struct masa_package *out,
                       char *err, size_t errcap);

/* Release the memory owned by a parsed package (safe on a zeroed struct). */
void masa_package_free(struct masa_package *pkg);

/*
 * Run the swiftc wrapper for the given compiler arguments (argc may be
 * 0).  Reads MASA_TARGET, MASA_SDK_PATH and MASA_DRY_RUN from the
 * environment.  Returns the process exit status, or 1 on failure.
 */
int masa_swiftc_main(int argc, char *const argv[]);

/*
 * `masa swift <info|build> [dir]` - inspect or build a Swift package
 * using the swiftc wrapper.  Returns the process exit status.
 */
int masa_swift_main(int argc, char *const argv[]);

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
/* Toolchain shims (src/tool_shims.c)                                 */
/* ------------------------------------------------------------------ */

/* Names handled by the toolchain shims, terminated by NULL. */
extern const char *const masa_tool_names[];

/*
 * 1 when name (a bare basename) is one of the toolchain shims.
 */
int masa_tool_is_shim(const char *name);

/*
 * Locate a host tool.  prog_name is queried with
 * `<cc> -print-prog-name=<prog_name>`; fallbacks are extra bare names
 * looked up on PATH and next to the compiler.  Returns a malloc'd path
 * the caller frees, or NULL when nothing was found.
 */
char *masa_locate_tool(const char *prog_name, const char *const fallbacks[]);

/*
 * Run a toolchain shim such as `ld`, `ar`, `ranlib`, `strip`, `nm` or
 * `otool`.  The name is dispatched to the matching LLVM tool and, for
 * the linker, a platform version is supplied so ld64.lld can link
 * Mach-O images.  Returns the process exit status, or -1 when name is
 * not a toolchain shim.
 */
int masa_tool_dispatch(const char *name, int argc, char *const argv[]);

/* ------------------------------------------------------------------ */
/* SDK helper (src/sdk.c)                                             */
/* ------------------------------------------------------------------ */

/* Relative path of the stub linker script inside an SDK root. */
#define MASA_SDK_LIBSYSTEM_RELPATH "usr/lib/libSystem.tbd"

/*
 * `masa sdk info` and `masa sdk init-stub <dir>`.  Returns the process
 * exit status.
 */
int masa_sdk_main(int argc, char *const argv[]);

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

/*
 * Run a list of argument vectors, each with the shim environment on PATH
 * and with the child chdir()ed into dir (when non-NULL).  Stops at the
 * first non-zero status and returns it; 0 when every command succeeded.
 * Used by `masa brew build` to run a formula's install steps.
 */
int masa_exec_run_all(const char *dir, char *const *const *cmds, size_t count);

/*
 * `masa shell [-c <command>]` - launch an interactive shell (or run a
 * single command with -c) inside the full shim environment.  Returns the
 * process exit status.
 */
int masa_shell_main(int argc, char *const argv[]);

/* ------------------------------------------------------------------ */
/* Xcode project builder (src/xcodeproj.c)                            */
/* ------------------------------------------------------------------ */

/* Directory a .xcodeproj is expected to hold its project file in. */
#define MASA_PBXPROJ_RELPATH "project.pbxproj"

/*
 * Parsed view of the parts of a project.pbxproj the builder needs: the
 * native target's product name and the source files (relative to the
 * .xcodeproj parent directory) it compiles, split into C and Swift
 * translation units so each can be handed to the right compiler.
 *
 * Both arrays are owned by the struct and released by
 * masa_pbxproj_free(); sources are NUL-terminated strings.
 */
struct masa_pbxproj {
    char *product_name;        /* PBXNativeTarget productName, or NULL */
    char **sources;            /* PBXBuildFile/PBXFileReference .c paths */
    size_t source_count;
    char **swift_sources;      /* PBXBuildFile/PBXFileReference .swift paths */
    size_t swift_source_count;
};

/*
 * Parse a project.pbxproj held in memory (len bytes, need not be
 * NUL-terminated).  Returns 0 on success and -1 on a malformed document,
 * writing a human-readable reason into err (when errcap > 0).  On
 * failure any partially built state is released, so *out need not be
 * cleared by the caller.
 */
int masa_pbxproj_parse(const char *text, size_t len,
                       struct masa_pbxproj *out, char *err, size_t errcap);

/* Release the memory owned by a parsed project (safe on a zeroed struct). */
void masa_pbxproj_free(struct masa_pbxproj *proj);

/*
 * `masa xcode build <path/to/App.xcodeproj> [-o <out_dir>] [--bundle]`.
 * Returns the process exit status.
 */
int masa_xcode_main(int argc, char *const argv[]);

/* ------------------------------------------------------------------ */
/* codesign shim (src/tool_shims.c)                                   */
/* ------------------------------------------------------------------ */

/*
 * 1 when target exists and starts with the 64-bit Mach-O magic
 * (CF FA ED FE), i.e. is a plausible thing to sign or verify.
 */
int masa_is_macho64(const char *path);

/*
 * Run the codesign shim.  Ad-hoc signing (`-s -`) is accepted and, for a
 * build script's benefit, never fails; `--verify` checks that the target
 * exists and is a 64-bit Mach-O.  Returns the process exit status.
 */
int masa_codesign_dispatch(int argc, char *const argv[]);

/* ------------------------------------------------------------------ */
/* Homebrew formula linter and builder (src/brew.c)                   */
/* ------------------------------------------------------------------ */

/* Prefix used by `masa brew build` when none is given or configured. */
#define MASA_BREW_DEFAULT_PREFIX "/tmp/masa-homebrew"
/* Environment variable that overrides the default prefix. */
#define MASA_BREW_PREFIX_ENV "MASA_BREW_PREFIX"
/* Environment variable consulted first when building the Cellar prefix. */
#define MASA_TMPDIR_ENV "TMPDIR"

/* One `system "..."` build step found inside `def install`. */
struct masa_brew_step {
    char *prog;    /* program to run, e.g. "make" */
    char **argv;   /* full argument vector, NULL-terminated */
    size_t argc;
    /*
     * A single-string `system "..."` runs through a shell, exactly as
     * Ruby's Kernel#system does, so spaces and shell metacharacters are
     * interpreted; a multi-argument `system "prog", "arg", ...` runs the
     * program directly through execvp.  Set when the step must go through
     * `sh -c`.
     */
    int use_shell;
};

/*
 * Parsed view of the parts of a Homebrew formula the linter and builder
 * need.  Every string is owned by the struct and released by
 * masa_brew_formula_free(); the strings are NULL when the field is
 * absent.
 */
struct masa_brew_formula {
    char *class_name;   /* `class Foo < Formula` -> "Foo" */
    char *name;         /* formula name derived from class_name */
    char *desc;
    char *homepage;
    char *url;
    char *sha256;
    char *license;
    char *version;      /* explicit `version "x"`, else NULL */
    int has_install;    /* `def install` present */
    int has_test;       /* `test do` present */
    struct masa_brew_step *steps;
    size_t step_count;
};

/*
 * Parse a formula held in memory (len bytes, need not be NUL-terminated).
 * Returns 0 on success and -1 on a document that is not a formula,
 * writing a reason into err (when errcap > 0).  On failure any partially
 * built state is released.
 */
int masa_brew_formula_parse(const char *text, size_t len,
                            struct masa_brew_formula *out,
                            char *err, size_t errcap);

/* Release the memory owned by a parsed formula (safe on a zeroed struct). */
void masa_brew_formula_free(struct masa_brew_formula *f);

/*
 * Lint a parsed formula: check the required fields (`desc`, `homepage`,
 * `url`, `sha256`, `def install`) and warn about a missing `test do`.
 * Returns 0 when the formula is valid and non-zero otherwise.  `label`
 * names the source in diagnostics (normally the file path).
 */
int masa_brew_lint_formula(const struct masa_brew_formula *f,
                           const char *label);

/*
 * `masa brew lint <formula.rb>` and
 * `masa brew build <formula.rb> --source <dir> [--prefix <dir>]`.
 * Returns the process exit status.
 */
int masa_brew_main(int argc, char *const argv[]);

/* ------------------------------------------------------------------ */
/* Doctor (src/doctor.c)                                              */
/* ------------------------------------------------------------------ */

/*
 * `masa doctor` - inspect the local machine and report whether it can
 * build macOS binaries (a C compiler, LLVM's ld64.lld, the LLVM tools, an
 * optional swiftc, the SDK and the target).  Returns 0 when the core
 * requirements are present and non-zero otherwise.
 */
int masa_doctor_main(int argc, char *const argv[]);

/* ------------------------------------------------------------------ */
/* Packer (src/pack.c)                                                */
/* ------------------------------------------------------------------ */

/*
 * `masa pack --ipa <App.app> -o <out.ipa>` and
 * `masa pack --dmg <App.app> -o <out.dmg>` - package a built `.app`
 * bundle into an iOS `.ipa` (a ZIP with the app under `Payload/`) or a
 * macOS UDIF `.dmg` disk image, using only the C library.  Returns 0 on
 * success and non-zero on error.
 */
int masa_pack_main(int argc, char *const argv[]);

/* ------------------------------------------------------------------ */
/* Mach-O runner (src/danroot.c)                                      */
/* ------------------------------------------------------------------ */

/*
 * `masa run <binary> [args...]` and its alias `masa danroot` - load a
 * 64-bit Mach-O MH_EXECUTE image built by `masa cc` into the current
 * process and run it, resolving its imports against a clean-room
 * libSystem shim.  Supported on POSIX hosts (Linux, Android/Termux);
 * on Windows it reports that the feature is POSIX-only.
 *
 * Returns the loaded program's exit status, or a non-zero loader error
 * code when the image cannot be loaded.
 */
int masa_danroot_main(int argc, char *const argv[]);

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
