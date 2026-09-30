package main

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// The test binary stands in for turf: pathHint is about "the running
// executable", whatever it is called.
func TestPathHint(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("POSIX PATH and symlink handling")
	}
	exe, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	exe, _ = filepath.EvalSymlinks(exe)

	t.Run("not on PATH gives the shell's command", func(t *testing.T) {
		t.Setenv("PATH", t.TempDir())
		t.Setenv("SHELL", "/bin/zsh")
		h := pathHint()
		for _, want := range []string{"is not on your PATH", exe, "~/.zshrc", filepath.Dir(exe)} {
			if !strings.Contains(h, want) {
				t.Errorf("hint lacks %q:\n%s", want, h)
			}
		}
	})

	t.Run("a different turf on PATH is named", func(t *testing.T) {
		dir := t.TempDir()
		other := filepath.Join(dir, "turf")
		if err := os.WriteFile(other, []byte("#!/bin/sh\n"), 0o755); err != nil {
			t.Fatal(err)
		}
		t.Setenv("PATH", dir)
		h := pathHint()
		if !strings.Contains(h, "on your PATH is "+other) {
			t.Errorf("hint does not name the other turf:\n%s", h)
		}
	})

	t.Run("this turf on PATH (through a symlink) gives no hint", func(t *testing.T) {
		dir := t.TempDir()
		if err := os.Symlink(exe, filepath.Join(dir, "turf")); err != nil {
			t.Fatal(err)
		}
		t.Setenv("PATH", dir)
		if h := pathHint(); h != "" {
			t.Errorf("expected no hint, got:\n%s", h)
		}
	})
}

func TestAddToPathCommandShells(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("POSIX shells")
	}
	cases := map[string]string{
		"/bin/zsh":      "~/.zshrc",
		"/usr/bin/fish": "fish_add_path /x/bin",
		"/bin/tcsh":     `export PATH="/x/bin:$PATH"`,
	}
	for shell, want := range cases {
		t.Setenv("SHELL", shell)
		if got := addToPathCommand("/x/bin"); !strings.Contains(got, want) {
			t.Errorf("SHELL=%s: %q lacks %q", shell, got, want)
		}
	}
	t.Setenv("SHELL", "/bin/bash")
	want := "~/.bashrc"
	if runtime.GOOS == "darwin" {
		want = "~/.bash_profile"
	}
	if got := addToPathCommand("/x/bin"); !strings.Contains(got, want) {
		t.Errorf("bash: %q lacks %q", got, want)
	}
}
