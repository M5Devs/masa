# Masa

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Language: C11](https://img.shields.io/badge/language-C11-00599C.svg)](#requirements)
[![Version: v1.4.0](https://img.shields.io/badge/version-v1.4.0-brightgreen.svg)](https://github.com/M5Devs/masa/releases)
[![Platforms: Linux | Windows | Android](https://img.shields.io/badge/platforms-Linux%20%7C%20Windows%20%7C%20Android-lightgrey.svg)](#portability)

> **Build and package macOS Mach-O binaries on Linux, Windows & Android — and
> run them in-process on Linux & Android — without a Mac, Xcode, the $99/yr
> Apple Developer tax, or a macOS CI runner.**

Masa is a cross-compilation wrapper and Darwin environment shim. It drives
`clang` and LLVM's `ld64.lld` to produce real macOS Mach-O executables on a
non-macOS host, answers the platform queries macOS build systems make,
packages the result as a `.dmg` or `.ipa`, and — with the **Danroot** loader —
*runs* a Mach-O binary in-process on a POSIX host.

Masa is licensed under the GNU General Public License v3.0 or later. See
[LICENSE](LICENSE).

## Why Masa?

- **No Apple hardware or software.** The SDK is a clean-room stub
  (`masa sdk init-stub`), the linker is LLVM's `ld64.lld`, and the compiler is
  the `clang` you already have. Nothing from Apple is downloaded or required.
- **It doesn't just build — it runs.** `masa danroot` executes a Mach-O
  `MH_EXECUTE` binary — and the `MH_DYLIB` libraries it links against —
  directly on a POSIX host (Linux, Android/Termux), in-process, with no
  virtual machine, no kernel module and no full Darwin userland. Build and
  run in one place.
- **It speaks your build systems.** `masa xcode build` reads a checked-in
  `.xcodeproj`, `masa brew` lints and builds Homebrew formulas, and
  `masa exec` / `masa shell` run `make` and shell scripts with `cc`, `ld`,
  `ar`, `ranlib`, `strip`, `nm`, `otool` and `codesign` resolved to Masa's
  shims.
- **It ships.** `masa pack --dmg` writes a UDIF disk image and `masa pack
  --ipa` writes the `Payload/<App>.app` ZIP — both from scratch in C11, with no
  Apple tooling.
- **It's small and portable.** Pure C11 built with `-std=c11 -Wall -Wextra
  -Werror` and no runtime dependencies for the core. Builds on Linux, Windows
  (MinGW-w64) and Android (NDK).

## How it compares

| | **Masa** | Darling | Mac + Xcode |
| --- | --- | --- | --- |
| Host OS | Linux, Windows, Android | Linux, macOS | macOS only |
| Build Mach-O binaries | ✅ `masa cc`, `masa xcode` | — | ✅ |
| Run Mach-O binaries | ✅ in-process (Danroot, POSIX hosts) | ✅ compatibility layer | ✅ |
| Run Mach-O dylibs | ✅ in-process (Danroot) | ✅ compatibility layer | ✅ |
| Needs a real Apple SDK | ❌ clean-room stub | ✅ macOS frameworks/SDK | ✅ |
| Needs a Mac | ❌ | ❌ | ✅ |
| Apple Developer Program ($99/yr) | ❌ | ❌ | ✅ to distribute |
| Package `.dmg` / `.ipa` | ✅ `masa pack` | — | ✅ |
| License | GPLv3 | GPLv3 | proprietary |

*Scope note: Masa targets self-contained C/C++ (and Swift) executables linked
against the stub SDK's `libSystem`. It is not a full macOS userland — a program
that needs Apple's private frameworks or a GUI toolkit will not run under
Danroot.*

## Quick Start

```sh
git clone https://github.com/M5Devs/masa && cd masa && make

# 1. one-time: write a clean-room stub SDK (no Apple SDK needed)
masa sdk init-stub /opt/masa/sdk

# 2. build a real Mach-O executable
MASA_SDK_PATH=/opt/masa/sdk masa cc hello.c -o hello

# 3. run it right here — no Mac, no VM, no kernel module
masa danroot ./hello
# hello from a Mach-O binary running on Linux!
```

`masa run` / `masa danroot` is architecture-matched: build for the host's
architecture (`MASA_TARGET=x86_64-apple-darwin` on an Intel/AMD host;
`arm64-apple-darwin`, the default, on Apple Silicon). The loader runs on POSIX
hosts (Linux, Android/Termux); on Windows Masa builds and packages, but
`masa run` reports that execution is POSIX-only.

**Build an Xcode project, then ship it:**

```sh
MASA_SDK_PATH=/opt/masa/sdk masa xcode build DemoApp.xcodeproj --bundle
# masa: xcode: target 'DemoApp' (2 sources)
# masa: xcode: bundled DemoApp.app
masa pack --dmg DemoApp.app -o DemoApp.dmg    # UDIF disk image
masa pack --ipa DemoApp.app -o DemoApp.ipa    # Payload/DemoApp.app ZIP
```

**Lint and build a Homebrew formula:**

```sh
masa brew lint mytool.rb
masa brew build mytool.rb --source ./mytool-src --prefix /tmp/homebrew
```

## Status

Milestone 14 (v1.4.0): the Masa Danroot loader gains **Layer 3** - a
clean-room CoreFoundation and Objective-C/Foundation runtime, plus the
`arm64` stack-protector fix that lets a default-configured arm64 program
link at all. The stub SDK now writes
`System/Library/Frameworks/CoreFoundation.framework/CoreFoundation.tbd`
(and its header) so `masa cc -framework CoreFoundation` compiles and links
cleanly, and the loader satisfies the framework's symbols in-process: a
reference-counted `CFString` (`CFStringCreateWithCString`,
`CFStringGetCStringPtr`, `CFStringGetCString`, `CFStringGetLength`) and
`CFArray` (`CFArrayCreate`, `CFArrayGetCount`, `CFArrayGetValueAtIndex`),
`CFRetain`/`CFRelease`, `CFShow` and `kCFAllocatorDefault`. A matching
`libobjc.A.tbd`, `Foundation.framework/Foundation.tbd` and the
`objc/objc.h`, `objc/runtime.h`, `objc/message.h` headers back a lightweight
Objective-C runtime (`objc_getClass`, `sel_registerName`, a variadic
`objc_msgSend` that understands the common `NSObject`/`NSString` selectors,
the autorelease-pool entry points, and `NSLog`). Every object the shimmed
runtimes hold is tracked and freed when a run finishes, so a program that
never balances its retains still leaves the process leak-free. The same
release exports `___stack_chk_fail` and `___stack_chk_guard` from
`libSystem.tbd` and provides them as Danroot shims (a clean canary and a
clean abort on mismatch), so an arm64 program that uses a stack buffer
links into a real `MH_EXECUTE` and runs. This sits on the Milestone 13
Danroot Layer 2 (v1.3.0) - the proot-style virtual root plus the directory,
stat and Mach time/task shims - the Milestone 12 dynamic library loading
(v1.2.0), the Milestone 11 loader core (`masa run` / `masa danroot`
executing a Mach-O `MH_EXECUTE` binary built by `masa cc` - a clean-room,
in-process reimplementation of the `dyld` steps a self-contained executable
needs), the Milestone 10 distribution work
(`masa pack` to `.ipa`/`.dmg`, the Homebrew single-string `system` fix), the
Milestone 9 local independence (`masa doctor`, `make dist`, self-hosted CI,
the Windows ConPTY backend), the Milestone 8 standalone GUI terminal
(`masa-term`, an optional SDL2 + SDL2_ttf + libvterm front end over a real
PTY), the Milestone 7 Swift toolchain support and Android cross-compilation,
the Milestone 6 interactive `masa shell` and portable host-services layer,
the Milestone 5 Homebrew formula linter and builder (`masa brew`), the
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
masa run [--root <dir>] <binary> [args...]
                              Run a Mach-O executable on this host
masa danroot [--root <dir>] <binary> [args...]
                              Alias for `masa run` (the Danroot loader)
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

# Build a Mach-O dynamic library and link a program against it.
MASA_SDK_PATH=/opt/masa/sdk masa cc -dynamiclib helper.c -o libhelper.dylib \
    -install_name @rpath/libhelper.dylib
MASA_SDK_PATH=/opt/masa/sdk masa cc main.c -L. -lhelper \
    -Wl,-rpath,@executable_path -o app
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
for `libSystem`, `libgcc_s.1`, `libobjc.A` and the `crt1.o` / `dylib1.o` link
stubs, plus the standard headers (`stdio.h`, `stdlib.h`, `string.h`,
`stdint.h`, `unistd.h`, `fcntl.h`, `dlfcn.h`, `dirent.h`, `sys/stat.h`,
`sys/utsname.h`, `sys/sysctl.h`, `mach/mach_time.h`, `mach/mach.h`, and the
`objc/objc.h`, `objc/runtime.h`, `objc/message.h` Objective-C headers). It
also writes the `CoreFoundation` and `Foundation` framework stubs
(`System/Library/Frameworks/<Name>.framework/<Name>.tbd` plus their
`Headers/<Name>.h`), so `masa cc -framework CoreFoundation` (or
`-framework Foundation`) compiles and links against them; `clang` finds them
through the `-isysroot` the wrapper injects. The SDK
contains no Apple code, and it is enough for `ld64.lld` to produce real
`MH_EXECUTE` and `MH_DYLIB` images. `sys/stat.h` carries the full 64-bit
Darwin `struct stat` and `dirent.h` the Darwin `struct dirent`, so a program
can read `st_size`/`S_ISREG` and `readdir` results straight back from the
Danroot shims. `libSystem.tbd` exports `___stack_chk_fail` and
`___stack_chk_guard`, so an `arm64` program that uses a stack buffer links
without extra flags.

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

### `masa run` / `masa danroot`

`masa run` executes a Mach-O `MH_EXECUTE` binary built by `masa cc` directly
on a POSIX host (Linux, Android/Termux), without a virtual machine, a kernel
module or a full Darwin userland. `masa danroot` is an exact alias.

```sh
masa sdk init-stub /opt/masa/sdk
MASA_SDK_PATH=/opt/masa/sdk masa cc hello.c -o hello
masa run ./hello alpha beta
# hello danroot, argc=3
# arg[1]=alpha
# arg[2]=beta
```

This is the **Masa Danroot** loader, a clean-room (GPLv3) reimplementation of
the parts of `dyld` a self-contained program needs:

- it reads the `LC_SEGMENT_64` load commands and maps each segment with
  `mmap()`, sliding them to whatever base the kernel returns;
- it follows the image's dynamic library dependencies - `LC_LOAD_DYLIB`,
  `LC_LOAD_WEAK_DYLIB` and `LC_REEXPORT_DYLIB` - loading each `MH_DYLIB` the
  same way (and recursively, so a library's own dependencies are loaded too).
  A weak dependency that is missing is skipped; a required one that is
  missing is a clear error. `/usr/lib/libSystem.B.dylib` and
  `/usr/lib/libSystem.dylib` are never read from disk - the built-in shim
  satisfies them;
- it resolves dependency paths the way dyld does: `@executable_path/`,
  `@loader_path/` and `@rpath/` (searched against the image's `LC_RPATH`
  entries), absolute paths, and bare names on `MASA_LIBRARY_PATH`,
  `DYLD_LIBRARY_PATH` and the loading image's own directory;
- it indexes each image's exports - the `LC_DYLD_EXPORTS_TRIE` trie, with an
  `LC_SYMTAB` fallback - so a bind in one image can resolve to a definition
  in another;
- it applies the image's relocations, whichever format the linker emitted:
  `LC_DYLD_CHAINED_FIXUPS` (`ptr_fmt = 2`, `DYLD_CHAINED_PTR_64`) or the
  classic `LC_DYLD_INFO_ONLY` rebase/bind opcodes (older `ld64.lld`, such as
  the clang-18 on Ubuntu 24.04), rewriting rebases to the real load address
  and resolving binds;
- it resolves the image's imports against a small clean-room `libSystem`
  shim (see the symbol list below) and against the exports of every loaded
  image;
- it calls the `LC_MAIN` entry point with `(argc, argv, envp, apple)` and
  returns the value it produces as the process exit status.

A Masa-linked program contains no raw `syscall` instructions - every libc
call goes through the shim - so no trap interception is needed and the loader
runs entirely in-process. It is architecture-matched: an `x86_64` host runs
`x86_64` images and an `arm64` host runs `arm64` images. Running an image for
the other architecture is rejected cleanly:

```
masa: run: architecture mismatch: './hello' is arm64, host is x86_64
```

**Dynamic libraries.** Build a library with `masa cc -dynamiclib` and link a
program against it as usual; `masa run` loads both:

```sh
masa cc -dynamiclib helper.c -o libhelper.dylib -install_name @rpath/libhelper.dylib
masa cc main.c -L. -lhelper -Wl,-rpath,@executable_path -o app
masa run ./app
```

**Virtual root (Layer 2).** A Darwin program that hard-codes `/Users/me`,
`/Applications` or a Homebrew prefix would otherwise read or write the host's
own filesystem. `--root <dir>` (or `MASA_DANROOT_ROOT=<dir>`) makes those
paths relative to `<dir>`:

```sh
mkdir -p /tmp/root/Users/me
masa run --root /tmp/root ./app
# the program's /Users/me/hello.txt is /tmp/root/Users/me/hello.txt
```

Every file and directory shim honours the root - `fopen`, `open`, `access`,
`stat`, `lstat`, `unlink`, `mkdir`, `rmdir`, `chdir`, `opendir` and `dlopen`.
A path is rewritten only when it is absolute *and* begins with one of the
macOS system prefixes (`/Users`, `/Applications`, `/Library`, `/System`,
`/opt/homebrew`, `/usr/local`), matched as whole path components; relative
paths and every other host path are left untouched, so ordinary Linux/Android
I/O keeps working. The `dlopen` resolver rewrites a dylib path the same way
before searching it.

The shim covers the C symbols a program built against the stub SDK can
reference. Standard I/O: `printf`, `fprintf`, `sprintf`, `snprintf`,
`vsnprintf`, `sscanf`, `puts`, `putchar`, `fopen`, `fclose`, `fread`,
`fwrite`, `fgets`, `fputs`, `fflush`, `fseek`, `ftell`, `feof`, `ferror`.
File descriptors: `read`, `write`, `open`, `close`, `lseek`, `access`,
`unlink`, `mkdir`, `rmdir`, `getcwd`, `chdir`. Directories and stat:
`opendir`, `readdir`, `closedir`, `stat`, `lstat`, `fstat` - the host
structures are translated into the 64-bit Darwin ABI layouts the program was
compiled against (a 144-byte `struct stat` with `st_size` at offset 96, and a
`struct dirent` whose `d_name` starts at byte 21). Strings and memory:
`malloc`, `calloc`, `realloc`, `free`, `strlen`, `strcmp`, `strncmp`,
`strcpy`, `strncpy`, `strcat`, `strncat`, `strchr`, `strrchr`, `strstr`,
`strdup`, `strndup`, `strerror`, `memcpy`, `memset`, `memmove`, `atoi`.
Process and environment: `getenv`, `setenv`, `unsetenv`, `getpid`, `getuid`,
`isatty`, `usleep`, `sleep`, `abort`, `exit`. Darwin system information:
`uname` (reporting `Darwin`) and `sysctlbyname` (`kern.ostype`,
`kern.osrelease`, `hw.machine`, `hw.ncpu`, `hw.memsize`, ...). Mach time and
task: `mach_absolute_time` (host monotonic clock in nanoseconds),
`mach_timebase_info` (a 1:1 timebase, `numer == denom == 1`) and
`mach_task_self_` (a stable non-zero task port handle, `0x103`). Dynamic
linker: `dlopen`, `dlsym`, `dlclose`, `dlerror`. A `dlopen` loads a library
through the same path resolver and then relocates and protects the freshly
mapped image, so a function it returns is immediately callable; `dlsym`
follows the Mach-O C convention, prepending the leading underscore
(`dlsym(h, "magic_add")` finds `_magic_add`), falling back to the name as
given. The standard streams are exposed as data symbols (`__stdoutp`
and friends). An `exit()` call unwinds back to the loader (so cleanup still
runs) and its status becomes the process exit code.

**CoreFoundation and Objective-C (Layer 3).** `masa run` also satisfies the
`CoreFoundation` and `Foundation` frameworks, so a program linked with
`-framework CoreFoundation` (or `-framework Foundation`) runs without the
real frameworks present. The CoreFoundation shim implements a
reference-counted `CFString` (`CFStringCreateWithCString`,
`CFStringGetCStringPtr`, `CFStringGetCString`, `CFStringGetLength`) and
`CFArray` (`CFArrayCreate`, `CFArrayGetCount`, `CFArrayGetValueAtIndex`),
plus `CFRetain`/`CFRelease`, `CFShow` and the `kCFAllocatorDefault` handle:

```sh
masa cc app.c -framework CoreFoundation -o app
masa run ./app
```

The Objective-C shim (`libobjc.A.dylib`, also reachable through
`Foundation.framework/Foundation`) provides `objc_getClass`,
`sel_registerName`, a variadic `objc_msgSend` that understands the common
`NSObject`/`NSString` selectors (`alloc`, `init`, `stringWithUTF8String:`,
`UTF8String`, `length`, `retain`, `release`, `autorelease`), the
autorelease-pool entry points `objc_autoreleasePoolPush` /
`objc_autoreleasePoolPop`, and `NSLog` (which accepts an `NSString` or a
plain C format string and prints to stderr). Every object the two runtimes
hold is tracked and released when the run finishes, so a program that never
balances its retains still leaves nothing allocated.

**Stack protector.** The stub `libSystem.tbd` exports `___stack_chk_fail`
and `___stack_chk_guard`, and the loader provides both as shims: the guard
is a fixed non-zero canary and a mismatch prints
`masa: danroot: stack smashing detected, aborting` and aborts cleanly. This
is what lets an `arm64` program - whose compiler enables the stack protector
by default - link into a real `MH_EXECUTE` at all.

Errors are reported clearly and never crash:

```
masa: run: cannot open './nope': No such file or directory
masa: run: './notmacho' is not a 64-bit Mach-O image
masa: run: './app' has no LC_MAIN entry point
masa: run: './app': cannot find dependency '@rpath/libhelper.dylib'
```

`masa run` is available on POSIX hosts; on Windows the subcommand reports
that it is POSIX-only, so the MinGW cross-build stays clean.

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
| `MASA_LIBRARY_PATH` | Extra colon-separated dirs `masa run` searches for dylibs |
| `MASA_DANROOT_ROOT` | Virtual root for `masa run` path virtualization        |
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
src/danroot.c          Masa Danroot: the Mach-O loader (executables and
                       dylibs) behind `masa run`
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
block table, and the pack error paths are checked to exit with status 2. The
Milestone 11 tests build a real Mach-O executable with `masa cc` for the host
architecture and run it through both `masa run` and `masa danroot`, checking
the program's `printf` output, its `argc`/`argv` handling and a `42` exit
status; a second program calls `exit()` and its status is checked to be
honoured; a foreign-architecture binary is built and `masa run` is asserted to
reject it with an `architecture mismatch` message; and the non-Mach-O,
missing-file and missing-argument paths are checked to exit non-zero without
leaking. A separate test compiles every source for Android with the NDK clang
(`ANDROID_CC`) when one is available, and skips otherwise. Tests that need
`clang` or `make` skip themselves when the tool is absent. The Milestone 12
tests link a program against a `masa cc -dynamiclib` helper and check the
cross-image call works, that a missing required dylib is a clear error while a
missing weak one is skipped, that both the chained-fixups and the classic
`LC_DYLD_INFO_ONLY` encodings bind, and that `dlsym` applies the Mach-O
leading underscore. The Milestone 13 tests build a program that reads
`/Users/demo/hello.txt`, creates `/Applications/MyApp`, lists `/Users/demo`
and `stat`s a file, run it with `masa run --root <tmp>` and again with
`MASA_DANROOT_ROOT`, and assert every operation happened inside the virtual
root (the host's `/Applications/MyApp` never appears); a second program calls
`mach_timebase_info` and `mach_absolute_time` twice, checking the `1:1`
timebase, monotonic non-decreasing nanoseconds and the `0x103` task port; and
the error paths (`stat` of a missing file under the root, `--root` with no
directory) are checked to fail cleanly without leaking. The Milestone 14
tests link a program with a stack buffer for `arm64` (whose compiler enables
the stack protector by default) and assert the result is a valid
`MH_EXECUTE` (`CF FA ED FE`, filetype 2), proving the `__stack_chk_*` shims
resolve; a CoreFoundation program linked with `-framework CoreFoundation`
creates a `CFString` and `CFArray`, calls `CFShow`, reads the string back
with `CFStringGetCString`/`CFStringGetCStringPtr`, counts the array and
balances one `CFRetain`/`CFRelease`, and is run through `masa danroot` with
its output checked and no LeakSanitizer report; and an Objective-C program
linked with `-framework Foundation` calls `objc_getClass("NSString")`,
`sel_registerName`, `objc_msgSend` and `NSLog` and is likewise checked to run
clean with no leaks.

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
