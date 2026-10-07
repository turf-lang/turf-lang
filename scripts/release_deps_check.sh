#!/bin/bash
#
# release_deps_check.sh -- REL-008 stage 1: what does a release archive need
# from the machine it is unpacked on?
#
# The 2.0.0 promise is that `turf new hello && cd hello && turf run` works with
# nothing installed. The first thing that can break it is a binary in the
# archive that is linked against a library the user does not have (LLVM, libstdc++,
# a MinGW DLL). This reads every executable (and, on Windows, every DLL) in the
# archive's bin/ and compares what it asks the loader for against a per-platform
# allowlist of libraries the operating system itself provides. Anything else is
# listed and the script exits 1.
#
# It reads the binaries' headers (`readelf -d`, `objdump -p`, `otool -L`); it
# never runs them, so it also works on an archive built for another machine.
#
# THIS CHECK FAILS TODAY, BY DESIGN (ROADMAP_v2.0.0.md, REL-008 stage 1). The
# release `turfc` links LLVM dynamically (stage 2 fixes that), links programs
# through the system `cc` (stage 3) and a MinGW/msvcrt runtime (stage 4). Each
# stage should remove lines from the report; the release is self-contained
# when the report is empty. Do not widen an allowlist to make a stage's work
# disappear: a library belongs on a list only if every machine the archive is
# for ships it.
#
# Allowlists (direct dependencies only; a system library's own dependencies are
# the system's business):
#   linux    libc.so.6 libm.so.6 the loader (ld-linux-*), and glibc's other
#            split-out parts libdl, libpthread, librt, libutil
#   macos    /usr/lib/libSystem.B.dylib, and libc++ / libc++abi. The roadmap
#            lists libSystem only; libc++ is added because it ships with the
#            OS (in the dyld shared cache), so a static libstdc++ equivalent is
#            not needed for it. Remove those two lines if the owner disagrees.
#            libresolv is added for the Go front end (turf), which cannot drop it;
#            libz and libncurses for a static LLVM (zlib and terminfo are the OS's).
#   windows  KERNEL32 KERNELBASE NTDLL UCRTBASE api-ms-win-* WS2_32 ADVAPI32,
#            plus ole32 shell32 bcrypt version psapi (what a static LLVM still
#            imports). msvcrt.dll is NOT allowed: the target is UCRT (Windows
#            10+), and MinGW64's default C runtime is msvcrt. Any DLL shipped in
#            bin/ is reported as bundled, whatever it imports.
#
# Usage: scripts/release_deps_check.sh --archive FILE [--platform NAME]
#   --archive FILE   a .tar.gz or .zip made by scripts/package_release.sh
#   --platform NAME  linux-*, macos-*, windows-*; default: taken from the
#                    archive's file name (turf-v<ver>-<platform>.<ext>)
# Exit: 0 = every dependency is on the list; 1 = something is not; 2 = usage or
#       the archive could not be read.

set -u

ARCHIVE=""
PLATFORM=""
while [ $# -gt 0 ]; do
  case "$1" in
    --archive) ARCHIVE="${2:?--archive needs a file}"; shift 2 ;;
    --platform) PLATFORM="${2:?--platform needs a name}"; shift 2 ;;
    -h|--help) sed -n '3,45p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "release_deps_check.sh: unknown option '$1' (try --help)" >&2; exit 2 ;;
  esac
done
[ -n "$ARCHIVE" ] || { echo "release_deps_check.sh: --archive is required" >&2; exit 2; }
[ -f "$ARCHIVE" ] || { echo "release_deps_check.sh: $ARCHIVE not found" >&2; exit 2; }

if [ -z "$PLATFORM" ]; then
  base=$(basename "$ARCHIVE")
  base=${base%.tar.gz}; base=${base%.zip}
  PLATFORM=""
  # "turf-v2.0.0-rc1-linux-arch-x86_64" -> "linux-arch-x86_64"
  case "$base" in
    *-linux-*)   PLATFORM="linux-${base#*-linux-}" ;;
    *-macos-*)   PLATFORM="macos-${base#*-macos-}" ;;
    *-windows-*) PLATFORM="windows-${base#*-windows-}" ;;
  esac
fi
case "$PLATFORM" in
  linux-*|macos-*|windows-*) ;;
  *) echo "release_deps_check.sh: cannot tell the platform from '$PLATFORM'; pass --platform" >&2; exit 2 ;;
esac

# Absolute, so it survives the cd into the scratch directory.
case "$ARCHIVE" in /*|[A-Za-z]:*) ;; *) ARCHIVE="$(pwd)/$ARCHIVE" ;; esac
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
case "$ARCHIVE" in
  *.zip)
    if command -v unzip >/dev/null 2>&1; then (cd "$WORK" && unzip -q "$ARCHIVE")
    elif command -v 7z >/dev/null 2>&1; then (cd "$WORK" && 7z x -y "$ARCHIVE" >/dev/null)
    else echo "release_deps_check.sh: need unzip or 7z to open a .zip" >&2; exit 2; fi ;;
  *) (cd "$WORK" && tar -xzf "$ARCHIVE") ;;
esac || { echo "release_deps_check.sh: could not unpack $ARCHIVE" >&2; exit 2; }

BIN=$(find "$WORK" -maxdepth 2 -type d -name bin | head -n 1)
[ -n "$BIN" ] || { echo "release_deps_check.sh: no bin/ in $ARCHIVE" >&2; exit 2; }

# ---- per-platform: list a file's direct dependencies, one per line ----------
deps_of() {
  case "$PLATFORM" in
    linux-*)
      # `NEEDED` entries of the dynamic section. A static binary has none.
      if command -v readelf >/dev/null 2>&1; then
        readelf -d "$1" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\].*/\1/p'
      else
        objdump -p "$1" 2>/dev/null | sed -n 's/^ *NEEDED *//p'
      fi ;;
    macos-*)
      # First line is the file's own name; the rest are `<path> (compatibility ...)`.
      otool -L "$1" 2>/dev/null | sed '1d' | sed 's/^[[:space:]]*//; s/ (.*$//' ;;
    windows-*)
      # llvm-readobj first: GNU objdump cannot read the import directory of a
      # Go-built PE (turf.exe's lands in a section it calls .zdebug_info and it
      # prints no DLL names; found on the first real Windows run). The MSYS2
      # LLVM package that builds turfc.exe provides llvm-readobj.
      if command -v llvm-readobj >/dev/null 2>&1; then
        llvm-readobj --coff-imports "$1" 2>/dev/null | sed -n 's/^[[:space:]]*Name: *\(.*\.[dD][lL][lL]\)[[:space:]]*$/\1/p'
      else
        objdump -p "$1" 2>/dev/null | sed -n 's/^[[:space:]]*DLL Name: *//p'
      fi ;;
  esac
}

