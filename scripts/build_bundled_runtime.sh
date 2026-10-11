#!/bin/bash
#
# build_bundled_runtime.sh -- REL-008 stage 4: build the C runtime a release
# archive carries under lib/rt/<platform>/, which turfc's embedded linker
# (include/BundledLink.h) links a program against instead of running `cc`.
#
# What goes in the directory is the *contract* in include/BundledLink.h; this
# script is the one place that produces it for each platform, so CI and a
# developer build the same thing:
#
#   linux-x86_64, linux-arm64
#                  (built natively: run it on the architecture it is for)
#                  musl libc, built from a pinned, hash-verified source tarball
#                  (crt1.o crti.o crtn.o libc.a libm.a), gcc's libgcc.a, and
#                  libturfrt.a COMPILED AGAINST THAT MUSL (a glibc-built runtime
#                  cannot be linked into a program that carries musl)
#   windows-x86_64 the MinGW-w64 C runtime of the toolchain running this script
#                  (crt2.o, libmingw32.a, libmingwex.a, the UCRT import library,
#                  the Win32 import libraries, libgcc), taken from it, and
#                  libturfrt.a compiled with it. The toolchain must target UCRT
#                  (MSYS2's UCRT64 environment; present on Windows 10+), not the
#                  old msvcrt.dll. CC/AR override the compiler and archiver.
#   macos-arm64    a stub libSystem.tbd written by this script (an install name and
#                  one symbol; no Apple file is copied or generated from the SDK),
#                  and libturfrt.a compiled for this machine with its clang
#
# plus, in every directory, MANIFEST (what was built from what) and the
# licence texts the redistributed pieces require.
#
# Usage: scripts/build_bundled_runtime.sh --platform linux-x86_64 [--out DIR]
#            [--musl-tarball FILE] [--jobs N]
#   --platform       which runtime to build; default: this host's
#   --out            where it goes (default: <repo>/lib/rt/<platform>)
#   --musl-tarball   use this musl-<version>.tar.gz instead of downloading it
#                    (it is still checked against the pinned SHA-256)
#   --jobs           make parallelism (default: the machine's CPUs)
# Needs: a C compiler (gcc), make, tar, curl or wget (unless --musl-tarball),
#        sha256sum or shasum.

