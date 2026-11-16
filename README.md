# Masa

Masa is a cross-compilation wrapper and Darwin environment shim. It lets you
build macOS Mach-O binaries on a non-macOS host, without Xcode, by driving
`clang` and answering the platform queries that macOS build systems make.

Masa is licensed under the GNU General Public License v3.0 or later. See
[LICENSE](LICENSE).

## Status

Milestone 10 (v1.0.0): distribution. `masa pack` turns a `.app` bundle into a
distributable `.ipa` (a ZIP of `Payload/<App>.app`) or `.dmg` (a UDIF disk
image), and the Homebrew builder now runs single-string `system "..."` install
steps through `/bin/sh -c` the way Homebrew does. This sits on the Milestone 9
local independence (`masa doctor`, `make dist`, self-hosted CI, the Windows
ConPTY backend), the Milestone 8 standalone GUI terminal (`masa-term`, an
optional SDL2 + SDL2_ttf + libvterm front end over a real PTY), the Milestone 7
Swift toolchain support and Android cross-compilation, the Milestone 6
interactive `masa shell` and portable host-services layer, the Milestone 5
Homebrew formula linter and builder (`masa brew`), the Milestone 4 Xcode
`.xcodeproj` builder and `codesign` shim, the Milestone 3 full Mach-O
executable linking, toolchain shims and `masa sdk`, and the Milestone 2
`argv[0]` dispatch and `masa exec` environment runner, which in turn rest on
the Milestone 1 `cc` wrapper and `sys` shims.

## Requirements

- A C11 compiler (`gcc` or `clang`) and `make`.
- `clang` and LLVM's `ld64.lld` for producing Mach-O output. The other LLVM
  binutils (`llvm-ar`, `llvm-ranlib`, `llvm-strip`, `llvm-nm`, `llvm-objdump`)
  are used by the corresponding shims.
- No Apple SDK is required. `masa sdk init-stub` writes a clean-room stub SDK
  that is enough to link real Mach-O executables.
- Optional: a Swift toolchain (`swiftc`) for the Swift wrapper and for Xcode
  projects whose target compiles `.swift` files.
- Optional: the Android NDK (`aarch64-linux-android24-clang`) to cross-compile
  Masa itself for Android (`make android`).
- Optional, for `make gui` only: `libsdl2-dev`, `libsdl2-ttf-dev`,
  `libvterm-dev` (plus a monospace font such as `fonts-dejavu-core`). None of
  these are needed for `make` or `make test`.

## Building

```sh
make          # builds build/masa
make test     # builds and runs the unit test suite
make gui      # builds build/masa-term (SDL2 + SDL2_ttf + libvterm)
make test-gui # builds and runs the headless GUI test suite
make dist     # packages local builds into dist/ (see below)
make clean
```

Sources are compiled with `-std=c11 -Wall -Wextra -Werror -fsanitize=address`,
so the build fails on any warning and the tests run under AddressSanitizer.

`make gui` and `make test-gui` are the only targets that need the SDL2,
SDL2_ttf and libvterm development packages; the Makefile asks `pkg-config`
for their flags. Override `GUI_CFLAGS` / `GUI_LIBS` if the headers are not on
the default `pkg-config` path.

To cross-compile Masa for Android with the NDK (no AddressSanitizer, which the
NDK does not ship):

```sh
make android ANDROID_CC=/path/to/ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang
```

## Portability

Masa builds on any POSIX host (Linux, and Android/Termux via the `/system/bin`
shell and `$TMPDIR`), and on Windows through MinGW-w64. The Makefile picks the
host-services backend automatically: `src/platform_posix.c` everywhere except
native Windows, where `src/platform_win.c` is used. The POSIX sources compile
cleanly for Android with the NDK clang, which CI verifies. The Windows binary
can be cross-compiled from Linux:

