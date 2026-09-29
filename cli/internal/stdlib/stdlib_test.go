package stdlib

import (
	"os"
	"path/filepath"
	"testing"
)

func TestModulesFindsStemsAtAnyDepth(t *testing.T) {
	root := t.TempDir()
	for _, f := range []string{"core/int.tr", "collections/list.tr", "top.tr", "core/notes.txt"} {
		p := filepath.Join(root, "lib", "std", filepath.FromSlash(f))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, nil, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	got, err := Modules(root)
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"int", "list", "top"} {
		if !got[want] {
			t.Errorf("missing module %q in %v", want, got)
		}
	}
	if got["notes"] || len(got) != 3 {
		t.Errorf("only .tr files are modules: %v", got)
	}
}

func TestModulesWithoutAStdlib(t *testing.T) {
	if _, err := Modules(t.TempDir()); err == nil {
		t.Fatal("want an error when lib/std does not exist")
	}
}

func TestInstallRoot(t *testing.T) {
	t.Setenv("TURF_HOME", "")
	dir := t.TempDir()
	dir, _ = filepath.EvalSymlinks(dir)
	c := filepath.Join(dir, "build", "turfc")
	if got, want := InstallRoot(c), filepath.Join(dir, "build", ".."); got != want {
		t.Errorf("InstallRoot = %q, want %q", got, want)
	}
	t.Setenv("TURF_HOME", "/opt/turf")
	if got := InstallRoot(c); got != "/opt/turf" {
		t.Errorf("TURF_HOME must win, got %q", got)
	}
}
