package main

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/turf-lang/turf-cli/internal/manifest"
)

func TestSplitNetFlags(t *testing.T) {
	nf, rest := splitNetFlags([]string{"-O2", "--offline", "-g", "--locked"})
	if !nf.offline || !nf.locked || !reflect.DeepEqual(rest, []string{"-O2", "-g"}) {
		t.Fatalf("got %+v %q", nf, rest)
	}
	nf, rest = splitNetFlags([]string{"-O2"})
	if nf.offline || nf.locked || !reflect.DeepEqual(rest, []string{"-O2"}) {
		t.Fatalf("got %+v %q", nf, rest)
	}
}

func writeFile(t *testing.T, path, content string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}

// A path-only project: app depends on ../geo.
func pathProject(t *testing.T) *manifest.Manifest {
	t.Helper()
	root, err := filepath.EvalSymlinks(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	writeFile(t, filepath.Join(root, "app", "turf.toml"),
		"[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\ngeo = { path = \"../geo\" }\n")
	writeFile(t, filepath.Join(root, "geo", "turf.toml"), "[package]\nname = \"geo\"\nversion = \"0.1.0\"\n")
	writeFile(t, filepath.Join(root, "geo", "src", "geo.tr"), "pub fn f(): int { return 1; }\n")
	m, err := manifest.Load(filepath.Join(root, "app", "turf.toml"))
	if err != nil {
		t.Fatal(err)
	}
	return m
}

func TestLockIsWrittenEvenForPathOnlyProjects(t *testing.T) {
	m := pathProject(t)
	g, old, err := resolveGraph(m, netFlags{}, nil)
	if err != nil || old != nil {
		t.Fatalf("resolve: %v (old lock %v)", err, old)
	}
	if err := commitLock(m, g, old, netFlags{}); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(lockPath(m))
	if err != nil || !strings.Contains(string(data), "name = \"geo\"\nsource = \"path+../geo\"\n") {
		t.Fatalf("turf.lock: %q %v", data, err)
	}

	// Building again changes nothing, not even the file's mtime.
	st1, _ := os.Stat(lockPath(m))
	g, old, err = resolveGraph(m, netFlags{}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if err := commitLock(m, g, old, netFlags{}); err != nil {
		t.Fatal(err)
	}
	again, _ := os.ReadFile(lockPath(m))
	st2, _ := os.Stat(lockPath(m))
	if string(again) != string(data) || !st1.ModTime().Equal(st2.ModTime()) {
		t.Error("an unchanged lock must not be rewritten")
	}
}

func TestLockedRefusesAMissingLockAndAStaleOne(t *testing.T) {
	m := pathProject(t)
	locked := netFlags{locked: true}

	_, _, err := resolveGraph(m, locked, nil)
	if err == nil || !strings.Contains(err.Error(), "--locked needs turf.lock, and there is none") {
		t.Fatalf("missing lock: %v", err)
	}
	if _, err := os.Stat(lockPath(m)); err == nil {
		t.Fatal("--locked must not create the lock")
	}

	// A stale lock: it lists a package the graph no longer has and misses one it does.
	stale := "version = 1\n\n[[package]]\nname = \"gone\"\nsource = \"path+../gone\"\n"
	writeFile(t, lockPath(m), stale)
	g, old, err := resolveGraph(m, locked, nil)
	if err != nil {
		t.Fatal(err)
	}
	err = commitLock(m, g, old, locked)
	if err == nil {
		t.Fatal("a stale lock must be refused")
	}
	for _, frag := range []string{"out of date", "--locked forbids changing it (nothing was written)", "added 'geo'", "removed 'gone'"} {
		if !strings.Contains(err.Error(), frag) {
			t.Errorf("error %q lacks %q", err, frag)
		}
	}
	if b, _ := os.ReadFile(lockPath(m)); string(b) != stale {
		t.Error("--locked rewrote the lock it refused")
	}
}

func TestMalformedLockIsAnErrorEvenWithoutLocked(t *testing.T) {
	m := pathProject(t)
	writeFile(t, lockPath(m), "version = 9\n")
	_, _, err := resolveGraph(m, netFlags{}, nil)
	if err == nil || !strings.Contains(err.Error(), "version 9 is not supported") || !strings.Contains(err.Error(), "delete it and run `turf build`") {
		t.Fatalf("got %v", err)
	}
}
