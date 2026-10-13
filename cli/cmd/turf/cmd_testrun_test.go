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
		err := testInterfaceError(driver.CompilerInfo{Release: "2.0.0", Interface: tc.got}, "/x/turfc")
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
