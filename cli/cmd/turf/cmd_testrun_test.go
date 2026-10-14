package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/turf-lang/turf-cli/internal/driver"
	"github.com/turf-lang/turf-cli/internal/version"
)

func TestTestInterfaceError(t *testing.T) {
	need := version.RequiredForTest
	cases := []struct {
		name string
		got  version.Interface
		bad  bool
	}{
		{"exactly the required", need, false},
		{"a later minor", version.Interface{Major: need.Major, Minor: need.Minor + 4}, false},
		{"an earlier minor", version.Interface{Major: need.Major, Minor: need.Minor - 1}, true},
		{"another major", version.Interface{Major: need.Major + 1, Minor: need.Minor}, true},
	}
	for _, tc := range cases {
		err := testInterfaceError(driver.CompilerInfo{Release: "2.0.0", Interface: tc.got}, "/x/turfc", need)
		if (err != nil) != tc.bad {
			t.Errorf("%s: error = %v, want error: %v", tc.name, err, tc.bad)
		}
		if err != nil && !strings.Contains(err.Error(), "upgrade turfc") {
			t.Errorf("%s: the message should say what to do: %v", tc.name, err)
		}
	}
}

// A project with no tests/ directory is an error and says how to start one;
// it is decided before any compiler is looked for, so it holds on a machine
// with no turfc at all.
func TestTestWithoutATestsDirectoryIsAnError(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "turf.toml"), []byte("[package]\nname = \"app\"\nversion = \"0.1.0\"\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	t.Chdir(dir)
	t.Setenv("TURF_COMPILER", filepath.Join(dir, "no-such-turfc"))

	old := os.Stderr
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	os.Stderr = w
	code := cmdTest(nil)
	w.Close()
	os.Stderr = old
	buf := make([]byte, 4096)
	n, _ := r.Read(buf)
	msg := string(buf[:n])

	if code != exitFailure {
		t.Errorf("exit status = %d, want %d", code, exitFailure)
	}
	if !strings.Contains(msg, "has no tests/ directory") || !strings.Contains(msg, "tests/basic.tr") {
		t.Errorf("message should name the missing directory and show a first test, got:\n%s", msg)
	}

	// A file called tests is not a tests directory.
	if err := os.WriteFile(filepath.Join(dir, "tests"), nil, 0o644); err != nil {
		t.Fatal(err)
	}
	if code := cmdTest(nil); code != exitFailure {
		t.Errorf("tests as a file: exit status = %d, want %d", code, exitFailure)
	}
}

func TestTestRefusesTheFlagsTurfOwns(t *testing.T) {
	for _, flag := range []string{"-o", "--package-graph"} {
		if code := cmdTest([]string{flag, "x"}); code != exitUsage {
			t.Errorf("%s: exit status = %d, want %d", flag, code, exitUsage)
		}
	}
}

func TestTestPaths(t *testing.T) {
	root := t.TempDir()
	tests := filepath.Join(root, "tests")
	for _, f := range []string{"a.tr", "sub/b.tr"} {
		p := filepath.Join(tests, f)
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, nil, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(filepath.Join(root, "x.tr"), nil, 0o644); err != nil {
		t.Fatal(err)
	}

	only, rest, err := testPaths([]string{"tests/a.tr", "--jobs", "2", "tests/sub", "-O2", "--exclude", "tests/a.tr"}, root, tests)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Join(only, ",") != "a.tr,sub" {
		t.Errorf("only = %q, want [a.tr sub]", only)
	}
	if strings.Join(rest, " ") != "--jobs 2 -O2 --exclude tests/a.tr" {
		t.Errorf("rest = %q: a flag's value is not a path", rest)
	}

	// From inside tests/, a path is relative to there.
	only, _, err = testPaths([]string{"sub/b.tr"}, tests, tests)
	if err != nil || strings.Join(only, ",") != "sub/b.tr" {
		t.Errorf("from tests/: only = %q, err = %v", only, err)
	}
	// tests/ itself selects everything.
	only, _, err = testPaths([]string{"."}, tests, tests)
	if err != nil || len(only) != 0 {
		t.Errorf("tests/ itself: only = %q, err = %v", only, err)
	}

	for _, bad := range []string{"x.tr", "tests/nope.tr", ".."} {
		cwd := root
		if bad == ".." {
			cwd = tests
		}
		if _, _, err := testPaths([]string{bad}, cwd, tests); err == nil {
			t.Errorf("%s: want an error", bad)
		}
	}
}
