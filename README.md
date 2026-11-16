# Masa

Masa is a cross-compilation wrapper and Darwin environment shim. It lets you
build macOS Mach-O binaries on a non-macOS host, without Xcode, by driving
`clang` and answering the platform queries that macOS build systems make.

Masa is licensed under the GNU General Public License v3.0 or later. See
[LICENSE](LICENSE).

## Status

Milestone 6 (v0.6.0): an interactive `masa shell` that drops you inside the
full shim environment, and a portable host-services layer. Process spawning,
temporary directories, `PATH` lookup, environment mutation and shim creation
now live behind a small platform interface, with a POSIX backend
(`src/platform_posix.c`, covering Linux and Android/Termux) and a Windows
backend (`src/platform_win.c`, using `CreateProcess` and MinGW-w64). This sits
on the Milestone 5 Homebrew formula linter and builder (`masa brew`), the
Milestone 4 Xcode `.xcodeproj` builder and `codesign` shim, the Milestone 3
full Mach-O executable linking, toolchain shims and `masa sdk`, and the
Milestone 2 `argv[0]` dispatch and `masa exec` environment runner, which in
turn rest on the Milestone 1 `cc` wrapper and `sys` shims.

## Requirements

- A C11 compiler (`gcc` or `clang`) and `make`.
- `clang` and LLVM's `ld64.lld` for producing Mach-O output. The other LLVM
  binutils (`llvm-ar`, `llvm-ranlib`, `llvm-strip`, `llvm-nm`, `llvm-objdump`)
  are used by the corresponding shims.
- No Apple SDK is required. `masa sdk init-stub` writes a clean-room stub SDK
  that is enough to link real Mach-O executables.

## Building

```sh
make          # builds build/masa
make test     # builds and runs the unit test suite
make clean
```

Sources are compiled with `-std=c11 -Wall -Wextra -Werror -fsanitize=address`,
so the build fails on any warning and the tests run under AddressSanitizer.

## Portability

Masa builds on any POSIX host (Linux, and Android/Termux via the `/system/bin`
shell and `$TMPDIR`), and on Windows through MinGW-w64. The Makefile picks the
host-services backend automatically: `src/platform_posix.c` everywhere except
native Windows, where `src/platform_win.c` is used. The Windows binary can be
cross-compiled from Linux:

```sh
# build the Windows binary (note: platform_posix.c is left out)
x86_64-w64-mingw32-gcc -std=c11 -Wall -Wextra -Werror -Iinclude \
  src/main.c src/cc_wrapper.c src/sys_shims.c src/tool_shims.c \
  src/sdk.c src/xcodeproj.c src/brew.c src/exec.c src/platform_win.c \
  -o masa.exe
```

## Usage

```
masa cc [clang-options...]    Compile or link for macOS with clang
masa sys <command> [args...]  Run a shimmed Darwin command
masa exec <command> [args...] Run a command inside a shimmed environment
masa shell [-c <command>]     Interactive shell in the shim environment
masa init-shims <dir>         Create the argv[0] shim symlinks in <dir>
masa sdk info                 Report whether the configured SDK is usable
masa sdk init-stub <dir>      Write a clean-room stub SDK to <dir>
masa xcode build <App.xcodeproj> [-o <dir>] [--bundle]
                              Build an Xcode project without Xcode
masa brew lint <formula.rb>   Lint a Homebrew formula
masa brew build <formula.rb> --source <dir> [--prefix <dir>]
                              Build a Homebrew formula into a Cellar
masa version                  Print the Masa version
masa help                     Show the usage message
```

### `masa cc`

`masa cc` forwards its arguments to `clang` and injects:

- `-target <MASA_TARGET>` (default `arm64-apple-darwin`)
- `-fuse-ld=lld` and `-Qunused-arguments`
- `-isysroot <MASA_SDK_PATH>` when `MASA_SDK_PATH` is set
- `-Wl,-platform_version,macos,15.0,15.0` when the invocation may link, which
  `ld64.lld` requires to emit an executable

```sh
# Inspect the command without running it.
MASA_DRY_RUN=1 masa cc -c hello.c -o hello.o
# clang -c hello.c -o hello.o -target arm64-apple-darwin -fuse-ld=lld -Qunused-arguments

# Compile a Mach-O object for Intel.
MASA_TARGET=x86_64-apple-darwin masa cc -c hello.c -o hello.o

# Link a real Mach-O executable against the stub SDK.
masa sdk init-stub /opt/masa/sdk
MASA_SDK_PATH=/opt/masa/sdk masa cc hello.c -o hello
```

