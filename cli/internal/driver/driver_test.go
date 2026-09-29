package driver

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// fakeTurfc writes an executable shell script that prints out for any
// invocation, standing in for the compiler. Skipped on Windows, which has no
// shell scripts to execute.
func fakeTurfc(t *testing.T, dir, out string) string {
	t.Helper()
	if runtime.GOOS == "windows" {
		t.Skip("needs a POSIX shell")
	}
	p := filepath.Join(dir, CompilerFile)
	script := "#!/bin/sh\nprintf '%s' " + shellQuote(out) + "\n"
	if err := os.WriteFile(p, []byte(script), 0o755); err != nil {
		t.Fatal(err)
	}
	return p
}

func shellQuote(s string) string {
	return "'" + strings.ReplaceAll(s, "'", `'\''`) + "'"
}

func touch(t *testing.T, p string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(p, nil, 0o755); err != nil {
		t.Fatal(err)
	}
}

// isolate clears the variables discovery reads, so a developer's real
// TURF_COMPILER/TURF_HOME/PATH can never satisfy (or fail) a test.
func isolate(t *testing.T) {
	t.Helper()
	t.Setenv("TURF_COMPILER", "")
	t.Setenv("TURF_HOME", "")
	t.Setenv("PATH", t.TempDir()) // an empty directory: nothing on PATH
}

func TestDiscoverOrder(t *testing.T) {
	isolate(t)
	base := t.TempDir()

	beside := filepath.Join(base, "beside")
	touch(t, filepath.Join(beside, "turf"))
	touch(t, filepath.Join(beside, CompilerFile))

	home := filepath.Join(base, "home")
	touch(t, filepath.Join(home, "bin", CompilerFile))

	onPath := filepath.Join(base, "onpath")
	touch(t, filepath.Join(onPath, "turfc"))

	override := filepath.Join(base, "override", "my-turfc")
	touch(t, override)

	exe := filepath.Join(beside, "turf")
	elsewhere := filepath.Join(base, "elsewhere", "turf")
	touch(t, elsewhere)

	t.Run("TURF_COMPILER wins over everything", func(t *testing.T) {
		t.Setenv("TURF_COMPILER", override)
		t.Setenv("TURF_HOME", home)
		t.Setenv("PATH", onPath)
		got, err := DiscoverFrom(exe)
		if err != nil || got != override {
			t.Fatalf("got %q, %v; want %q", got, err, override)
		}
	})

	t.Run("beside the executable is next", func(t *testing.T) {
		t.Setenv("TURF_HOME", home)
		t.Setenv("PATH", onPath)
		got, err := DiscoverFrom(exe)
		want := filepath.Join(beside, CompilerFile)
		if err != nil || got != want {
			t.Fatalf("got %q, %v; want %q", got, err, want)
		}
	})

	t.Run("TURF_HOME/bin is third", func(t *testing.T) {
		t.Setenv("TURF_HOME", home)
		t.Setenv("PATH", onPath)
		got, err := DiscoverFrom(elsewhere)
		want := filepath.Join(home, "bin", CompilerFile)
		if err != nil || got != want {
			t.Fatalf("got %q, %v; want %q", got, err, want)
		}
	})

	t.Run("PATH is last", func(t *testing.T) {
		t.Setenv("PATH", onPath)
		got, err := DiscoverFrom(elsewhere)
		want := filepath.Join(onPath, "turfc")
		if err != nil || got != want {
			t.Fatalf("got %q, %v; want %q", got, err, want)
		}
	})
}

func TestDiscoverNotFound(t *testing.T) {
	isolate(t)
	exe := filepath.Join(t.TempDir(), "turf")
	touch(t, exe)
	_, err := DiscoverFrom(exe)
	if err == nil {
		t.Fatal("expected an error")
	}
	msg := err.Error()
	for _, want := range []string{"cannot find the compiler (turfc)", "looked in:", "$TURF_COMPILER", "TURF_HOME", "PATH", "hint: set TURF_COMPILER"} {
		if !strings.Contains(msg, want) {
			t.Errorf("error lacks %q:\n%s", want, msg)
		}
	}
}

