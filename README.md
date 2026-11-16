# Masa

Masa is a cross-compilation wrapper and Darwin environment shim. It lets you
build macOS Mach-O binaries on a non-macOS host, without Xcode, by driving
`clang` and answering the platform queries that macOS build systems make.

Masa is licensed under the GNU General Public License v3.0 or later. See
[LICENSE](LICENSE).

## Status

Milestone 2: BusyBox-style `argv[0]` dispatch and the `masa exec`
environment runner on top of the Milestone 1 `cc` wrapper and `sys` shims.
The process-spawning layer is isolated so Windows and Android backends can be
added later.

## Requirements

- A C11 compiler (`gcc` or `clang`) and `make`.
- `clang` (and optionally `lld`) for actually producing Mach-O output.
  An SDK is not required to produce object files; it is required to link.

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
masa version                  Print the Masa version
masa help                     Show the usage message
```

### `masa cc`

`masa cc` forwards its arguments to `clang` and injects:

- `-target <MASA_TARGET>` (default `arm64-apple-darwin`)
- `-fuse-ld=lld`
- `-isysroot <MASA_SDK_PATH>` when `MASA_SDK_PATH` is set

```sh
# Inspect the command without running it.
MASA_DRY_RUN=1 masa cc -c hello.c -o hello.o
# clang -c hello.c -o hello.o -target arm64-apple-darwin -fuse-ld=lld

# Compile a Mach-O object for Intel.
MASA_TARGET=x86_64-apple-darwin masa cc -c hello.c -o hello.o
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
`uname`, `sw_vers`, `xcode-select`, `xcrun`) it re-enters Masa through
`argv[0]` dispatch and gets the shimmed behaviour. `MASA_REAL_CC` is exported
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

Any other name falls through to the normal `masa <subcommand>` router.

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
src/exec.c            init-shims, exec and the shim environment
src/platform_linux.c  process spawning (fork + execvp)
tests/test_masa.c     unit tests
```

## Testing

`make test` covers the argument builder, every shim, the CLI router,
`argv[0]` dispatch, `init-shims`, `exec`, and compiler-recursion safety.

Two end-to-end tests compile real Mach-O objects and check that the output
begins with the 64-bit little-endian Mach-O magic number (`CF FA ED FE`): one
drives `masa cc` directly for both the `arm64` and `x86_64` targets, the other
runs `masa exec make` on a generated project whose `$(CC)` resolves through
the shim. Tests that need `clang` or `make` skip themselves when the tool is
absent.
