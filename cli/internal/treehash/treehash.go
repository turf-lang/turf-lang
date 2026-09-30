// Package treehash computes the content hash `turf.lock` pins for a
// dependency's extracted source tree (docs/development/FREEZE.md 6.16,
// F-PKG-3: the lockfile pins commit and SHA-256, "verified on every build").
//
// It hashes the extracted tree in the cache, not git's objects, because that
// tree is what the compiler reads: a file edited or corrupted after extraction
// must be caught even though the commit it came from is intact.
package treehash

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// Prefix names the hash algorithm and, with Version, the format below. The
// lockfile stores "sha256:<hex>".
const Prefix = "sha256:"

// Version is the tree-hash format. It is implied by turf.lock's own `version`:
// changing what is hashed (a new entry kind, permission bits) changes every
// hash, so it must bump the lockfile's version and never be done silently.
const Version = 1

type entry struct {
	path string // slash-separated, relative to the tree root
	line string
}

// Dir returns the hash of the tree rooted at root.
//
// Format, version 1: for every regular file and every symlink, one line
//
//	<hex sha256 of the file's bytes, or of the symlink's target string> <f|l> <path>\n
//
// with the path relative to root and slash-separated, sorted by path in byte
// order; the result is the SHA-256 of the concatenated lines. So it depends
// only on names and contents: not on the order the filesystem lists entries,
// nor on mtimes, permission bits, ownership or empty directories (FREEZE's
// "independent of mtimes and permissions"), and it changes if a byte, a name or
// a symlink target does.
//
// Any path component named `.git` is skipped, whether directory or file: an
// extracted tree never has one (`git archive` does not emit it), but a tree a
// user copied by hand might, and a repository's own metadata is not source.
//
// A path containing a newline is refused. The line format is newline
// terminated, so such a name could spell a second, forged line and give two
// different trees the same hash. (A carriage return cannot forge one, so it
// is hashed like any other byte.)
func Dir(root string) (string, error) {
	// WalkDir does not follow a symlink given as the root, and a cache
	// directory can legitimately sit behind one (macOS temp dirs, a symlinked
	// $HOME/.cache); resolving it here keeps the hash a function of the tree.
	root, err := filepath.EvalSymlinks(root)
	if err != nil {
		return "", err
	}
	var entries []entry
	err = filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if p == root {
			return nil
		}
		if d.Name() == ".git" {
			if d.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		rel, err := filepath.Rel(root, p)
		if err != nil {
			return err
		}
		rel = filepath.ToSlash(rel)
		if strings.Contains(rel, "\n") {
			return fmt.Errorf("cannot hash %s: the path %q contains a newline", root, rel)
		}
		var kind string
		var sum [sha256.Size]byte
		switch {
		case d.Type()&fs.ModeSymlink != 0:
			target, err := os.Readlink(p)
			if err != nil {
				return err
			}
			kind, sum = "l", sha256.Sum256([]byte(target))
		case d.Type().IsRegular():
			h := sha256.New()
			f, err := os.Open(p)
			if err != nil {
				return err
			}
			_, err = io.Copy(h, f)
			f.Close()
			if err != nil {
				return err
			}
			kind = "f"
			copy(sum[:], h.Sum(nil))
		default:
			// Directories are implied by their files; sockets, devices and
			// the like cannot come out of git and are not source.
			return nil
		}
		entries = append(entries, entry{rel, hex.EncodeToString(sum[:]) + " " + kind + " " + rel + "\n"})
		return nil
	})
	if err != nil {
		return "", err
	}
	sort.Slice(entries, func(i, j int) bool { return entries[i].path < entries[j].path })
	all := sha256.New()
	for _, e := range entries {
		io.WriteString(all, e.line)
	}
	return Prefix + hex.EncodeToString(all.Sum(nil)), nil
}