### `masa exec`, `masa shell` and `masa init-shims`

`masa exec` makes a build system believe it is running on macOS. It creates a
directory of symlinks named after the emulated commands, prepends that
directory to `PATH` and runs your command:

```sh
masa exec make            # make sees Darwin, a macOS SDK and a working cc
masa exec ./configure
```

The symlinks point back at the Masa binary, so when `make` invokes `cc` (or
`uname`, `sw_vers`, `xcode-select`, `xcrun`, `ld`, `ar`, `ranlib`, `strip`,
`nm`, `otool`, `codesign`) it re-enters Masa through `argv[0]` dispatch and gets the
shimmed behaviour. `MASA_REAL_CC` is exported
first, recording the real host compiler, so the `cc` shim never calls itself
recursively.

`masa shell` drops you into an interactive shell with that same environment
already installed, so you can explore or drive a build by hand:

```sh
masa shell                 # zsh, else bash, else sh, inside the shim env
masa shell -c 'uname -s'   # run one command and exit
```

The shim directory is first on `PATH`, `MASA_SHIM_DIR` and `MASA_REAL_CC` are
exported, and `TERM` is defaulted when it is unset. Masa installs a clean
`masa@Darwin` prompt unless `MASA_KEEP_PS1=1` is set, in which case your own
prompt is left alone.

`masa init-shims <dir>` performs just the first step, which is useful when you
want to add the directory to `PATH` yourself:

```sh
masa init-shims /opt/masa/shims
export PATH=/opt/masa/shims:$PATH
uname -s          # Darwin
sw_vers           # ProductName: macOS ...
cc -c hello.c     # emits Mach-O for macOS
```

### BusyBox-style `argv[0]` dispatch

When Masa is invoked through one of its shim names it behaves as that command
directly, without a `masa` subcommand:

| `argv[0]` basename             | Behaviour                    |
| ------------------------------ | ---------------------------- |
| `cc`, `clang`, `gcc`           | same as `masa cc <args...>`  |
| `uname`, `sw_vers`, `xcode-select`, `xcrun` | same as `masa sys <name> <args...>` |
| `ld`, `ar`, `ranlib`, `strip`, `nm`, `otool` | the matching LLVM tool |
| `codesign`                     | ad-hoc sign / verify (see below) |

Any other name falls through to the normal `masa <subcommand>` router.

### Toolchain shims

Darwin build systems call `ld`, `ar`, `ranlib`, `strip`, `nm` and `otool`
directly, so Masa routes those names to LLVM's equivalents:

| Shim     | Runs                     |
| -------- | ------------------------ |
| `ld`     | `ld64.lld`               |
| `ar`     | `llvm-ar`                |
| `ranlib` | `llvm-ranlib`            |
| `strip`  | `llvm-strip`             |
| `nm`     | `llvm-nm`                |
| `otool`  | `llvm-objdump --macho`   |
| `codesign` | handled in-process     |

Each tool is located with `clang -print-prog-name=<tool>`, then next to the
host compiler, then on `PATH`. `ld` additionally supplies a
`-platform_version`, because `ld64.lld` refuses to link without one.

### `codesign`

Darwin build scripts commonly sign their output with `codesign -s -` (ad-hoc)
and then check it with `codesign --verify`. Masa answers both without a real
signing backend:

- `codesign -s - [--force] <target>` accepts the request and reports
  `signed (ad-hoc)`. It never fails a build, which is what a script expects.
- `codesign --verify <target>` succeeds when `<target>` is a valid 64-bit
  Mach-O image (`CF FA ED FE`), and reports whether it carries an
  `LC_CODE_SIGNATURE`.

`ld64.lld` already writes an ad-hoc signature for `arm64` output, so a
Masa-built arm64 executable is genuinely signed. A non-Mach-O target is
rejected by both modes.

### `masa sdk`

`masa sdk info` reports whether the configured SDK root contains a
`usr/lib/libSystem.tbd`, which is what a link needs.

`masa sdk init-stub <dir>` writes a clean-room stub SDK: TAPI v4 `.tbd` files
for `libSystem` and `crt1.o` plus a few standard headers. It contains no Apple
code, and it is enough for `ld64.lld` to produce real `MH_EXECUTE` images.

