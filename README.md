# Masa

Masa is a cross-compilation wrapper and Darwin environment shim. It lets you
build macOS Mach-O binaries on a non-macOS host, without Xcode, by driving
`clang` and answering the platform queries that macOS build systems make.

Masa is licensed under the GNU General Public License v3.0 or later. See
[LICENSE](LICENSE).

## Status

Milestone 3 (v0.3.0): full Mach-O executable linking, the toolchain shims
(`ld`, `ar`, `ranlib`, `strip`, `nm`, `otool`) and `masa sdk`, on top of the
Milestone 2 `argv[0]` dispatch and `masa exec` environment runner, which sit on
the Milestone 1 `cc` wrapper and `sys` shims. The process-spawning layer is
isolated so Windows and Android backends can be added later.

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

## Usage

```
masa cc [clang-options...]    Compile or link for macOS with clang
masa sys <command> [args...]  Run a shimmed Darwin command
masa exec <command> [args...] Run a command inside a shimmed environment
masa init-shims <dir>         Create the argv[0] shim symlinks in <dir>
masa sdk info                 Report whether the configured SDK is usable
masa sdk init-stub <dir>      Write a clean-room stub SDK to <dir>
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

### `masa exec` and `masa init-shims`

`masa exec` makes a build system believe it is running on macOS. It creates a
directory of symlinks named after the emulated commands, prepends that
directory to `PATH` and runs your command:

```sh
masa exec make            # make sees Darwin, a macOS SDK and a working cc
masa exec ./configure
```

The symlinks point back at the Masa binary, so when `make` invokes `cc` (or
`uname`, `sw_vers`, `xcode-select`, `xcrun`, `ld`, `ar`, `ranlib`, `strip`,
`nm`, `otool`) it re-enters Masa through `argv[0]` dispatch and gets the
shimmed behaviour. `MASA_REAL_CC` is exported
first, recording the real host compiler, so the `cc` shim never calls itself
recursively.

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

Each tool is located with `clang -print-prog-name=<tool>`, then next to the
host compiler, then on `PATH`. `ld` additionally supplies a
`-platform_version`, because `ld64.lld` refuses to link without one.

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

## Layout

```
include/masa.h        public interface
src/main.c            CLI router and argv[0] dispatch
src/cc_wrapper.c      clang wrapper and argument injection
src/sys_shims.c       uname / sw_vers / xcode-select / xcrun shims
src/tool_shims.c      ld / ar / ranlib / strip / nm / otool shims
src/sdk.c             sdk info and the clean-room stub SDK writer
src/exec.c            init-shims, exec and the shim environment
src/platform_linux.c  process spawning (fork + execvp)
tests/test_masa.c     unit tests
```

## Testing

`make test` covers the argument builder, every shim, the CLI router,
`argv[0]` dispatch, `init-shims`, `exec`, `sdk`, the toolchain shims, and
compiler-recursion safety.

End-to-end tests produce real Mach-O files and check the header bytes: the
magic number (`CF FA ED FE`) and, for linked output, the file type
(`MH_EXECUTE`). One drives `masa cc` for both the `arm64` and `x86_64`
targets, another links through the toolchain shims, and a third runs
`masa exec make` on a generated project whose `$(CC)` resolves through
the shim. Tests that need `clang` or `make` skip themselves when the tool is
absent.
