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
#   linux-x86_64   musl libc, built from a pinned, hash-verified source tarball
#                  (crt1.o crti.o crtn.o libc.a libm.a), gcc's libgcc.a, and
#                  libturfrt.a COMPILED AGAINST THAT MUSL (a glibc-built runtime
#                  cannot be linked into a program that carries musl)
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

build_linux_x86_64() {
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
  mkdir -p "$WORK/rt"
  for src in "$ROOT"/runtime/*.c; do
    "$PREFIX/bin/musl-gcc" -std=gnu11 -O3 -DNDEBUG -Wall -Wextra -I"$ROOT/runtime" \
      -c "$src" -o "$WORK/rt/$(basename "${src%.c}").o"
  done
  ar rcs "$WORK/rt/libturfrt.a" "$WORK"/rt/*.o

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
    echo "TurfLang bundled C runtime, linux-x86_64 (scripts/build_bundled_runtime.sh)"
    echo "musl        $MUSL_VERSION  sha256 $MUSL_SHA256  (MIT; see COPYRIGHT.musl)"
    echo "libgcc.a    from $(gcc --version | head -n 1) (GPL-3.0 with the GCC Runtime Library Exception)"
    echo "libturfrt.a built from this repository's runtime/*.c against the musl above"
    echo "linking     static, by the embedded lld (include/BundledLink.h)"
  } > "$OUT/MANIFEST"
}

case "$PLATFORM" in
  linux-x86_64) build_linux_x86_64 ;;
  *) echo "build_bundled_runtime.sh: the $PLATFORM runtime is not implemented here yet" >&2; exit 2 ;;
esac

echo "== bundled runtime for $PLATFORM:"
ls -l "$OUT" | sed 's/^/   /'
echo "$OUT"