```sh
masa sdk init-stub /opt/masa/sdk
masa sdk info                     # status: ready (with MASA_SDK_PATH set)
```

### `masa xcode`

`masa xcode build` reads the text-based `project.pbxproj` inside a
`.xcodeproj` and turns it into a real Mach-O executable, so a checked-in Xcode
project can be built on a host without Xcode:

```sh
masa sdk init-stub /opt/masa/sdk
MASA_SDK_PATH=/opt/masa/sdk masa xcode build DemoApp.xcodeproj
# masa: xcode: target 'DemoApp' (2 sources)
# masa: xcode: built ./DemoApp
```

The parser is a clean-room recursive-descent reader for the old-style ASCII
plist grammar the format uses. It finds the `PBXNativeTarget`, takes its
`productName`, and follows `PBXSourcesBuildPhase` → `PBXBuildFile` →
`PBXFileReference` to collect the `.c` files the target compiles. Paths are
resolved through the `PBXGroup` tree, so a source in a `src` group is joined
back to `src/main.c`. The files are then compiled and linked through the Masa
`cc` wrapper, which supplies the target triple, the SDK sysroot and `ld64.lld`.

Options:

| Option            | Meaning                                                    |
| ----------------- | ---------------------------------------------------------- |
| `-o <dir>`        | Output directory (default: the `.xcodeproj` parent)        |
| `--bundle`        | Lay the result out as `<dir>/<ProductName>.app`            |

With `--bundle`, the executable is written to
`<dir>/<ProductName>.app/Contents/MacOS/<ProductName>` alongside a minimal
`Contents/Info.plist`. Missing or malformed inputs are reported clearly:

```
masa: xcode build: '/tmp/Nope.xcodeproj' does not exist: No such file or directory
masa: xcode build: missing /tmp/Empty.xcodeproj/project.pbxproj
masa: xcode build: malformed project: no PBXNativeTarget in project
```

### `masa brew`

`masa brew` reads a Homebrew formula (`.rb`) without needing Ruby. The reader
is a line-oriented scanner, not a Ruby interpreter: it understands the class
declaration, the scalar `key "value"` metadata fields, and the
`system "prog", "arg", ...` calls inside `def install`, including the
`#{prefix}`, `#{bin}`, `#{version}` and `#{name}` interpolations.

`masa brew lint <formula.rb>` checks the fields a working formula needs and
prints what it found:

```sh
masa brew lint mytool.rb
# mytool.rb: ok
#   name:     mytool
#   version:  1.2.3
#   install:  2 steps
#   test:     present
```

It fails (non-zero) when a required field is missing — `desc`, `homepage`,
`url`, `sha256` or `def install` — and warns (without failing) when there is no
`test do` block:

```
masa: brew lint: mytool.rb: missing required field 'sha256'
masa: brew lint: mytool.rb: warning: no 'test do' block
```

The formula name is derived from the class name (`FooBar` -> `foo-bar`); the
version comes from an explicit `version "..."` or, failing that, from the URL.

`masa brew build <formula.rb> --source <dir> [--prefix <dir>]` runs the install
steps and lays out a Homebrew-style Cellar:

```sh
masa brew build mytool.rb --source ./mytool-src --prefix /tmp/homebrew
# masa: brew: mytool 1.2.3
# masa: brew: keg    /tmp/homebrew/Cellar/mytool/1.2.3
# masa: brew: running 2 install steps
# masa: brew: installed .../Cellar/mytool/1.2.3/bin/mytool (Mach-O MH_EXECUTE)
# masa: brew: linked    /tmp/homebrew/bin/mytool -> .../bin/mytool
```

The steps run inside `--source` with the Masa shim environment on `PATH`
(`cc`, `clang`, `uname`, `sw_vers`, `xcrun`, `codesign`, `ar`, `ld`, ...) and
`MASA_SDK_PATH`, so a `system "make"` that calls `$(CC)` produces Mach-O. Every
regular file the steps place in `Cellar/<name>/<version>/bin/` is checked for
the 64-bit Mach-O magic (`CF FA ED FE`) and the `MH_EXECUTE` file type, then
linked into `<prefix>/bin/`. The prefix defaults to `$MASA_BREW_PREFIX`, or
`/tmp/masa-homebrew` when that is unset.

### `masa sys`

Shims the platform queries that build systems use to detect macOS:

| Command                 | Output                                                     |
| ----------------------- | ---------------------------------------------------------- |
| `uname -s`              | `Darwin`                                                   |
| `uname -m`              | `arm64`, or `x86_64` when `MASA_TARGET` starts with x86_64 |
| `sw_vers`               | `ProductName`, `ProductVersion` (15.0), `BuildVersion`     |
| `xcode-select -p`       | `MASA_SDK_PATH`, or `/opt/masa/sdk`                        |
| `xcrun --show-sdk-path` | `MASA_SDK_PATH`, or `/opt/masa/sdk`                        |

`sw_vers` also accepts `-productName`, `-productVersion` and `-buildVersion`.

## Environment variables

| Variable        | Meaning                                                |
| --------------- | ------------------------------------------------------ |
| `MASA_TARGET`   | Target triple (default `arm64-apple-darwin`)           |
| `MASA_SDK_PATH` | SDK root used for `-isysroot` and reported by the shims |
| `MASA_DRY_RUN`  | `1` prints the `cc` command instead of executing it    |
| `MASA_CC`       | Compiler invoked by the wrapper (default `clang`)      |
| `MASA_REAL_CC`  | Host compiler the `cc` shim must forward to            |
| `MASA_SHIM_DIR` | Directory holding the `argv[0]` shims                  |
| `MASA_SHIM_TARGET` | Symlink target for `init-shims` (default: this binary) |
| `MASA_BREW_PREFIX` | Prefix for `masa brew build` (default `/tmp/masa-homebrew`) |
| `MASA_KEEP_PS1` | `1` leaves your existing `PS1` alone in `masa shell`   |
| `TMPDIR`        | Preferred root for temporary directories and Cellars   |

## Layout

```
include/masa.h         public interface
include/masa_compat.h  portability shims (dirent, mkdir -p, ...)
src/main.c             CLI router and argv[0] dispatch
src/cc_wrapper.c       clang wrapper and argument injection
src/sys_shims.c        uname / sw_vers / xcode-select / xcrun shims
src/tool_shims.c       ld / ar / ranlib / strip / nm / otool shims and codesign
src/sdk.c              sdk info and the clean-room stub SDK writer
src/xcodeproj.c        pbxproj parser and the Xcode project builder
src/brew.c             Homebrew formula linter and builder
src/exec.c             init-shims, exec, shell and the shim environment
src/platform_posix.c   host services on POSIX (Linux, Android/Termux)
src/platform_win.c     host services on Windows (CreateProcess)
tests/test_masa.c      unit tests
```

## Testing

`make test` covers the argument builder, every shim, the CLI router,
`argv[0]` dispatch, `init-shims`, `exec`, `shell`, `sdk`, `xcode`, `brew`, the
toolchain shims, `codesign`, and compiler-recursion safety.

End-to-end tests produce real Mach-O files and check the header bytes: the
magic number (`CF FA ED FE`) and, for linked output, the file type
(`MH_EXECUTE`). One drives `masa cc` for both the `arm64` and `x86_64`
targets, another links through the toolchain shims, and a third runs
`masa exec make` on a generated project whose `$(CC)` resolves through
the shim. The Milestone 4 tests parse a realistic `project.pbxproj` (target
name plus several `.c` sources, and malformed inputs rejected without leaks),
build a generated `DemoApp.xcodeproj` into a `DemoApp.app` bundle and check
the bundled executable is `MH_EXECUTE` with an `Info.plist` beside it, then
sign and verify that binary through `masa exec codesign`. The Milestone 5 tests
parse a valid formula and malformed/incomplete ones (rejected without leaks),
then run `masa brew build` on a generated formula whose install steps compile a
C source with `$(CC)` and install it, checking the Cellar binary is
`MH_EXECUTE` and that `<prefix>/bin/<name>` symlinks to it. The Milestone 6
tests drive `masa shell` both with `-c` and interactively over a pipe,
checking the shim environment is installed (shim directory first on `PATH`,
`MASA_SHIM_DIR`/`MASA_REAL_CC` exported, `TERM` defaulted) and that `uname`
inside the shell reports `Darwin`, and exercise the host-services layer
(`masa_path_separator`, `masa_shell_path`, `masa_make_temp_dir`, `masa_which`).
Tests that need `clang` or `make` skip themselves when the tool is absent.

A GitHub Actions workflow (`.github/workflows/build.yml`) builds and tests with
both `gcc` and `clang` on Linux, and cross-compiles the Windows backend with
MinGW-w64 (`-Wall -Wextra -Werror`).
