package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/turf-lang/turf-lang/cli/internal/manifest"
)

func TestParseScaffoldArgs(t *testing.T) {
	tests := []struct {
		args []string
		name string
		lib  bool
		bad  string
	}{
		{[]string{"app"}, "app", false, ""},
		{[]string{"app", "--lib"}, "app", true, ""},
		{[]string{"--lib", "app"}, "app", true, ""},
		{nil, "", false, ""},
		{[]string{"--bin"}, "", false, "unknown flag '--bin'"},
		{[]string{"a", "b"}, "", false, "unexpected argument 'b'"},
	}
	for _, tc := range tests {
		name, lib, err := parseScaffoldArgs(tc.args)
		if tc.bad != "" {
			if err == nil || !strings.Contains(err.Error(), tc.bad) {
				t.Errorf("%v: want error %q, got %v", tc.args, tc.bad, err)
			}
			continue
		}
		if err != nil || name != tc.name || lib != tc.lib {
			t.Errorf("%v: got %q %v %v", tc.args, name, lib, err)
		}
	}
}

func TestEnsureGitignore(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, ".gitignore")

	if _, err := ensureGitignore(p); err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(p); string(b) != "/build/\n" {
		t.Fatalf("new file: %q", b)
	}
	// Already present: untouched.
	if _, err := ensureGitignore(p); err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(p); string(b) != "/build/\n" {
		t.Fatalf("second call changed the file: %q", b)
	}
	// Existing file without the line, and without a final newline.
	if err := os.WriteFile(p, []byte("*.o\ntmp"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := ensureGitignore(p); err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(p); string(b) != "*.o\ntmp\n/build/\n" {
		t.Fatalf("append: %q", b)
	}
}

// Every template the scaffold writes must itself satisfy the manifest rules,
// or `turf new` would create a project `turf build` rejects.
func TestManifestTemplateIsValid(t *testing.T) {
	for _, req := range []string{"2.0", fallbackTurfRequirement} {
		m, err := manifest.Parse("/p/turf.toml", []byte(manifestText("app", req)))
		if err != nil {
			t.Fatalf("template invalid: %v", err)
		}
		if m.Package.Name != "app" || m.Package.Version != "0.1.0" || m.Package.Turf != req {
			t.Errorf("got %+v", m.Package)
		}
	}
}

func TestCheckNewNameRules(t *testing.T) {
	for _, bad := range []string{"my-app", "2fast", "fn", "", "a b"} {
		if err := checkNewName(bad, false); err == nil {
			t.Errorf("%q must be refused", bad)
		}
	}
	if err := checkNewName("geometry", true); err != nil && !strings.Contains(err.Error(), "standard-library") {
		t.Errorf("geometry should be fine, got %v", err)
	}
}