```sh
# build the Windows binary (note: platform_posix.c is left out)
x86_64-w64-mingw32-gcc -std=c11 -Wall -Wextra -Werror -Iinclude \
  src/main.c src/cc_wrapper.c src/swift_wrapper.c src/sys_shims.c \
  src/tool_shims.c src/sdk.c src/xcodeproj.c src/brew.c src/pack.c src/exec.c \
  src/platform_win.c -o masa.exe
```

## Usage

```
masa cc [clang-options...]    Compile or link for macOS with clang
masa swiftc [swift-options...] Compile or link for macOS with swiftc
masa swift info [dir]         Report a Package.swift's name and layout
masa swift build [dir]        Build a Swift package with swiftc
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
masa pack --ipa <App.app> -o <out.ipa>
masa pack --dmg <App.app> -o <out.dmg>
                              Package a .app as an .ipa or a .dmg
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

### `masa swift`

`masa swiftc` (and the `swift`/`swiftc` `argv[0]` shims) forwards to the host
Swift compiler and injects the cross-compilation flags:

- `-target <MASA_TARGET>` (default `arm64-apple-macosx15.0`)
- `-sdk <MASA_SDK_PATH>` when `MASA_SDK_PATH` is set, since `swiftc` takes the
  sysroot through `-sdk` rather than `-isysroot`

The host compiler is taken from `MASA_SWIFTC` when set, otherwise `swiftc` is
looked up on `PATH`.

```sh
# Inspect the command without running it.
MASA_DRY_RUN=1 masa swiftc -O main.swift -o app
# swiftc -O main.swift -o app -target arm64-apple-macosx15.0
```

`masa swift info [dir]` reads the `Package.swift` in `dir` (default `.`) and
reports the package name and whether a conventional `Sources/` tree exists;
`masa swift build [dir]` hands the manifest to `swiftc`:

```sh
masa swift info
# Package.swift: ./Package.swift
# name:          DemoKit
# root:          .
# sources:       yes (Sources/)
```

### `masa doctor`

`masa doctor` inspects the local machine and reports whether it can build
macOS binaries here, without a Mac or hosted CI. Each item is tagged
`[OK]`, `[WARN]` or `[MISSING]`:

```sh
masa doctor
# masa 1.0.0 doctor - local macOS cross-build readiness
#
# [OK]      C compiler     /usr/bin/clang
# [OK]      linker         /usr/lib/llvm-19/bin/ld64.lld
# [OK]      archiver       /usr/lib/llvm-19/bin/llvm-ar
# [OK]      ranlib         /usr/lib/llvm-19/bin/llvm-ranlib
# [OK]      nm             /usr/lib/llvm-19/bin/llvm-nm
# [OK]      objdump        /usr/lib/llvm-19/bin/llvm-objdump
# [WARN]    swiftc         not found (optional; only for Swift targets)
# [WARN]    SDK            /opt/masa/sdk missing usr/lib/libSystem.tbd and usr/lib/crt1.o
#          hint: run `masa sdk init-stub /opt/masa/sdk`
# [OK]      target         arm64-apple-darwin
#
# result: core C cross-build ready (clang + ld64.lld)
```

It checks the C compiler (`MASA_REAL_CC` / `MASA_CC` / `clang`), LLVM's
`ld64.lld`, the LLVM binutils, an optional `swiftc`, the SDK
(`$MASA_SDK_PATH`, looking for `usr/lib/libSystem.tbd` and `usr/lib/crt1.o`)
and the target triple. It exits `0` when the core requirements (a C compiler
and a Mach-O linker) are present and non-zero otherwise, so it can gate a
build script.

### `make dist` - local reproducible packaging

`make dist` (which runs `scripts/dist.sh`) builds and packages Masa without
any hosted CI. It always builds the native Linux binary and, when the
toolchain is on `PATH`, also the Windows `x86_64` (`x86_64-w64-mingw32-gcc`)
and Android `aarch64` (`ANDROID_CC`) builds. Each target lands in
`dist/masa-<version>-<os>-<arch>/` with the binary, `LICENSE` and
`README.md`, is archived as both a `.tar.gz` and a `.zip`, and a
`dist/SHA256SUMS` file is written at the end:

```sh
make dist
# dist: Masa 1.0.0 (x86_64 host)
# dist: building native masa-1.0.0-linux-x86_64
# dist: packaged masa-1.0.0-linux-x86_64.tar.gz and masa-1.0.0-linux-x86_64.zip
# dist: building masa-1.0.0-windows-x86_64
# dist: packaged masa-1.0.0-windows-x86_64.tar.gz and masa-1.0.0-windows-x86_64.zip
# dist: skipping Android (set ANDROID_CC to an NDK clang to include it)
# dist: wrote .../dist/SHA256SUMS
```

`zip` is used when available; otherwise the archives are written with
Python's standard `zipfile` module.

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
`PBXFileReference` to collect the source files the target compiles. Paths are
resolved through the `PBXGroup` tree, so a source in a `src` group is joined
back to `src/main.c`. `.c` files are compiled and linked through the Masa `cc`
wrapper, which supplies the target triple, the SDK sysroot and `ld64.lld`.
A target whose sources are `.swift` is handed to the Swift wrapper instead
(see `masa swift`). A target that mixes both kinds is compiled with `swiftc`
and warns, since Masa does not model a two-stage build.

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
`MASA_SDK_PATH`, so a `system "make"` that calls `$(CC)` produces Mach-O. A
single-string `system "cc main.c -o mytool"` (spaces or shell syntax) is run
through `/bin/sh -c` the way Homebrew does, while the multi-argument form
`system "cc", "main.c", ...` is executed directly with `execvp`. Every
regular file the steps place in `Cellar/<name>/<version>/bin/` is checked for
the 64-bit Mach-O magic (`CF FA ED FE`) and the `MH_EXECUTE` file type, then
linked into `<prefix>/bin/`. The prefix defaults to `$MASA_BREW_PREFIX`, or
`/tmp/masa-homebrew` when that is unset.

### `masa pack`

`masa pack` packages a `.app` bundle (for example the one produced by
`masa xcode build --bundle`) into a distributable artifact, without any Apple
tools:

```sh
MASA_SDK_PATH=/opt/masa/sdk masa xcode build DemoApp.xcodeproj --bundle
masa pack --ipa DemoApp.app -o DemoApp.ipa
masa pack --dmg DemoApp.app -o DemoApp.dmg
```

- `--ipa` writes a ZIP archive whose entries live under
  `Payload/<App>.app/`, which is the layout the iOS/macOS App Store expects.
  The writer is a from-scratch C11 implementation: it uses the `STORE` method
  and computes CRC-32 for each entry, so the result opens with any ZIP reader.
- `--dmg` writes a UDIF disk image: an XML property list describing the
  device and a `blkx` block table, followed by a 512-byte `koly` trailer
  (version 4, header size 512) with matching master and data CRC-32s.

| Option       | Meaning                                             |
| ------------ | --------------------------------------------------- |
| `--ipa`      | Package as a `.ipa` (ZIP of `Payload/<App>.app`)    |
| `--dmg`      | Package as a `.dmg` (UDIF image)                    |
| `-o <file>`  | Output path (required)                              |

Exactly one of `--ipa` or `--dmg` is required, and the input must be a
directory. Missing or invalid arguments are reported clearly and exit with
status 2:

```
masa: pack: missing format (--ipa | --dmg)
masa: pack: choose one of --ipa or --dmg
masa: pack: missing input <App.app>
masa: pack: missing -o <output>
masa: pack: unknown option '--bogus'
masa: pack: 'Demo.app' is not a directory
```

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

### `masa-term` (GUI terminal)

`masa-term` is a standalone graphical terminal for the Masa shell, built only
by `make gui` (it links SDL2, SDL2_ttf and libvterm). It opens a real
pseudo-terminal, runs `masa shell` on it, feeds the output through `libvterm`
and draws a dark, macOS-style window: a header bar with the red/yellow/green
traffic lights and the `masa — Darwin` title, the full 256-colour and
24-bit-RGB ANSI palette, and a blinking block cursor.

```sh
make gui
./build/masa-term              # open a window running `masa shell`
./build/masa-term --headless   # render a few frames with the dummy driver
```

The window is resizable; the terminal recomputes its rows and columns and
tells the child through `TIOCSWINSZ` (POSIX) or `ResizePseudoConsole`
(Windows). The monospace font is discovered from a list of common system
fonts (DejaVu Sans Mono, Liberation Mono, JetBrains Mono, Menlo, ...), or set
explicitly:

```sh
MASA_FONT=/path/to/Font.ttf ./build/masa-term
```

Set `MASA_BIN` if the `masa` executable is not next to `masa-term` or on
`PATH`.

On POSIX the pseudo-terminal backend is `src/pty_posix.c` (`openpty` +
`fork`/`exec`). On Windows it is `src/pty_win.c`, which uses the Console
Pseudo Terminal API (`CreatePseudoConsole`, `ResizePseudoConsole`,
`ClosePseudoConsole`) available in Windows 10 1809 and later to run
`masa.exe shell` with the `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE` thread
attribute. Both backends implement the same SDL-free interface declared in
`include/masa_pty.h`, and the shared `masa shell` command/environment
construction lives in `src/pty_common.c`, so the Windows backend
cross-compiles with `x86_64-w64-mingw32-gcc` even when SDL2 is not installed
on the build host.

## Environment variables

| Variable        | Meaning                                                |
| --------------- | ------------------------------------------------------ |
| `MASA_TARGET`   | Target triple (default `arm64-apple-darwin`)           |
| `MASA_SDK_PATH` | SDK root used for `-isysroot` and reported by the shims |
| `MASA_DRY_RUN`  | `1` prints the `cc`/`swiftc` command instead of running it |
| `ANDROID_CC`    | NDK clang used by `make android` and the NDK test      |
| `MASA_CC`       | Compiler invoked by the wrapper (default `clang`)      |
| `MASA_SWIFTC`   | Swift compiler the `swift`/`swiftc` shims forward to   |
| `MASA_REAL_CC`  | Host compiler the `cc` shim must forward to            |
| `MASA_SHIM_DIR` | Directory holding the `argv[0]` shims                  |
| `MASA_SHIM_TARGET` | Symlink target for `init-shims` (default: this binary) |
| `MASA_BREW_PREFIX` | Prefix for `masa brew build` (default `/tmp/masa-homebrew`) |
| `MASA_KEEP_PS1` | `1` leaves your existing `PS1` alone in `masa shell`   |
| `MASA_FONT`     | TrueType font `masa-term` uses (overrides discovery)   |
| `MASA_BIN`      | `masa` executable `masa-term` runs on the PTY          |
| `TMPDIR`        | Preferred root for temporary directories and Cellars   |

## Layout

```
include/masa.h         public interface
include/masa_compat.h  portability shims (dirent, mkdir -p, ...)
src/main.c             CLI router and argv[0] dispatch
src/cc_wrapper.c       clang wrapper and argument injection
src/swift_wrapper.c    swiftc wrapper, Package.swift reader and `masa swift`
src/sys_shims.c        uname / sw_vers / xcode-select / xcrun shims
src/tool_shims.c       ld / ar / ranlib / strip / nm / otool shims and codesign
src/sdk.c              sdk info and the clean-room stub SDK writer
src/xcodeproj.c        pbxproj parser and the Xcode project builder
src/brew.c             Homebrew formula linter and builder
src/pack.c             .app packager (.ipa ZIP and .dmg UDIF)
src/exec.c             init-shims, exec, shell and the shim environment
src/doctor.c           masa doctor: local cross-build readiness report
src/platform_posix.c   host services on POSIX (Linux, Android/Termux)
src/platform_win.c     host services on Windows (CreateProcess)
include/masa_pty.h     SDL-free pseudo-terminal interface (GUI backends)
include/masa_gui.h     interface for masa-term (GUI build only)
src/pty_posix.c        POSIX pseudo-terminal backend for masa-term
src/pty_win.c          Windows ConPTY pseudo-terminal backend
src/pty_common.c       shared `masa shell` argv/env for the PTY backends
src/gui_term.c         masa-term: SDL2 + libvterm window (make gui)
scripts/dist.sh        local reproducible packaging (make dist)
tests/test_masa.c      unit tests
tests/test_gui.c       masa-term tests (make test-gui)
```

## Testing

`make test` covers the argument builder, every shim, the CLI router,
`argv[0]` dispatch, `init-shims`, `exec`, `shell`, `sdk`, `xcode`, `brew`,
`swift`, `pack`, the toolchain shims, `codesign`, and compiler-recursion
safety.

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
The Milestone 7 tests build the `swiftc` argument list, check the wrapper
injects `-target`/`-sdk` (and the Swift default triple) in dry-run mode,
dispatch the `swift`/`swiftc` `argv[0]` shims, read a generated `Package.swift`
(reporting the name and `Sources/` tree, and rejecting a non-manifest without
leaks), drive `masa swift build`, and confirm the Xcode builder routes a
`.swift` target to `swiftc`. The Milestone 9 tests run `masa doctor` against a
healthy environment (a stub SDK plus clang and lld present, checking the `[OK]`
lines and a zero exit) and against a missing SDK (checking the `[WARN]` line
and the `init-stub` hint), and confirm `doctor` reports a `[MISSING]` compiler
and exits non-zero when no compiler is on `PATH`; a further test cross-compiles
the ConPTY backend (`src/pty_win.c`) and its shared helper with
`x86_64-w64-mingw32-gcc`, skipping when MinGW-w64 is absent. The Milestone 10
tests build a formula whose install steps are single-string `system "..."`
calls (checking the resulting Cellar binary is `MH_EXECUTE` and the
`<prefix>/bin` symlink is present) and assert that the parser routes a bare
word to `execvp` but a command with spaces or shell syntax through `/bin/sh`;
`masa pack --ipa` is run over a generated bundle and the output checked to be
a ZIP (`PK\x03\x04`) whose entries include `Payload/DemoApp.app/` and the
`Contents/MacOS/` executable, `masa pack --dmg` is checked to end in a `koly`
UDIF trailer (version 4, header size 512) wrapping an XML plist with a `blkx`
block table, and the pack error paths are checked to exit with status 2. A
separate test compiles every source for Android with the NDK clang
(`ANDROID_CC`) when one is available, and skips otherwise. Tests that need
`clang` or `make` skip themselves when the tool is absent.

`make test-gui` covers `masa-term` (`tests/test_gui.c`), compiled with the
same `-Wall -Wextra -Werror -fsanitize=address` flags. It runs `masa shell -c
"uname -s && sw_vers -productName"` on a real pseudo-terminal, feeds the bytes
through libvterm and asserts the screen buffer contains `Darwin` and `macOS`;
it exercises the PTY resize and write paths; and it brings up the SDL2 +
SDL2_ttf renderer under `SDL_VIDEODRIVER=dummy`, draws a frame, resizes and
tears down with no ASan/LSan leaks.

A GitHub Actions workflow (`.github/workflows/ci.yml`) builds and tests with
both `gcc` and `clang` on Linux, builds `masa-term` and runs its headless
tests (`libsdl2-dev`, `libsdl2-ttf-dev`, `libvterm-dev`, `fonts-dejavu-core`),
cross-compiles the Windows backend with MinGW-w64, cross-compiles for
Android with the NDK (`-Wall -Wextra -Werror`), and runs `make dist` plus
`masa doctor`. A `.woodpecker.yml` pipeline runs the same core, GUI and
packaging steps on self-hosted / Codeberg Woodpecker CI, so the project can be
built and released without depending on Microsoft/GitHub-hosted
infrastructure.