allowed() {
  local d="$1"
  case "$PLATFORM" in
    linux-*)
      case "$d" in
        libc.so.6|libm.so.6|libdl.so.2|libpthread.so.0|librt.so.1|libutil.so.1) return 0 ;;
        ld-linux-*.so.*|ld64.so.*|ld-linux.so.*) return 0 ;;
      esac ;;
    macos-*)
      case "$d" in
        /usr/lib/libSystem.B.dylib|/usr/lib/libc++.1.dylib|/usr/lib/libc++abi.dylib) return 0 ;;
        # Go's net package on darwin: the front end (turf) imports it.
        /usr/lib/libresolv.9.dylib) return 0 ;;
        # What a static LLVM links on macOS: zlib and terminfo, both part of the OS.
        /usr/lib/libz.1.dylib|/usr/lib/libncurses.5.4.dylib) return 0 ;;
      esac ;;
    windows-*)
      local l
      l=$(printf '%s' "$d" | tr '[:upper:]' '[:lower:]')
      case "$l" in
        kernel32.dll|kernelbase.dll|ntdll.dll|ucrtbase.dll|ws2_32.dll|advapi32.dll|api-ms-win-*.dll) return 0 ;;
        # What a static LLVM still imports on Windows (ole32, shell32 and
        # bcrypt were seen in the first real run, imported by libLLVM-22.dll
        # and libxml2; version and psapi are LLVM's own and not yet seen).
        # All ship with every Windows, so no stage of REL-008 removes them.
        ole32.dll|shell32.dll|bcrypt.dll|version.dll|psapi.dll) return 0 ;;
      esac ;;
  esac
  return 1
}

TOOL_CHECK=""
case "$PLATFORM" in
  linux-*)   command -v readelf >/dev/null 2>&1 || command -v objdump >/dev/null 2>&1 || TOOL_CHECK="readelf or objdump" ;;
  macos-*)   command -v otool >/dev/null 2>&1 || TOOL_CHECK="otool" ;;
  windows-*) command -v llvm-readobj >/dev/null 2>&1 || command -v objdump >/dev/null 2>&1 || TOOL_CHECK="llvm-readobj or objdump" ;;
esac
[ -z "$TOOL_CHECK" ] || { echo "release_deps_check.sh: $TOOL_CHECK not found; cannot inspect a $PLATFORM archive here" >&2; exit 2; }

echo "== $(basename "$ARCHIVE") ($PLATFORM) =="
BAD=0
CHECKED=0
# bin/ holds turf and turfc; on Windows also whatever DLLs were bundled.
while IFS= read -r f; do
  name=$(basename "$f")
  case "$PLATFORM" in
    windows-*) case "$name" in *.exe|*.dll) ;; *) continue ;; esac ;;
  esac
  CHECKED=$((CHECKED + 1))
  deps=$(deps_of "$f")
  printf '%s\n' "bin/$name"
  case "$PLATFORM:$name" in
    windows-*:*.dll)
      echo "    NOT ALLOWED  (this DLL is bundled in bin/: a self-contained archive ships none)"
      BAD=$((BAD + 1)) ;;
  esac
  if [ -z "$deps" ]; then
    case "$PLATFORM" in
      linux-*) echo "    (no dynamic dependencies)"; continue ;;
      *)
        # Every Windows and macOS executable imports at least the system
        # library; an empty answer means the tool could not read the file, and
        # that must not look like a pass.
        echo "    NOT ALLOWED  (the dependency list could not be read)"
        BAD=$((BAD + 1)); continue ;;
    esac
  fi
  while IFS= read -r d; do
    [ -n "$d" ] || continue
    if allowed "$d"; then
      echo "    ok           $d"
    else
      echo "    NOT ALLOWED  $d"
      BAD=$((BAD + 1))
    fi
  done <<< "$deps"
done < <(find "$BIN" -maxdepth 1 -type f | sort)

if [ "$CHECKED" -eq 0 ]; then
  echo "release_deps_check.sh: found nothing to inspect in bin/ of $ARCHIVE" >&2
  exit 2
fi
if [ "$BAD" -ne 0 ]; then
  echo "FAIL: $BAD dependenc$([ "$BAD" -eq 1 ] && echo y || echo ies) outside the $PLATFORM allowlist; this archive needs more than the operating system provides."
  exit 1
fi
echo "PASS: every dependency of $CHECKED file(s) is on the $PLATFORM allowlist."