set -eu
CALLER=$(pwd)
abs() { case "$1" in /*|[A-Za-z]:*) printf '%s\n' "$1" ;; *) printf '%s\n' "$CALLER/$1" ;; esac; }
cd "$(dirname "$0")/.."
ROOT=$(pwd)

# ---- pinned inputs ----------------------------------------------------------
# musl 1.2.6. The SHA-256 is of the tarball whose detached PGP signature
# (musl-1.2.6.tar.gz.asc) was verified against musl's published key
# (https://musl.libc.org/musl.pub, fingerprint
# 8364 8929 0BB6 B70F 99FF DA05 56BC DB59 3020 450F) on 2026-10-07; CI checks
# the hash, not the signature, so a changed hash is a deliberate act.
MUSL_VERSION=1.2.6
MUSL_SHA256=d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a
MUSL_URL="https://musl.libc.org/releases/musl-$MUSL_VERSION.tar.gz"

PLATFORM=""
OUT=""
TARBALL=""
JOBS=""
while [ $# -gt 0 ]; do
  case "$1" in
    --platform) PLATFORM="${2:?--platform needs a name}"; shift 2 ;;
    --out) OUT=$(abs "${2:?--out needs a directory}"); shift 2 ;;
    --musl-tarball) TARBALL=$(abs "${2:?--musl-tarball needs a file}"); shift 2 ;;
    --jobs) JOBS="${2:?--jobs needs a number}"; shift 2 ;;
    -h|--help) sed -n '3,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "build_bundled_runtime.sh: unknown option '$1' (try --help)" >&2; exit 2 ;;
  esac
done
if [ -z "$PLATFORM" ]; then
  case "$(uname -s)/$(uname -m)" in
    Linux/x86_64) PLATFORM=linux-x86_64 ;;
    Linux/aarch64|Linux/arm64) PLATFORM=linux-arm64 ;;
    Darwin/arm64) PLATFORM=macos-arm64 ;;
    MINGW64*|MSYS*|CYGWIN*) PLATFORM=windows-x86_64 ;;
    *) echo "build_bundled_runtime.sh: no bundled runtime for $(uname -s)/$(uname -m); pass --platform" >&2; exit 2 ;;
  esac
fi
[ -n "$OUT" ] || OUT="$ROOT/lib/rt/$PLATFORM"
[ -n "$JOBS" ] || JOBS=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 2)

sha256_of() { { sha256sum "$1" 2>/dev/null || shasum -a 256 "$1"; } | awk '{print $1}'; }
fetch() { # url dest
  if command -v curl >/dev/null 2>&1; then curl -fsSL --retry 3 --retry-delay 5 -o "$2" "$1"
  elif command -v wget >/dev/null 2>&1; then wget -q -O "$2" "$1"
  else echo "build_bundled_runtime.sh: need curl or wget to download $1" >&2; return 1; fi
}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

build_linux() {
  # musl and libturfrt are built by the host's gcc for the host, so the
  # platform asked for has to be the machine this runs on.
  case "$PLATFORM/$(uname -m)" in
    linux-x86_64/x86_64|linux-arm64/aarch64|linux-arm64/arm64) ;;
    *) echo "build_bundled_runtime.sh: $PLATFORM is built natively, and this machine is $(uname -m)" >&2; exit 1 ;;
  esac
  command -v gcc >/dev/null 2>&1 || { echo "build_bundled_runtime.sh: gcc is required" >&2; exit 1; }

  # musl: pinned, verified, built static-only into a private prefix.
  if [ -z "$TARBALL" ]; then
    TARBALL="$WORK/musl-$MUSL_VERSION.tar.gz"
    echo "== downloading $MUSL_URL"
    fetch "$MUSL_URL" "$TARBALL"
  fi
  GOT=$(sha256_of "$TARBALL")
  if [ "$GOT" != "$MUSL_SHA256" ]; then
    echo "build_bundled_runtime.sh: musl-$MUSL_VERSION.tar.gz has SHA-256 $GOT, expected $MUSL_SHA256. Refusing to build from it." >&2
    exit 1
  fi
  echo "== building musl $MUSL_VERSION"
  tar -xzf "$TARBALL" -C "$WORK"
  PREFIX="$WORK/musl-out"
  (
    cd "$WORK/musl-$MUSL_VERSION"
    ./configure --prefix="$PREFIX" --disable-shared > "$WORK/musl-configure.log" 2>&1 \
      || { tail -20 "$WORK/musl-configure.log" >&2; exit 1; }
    make -j"$JOBS" > "$WORK/musl-make.log" 2>&1 || { tail -20 "$WORK/musl-make.log" >&2; exit 1; }
    make install > "$WORK/musl-install.log" 2>&1 || { tail -20 "$WORK/musl-install.log" >&2; exit 1; }
  )

  # libturfrt, compiled against that musl: musl-gcc is musl's own gcc wrapper
  # (its specs: musl's headers, no glibc). The same sources and flags as the
  # `turfrt` CMake target (C11, -O3, -DNDEBUG), nothing else.
  echo "== compiling libturfrt against musl"
  build_turfrt "$PREFIX/bin/musl-gcc" ar

  # The compiler's own runtime support (division helpers and the like, which the
  # code generator and libturfrt use). libgcc.a is GPL-3.0 with the GCC Runtime
  # Library Exception, which permits linking it into a program under any licence.
  LIBGCC=$(gcc -print-libgcc-file-name)
  [ -f "$LIBGCC" ] || { echo "build_bundled_runtime.sh: gcc has no libgcc.a ($LIBGCC)" >&2; exit 1; }

  rm -rf "$OUT"
  mkdir -p "$OUT"
  cp "$PREFIX"/lib/crt1.o "$PREFIX"/lib/crti.o "$PREFIX"/lib/crtn.o \
     "$PREFIX"/lib/libc.a "$PREFIX"/lib/libm.a "$OUT/"
  cp "$LIBGCC" "$OUT/libgcc.a"
  cp "$WORK/rt/libturfrt.a" "$OUT/"
  cp "$WORK/musl-$MUSL_VERSION/COPYRIGHT" "$OUT/COPYRIGHT.musl"
  {
    echo "TurfLang bundled C runtime, $PLATFORM (scripts/build_bundled_runtime.sh)"
    echo "musl        $MUSL_VERSION  sha256 $MUSL_SHA256  (MIT; see COPYRIGHT.musl)"
    echo "libgcc.a    from $(gcc --version | head -n 1) (GPL-3.0 with the GCC Runtime Library Exception)"
    echo "libturfrt.a built from this repository's runtime/*.c against the musl above"
    echo "linking     static, by the embedded lld (include/BundledLink.h)"
  } > "$OUT/MANIFEST"
}

# The same sources and flags as the `turfrt` CMake target (C11, -O3, -DNDEBUG),
# compiled with whatever compiler `$1` names, into $WORK/rt/libturfrt.a.
build_turfrt() { # compiler archiver [extra flags...]
  local cc="$1" ar="$2"; shift 2
  mkdir -p "$WORK/rt"
  for src in "$ROOT"/runtime/*.c; do
    "$cc" -std=gnu11 -O3 -DNDEBUG -Wall -Wextra -I"$ROOT/runtime" "$@" \
      -c "$src" -o "$WORK/rt/$(basename "${src%.c}").o"
  done
  "$ar" rcs "$WORK/rt/libturfrt.a" "$WORK"/rt/*.o
}

build_windows_x86_64() {
  local cc="${CC:-gcc}" ar="${AR:-ar}"
  command -v "$cc" >/dev/null 2>&1 || { echo "build_bundled_runtime.sh: $cc not found (run this in MSYS2's UCRT64 shell, or set CC)" >&2; exit 1; }
  # UCRT, not msvcrt: the compiler reports its C runtime in its target macros.
  if ! echo '#include <stdio.h>
#ifndef _UCRT
#error not UCRT
#endif' | "$cc" -x c -E - > /dev/null 2>&1; then
    echo "build_bundled_runtime.sh: $cc does not target UCRT (_UCRT is not defined); a program linked against its CRT would need msvcrt.dll. Use MSYS2's UCRT64 environment." >&2
    exit 1
  fi
  build_turfrt "$cc" "$ar"

  rm -rf "$OUT"; mkdir -p "$OUT"
  local missing=""
  for f in crt2.o libmingw32.a libmingwex.a libucrt.a libkernel32.a libgcc.a; do
    p=$("$cc" -print-file-name="$f")
    if [ -f "$p" ]; then cp "$p" "$OUT/$f"; else missing="$missing $f"; fi
  done
  [ -z "$missing" ] || { echo "build_bundled_runtime.sh: the toolchain has no:$missing" >&2; exit 1; }
  # Optional: used when present.
  for f in crtbegin.o crtend.o libgcc_eh.a libmoldname.a libadvapi32.a libshell32.a libuser32.a libws2_32.a; do
    p=$("$cc" -print-file-name="$f")
    [ -f "$p" ] && cp "$p" "$OUT/$f"
  done
  cp "$WORK/rt/libturfrt.a" "$OUT/"
  {
    echo "TurfLang bundled C runtime, windows-x86_64 (scripts/build_bundled_runtime.sh)"
    echo "toolchain   $("$cc" --version | head -n 1), target $("$cc" -dumpmachine), C runtime UCRT"
    echo "crt/libs    mingw-w64 (ZPL-2.1 / public domain, with the runtime exception text in its COPYING.MinGW-w64-runtime); import libraries for the Windows 10+ system DLLs"
    echo "libgcc.a    GPL-3.0 with the GCC Runtime Library Exception"
    echo "libturfrt.a built from this repository's runtime/*.c with the toolchain above"
    echo "linking     by the embedded lld's MinGW driver (include/BundledLink.h)"
  } > "$OUT/MANIFEST"
}

build_macos_arm64() {
  command -v clang >/dev/null 2>&1 || { echo "build_bundled_runtime.sh: clang is required" >&2; exit 1; }
  build_turfrt clang ar -arch arm64 -mmacosx-version-min=11.0

  rm -rf "$OUT"; mkdir -p "$OUT"
  # A stub libSystem, written here, not copied from Apple's SDK: it makes the
  # linked executable load /usr/lib/libSystem.B.dylib (that is what the install
  # name does), and exports one symbol the linker looks for. The symbols
  # libSystem provides are bound by name when the program starts
  # (-undefined dynamic_lookup in src/BundledLink.cpp), so no list of them is
  # needed. A tbd is an ordinary text file; this one is ours.
  cat > "$OUT/libSystem.tbd" <<'TBD'
--- !tapi-tbd
tbd-version:     4
targets:         [ arm64-macos ]
install-name:    '/usr/lib/libSystem.B.dylib'
exports:
  - targets:         [ arm64-macos ]
    symbols:         [ dyld_stub_binder ]
...
TBD
  cp "$WORK/rt/libturfrt.a" "$OUT/"
  {
    echo "TurfLang bundled C runtime, macos-arm64 (scripts/build_bundled_runtime.sh)"
    echo "libSystem.tbd  a stub written by this script (install name /usr/lib/libSystem.B.dylib); no Apple file"
    echo "libturfrt.a    built from this repository's runtime/*.c with $(clang --version | head -n 1), -mmacosx-version-min=11.0"
    echo "linking        by the embedded lld's Mach-O driver, -undefined dynamic_lookup (include/BundledLink.h)"
  } > "$OUT/MANIFEST"
}

case "$PLATFORM" in
  linux-x86_64|linux-arm64) build_linux ;;
  windows-x86_64) build_windows_x86_64 ;;
  macos-arm64) build_macos_arm64 ;;
  *) echo "build_bundled_runtime.sh: the $PLATFORM runtime is not implemented here yet" >&2; exit 2 ;;
esac

echo "== bundled runtime for $PLATFORM:"
ls -l "$OUT" | sed 's/^/   /'
echo "$OUT"
