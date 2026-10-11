# REL-008 — verifying macOS arm64 on a real Mac

Written 2026-10-11. This is the checklist for the one machine the project has not
yet had: an Apple-silicon Mac. It closes REL-008 stage 6 (the macOS port) and the
macOS half of stage 8 (the clean-machine proof). Every step says what a pass looks
like. **Write down every result, pass or fail, in `docs/development/changes/REL-008.md`**
(a "Stage 6" section, like Stage 5's); a step that was skipped is recorded as skipped.

What is already proven, so you know what is NOT in question: on the macOS CI runner
(run 37656632136, `d5f7ebb`) the build, the stub `libSystem.tbd`, and the link of a
Turf object to a valid Mach-O all worked. **What nobody has ever done: run a compiled
Turf program on macOS from this tree, run the test suite there, or run the GC there.**

**Step 0, before touching a Mac: read the CI run.** The macOS job of
`build-archives.yml` now runs the suite, the GC directory under stress and validate, and
fifteen of the smoke scripts (section 5's list, minus `bundled_link_smoke`), all
informational (`continue-on-error`) and uploaded as the `macos-suite-logs` artifact. The
first push after the Actions minutes return is the first time any of it executes on
macOS. Read those logs first: a failure there that matches section 7 tells you where to
look, and what passes there needs no hand run of sections 3 to 5 (record the CI run id
instead). Sections 2 and 6 cannot be done by CI (6 needs the toolchain removed), so
those are by hand either way. When the logs are green, make the macOS steps enforcing:
delete `continue-on-error` from them in `build-archives.yml`.

You need: an Apple-silicon Mac on macOS 11 or later, admin rights for one step
(section 6), Xcode Command Line Tools (to BUILD only; section 6 removes them from the
picture), Homebrew, `git`, ~15 GB free disk and ~30 minutes of build time.

## 1. Build exactly as CI does

```sh
git clone <repo> turflang && cd turflang && git checkout <the pushed branch or tag>
brew install llvm@22 cmake zstd go
export LLVM_PREFIX="$(brew --prefix llvm@22)"
export PATH="$LLVM_PREFIX/bin:$PATH"
llvm-config --version                      # must print 22.x. 23 does not build.

# lld as STATIC archives (Homebrew ships only dylibs); ~10 minutes
./scripts/build_static_lld.sh --llvm-dir "$LLVM_PREFIX/lib/cmake/llvm" --prefix "$HOME/turf-lld"
export CMAKE_PREFIX_PATH="$LLVM_PREFIX:$HOME/turf-lld"

./scripts/cmake_build.sh --release-static --embed-lld
./scripts/build_bundled_runtime.sh --platform macos-arm64
```

Pass: both scripts exit 0; `build/turfc` and `build/turf` exist;
`lib/rt/macos-arm64/` holds `libSystem.tbd`, `libturfrt.a`.

```sh
./build/turfc --version
```

Pass: lines `llvm 22.x`, `linker embedded-lld`, `bundled-runtime <path>/lib/rt/macos-arm64`.
If it says `linker system`, the embedded lld was not built in: stop and fix that first.

## 2. The first execution of a Turf program on macOS

```sh
mkdir -p /tmp/h && cd /tmp/h
printf 'import io;\nfn main(): int { io::printline("hello"); return 0; }\n' > hello.tr
env -u TURF_HOME "$OLDPWD/build/turfc" hello.tr -o hello && ./hello; echo "exit=$?"
otool -L hello
codesign -dv hello 2>&1 | head -3
sw_vers
```

Pass: prints `hello`, exit 0; `otool -L` lists ONLY `/usr/lib/libSystem.B.dylib`;
`codesign` shows an ad-hoc signature. **This is the dyld question the stage 4 record
left open**: the stub links with `-undefined dynamic_lookup`, so if the Mac refuses to
run the binary (`dyld: symbol not found`, or killed with `Code Signature Invalid`,
exit 137), that is the finding. Record `sw_vers` with it. Do not proceed past a failure
here; paste the exact error into the record.

## 3. The test suite, at both optimisation levels

Run from the repo root. `env -u TURF_HOME` keeps your shell's override out.

```sh
env -u TURF_HOME ./build/turfc --test tests --jobs 4 \
  --diag-register tests/DIAGNOSTIC_GAPS.txt --exclude custom --exclude 99_known_bugs
env -u TURF_HOME ./build/turfc --test tests --jobs 4 -O0 \
  --diag-register tests/DIAGNOSTIC_GAPS.txt --exclude custom --exclude 99_known_bugs
```

Pass: `Failed: 0` on both (currently 1191 tests on Linux and Windows). For every
failure, `Failed Tests Details` names the file; **look at the reason before changing
anything** (a test that fails for a different reason than the one it pins is not a
pass; AGENTS.md). Expected hot spots, from the roadmap's stage 6 list. For each one
run `./build/turfc --test <that dir>` on its own:

| Area | Directory / test | What it exercises |
|---|---|---|
| `mem` | `tests/05_arrays_pointers_ffi/17_mem_pool_and_aligned_alloc.tr` | `malloc_usable_size` -> `@ffi(macos: malloc_size)` forwarding |
| `net` | the `net` tests under `tests/` (`grep -l "import net" -r tests`) | `sin_len` in `sockaddr_in` (`turfrt_net.c`, the `__APPLE__` branch), `SO_REUSEADDR` |
| GC | `tests/41_gc` | AAPCS64 register capture (`x19-x28`, `fp`, `d8-d15`), `pthread_get_stackaddr_np` |
| stack overflow | `tests/01_syntax/fail_deeply_nested_expression.tr`, the `EXPECT-RUNTIME` stack tests | `SIGBUS` handling on the alternate stack; the main thread's stack size |
| `time`, `random` | `tests/29_time_stdlib`, `tests/30_random_stdlib` | `clock_gettime(CLOCK_PROCESS_CPUTIME_ID)`, `/dev/urandom` seeding (`turfrt_sys.c`) |
| `fs`, `sys` | `grep -l "import fs\|import sys" -r tests` | BSD `readdir`/`stat` layouts, `environ` |

## 4. The GC under stress (the part that has never run on this OS)

```sh
env -u TURF_HOME TURF_GC_STRESS=1 TURF_TEST_TIMEOUT_SEC=300 ./build/turfc --test tests --jobs 4 \
  --diag-register tests/DIAGNOSTIC_GAPS.txt --exclude custom --exclude 99_known_bugs
env -u TURF_HOME TURF_GC_VALIDATE=1 ./build/turfc --test tests/41_gc
env -u TURF_HOME TURF_GC_STRESS=1 TURF_GC_VALIDATE=1 ./build/turfc --test tests/41_gc
```

Pass: `Failed: 0` each. The whole suite under `TURF_GC_STRESS=1` is the equivalent of
what already passes on Linux and (as of 2026-10-11) Windows, 1191/1191. A crash that
only happens under stress points at register capture or the stack bounds: record the
test, the signal, and whether it is the same test at `-O0`.

## 5. The bash smoke scripts

Run each from the repo root; `CI=1` turns a skipped prerequisite into a failure.

```sh
for s in cli_smoke project_smoke test_cmd_smoke fmt_smoke package_graph_smoke \
         git_deps_smoke examples_smoke pattern_warnings_smoke migrate_pub_smoke \
         opt_levels_smoke runtime_smoke mem_smoke gc_smoke debuginfo_smoke \
         std002_caller_location_smoke bundled_link_smoke; do
  echo "=== $s"; CI=1 ./scripts/$s.sh > "/tmp/smoke_$s.log" 2>&1; echo "rc=$?"
done
```

Pass: every `rc=0`. (`runtime_smoke` now includes check 6d, the "a library Turf does not
bundle" diagnostics; on macOS its empty-`PATH` half should print `linking needs a C
compiler`. The PowerShell twins `scripts/*.ps1` are Windows-only: nothing to run here.)
Known not applicable on macOS, record as "N/A, reason", do not fix:
`gc_no_procfs_smoke.sh` (Linux `/proc`), and `gc_rss_smoke.sh` / `gc_stress_corpus_smoke.sh`
need GNU `time -v` (`brew install gnu-time` and a `/usr/bin/time` that is not BSD's,
or record N/A). Any other failure: read the log and decide whether the SCRIPT assumes
Linux (BSD `sed`/`date`/`stat` flags, `/proc`, `ldd`) or the PRODUCT is wrong. On
Windows, 13 of 17 scripts failed and nearly all were the scripts, so expect to triage.

## 6. The clean-machine proof (stage 8, macOS half): the part that closes the promise

The promise is `turf new hello && cd hello && turf run` with **no Xcode and no C
toolchain**. A decoy `cc` on `PATH` is weaker than the truth (macOS's `/usr/bin/cc`,
`clang` and `ld` are shims that find Xcode), so remove the toolchain itself:

```sh
xcode-select -p                               # note the path
sudo mv /Library/Developer/CommandLineTools /Library/Developer/CommandLineTools.off
clang --version 2>&1 | head -1                # must now FAIL ("no developer tools were found")
cc 2>&1 | head -1                             # same
```

(If `xcode-select -p` shows `/Applications/Xcode.app/...`, move that aside instead.
**Restore at the end of this section**: `sudo mv /Library/Developer/CommandLineTools.off /Library/Developer/CommandLineTools`.)

Package and install the archive exactly as a user gets it:

```sh
./scripts/package_release.sh --platform macos-arm64 --out /tmp/dist   # needs no compiler
ls /tmp/dist                                  # turf-v<VERSION>-macos-arm64.tar.gz + .sha256
./scripts/release_deps_check.sh --archive /tmp/dist/turf-v*-macos-arm64.tar.gz
```

Pass: the dependency check exits 0. Expected: `turfc` needs `libSystem`, `libc++`,
`libz`, `libncurses`; `turf` needs `libSystem`, `libresolv`; nothing under `/opt/homebrew`.

```sh
./scripts/bundled_link_smoke.sh --archive /tmp/dist/turf-v*-macos-arm64.tar.gz
./scripts/release_smoke.sh --archive /tmp/dist/turf-v*-macos-arm64.tar.gz
```

Pass: both exit 0 (the second runs `install.sh` into a throwaway HOME, checks the
SHA-256, a new `bash -li` resolving `turf`, `turf new`/`run`/`build`/`explain`, and
uninstall restoring the start-up file byte for byte; macOS uses `~/.bash_profile`).

Now the walkthrough by hand, in a **new** terminal with a stripped environment:

```sh
export TURF_INSTALL_BASE_URL="file:///tmp/dist-site"       # a dir laid out as download/<tag>/<archive>(.sha256)
mkdir -p /tmp/dist-site/download/v$(head -1 VERSION) && cp /tmp/dist/turf-* /tmp/dist-site/download/v$(head -1 VERSION)/
TURF_INSTALL_PLATFORM=macos-arm64 sh ./install.sh --version v$(head -1 VERSION) --prefix "$HOME/turf-clean"
env -i HOME="$HOME" PATH="$HOME/turf-clean/bin:/usr/bin:/bin:/usr/sbin:/sbin" TERM=xterm bash -c '
  set -e
  command -v turf
  cd "$(mktemp -d)"
  turf new hello && cd hello && turf run
  turf build
'
```

Pass: prints `Hello, TurfLang! This is hello.`, exit 0, with the toolchain moved aside.
Then, in that same stripped environment: a project with a **path dependency** (copy
`examples/geometry` and `examples/plotter`; `cd examples/plotter && turf build --locked --offline && ./build/plotter`),
a project with a **git dependency** (the manifest form is `jsonx = { git = "<url>", tag = "v1" }`;
the quickest source is a local bare repo, as in `scripts/git_deps_smoke.sh`), and a
program that imports `net` and talks to itself over loopback (copy
`tests/34_http_and_json/03_http_manual_routing_roundtrip.tr`: it binds, connects and
answers within one process).
Pass: each builds and runs.

One more check in the same stripped environment, for the library diagnostics added with
`docs/reference/ffi_and_unsafe.md` "Linking external libraries": a program that names a
library the archive does not bundle must say so, not fail obscurely.

```sh
printf 'fn main(): int { return 0; }\n' > p.tr
turfc p.tr -o p --link nosuchlib; echo "exit=$?"
```

Pass: exit 1 and the output names `"nosuchlib"`. With the toolchain moved aside
`/usr/bin/cc` still exists as a shim that fails, so expect `Linking failed (exit code 1)`
followed by `This program links against "nosuchlib", which Turf does not bundle`; the
`linking needs a C compiler` wording appears only when no `cc` can be started at all. A
plain `printf 'fn main(): int { return 0; }\n'` program with no `--link` must still link
and run (that is the promise).

Finally prove nothing hidden ran a compiler. With the toolchain still moved aside,
any attempt to execute `clang`/`ld`/`cc` already fails, so a pass above IS the proof.
Add, for the record: `sudo fs_usage -w -f exec` in another terminal during `turf run`,
and confirm no `clang`, `ld`, `cc`, `ld64` or `gcc` line appears.

Restore the toolchain (command above) and confirm `clang --version` works again.

## 7. What a failure here usually means

- `dyld: symbol not found` / killed on launch, section 2: the flat-namespace stub is not
  accepted by this macOS. The fallback named in the stage 4 record: generate the stub from
  the SDK's `libSystem.tbd` instead (redistribution must be checked first).
- Crashes only under `TURF_GC_STRESS`: AAPCS64 register capture or the stack base in
  `runtime/turfrt_gc.c` (`#elif defined(__APPLE__)`, line ~175).
- `net` failures: `turfrt_net.c` `__APPLE__` branch (`sin_len`); `net.tr` exports
  Linux values for `SOL_SOCKET`/`SO_REUSEADDR` (public API) and uses the runtime's
  `turfrt_net_reuseaddr()` for the real one.
- A test that passes only when run alone: parallelism (`--jobs 4`) and ports; rerun that
  directory with `--jobs 1`.
- Findings from Windows that may exist here too: a program that returns a small exit code
  being reported as a crash (the runner's status mapping), CRLF/LF in `--format` output,
  path separators in diagnostics. On macOS these should not appear; if they do, record them.

## 8. What else keeps REL-008 open after macOS is green

Be explicit in the record, so "closed" is not claimed early:
- **Stage 7:** `install.ps1` exists and passes `scripts/release_smoke.ps1` on a real
  Windows machine (35 checks); its CI step has not run on a runner. Archive signing
  (minisign/cosign) is not done for any platform.
- **Stage 8:** the Windows clean-machine proof is `release_smoke.ps1` (a `PATH` of
  `System32` plus the install, no gcc/git; passes locally, CI step unrun; no git-dependency
  project, since git is not on that PATH). Linux's `docker run
  ubuntu:24.04` with no `cc` is not built either (`release_smoke.sh` says the clean-machine half is
  stage 8's, and keeps `cc` on `PATH`).
- **Stage 9:** Homebrew/Scoop/AUR recipes bumped by CI after a release.
- The Windows CI run of the whole suite (the local run is evidence, not the gate).
- The four owner decisions listed under Stage 5 in `REL-008.md`.
