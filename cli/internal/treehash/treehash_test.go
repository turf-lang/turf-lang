package treehash

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
)

func put(t *testing.T, root, rel, content string) string {
	t.Helper()
	p := filepath.Join(root, filepath.FromSlash(rel))
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(p, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

func hash(t *testing.T, root string) string {
	t.Helper()
	h, err := Dir(root)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(h, "sha256:") || len(h) != len("sha256:")+64 {
		t.Fatalf("malformed hash %q", h)
	}
	return h
}

func symlink(t *testing.T, target, link string) {
	t.Helper()
	if err := os.Symlink(target, link); err != nil {
		t.Skipf("cannot create a symlink here: %v", err)
	}
}

// The format is part of turf.lock: a change to it invalidates every lock in the
// wild. This pins one tree to a hash computed outside Go, with
//
//	printf '%s f src/a.tr\n%s f turf.toml\n' $(printf 'y\n' | sha256sum) $(printf 'x\n' | sha256sum) | sha256sum
func TestKnownVector(t *testing.T) {
	root := t.TempDir()
	put(t, root, "turf.toml", "x\n")
	put(t, root, "src/a.tr", "y\n")
	const want = "sha256:77e7eb432a3586bc0ffb879220f7690f3cff2f95a7f92005b533eb4d7f8dce72"
	if got := hash(t, root); got != want {
		t.Fatalf("hash %s, want %s", got, want)
	}
}

func TestIgnoresCreationOrderMtimeAndPermissions(t *testing.T) {
	a, b := t.TempDir(), t.TempDir()
	put(t, a, "one.tr", "1")
	put(t, a, "d/two.tr", "2")
	put(t, a, "d/e/three.tr", "3")
	// Same tree, created in the opposite order.
	put(t, b, "d/e/three.tr", "3")
	put(t, b, "d/two.tr", "2")
	p := put(t, b, "one.tr", "1")
	if err := os.Chmod(p, 0o755); err != nil && runtime.GOOS != "windows" {
		t.Fatal(err)
	}
	old := time.Now().Add(-1000 * time.Hour)
	if err := os.Chtimes(p, old, old); err != nil {
		t.Fatal(err)
	}
	if hash(t, a) != hash(t, b) {
		t.Fatal("order, mtime and permission bits must not change the hash")
	}
}

func TestEmptyDirectoriesDoNotCount(t *testing.T) {
	a, b := t.TempDir(), t.TempDir()
	put(t, a, "x.tr", "1")
	put(t, b, "x.tr", "1")
	if err := os.MkdirAll(filepath.Join(b, "empty", "deeper"), 0o755); err != nil {
		t.Fatal(err)
	}
	if hash(t, a) != hash(t, b) {
		t.Fatal("an empty directory must not change the hash")
	}
}

func TestContentRenameAndAdditionChangeIt(t *testing.T) {
	root := t.TempDir()
	p := put(t, root, "a.tr", "hello")
	base := hash(t, root)

	if err := os.WriteFile(p, []byte("hellp"), 0o644); err != nil { // one byte
		t.Fatal(err)
	}
	if hash(t, root) == base {
		t.Error("changing one byte must change the hash")
	}
	if err := os.WriteFile(p, []byte("hello"), 0o644); err != nil {
		t.Fatal(err)
	}
	if hash(t, root) != base {
		t.Fatal("restoring the byte must restore the hash")
	}

	if err := os.Rename(p, filepath.Join(root, "b.tr")); err != nil {
		t.Fatal(err)
	}
	if hash(t, root) == base {
		t.Error("renaming a file must change the hash")
	}
	if err := os.Rename(filepath.Join(root, "b.tr"), p); err != nil {
		t.Fatal(err)
	}

	put(t, root, "extra.tr", "")
	if hash(t, root) == base {
		t.Error("adding an empty file must change the hash")
	}
}

// A path is part of the line, so moving a file into a directory of the same
// total spelling cannot collide with a differently split one.
func TestPathIsNotJustItsBytes(t *testing.T) {
	a, b := t.TempDir(), t.TempDir()
	put(t, a, "ab/c.tr", "1")
	put(t, b, "a/bc.tr", "1")
	if hash(t, a) == hash(t, b) {
		t.Fatal("different paths must hash differently")
	}
}

func TestSymlinkTargetIsHashedNotFollowed(t *testing.T) {
	root := t.TempDir()
	put(t, root, "real.tr", "data")
	symlink(t, "real.tr", filepath.Join(root, "link.tr"))
	base := hash(t, root)

	if err := os.Remove(filepath.Join(root, "link.tr")); err != nil {
		t.Fatal(err)
	}
	symlink(t, "other.tr", filepath.Join(root, "link.tr")) // dangling, different target
	if hash(t, root) == base {
		t.Error("a different symlink target must change the hash")
	}

	// A symlink is not the file it points at: same bytes as a regular file is a
	// different entry kind.
	if err := os.Remove(filepath.Join(root, "link.tr")); err != nil {
		t.Fatal(err)
	}
	put(t, root, "link.tr", "real.tr")
	if hash(t, root) == base {
		t.Error("a regular file must not hash like a symlink with the same text")
	}
}

func TestSymlinkToADirectoryIsNotWalked(t *testing.T) {
	root := t.TempDir()
	outside := t.TempDir()
	put(t, outside, "secret.tr", "not part of the tree")
	put(t, root, "a.tr", "1")
	symlink(t, outside, filepath.Join(root, "out"))
	base := hash(t, root)
	put(t, outside, "secret.tr", "changed")
	if hash(t, root) != base {
		t.Fatal("the contents behind a directory symlink must not count")
	}
}

func TestGitIsSkipped(t *testing.T) {
	a, b := t.TempDir(), t.TempDir()
	put(t, a, "x.tr", "1")
	put(t, b, "x.tr", "1")
	put(t, b, ".git/HEAD", "ref: refs/heads/main")
	put(t, b, "sub/.git", "gitdir: ../../elsewhere") // a submodule-style .git file
	put(t, b, "sub2/.git/objects/x", "y")
	if hash(t, a) != hash(t, b) {
		t.Fatal("a .git directory or file must not change the hash")
	}
	// Only the component name matters, not a name that merely contains it.
	put(t, b, ".github/ci.yml", "z")
	if hash(t, a) == hash(t, b) {
		t.Fatal(".github is not .git")
	}
}

func TestNewlineInAPathIsRefused(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("Windows cannot name a file with a newline")
	}
	root := t.TempDir()
	put(t, root, "a.tr", "1")
	if err := os.WriteFile(filepath.Join(root, "b\nc.tr"), []byte("2"), 0o644); err != nil {
		t.Skipf("filesystem refuses the name: %v", err)
	}
	_, err := Dir(root)
	if err == nil || !strings.Contains(err.Error(), "newline") {
		t.Fatalf("want a newline error, got %v", err)
	}
}

func TestMissingRootIsAnError(t *testing.T) {
	if _, err := Dir(filepath.Join(t.TempDir(), "nope")); err == nil {
		t.Fatal("hashing a missing directory must fail, not hash nothing")
	}
}

func TestSymlinkedRootIsResolved(t *testing.T) {
	real := t.TempDir()
	put(t, real, "a.tr", "1")
	link := filepath.Join(t.TempDir(), "l")
	symlink(t, real, link)
	if hash(t, link) != hash(t, real) {
		t.Fatal("a root reached through a symlink must hash like the real directory")
	}
}