// A TURF_COMPILER naming a missing file must NOT fall through to the next
// place: a typo in the override would otherwise silently run another compiler.
func TestDiscoverBadOverrideDoesNotFallThrough(t *testing.T) {
	isolate(t)
	beside := t.TempDir()
	touch(t, filepath.Join(beside, "turf"))
	touch(t, filepath.Join(beside, CompilerFile)) // would match if we fell through
	t.Setenv("TURF_COMPILER", filepath.Join(beside, "nonexistent"))
	_, err := DiscoverFrom(filepath.Join(beside, "turf"))
	if err == nil || !strings.Contains(err.Error(), "TURF_COMPILER") {
		t.Fatalf("want an error naming TURF_COMPILER, got %v", err)
	}
}

func TestDiscoverFollowsSymlinks(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("symlinks need privileges on Windows")
	}
	isolate(t)
	base := t.TempDir()
	real := filepath.Join(base, "build")
	touch(t, filepath.Join(real, "turf"))
	touch(t, filepath.Join(real, CompilerFile))

	link := filepath.Join(base, "bin", "turf")
	if err := os.MkdirAll(filepath.Dir(link), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(filepath.Join(real, "turf"), link); err != nil {
		t.Fatal(err)
	}

	got, err := DiscoverFrom(link)
	// The temp dir itself may sit behind a symlink (macOS /var -> /private/var),
	// so compare resolved paths.
	want, _ := filepath.EvalSymlinks(filepath.Join(real, CompilerFile))
	gotR, _ := filepath.EvalSymlinks(got)
	if err != nil || gotR != want {
		t.Fatalf("got %q, %v; want %q", got, err, want)
	}
}

func TestHandshake(t *testing.T) {
	dir := t.TempDir()

	t.Run("match", func(t *testing.T) {
		c := fakeTurfc(t, dir, "turfc 2.0.0-dev\nLLVM 22\n")
		if err := Handshake(c, "/x/turf", "2.0.0-dev"); err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
	})

	t.Run("later lines are ignored", func(t *testing.T) {
		c := fakeTurfc(t, dir, "turfc 2.0.0-dev\nsomething else entirely\n")
		if err := Handshake(c, "/x/turf", "2.0.0-dev"); err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
	})

	t.Run("mismatch names both versions and both paths", func(t *testing.T) {
		c := fakeTurfc(t, dir, "turfc 0.0.0\n")
		err := Handshake(c, "/x/turf", "2.0.0-dev")
		if err == nil {
			t.Fatal("expected a mismatch error")
		}
		msg := err.Error()
		for _, want := range []string{"compiler version mismatch", "2.0.0-dev", "0.0.0", "/x/turf", c, "hint:"} {
			if !strings.Contains(msg, want) {
				t.Errorf("error lacks %q:\n%s", want, msg)
			}
		}
	})

	t.Run("unstamped turf is refused before running anything", func(t *testing.T) {
		// The compiler path does not even exist: the refusal must not need it.
		err := Handshake("/nonexistent/turfc", "/x/turf", "unknown")
		if err == nil || !strings.Contains(err.Error(), "without a version stamp") ||
			!strings.Contains(err.Error(), "--target turf-cli") {
			t.Fatalf("got %v", err)
		}
	})

	t.Run("empty output", func(t *testing.T) {
		c := fakeTurfc(t, dir, "")
		err := Handshake(c, "/x/turf", "2.0.0-dev")
		if err == nil || !strings.Contains(err.Error(), "printed nothing") || !strings.Contains(err.Error(), c) {
			t.Fatalf("got %v", err)
		}
	})

	t.Run("output that is not a turfc version line", func(t *testing.T) {
		c := fakeTurfc(t, dir, "GNU bash 5.2\n")
		err := Handshake(c, "/x/turf", "2.0.0-dev")
		if err == nil || !strings.Contains(err.Error(), "does not look like turfc") {
			t.Fatalf("got %v", err)
		}
	})

	t.Run("a compiler that will not run", func(t *testing.T) {
		err := Handshake(filepath.Join(dir, "no-such-file"), "/x/turf", "2.0.0-dev")
		if err == nil || !strings.Contains(err.Error(), "no-such-file") {
			t.Fatalf("got %v", err)
		}
	})
}

func TestVersionLine(t *testing.T) {
	c := fakeTurfc(t, t.TempDir(), "turfc 2.0.0-dev\nextra\n")
	got, err := VersionLine(c)
	if err != nil || got != "turfc 2.0.0-dev" {
		t.Fatalf("got %q, %v", got, err)
	}
}
