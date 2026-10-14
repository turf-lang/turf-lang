#!/bin/bash
#
# build_static_lld.sh -- REL-008 stage 4: lld as STATIC archives, built from
# LLVM's own source, for a platform whose package manager only offers shared ones.
#
# turfc embeds lld (src/codegen/BundledLink.h), which has to be static or the archive
# would need the machine to have it. Debian/Ubuntu (liblld-22) and MSYS2 (lld)
# ship liblld*.a. **Homebrew's lld@22 ships only liblld*.dylib** (its bottle was
# unpacked to check), and the first macOS CI run, which linked them, failed the
# dependency check with `/opt/homebrew/opt/lld@22/lib/liblldMachO.dylib` and
# `liblldCommon.dylib`. This builds the same lld, static, against the LLVM
# already installed (which must match its version), into a private prefix to
# pass to CMake as -DLLD_DIR=<prefix>/lib/cmake/lld.
#
# The source is llvm-project-<version>.src.tar.xz from LLVM's GitHub release,
# pinned by SHA-256 (checked every time). The pinned file is the one whose
# sigstore attestation was verified against llvm/llvm-project with
# `gh attestation verify ... --bundle <the release's .jsonl>` on 2026-10-07 (a
# tampered copy was checked to fail). Only lld/, cmake/ and libunwind/include are unpacked.
#
# Usage: scripts/release/build_static_lld.sh --llvm-dir DIR --prefix DIR
#            [--tarball FILE] [--jobs N] [--tblgen FILE]
#   --llvm-dir  the directory holding LLVMConfig.cmake of the installed LLVM
#               (for Homebrew: $(brew --prefix llvm@22)/lib/cmake/llvm)
#   --prefix    where to install liblld*.a, lld's headers and its CMake config
#   --tarball   use this llvm-project-22.1.8.src.tar.xz instead of downloading it
#   --jobs      build parallelism (default: the machine's CPUs)
#   --tblgen    an llvm-tblgen to run for lld's option tables, if the installed
#               LLVM's own cannot run on this machine (it must be the same version)
# Needs: cmake, a C++ compiler, tar with xz, curl or wget (unless --tarball).

set -eu
CALLER=$(pwd)
abs() { case "$1" in /*|[A-Za-z]:*) printf '%s\n' "$1" ;; *) printf '%s\n' "$CALLER/$1" ;; esac; }

LLVM_VERSION=22.1.8
LLVM_SHA256=922f1817a0df7b1489272d18134ee0087a8b068828f87ac63b9861b1a9965888
LLVM_URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VERSION/llvm-project-$LLVM_VERSION.src.tar.xz"

LLVM_DIR=""; PREFIX=""; TARBALL=""; JOBS=""; TBLGEN=""
while [ $# -gt 0 ]; do
  case "$1" in
    --llvm-dir) LLVM_DIR=$(abs "${2:?--llvm-dir needs a directory}"); shift 2 ;;
    --prefix) PREFIX=$(abs "${2:?--prefix needs a directory}"); shift 2 ;;
    --tarball) TARBALL=$(abs "${2:?--tarball needs a file}"); shift 2 ;;
    --jobs) JOBS="${2:?--jobs needs a number}"; shift 2 ;;
    --tblgen) TBLGEN=$(abs "${2:?--tblgen needs a file}"); shift 2 ;;
    -h|--help) sed -n '3,36p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "build_static_lld.sh: unknown option '$1' (try --help)" >&2; exit 2 ;;
  esac
done
[ -n "$LLVM_DIR" ] && [ -n "$PREFIX" ] || { echo "build_static_lld.sh: --llvm-dir and --prefix are required (try --help)" >&2; exit 2; }
[ -f "$LLVM_DIR/LLVMConfig.cmake" ] || { echo "build_static_lld.sh: no LLVMConfig.cmake in $LLVM_DIR" >&2; exit 2; }
[ -n "$JOBS" ] || JOBS=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 2)

# The installed LLVM must be the version lld is built from: lld uses LLVM's
# internal headers and symbols.
HAVE=$(sed -n 's/^set(PACKAGE_VERSION "\(.*\)")/\1/p' "$LLVM_DIR/LLVMConfigVersion.cmake" 2>/dev/null | head -n 1)
case "$HAVE" in
  "$LLVM_VERSION"*) ;;
  *) echo "build_static_lld.sh: the installed LLVM is '${HAVE:-unknown}', but this builds lld $LLVM_VERSION; they must match." >&2; exit 1 ;;
esac

sha256_of() { { sha256sum "$1" 2>/dev/null || shasum -a 256 "$1"; } | awk '{print $1}'; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

if [ -z "$TARBALL" ]; then
  TARBALL="$WORK/llvm-project-$LLVM_VERSION.src.tar.xz"
  echo "== downloading $LLVM_URL"
  if command -v curl >/dev/null 2>&1; then curl -fsSL --retry 3 --retry-delay 5 -o "$TARBALL" "$LLVM_URL"
  else wget -q -O "$TARBALL" "$LLVM_URL"; fi
fi
GOT=$(sha256_of "$TARBALL")
if [ "$GOT" != "$LLVM_SHA256" ]; then
  echo "build_static_lld.sh: $(basename "$TARBALL") has SHA-256 $GOT, expected $LLVM_SHA256. Refusing to build from it." >&2
  exit 1
fi

# libunwind/include: lld's Mach-O sources include <mach-o/compact_unwind_encoding.h>,
# which macOS has in its SDK and other hosts get from there (found by building lld
# on Linux, where it was missing).
echo "== unpacking lld, cmake and libunwind/include"
tar -xJf "$TARBALL" -C "$WORK" "llvm-project-$LLVM_VERSION.src/lld" "llvm-project-$LLVM_VERSION.src/cmake" "llvm-project-$LLVM_VERSION.src/libunwind/include"
SRC="$WORK/llvm-project-$LLVM_VERSION.src"
# lld's Mach-O CMake adds ${LLVM_MAIN_SRC_DIR}/../libunwind/include, and a
# standalone build defaults LLVM_MAIN_SRC_DIR to <src>/llvm. That directory is not
# unpacked (the installed LLVM stands in for it), but `..` cannot be resolved
# through a directory that does not exist, so make an empty one.
mkdir -p "$SRC/llvm"

echo "== configuring lld $LLVM_VERSION (static) against $LLVM_DIR"
cmake -S "$SRC/lld" -B "$WORK/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR="$LLVM_DIR" \
  -DLLVM_COMMON_CMAKE_UTILS="$SRC/cmake" \
  ${TBLGEN:+-DLLVM_TABLEGEN_EXE="$TBLGEN"} \
  -DBUILD_SHARED_LIBS=OFF \
  -DLLD_BUILD_TOOLS=OFF \
  -DLLVM_INCLUDE_TESTS=OFF -DLLD_INCLUDE_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" > "$WORK/configure.log" 2>&1 \
  || { tail -30 "$WORK/configure.log" >&2; exit 1; }

echo "== building (-j$JOBS)"
cmake --build "$WORK/build" -j"$JOBS" > "$WORK/build.log" 2>&1 \
  || { grep -E "error|Error|undefined" "$WORK/build.log" | head -20 >&2; tail -10 "$WORK/build.log" >&2; exit 1; }
cmake --install "$WORK/build" > "$WORK/install.log" 2>&1 || { tail -30 "$WORK/install.log" >&2; exit 1; }

echo "== installed:"
ls "$PREFIX"/lib/liblld*.a | sed 's/^/   /'
echo "$PREFIX/lib/cmake/lld"
