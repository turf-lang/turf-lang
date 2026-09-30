package gitdep

import (
	"archive/tar"
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/turf-lang/turf-lang/cli/internal/lockfile"
	"github.com/turf-lang/turf-lang/cli/internal/treehash"
)

// These tests run the real git against bare repositories made in a temp dir:
// no network, and no mocking of the very program whose behaviour (a mirror, a
// force-moved tag, `git archive`) the design leans on.

func needGit(t *testing.T) {
	t.Helper()
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git is not installed")
	}
}

// git runs git in dir with an environment that cannot see the developer's own
// configuration (autocrlf, commit signing, insteadOf rewrites) or identity.
func git(t *testing.T, dir string, args ...string) string {
	t.Helper()
	cmd := exec.Command("git", args...)
	cmd.Dir = dir
	cmd.Env = append(os.Environ(),
		"GIT_CONFIG_GLOBAL="+os.DevNull, "GIT_CONFIG_NOSYSTEM=1", "GIT_TERMINAL_PROMPT=0",
		"GIT_AUTHOR_NAME=t", "GIT_AUTHOR_EMAIL=t@t", "GIT_COMMITTER_NAME=t", "GIT_COMMITTER_EMAIL=t@t",
	)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("git %s: %v\n%s", strings.Join(args, " "), err, out)
	}
	return strings.TrimSpace(string(out))
}

// upstream is a bare repository plus the scratch clone commits are made in.
type upstream struct {
	t       *testing.T
	bare    string
	scratch string
}

func newUpstream(t *testing.T) *upstream {
	t.Helper()
	needGit(t)
	base := t.TempDir()
	u := &upstream{t: t, bare: filepath.Join(base, "up.git"), scratch: filepath.Join(base, "scratch")}
	git(t, base, "init", "--bare", "--quiet", "-b", "main", u.bare)
	git(t, base, "clone", "--quiet", u.bare, u.scratch)
	git(t, u.scratch, "checkout", "--quiet", "-B", "main")
	return u
}

// commit writes files, commits and pushes; it returns the new commit.
func (u *upstream) commit(files map[string]string) string {
	u.t.Helper()
	for name, content := range files {
		p := filepath.Join(u.scratch, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			u.t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte(content), 0o644); err != nil {
			u.t.Fatal(err)
		}
	}
	git(u.t, u.scratch, "add", "-A")
	git(u.t, u.scratch, "commit", "--quiet", "-m", "c")
	git(u.t, u.scratch, "push", "--quiet", "origin", "main")
	return git(u.t, u.scratch, "rev-parse", "HEAD")
}

func (u *upstream) tag(name string) {
	u.t.Helper()
	git(u.t, u.scratch, "tag", "-f", name)
	git(u.t, u.scratch, "push", "--quiet", "--force", "origin", "refs/tags/"+name)
}

func (u *upstream) req(ref string) Request {
	return Request{Name: "jsonx", URL: u.bare, Ref: ref, File: "/p/turf.toml"}
}

func cacheEnv(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	t.Setenv("TURF_CACHE_DIR", dir)
	return dir
}

func lockOf(req Request, tr *Tree) *lockfile.Lock {
	return &lockfile.Lock{Packages: []lockfile.Package{{
		Name: req.Name, Source: lockfile.GitSource(req.URL), Ref: req.Ref, Commit: tr.Commit, Hash: tr.Hash,
	}}}
}

func mustMaterialize(t *testing.T, f *Fetcher, req Request) *Tree {
	t.Helper()
	tr, err := f.Materialize(req)
	if err != nil {
		t.Fatalf("Materialize: %v", err)
	}
	return tr
}

func mustFailWith(t *testing.T, err error, frags ...string) {
	t.Helper()
	if err == nil {
		t.Fatalf("expected an error containing %q", frags)
	}
	for _, f := range frags {
		if !strings.Contains(err.Error(), f) {
			t.Errorf("error %q lacks %q", err.Error(), f)
		}
	}
}

