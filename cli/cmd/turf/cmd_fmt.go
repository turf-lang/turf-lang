package main

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"

	"github.com/turf-lang/turf-lang/cli/internal/driver"
	"github.com/turf-lang/turf-lang/cli/internal/project"
	"github.com/turf-lang/turf-lang/cli/internal/version"
)

// cmdFmt drives `turfc --format`.
//
//	turf fmt <file.tr>                 the formatted text, on stdout
//	turf fmt --write <path>...         rewrite what differs, in place
//	turf fmt --check <path>...         name what would change; exit 1 if any
//	turf fmt --write | --check         the project's src/ and tests/
//
// A path is a file, or a directory searched for .tr files. How a file is
// formatted, and the guarantees about it (it only moves whitespace, the token
// stream is unchanged, formatting twice changes nothing), are turfc's: every
// result is verified there before it is written or reported (FREEZE.md 6.23).
// turf only decides which files, and which of them may be skipped when the
// lexer rejects them (the ones found under a directory, not the ones named).
//
// Without --write or --check the formatted text goes to stdout, so several
// files would concatenate into output nobody could tell apart; that is refused
// instead of guessed at. A whole project is refused the same way: formatting
// it means rewriting files, so the command line must say whether to.
func cmdFmt(args []string) int {
	opts, code := parseFmtArgs(args)
	if code != 0 {
		return code
	}

	dir := "" // where turfc runs; empty is where the user is
	if len(opts.paths) == 0 {
		if !opts.write && !opts.check {
			return usagef("fmt: formatting a whole project rewrites files, so say which: --write to reformat them, --check to only report what would change (or name a single file to print it)")
		}
		root, err := fmtProjectRoot()
		if err != nil {
			return usagef("fmt: no files given, and %v", err)
		}
		dir = root
		for _, sub := range []string{"src", testsDirName} {
			if st, err := os.Stat(filepath.Join(root, sub)); err == nil && st.IsDir() {
				opts.paths = append(opts.paths, sub)
			}
		}
		if len(opts.paths) == 0 {
			return failf("fmt: the project at %s has neither src/ nor %s/, so there is nothing to format", root, testsDirName)
		}
	}

	named, walked, err := collectFmtFiles(dir, opts.paths, opts.excludes)
	if err != nil {
		return failf("fmt: %v", err)
	}
	if len(named)+len(walked) == 0 {
		return failf("fmt: no .tr files found in %s", strings.Join(opts.paths, ", "))
	}
	stdout := !opts.write && !opts.check
	if stdout && (len(named)+len(walked) > 1 || fmtGivenDirectory(dir, opts.paths)) {
		return usagef("fmt: without --write or --check the formatted text goes to stdout, so exactly one file is accepted; pass --write to reformat files in place, or --check to only report")
	}

	flags := []string{"--format"}
	switch {
	case opts.write:
		flags = append(flags, "--write")
	case opts.check:
		flags = append(flags, "--check")
	}
	return withCompiler(func(c string) int {
		info, err := driver.Probe(c)
		if err != nil {
			return failf("%v", err)
		}
		if err := fmtInterfaceError(info, c); err != nil {
			return failf("%v", err)
		}
		if stdout {
			return driver.Exec(c, append(flags, named[0]))
		}
		// Files the user named must format; files only found under a
		// directory are skipped if the lexer rejects them (see
		// collectFmtFiles). Two kinds of run, so turfc is told which.
		status := 0
		run := func(files, extra []string) {
			for _, batch := range chunkFiles(files, fmtArgBudget()) {
				argv := append(append(append([]string{}, flags...), extra...), batch...)
				if s := driver.RunIn(dir, c, argv); s != 0 && status == 0 {
					status = s
				}
			}
		}
		run(named, nil)
		run(walked, []string{"--skip-unlexable"})
		return status
	})
}

type fmtOptions struct {
	write, check bool
	excludes     []string
	paths        []string
}

