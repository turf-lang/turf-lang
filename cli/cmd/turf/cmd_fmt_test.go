package main

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/turf-lang/turf-lang/cli/internal/driver"
	"github.com/turf-lang/turf-lang/cli/internal/version"
)

func touch(t *testing.T, root string, rels ...string) {
	t.Helper()
	for _, rel := range rels {
		p := filepath.Join(root, filepath.FromSlash(rel))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte("fn main(): int { return 0; }\n"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
}

func TestParseFmtArgs(t *testing.T) {
	o, code := parseFmtArgs([]string{"--check", "--exclude", "a/b", "--exclude=c", "src", "x.tr"})
	if code != 0 || !o.check || o.write {
		t.Fatalf("flags: %+v code %d", o, code)
	}
	if !reflect.DeepEqual(o.excludes, []string{"a/b", "c"}) || !reflect.DeepEqual(o.paths, []string{"src", "x.tr"}) {
		t.Errorf("excludes %v paths %v", o.excludes, o.paths)
	}
	for _, bad := range [][]string{
		{"--write", "--check"},
		{"--exclude"},
		{"--wirte"},
	} {
		if _, code := parseFmtArgs(bad); code != exitUsage {
			t.Errorf("%v: exit %d, want %d", bad, code, exitUsage)
		}
	}
}

func TestCollectFmtFiles(t *testing.T) {
	root := t.TempDir()
	touch(t, root,
		"src/main.tr", "src/util/text.tr", "src/.hidden/x.tr", "src/.swap.tr",
		"src/build/out.tr", "tests/a.tr", "tests/fail_b.tr", "tests/known/c.tr", "notes.txt")

	named, got, err := collectFmtFiles(root, []string{"src", "tests"}, []string{"tests/known", "tests/fail_b.tr"})
	if err != nil {
		t.Fatal(err)
	}
	want := []string{"src/main.tr", "src/util/text.tr", "tests/a.tr"}
	if !reflect.DeepEqual(got, want) || len(named) != 0 {
		t.Errorf("walked %v named %v, want walked %v (hidden entries, build/ and the excludes must be dropped, order stable)", got, named, want)
	}

	// A file stands for itself, a repeat is one entry, the spelling is kept,
	// and a file that is both named and found by a walk is named: the user
	// asked for it, so a lexical error in it must not be skipped.
	named, got, err = collectFmtFiles(root, []string{"src/main.tr", "src", "src/main.tr"}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if want := []string{"src/main.tr"}; !reflect.DeepEqual(named, want) {
		t.Errorf("named %v, want %v", named, want)
	}
	if want := []string{"src/util/text.tr"}; !reflect.DeepEqual(got, want) {
		t.Errorf("walked %v, want %v", got, want)
	}

	// A path that is not there is an error naming it, not a silent skip.
	if _, _, err := collectFmtFiles(root, []string{"srcc"}, nil); err == nil || !strings.Contains(err.Error(), "srcc") {
		t.Errorf("a missing path must be an error naming it, got %v", err)
	}
}

func TestChunkFiles(t *testing.T) {
	files := []string{"aaaa", "bbbb", "cccc", "dd"}
	got := chunkFiles(files, 10) // each name costs len+1: 5, 5 | 5, 3
	if want := [][]string{{"aaaa", "bbbb"}, {"cccc", "dd"}}; !reflect.DeepEqual(got, want) {
		t.Errorf("got %v, want %v", got, want)
	}
	// One name longer than the budget still runs, alone.
	if got := chunkFiles([]string{"a-very-long-name.tr"}, 3); !reflect.DeepEqual(got, [][]string{{"a-very-long-name.tr"}}) {
		t.Errorf("oversized name: %v", got)
	}
	if got := chunkFiles(nil, 10); len(got) != 0 {
		t.Errorf("no files must be no runs, got %v", got)
	}
	var all []string
	for _, c := range chunkFiles(files, 1) {
		all = append(all, c...)
	}
	if !reflect.DeepEqual(all, files) {
		t.Errorf("chunking must keep every file in order, got %v", all)
	}
}

func TestFmtInterfaceError(t *testing.T) {
	need := version.RequiredForFmt
	for _, tc := range []struct {
		got version.Interface
		bad bool
	}{
		{need, false},
		{version.Interface{Major: need.Major, Minor: need.Minor + 1}, false},
		{version.Interface{Major: need.Major, Minor: need.Minor - 1}, true},
		{version.Interface{Major: need.Major + 1, Minor: need.Minor}, true},
	} {
		err := fmtInterfaceError(driver.CompilerInfo{Release: "2.0.0", Interface: tc.got}, "/x/turfc")
		if (err != nil) != tc.bad {
			t.Errorf("%+v: error %v, want error: %v", tc.got, err, tc.bad)
		}
	}
}

// A whole project rewrites files, so the command line must say whether to:
// decided before any compiler is looked for.
func TestFmtOfAProjectNeedsWriteOrCheck(t *testing.T) {
	dir := t.TempDir()
	t.Chdir(dir)
	t.Setenv("TURF_COMPILER", filepath.Join(dir, "no-such-turfc"))
	if code := cmdFmt(nil); code != exitUsage {
		t.Errorf("turf fmt with nothing: exit %d, want %d", code, exitUsage)
	}
	// Outside a project there is no default set of files either.
	if code := cmdFmt([]string{"--check"}); code != exitUsage {
		t.Errorf("turf fmt --check outside a project: exit %d, want %d", code, exitUsage)
	}
	// Several files cannot share stdout.
	touch(t, dir, "a.tr", "b.tr")
	if code := cmdFmt([]string{"a.tr", "b.tr"}); code != exitUsage {
		t.Errorf("two files to stdout: exit %d, want %d", code, exitUsage)
	}
	if code := cmdFmt([]string{"."}); code != exitUsage {
		t.Errorf("a directory to stdout: exit %d, want %d", code, exitUsage)
	}
}