func TestTagDependencyIsExtractedPinnedAndHashed(t *testing.T) {
	cache := cacheEnv(t)
	u := newUpstream(t)
	c := u.commit(map[string]string{"turf.toml": "x", "src/jsonx.tr": "pub fn f(): int { return 1; }\n", "a/b/c.txt": "deep"})
	u.tag("v1.2.0")

	tr := mustMaterialize(t, NewFetcher(nil, false), u.req("tag=v1.2.0"))
	if tr.Commit != c || len(tr.Commit) != 40 {
		t.Errorf("commit %q, want %q", tr.Commit, c)
	}
	if want := filepath.Join(cache, "src", "jsonx-"+c); tr.Dir != want {
		t.Errorf("tree at %q, want %q", tr.Dir, want)
	}
	if b, err := os.ReadFile(filepath.Join(tr.Dir, "a", "b", "c.txt")); err != nil || string(b) != "deep" {
		t.Errorf("nested file: %q %v", b, err)
	}
	if _, err := os.Stat(filepath.Join(tr.Dir, ".git")); err == nil {
		t.Error("an extracted tree must never contain .git")
	}
	if h, err := treehash.Dir(tr.Dir); err != nil || h != tr.Hash || !strings.HasPrefix(h, "sha256:") {
		t.Errorf("Tree.Hash %q is not the tree's hash %q (%v)", tr.Hash, h, err)
	}
	// A bare mirror of the URL is in the cache, under the hash of the URL.
	mirrors, _ := filepath.Glob(filepath.Join(cache, "git", "*"))
	if len(mirrors) != 1 || len(filepath.Base(mirrors[0])) != 16 {
		t.Errorf("want one 16-character mirror directory, got %v", mirrors)
	}
	// No leftovers from the extraction next to the tree.
	entries, _ := os.ReadDir(filepath.Join(cache, "src"))
	if len(entries) != 1 {
		t.Errorf("src/ should hold only the tree, has %v", entries)
	}
}

