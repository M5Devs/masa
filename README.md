# Masa

Masa is a cross-compilation wrapper and Darwin environment shim. It lets you
build macOS Mach-O binaries on a non-macOS host, without Xcode, by driving
`clang` and answering the platform queries that macOS build systems make.

Masa is licensed under the GNU General Public License v3.0 or later. See
[LICENSE](LICENSE).

## Status

Milestone 1: the `cc` wrapper and the `sys` environment shims for a Linux
host. The process-spawning layer is isolated so Windows and Android backends
can be added later.

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

## Layout

```
include/masa.h        public interface
src/main.c            CLI router (cc, sys, version)
src/cc_wrapper.c      clang wrapper and argument injection
src/sys_shims.c       uname / sw_vers / xcode-select / xcrun shims
src/platform_linux.c  process spawning (fork + execvp)
tests/test_masa.c     unit tests
```

## Testing

`make test` covers the argument builder, every shim, the CLI router, and an
end-to-end compile. When `clang` is available the end-to-end test compiles a
minimal `hello.c` with `-c` and checks that the output begins with the 64-bit
little-endian Mach-O magic number (`CF FA ED FE`) for both the `arm64` and
`x86_64` targets.