const fmtUsage = "usage: turf fmt [--write | --check] [--exclude <path>]... [<file.tr | dir>...]"

func parseFmtArgs(args []string) (fmtOptions, int) {
	var o fmtOptions
	for i := 0; i < len(args); i++ {
		a := args[i]
		switch {
		case a == "--write":
			o.write = true
		case a == "--check":
			o.check = true
		case a == "--exclude":
			if i+1 >= len(args) {
				return o, usagef("fmt: --exclude needs a path (%s)", fmtUsage)
			}
			i++
			o.excludes = append(o.excludes, args[i])
		case strings.HasPrefix(a, "--exclude="):
			o.excludes = append(o.excludes, strings.TrimPrefix(a, "--exclude="))
		case strings.HasPrefix(a, "-"):
			return o, usagef("fmt: unknown flag '%s' (%s)", a, fmtUsage)
		default:
			o.paths = append(o.paths, a)
		}
	}
	if o.write && o.check {
		return o, usagef("fmt: --write and --check cannot be combined: one rewrites the files, the other promises not to")
	}
	return o, 0
}

// fmtProjectRoot is the project the cwd is in, or an error saying there is none.
func fmtProjectRoot() (string, error) {
	cwd, err := os.Getwd()
	if err != nil {
		return "", err
	}
	root, err := project.FindRoot(cwd)
	if errors.Is(err, project.ErrNoProject) {
		return "", errors.New("not inside a TurfLang project (no turf.toml here or in any parent directory); name the files or directories to format")
	}
	return root, err
}

// fmtGivenDirectory reports whether any path is a directory, which stdout mode
// cannot print as one file's text.
func fmtGivenDirectory(dir string, paths []string) bool {
	for _, p := range paths {
		if st, err := os.Stat(filepath.Join(dir, p)); err == nil && st.IsDir() {
			return true
		}
	}
	return false
}

// collectFmtFiles expands paths (relative to dir, or to the cwd when dir is
// empty) into the .tr files to format: a file stands for itself, a directory
// for every .tr file under it, in a stable order, without hidden entries (.git,
// editor scratch) or a nested build/ (output, never source). An exclude drops a
// file or everything under a directory, spelled the way the path was. A path
// that does not exist is an error naming it, never skipped: a typo would
// otherwise pass `--check` having checked nothing.
//
// The files come back in two lists because they are not held to the same
// rule: `named` are files the user spelled out, which must be formatted or the
// run fails; `walked` were only found under a directory, and a tree may hold
// sources that are invalid on purpose (this repository's `fail_*` tests), so
// one the lexer rejects is skipped, not a failure. A file that is both named
// and found by a walk is `named`: asking for it explicitly wins.
func collectFmtFiles(dir string, paths, excludes []string) (named, walked []string, err error) {
	excluded := func(f string) bool {
		f = filepath.ToSlash(filepath.Clean(f))
		for _, x := range excludes {
			x = filepath.ToSlash(filepath.Clean(x))
			if f == x || strings.HasPrefix(f, x+"/") {
				return true
			}
		}
		return false
	}
	seenNamed := map[string]bool{}
	seenWalked := map[string]bool{}
	var walkedAll []string
	for _, p := range paths {
		abs := filepath.Join(dir, p)
		st, err := os.Stat(abs)
		if err != nil {
			return nil, nil, fmt.Errorf("%s: no such file or directory", p)
		}
		if !st.IsDir() {
			if !seenNamed[p] && !excluded(p) {
				seenNamed[p] = true
				named = append(named, p)
			}
			continue
		}
		var found []string
		err = filepath.WalkDir(abs, func(path string, d fs.DirEntry, err error) error {
			if err != nil {
				return err
			}
			name := d.Name()
			if d.IsDir() {
				if path != abs && (strings.HasPrefix(name, ".") || name == buildDirName) {
					return filepath.SkipDir
				}
				return nil
			}
			if strings.HasSuffix(name, ".tr") && !strings.HasPrefix(name, ".") {
				rel, _ := filepath.Rel(abs, path)
				// Slashes on every OS: these names are echoed in turfc's output
				// ("would reformat src/main.tr") and in the docs, which spell them one way.
				found = append(found, filepath.ToSlash(filepath.Join(p, rel)))
			}
			return nil
		})
		if err != nil {
			return nil, nil, err
		}
		sort.Strings(found)
		for _, f := range found {
			if !seenWalked[f] && !excluded(f) {
				seenWalked[f] = true
				walkedAll = append(walkedAll, f)
			}
		}
	}
	for _, f := range walkedAll {
		if !seenNamed[f] {
			walked = append(walked, f)
		}
	}
	return named, walked, nil
}