func TestExecutableBitsAndSymlinksSurviveExtraction(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("modes and symlinks are not portable to Windows")
	}
	cacheEnv(t)
	u := newUpstream(t)
	u.commit(map[string]string{"run.sh": "#!/bin/sh\n", "real.txt": "r"})
	if err := os.Chmod(filepath.Join(u.scratch, "run.sh"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink("real.txt", filepath.Join(u.scratch, "link.txt")); err != nil {
		t.Fatal(err)
	}
	git(t, u.scratch, "add", "-A")
	git(t, u.scratch, "commit", "--quiet", "-m", "modes")
	git(t, u.scratch, "push", "--quiet", "origin", "main")
	u.tag("v1")

	tr := mustMaterialize(t, NewFetcher(nil, false), u.req("tag=v1"))
	st, err := os.Stat(filepath.Join(tr.Dir, "run.sh"))
	if err != nil || st.Mode()&0o111 == 0 {
		t.Errorf("run.sh lost its executable bit: %v %v", st, err)
	}
	target, err := os.Readlink(filepath.Join(tr.Dir, "link.txt"))
	if err != nil || target != "real.txt" {
		t.Errorf("symlink: %q %v", target, err)
	}
}

func TestBranchAndAbbreviatedRevResolveToFullCommits(t *testing.T) {
	cacheEnv(t)
	u := newUpstream(t)
	c1 := u.commit(map[string]string{"turf.toml": "1"})

	byBranch := mustMaterialize(t, NewFetcher(nil, false), u.req("branch=main"))
	byRev := mustMaterialize(t, NewFetcher(nil, false), u.req("rev="+c1[:7]))
	if byBranch.Commit != c1 || byRev.Commit != c1 {
		t.Errorf("branch -> %s, rev -> %s, want %s", byBranch.Commit, byRev.Commit, c1)
	}
	if byBranch.Dir != byRev.Dir || byBranch.Hash != byRev.Hash {
		t.Error("the same commit must be the same tree however it was asked for")
	}
}

func TestUnknownRefsNameTheURLAndTheRef(t *testing.T) {
	cacheEnv(t)
	u := newUpstream(t)
	u.commit(map[string]string{"turf.toml": "1"})
	f := NewFetcher(nil, false)
	for _, tc := range []struct{ ref, frag string }{
		{"tag=nope", "has no tag named 'nope'"},
		{"branch=nope", "has no branch named 'nope'"},
		{"rev=deadbeef", "has no commit named 'deadbeef'"},
	} {
		_, err := f.Materialize(u.req(tc.ref))
		mustFailWith(t, err, u.bare, tc.frag, "'jsonx'")
	}
	_, err := f.Materialize(u.req("rev=--upload-pack=x"))
	mustFailWith(t, err, "is not a commit hash")
}

func TestAnUnreachableRepositoryIsAFetchError(t *testing.T) {
	cacheEnv(t)
	needGit(t)
	_, err := NewFetcher(nil, false).Materialize(Request{Name: "x", URL: filepath.Join(t.TempDir(), "gone.git"), Ref: "tag=v1", File: "/p/turf.toml"})
	mustFailWith(t, err, "cannot fetch 'x' from", "declared in /p/turf.toml")
}

// The point of the cache and the lock: with both, the upstream is not needed.
func TestLockedAndCachedNeedsNoNetwork(t *testing.T) {
	cacheEnv(t)
	u := newUpstream(t)
	u.commit(map[string]string{"turf.toml": "1"})
	u.tag("v1")
	req := u.req("tag=v1")
	first := mustMaterialize(t, NewFetcher(nil, false), req)

	moved := u.bare + ".moved"
	if err := os.Rename(u.bare, moved); err != nil {
		t.Fatal(err)
	}
	for _, offline := range []bool{false, true} {
		tr := mustMaterialize(t, NewFetcher(lockOf(req, first), offline), req)
		if *tr != *first {
			t.Errorf("offline=%v: got %+v, want %+v", offline, tr, first)
		}
	}
}

func TestLockedTreeThatWasEditedIsAMismatch(t *testing.T) {
	cacheEnv(t)
	u := newUpstream(t)
	u.commit(map[string]string{"turf.toml": "1", "src/a.tr": "one"})
	u.tag("v1")
	req := u.req("tag=v1")
	first := mustMaterialize(t, NewFetcher(nil, false), req)

	if err := os.WriteFile(filepath.Join(first.Dir, "src", "a.tr"), []byte("two"), 0o644); err != nil {
		t.Fatal(err)
	}
	_, err := NewFetcher(lockOf(req, first), false).Materialize(req)
	mustFailWith(t, err, "the cached tree of 'jsonx' ("+first.Dir+")", "does not match turf.lock",
		"expected "+first.Hash, "it was modified or corrupted",
		"hint: delete "+first.Dir+" and build again to re-extract it; if turf.lock itself is wrong, 'turf update jsonx'")

	// The advice works: delete the tree and the same lock is satisfied again.
	if err := os.RemoveAll(first.Dir); err != nil {
		t.Fatal(err)
	}
	if tr := mustMaterialize(t, NewFetcher(lockOf(req, first), false), req); tr.Hash != first.Hash {
		t.Errorf("re-extraction gave %s, want %s", tr.Hash, first.Hash)
	}
	// An extra file counts as well as an edit.
	if err := os.WriteFile(filepath.Join(first.Dir, "extra.tr"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	_, err = NewFetcher(lockOf(req, first), false).Materialize(req)
	mustFailWith(t, err, "does not match turf.lock")
}

func TestAWrongHashInTheLockFailsEvenOnAFreshExtraction(t *testing.T) {
	cacheEnv(t)
	u := newUpstream(t)
	u.commit(map[string]string{"turf.toml": "1"})
	u.tag("v1")
	req := u.req("tag=v1")
	first := mustMaterialize(t, NewFetcher(nil, false), req)
	bad := lockOf(req, first)
	bad.Packages[0].Hash = "sha256:" + strings.Repeat("0", 64)

	// Tree missing: the extraction is verified before it is installed, and a
	// tree that fails is not left in the cache.
	if err := os.RemoveAll(first.Dir); err != nil {
		t.Fatal(err)
	}
	_, err := NewFetcher(bad, false).Materialize(req)
	mustFailWith(t, err, "does not match turf.lock", "expected sha256:"+strings.Repeat("0", 64), "found "+first.Hash)
	if _, err := os.Stat(first.Dir); err == nil {
		t.Error("a tree that failed verification must not be installed")
	}
}

func TestOfflineMessages(t *testing.T) {
	cacheEnv(t)
	u := newUpstream(t)
	u.commit(map[string]string{"turf.toml": "1"})
	u.tag("v1")
	req := u.req("tag=v1")

	// Empty cache, nothing locked.
	_, err := NewFetcher(nil, true).Materialize(req)
	mustFailWith(t, err, "'jsonx'", u.bare, "tag=v1", "is not in turf.lock", "needs the network; run without --offline")

	first := mustMaterialize(t, NewFetcher(nil, false), req)
	// Cached and locked, but the ref in the manifest changed: an unlocked
	// request must not be resolved from a mirror that may be stale.
	req2 := u.req("branch=main")
	_, err = NewFetcher(lockOf(req, first), true).Materialize(req2)
	mustFailWith(t, err, "is not in turf.lock", "needs the network; run without --offline")

	// Locked, but both the tree and the mirror are gone.
	if err := os.RemoveAll(first.Dir); err != nil {
		t.Fatal(err)
	}
	mirrors, _ := filepath.Glob(filepath.Join(os.Getenv("TURF_CACHE_DIR"), "git", "*"))
	for _, m := range mirrors {
		os.RemoveAll(m)
	}
	_, err = NewFetcher(lockOf(req, first), true).Materialize(req)
	mustFailWith(t, err, "'jsonx'", "is locked (commit "+first.Commit[:12]+")", "needs the network; run without --offline")
}

// Locked, tree gone, mirror still holds the commit: no network is needed even
// with --offline, because rebuilding the tree from the mirror is local.
func TestLockedTreeIsRebuiltFromTheMirrorOffline(t *testing.T) {
	cacheEnv(t)
	u := newUpstream(t)
	u.commit(map[string]string{"turf.toml": "1"})
	u.tag("v1")
	req := u.req("tag=v1")
	first := mustMaterialize(t, NewFetcher(nil, false), req)
	if err := os.RemoveAll(first.Dir); err != nil {
		t.Fatal(err)
	}
	if err := os.Rename(u.bare, u.bare+".gone"); err != nil {
		t.Fatal(err)
	}
	tr := mustMaterialize(t, NewFetcher(lockOf(req, first), true), req)
	if tr.Hash != first.Hash {
		t.Errorf("hash %s, want %s", tr.Hash, first.Hash)
	}
}

// F-PKG-3: a moved branch or a re-pointed tag is invisible to a build that has
// a lock, and only an update moves it.
func TestOnlyUpdateMovesABranchOrATag(t *testing.T) {
	cacheEnv(t)
	u := newUpstream(t)
	c1 := u.commit(map[string]string{"turf.toml": "1"})
	u.tag("v1")

	for _, ref := range []string{"branch=main", "tag=v1"} {
		req := u.req(ref)
		first := mustMaterialize(t, NewFetcher(nil, false), req)
		if first.Commit != c1 && ref == "branch=main" {
			// A previous iteration already moved main; start the table over.
			c1 = first.Commit
		}
		before := first.Commit

		c2 := u.commit(map[string]string{"turf.toml": "2" + ref})
		u.tag("v1") // force-move the tag to the new commit as well

		// A build with the lock: still the old commit, and no network needed to say so.
		kept := mustMaterialize(t, NewFetcher(lockOf(req, first), false), req)
		if kept.Commit != before {
			t.Errorf("%s: a build moved the pin from %s to %s", ref, before, kept.Commit)
		}
		// An update: the new commit.
		f := NewFetcher(lockOf(req, first), false)
		f.Update = func(name string) bool { return name == "jsonx" }
		moved := mustMaterialize(t, f, req)
		if moved.Commit != c2 {
			t.Errorf("%s: update gave %s, want %s", ref, moved.Commit, c2)
		}
		// An update naming another package leaves this one alone.
		g := NewFetcher(lockOf(req, first), false)
		g.Update = func(name string) bool { return name == "other" }
		if same := mustMaterialize(t, g, req); same.Commit != before {
			t.Errorf("%s: an update of another package moved this one", ref)
		}
		c1 = c2
	}
}

func TestUpdateOfARevIsANoOp(t *testing.T) {
	cacheEnv(t)
	u := newUpstream(t)
	c1 := u.commit(map[string]string{"turf.toml": "1"})
	req := u.req("rev=" + c1[:8])
	first := mustMaterialize(t, NewFetcher(nil, false), req)
	u.commit(map[string]string{"turf.toml": "2"})
	f := NewFetcher(lockOf(req, first), false)
	f.Update = func(string) bool { return true }
	if moved := mustMaterialize(t, f, req); *moved != *first {
		t.Errorf("a rev resolves to the same commit: got %+v want %+v", moved, first)
	}
}

// The same commit id with different content should be impossible. It is what
// the hash is for, so an update that resolves to the locked commit but a
// different tree must say so instead of silently re-pinning.
func TestSameCommitDifferentContentIsRefused(t *testing.T) {
	cacheEnv(t)
	u := newUpstream(t)
	u.commit(map[string]string{"turf.toml": "1"})
	u.tag("v1")
	req := u.req("tag=v1")
	first := mustMaterialize(t, NewFetcher(nil, false), req)
	lie := lockOf(req, first)
	lie.Packages[0].Hash = "sha256:" + strings.Repeat("1", 64)
	f := NewFetcher(lie, false)
	f.Update = func(string) bool { return true }
	_, err := f.Materialize(req)
	mustFailWith(t, err, "does not match turf.lock")
}

func TestAManifestRefChangeIsANewRequest(t *testing.T) {
	cacheEnv(t)
	u := newUpstream(t)
	c1 := u.commit(map[string]string{"turf.toml": "1"})
	u.tag("v1")
	c2 := u.commit(map[string]string{"turf.toml": "2"})
	u.tag("v2")
	first := mustMaterialize(t, NewFetcher(nil, false), u.req("tag=v1"))
	if first.Commit != c1 {
		t.Fatalf("v1 is %s, want %s", first.Commit, c1)
	}
	// The lock says tag=v1; the manifest now says tag=v2: resolve afresh.
	second := mustMaterialize(t, NewFetcher(lockOf(u.req("tag=v1"), first), false), u.req("tag=v2"))
	if second.Commit != c2 {
		t.Errorf("v2 resolved to %s, want %s", second.Commit, c2)
	}
	// A different URL for the same name is also not the locked request.
	other := *first
	l := lockOf(u.req("tag=v1"), &other)
	l.Packages[0].Source = lockfile.GitSource("/somewhere/else.git")
	third := mustMaterialize(t, NewFetcher(l, false), u.req("tag=v1"))
	if third.Commit != c1 {
		t.Errorf("got %s", third.Commit)
	}
}

func TestResolveCommitComparesRefsWithoutExtracting(t *testing.T) {
	cache := cacheEnv(t)
	u := newUpstream(t)
	c1 := u.commit(map[string]string{"turf.toml": "1"})
	u.tag("v1")
	c2 := u.commit(map[string]string{"turf.toml": "2"})
	u.tag("v2")
	f := NewFetcher(nil, false)
	for ref, want := range map[string]string{"tag=v1": c1, "tag=v2": c2, "branch=main": c2, "rev=" + c1[:9]: c1} {
		got, err := f.ResolveCommit(u.req(ref))
		if err != nil || got != want {
			t.Errorf("ResolveCommit(%s) = %s, %v; want %s", ref, got, err, want)
		}
	}
	if _, err := os.Stat(filepath.Join(cache, "src")); err == nil {
		t.Error("ResolveCommit must not extract a tree")
	}
	// A ref newer than the mirror makes it refresh, once.
	c3 := u.commit(map[string]string{"turf.toml": "3"})
	u.tag("v3")
	// (A new Fetcher is a new process here: this one already cloned the mirror,
	// which counts as its one refresh.)
	if got, err := NewFetcher(nil, false).ResolveCommit(u.req("tag=v3")); err != nil || got != c3 {
		t.Errorf("ResolveCommit(v3) after the mirror went stale = %s, %v; want %s", got, err, c3)
	}
	_, err := NewFetcher(nil, true).ResolveCommit(u.req("tag=v4"))
	mustFailWith(t, err, "has no tag named 'v4'")
}

func TestOneMirrorRefreshPerProcess(t *testing.T) {
	cacheEnv(t)
	u := newUpstream(t)
	u.commit(map[string]string{"turf.toml": "1"})
	u.tag("v1")
	var log bytes.Buffer
	f := NewFetcher(nil, false)
	f.Log = &log
	mustMaterialize(t, f, u.req("tag=v1"))
	r2 := u.req("tag=v1")
	r2.Name = "other"
	mustMaterialize(t, f, r2)
	if n := strings.Count(log.String(), "turf: fetching"); n != 1 {
		t.Errorf("one URL should be fetched once per process, log:\n%s", log.String())
	}
}

func TestGitMissingFromPath(t *testing.T) {
	needGit(t)
	t.Setenv("PATH", t.TempDir())
	t.Setenv("TURF_CACHE_DIR", t.TempDir())
	_, err := NewFetcher(nil, false).Materialize(Request{Name: "jsonx", URL: "u", Ref: "tag=v1", File: "/p/turf.toml"})
	if err == nil || err.Error() != "git dependencies need the 'git' program, which is not on PATH ('jsonx' in /p/turf.toml)" {
		t.Fatalf("got %v", err)
	}
}

func TestNoCacheDirectoryIsReportedOnUse(t *testing.T) {
	needGit(t)
	t.Setenv("TURF_CACHE_DIR", "")
	t.Setenv("HOME", "")
	t.Setenv("XDG_CACHE_HOME", "")
	if runtime.GOOS != "linux" {
		t.Skip("the cache directory lookup fails only on Linux-style environments")
	}
	f := NewFetcher(nil, false) // must not fail: a path-only project never gets here
	_, err := f.Materialize(Request{Name: "x", URL: "u", Ref: "tag=v1", File: "/p/turf.toml"})
	mustFailWith(t, err, "cannot find a cache directory", "TURF_CACHE_DIR")
}

func TestParseRef(t *testing.T) {
	for _, ok := range []string{"tag=v1", "branch=feat/x", "rev=abcd"} {
		if _, _, err := ParseRef(ok); err != nil {
			t.Errorf("%s: %v", ok, err)
		}
	}
	for _, bad := range []string{"", "tag", "tag=", "commit=abc", "=x"} {
		if _, _, err := ParseRef(bad); err == nil {
			t.Errorf("%q should be rejected", bad)
		}
	}
}

// git archive cannot produce these, but the reader is the last line between an
// archive and the filesystem, so it is tested with hand-made ones.
func TestUnpackRefusesEscapesAndOddEntries(t *testing.T) {
	mk := func(entries ...tar.Header) *bytes.Reader {
		var b bytes.Buffer
		w := tar.NewWriter(&b)
		for _, h := range entries {
			h := h
			if h.Typeflag == tar.TypeReg {
				h.Size = 1
			}
			if err := w.WriteHeader(&h); err != nil {
				t.Fatal(err)
			}
			if h.Typeflag == tar.TypeReg {
				w.Write([]byte("x"))
			}
		}
		w.Close()
		return bytes.NewReader(b.Bytes())
	}
	tests := []struct {
		name string
		in   *bytes.Reader
		frag string
	}{
		{"dotdot", mk(tar.Header{Name: "../evil", Typeflag: tar.TypeReg, Mode: 0o644}), "outside the tree"},
		{"nested dotdot", mk(tar.Header{Name: "a/../../evil", Typeflag: tar.TypeReg, Mode: 0o644}), "outside the tree"},
		{"absolute", mk(tar.Header{Name: "/etc/evil", Typeflag: tar.TypeReg, Mode: 0o644}), "outside the tree"},
		{"below a symlink", mk(
			tar.Header{Name: "l", Typeflag: tar.TypeSymlink, Linkname: "/tmp"},
			tar.Header{Name: "l/evil", Typeflag: tar.TypeReg, Mode: 0o644}), "below a symlink"},
		{"a device", mk(tar.Header{Name: "dev", Typeflag: tar.TypeChar}), "kind turf does not extract"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			err := unpack(tc.in, t.TempDir())
			mustFailWith(t, err, tc.frag)
		})
	}
	// And a plain archive is fine, including a global header like git's.
	var b bytes.Buffer
	w := tar.NewWriter(&b)
	w.WriteHeader(&tar.Header{Name: "pax_global_header", Typeflag: tar.TypeXGlobalHeader, PAXRecords: map[string]string{"comment": "abc"}})
	w.WriteHeader(&tar.Header{Name: "d/", Typeflag: tar.TypeDir, Mode: 0o755})
	w.WriteHeader(&tar.Header{Name: "d/f", Typeflag: tar.TypeReg, Mode: 0o644, Size: 2})
	w.Write([]byte("ok"))
	w.Close()
	dir := t.TempDir()
	if err := unpack(&b, dir); err != nil {
		t.Fatal(err)
	}
	if got, err := os.ReadFile(filepath.Join(dir, "d", "f")); err != nil || string(got) != "ok" {
		t.Fatalf("d/f: %q %v", got, err)
	}
}