// fmtArgBudget is how many bytes of file names one turfc run is given. A
// project's whole tree is normally one run, which keeps turfc's own summary a
// single line; the cap exists for the platform limits on a command line (32 KB
// on Windows).
func fmtArgBudget() int {
	if runtime.GOOS == "windows" {
		return 24 * 1024
	}
	return 100 * 1024
}

// chunkFiles splits files into runs whose names total at most budget bytes
// (one name per run at least, however long).
func chunkFiles(files []string, budget int) [][]string {
	var out [][]string
	var cur []string
	size := 0
	for _, f := range files {
		if len(cur) > 0 && size+len(f)+1 > budget {
			out = append(out, cur)
			cur, size = nil, 0
		}
		cur = append(cur, f)
		size += len(f) + 1
	}
	if len(cur) > 0 {
		out = append(out, cur)
	}
	return out
}

// fmtInterfaceError is nil when the compiler has the batch form of --format
// (several files, --check). An older turfc would read `--check` as the
// diagnostic check and silently compile instead of formatting.
func fmtInterfaceError(info driver.CompilerInfo, compiler string) error {
	need := version.RequiredForFmt
	if info.Interface.Major == need.Major && info.Interface.Minor >= need.Minor {
		return nil
	}
	return fmt.Errorf("`turf fmt` needs a compiler that speaks driver interface %d.%d or a later %d.x, "+
		"but turfc %s (%s) speaks %d.%d\nhint: upgrade turfc, or point TURF_COMPILER at a newer one",
		need.Major, need.Minor, need.Major, info.Release, compiler,
		info.Interface.Major, info.Interface.Minor)
}

const helpFmt = `turf fmt - format TurfLang source

Usage:
  turf fmt <file.tr>                    print the formatted file to stdout
  turf fmt --write [<path>...]          rewrite the files that differ, in place
  turf fmt --check [<path>...]          change nothing; name each file that would
                                        change, and exit 1 if there is any
  --exclude <path>                      skip a file, or everything under a
                                        directory (repeatable)

A path is a .tr file, or a directory, which stands for every .tr file under it
(not hidden directories, not a build/). With no path, inside a project, the
project's src/ and tests/ are formatted (one of --write or --check is then
required, since the output would otherwise be many files at once). Without
--write or --check exactly one file is accepted, because the output goes to
stdout. A path that does not exist is an error, never skipped.

The formatter fixes indentation (each line's depth follows the real lexer's
brackets), trailing whitespace, runs of blank lines and the final newline, and
nothing else: it never re-flows, re-spaces or reorders code. Every result is
checked before it is written or reported: no non-whitespace byte changed, the
lexer reads the same tokens from both texts, and formatting the result again
changes nothing. A file that fails a check is left alone and reported as a
formatter bug. A file with a lexical error (an unterminated string, a malformed
number) cannot be formatted; the rest are still done. If you named that file, it
is an error. If it was only found under a directory it is listed as "skipped"
and is not an error, because a tree can hold sources that are invalid on
purpose (a test of the compiler's own error messages). The exit status is 0
when every file was formatted or skipped (or, with --check, already formatted)
and 1 otherwise. Runs turfc --format.
`
