// Package gitdep fetches git dependencies into a cache and pins them
// (docs/development/FREEZE.md 6.16, F-PKG-3; design in
// docs/development/changes/LEC-001.md, "Stage 6 spec").
//
// The cache is disposable, and holds two things under $TURF_CACHE_DIR (else
// the OS user cache directory's `turf`):
//
//	git/<sha256(url)[:16]>/     a bare mirror of one repository URL
//	src/<name>-<commit>/        the extracted tree of one commit
//
// The compiler reads the extracted tree, never git objects, so every build
// verifies that tree against the hash in turf.lock. Nothing here touches the
// manifest or turf.lock: the caller hands in the lock it read and decides what
// to write, which keeps "who may move a pinned commit" in one place (the
// callers of Fetcher, and `turf update` above all).
package gitdep

import (
	"archive/tar"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"regexp"
	"strings"
	"time"

	"github.com/turf-lang/turf-lang/cli/internal/lockfile"
	"github.com/turf-lang/turf-lang/cli/internal/treehash"
)

// Request names one git dependency as one requirer wrote it.
type Request struct {
	Name string // the package name, which is also the manifest key
	URL  string
	Ref  string // "tag=…", "branch=…" or "rev=…", exactly as lockfile.Package.Ref
	// File is the turf.toml that declares the dependency, for messages.
	File string
}

// Tree is a dependency after it has been pinned and verified.
type Tree struct {
	Dir    string // the extracted tree in the cache
	Commit string // the full 40-hex commit
	Hash   string // "sha256:<hex>" of Dir
}

// Source is what the graph walker needs from a fetcher. It is an interface so
// project's tests can resolve graphs with a fake and so the package graph code
// never learns how git is run.
type Source interface {
	// Materialize pins req to a commit and returns its verified tree.
	Materialize(req Request) (*Tree, error)
	// ResolveCommit returns the commit req names without extracting it, for
	// deciding whether two requirers of one package agree.
	ResolveCommit(req Request) (string, error)
}

// Fetcher is the system-git implementation of Source.
type Fetcher struct {
	Cache   string            // the cache root
	Lock    *lockfile.Lock    // what the last build pinned; nil when there is none
	Offline bool              // run no network git command at all
	Update  func(string) bool // names whose pin `turf update` is moving; nil for none
	Log     io.Writer         // one line per network operation; nil to stay silent

	refreshed map[string]bool // mirrors already fetched by this process
	cacheErr  error           // why there is no cache directory, reported on first use
}

// NewFetcher returns a Fetcher over the default cache directory. It cannot
// fail: a project with no git dependencies never touches the cache, and must
// not be refused because a machine has no $HOME. The problem is reported the
// first time a git dependency needs the cache.
func NewFetcher(lock *lockfile.Lock, offline bool) *Fetcher {
	dir, err := CacheDir()
	return &Fetcher{Cache: dir, Lock: lock, Offline: offline, refreshed: map[string]bool{}, cacheErr: err}
}

// CacheDir is $TURF_CACHE_DIR if set, else <user cache dir>/turf. It is made
// absolute, because the cache path ends up in the package graph and in error
// messages and turfc runs in another working directory.
func CacheDir() (string, error) {
	dir := os.Getenv("TURF_CACHE_DIR")
	if dir == "" {
		base, err := os.UserCacheDir()
		if err != nil {
			return "", fmt.Errorf("cannot find a cache directory (%v); set TURF_CACHE_DIR", err)
		}
		dir = filepath.Join(base, "turf")
	}
	return filepath.Abs(dir)
}

// ParseRef splits "tag=v1" into ("tag", "v1").
func ParseRef(ref string) (kind, value string, err error) {
	kind, value, ok := strings.Cut(ref, "=")
	if !ok || value == "" || (kind != "tag" && kind != "branch" && kind != "rev") {
		return "", "", fmt.Errorf("malformed ref %q (want tag=…, branch=… or rev=…)", ref)
	}
	return kind, value, nil
}

var hexRe = regexp.MustCompile(`^[0-9a-fA-F]{4,40}$`)

// RequireGit fails with the spec's message when the git program is missing.
// It is checked for every git dependency, even one that is already cached: a
// build that works on one machine and prints a different error on the next
// because of what happens to be cached is harder to debug than one that says
// plainly what it needs.
func RequireGit(name, file string) error {
	if _, err := exec.LookPath("git"); err != nil {
		return fmt.Errorf("git dependencies need the 'git' program, which is not on PATH ('%s' in %s)", name, file)
	}
	return nil
}

// ready checks what every operation needs before it can start: the git
// program, and a cache directory to put things in.
func (f *Fetcher) ready(req Request) error {
	if err := RequireGit(req.Name, req.File); err != nil {
		return err
	}
	return f.cacheErr
}

func (f *Fetcher) mirrorDir(url string) string {
	sum := sha256.Sum256([]byte(url))
	return filepath.Join(f.Cache, "git", hex.EncodeToString(sum[:])[:16])
}

func (f *Fetcher) srcDir(name, commit string) string {
	return filepath.Join(f.Cache, "src", name+"-"+commit)
}

func (f *Fetcher) logf(format string, a ...any) {
	if f.Log != nil {
		fmt.Fprintf(f.Log, format+"\n", a...)
	}
}

func gitCmd(args ...string) *exec.Cmd {
	cmd := exec.Command("git", args...)
	// A private repository must fail with git's own message, never hang
	// waiting for a password on a terminal turf is not showing the user.
	cmd.Env = append(os.Environ(), "GIT_TERMINAL_PROMPT=0")
	return cmd
}

// run runs git and returns its trimmed stdout. On failure the error carries
// git's own stderr, unedited: turf never rewrites a tool's diagnostic.
func run(args ...string) (string, error) {
	cmd := gitCmd(args...)
	var stderr strings.Builder
	cmd.Stderr = &stderr
	out, err := cmd.Output()
	if err != nil {
		msg := strings.TrimSpace(stderr.String())
		if msg == "" {
			msg = err.Error()
		}
		return "", errors.New(msg)
	}
	return strings.TrimSpace(string(out)), nil
}

func exists(p string) bool {
	_, err := os.Stat(p)
	return err == nil
}

func offlineErr(req Request, why string) error {
	return fmt.Errorf("dependency '%s' (%s, %s) %s, so it needs the network; run without --offline", req.Name, req.URL, req.Ref, why)
}

// ensureMirror makes sure a mirror of req.URL exists and, when refresh is
// set, that it has been fetched by this process. Each URL is fetched at most
// once per process, however many packages come from it.
func (f *Fetcher) ensureMirror(req Request, refresh bool) (string, error) {
	m := f.mirrorDir(req.URL)
	if !exists(m) {
		if f.Offline {
			return "", offlineErr(req, "has no cached mirror")
		}
		if err := os.MkdirAll(filepath.Dir(m), 0o755); err != nil {
			return "", err
		}
		f.logf("turf: fetching %s (%s)", req.Name, req.URL)
		// Cloned beside the final name and renamed, so an interrupted clone is
		// not later mistaken for a mirror.
		tmp, err := os.MkdirTemp(filepath.Dir(m), ".clone-*")
		if err != nil {
			return "", err
		}
		defer os.RemoveAll(tmp)
		if _, err := run("clone", "--mirror", "--quiet", req.URL, tmp); err != nil {
			return "", fmt.Errorf("cannot fetch '%s' from %s (declared in %s): %v", req.Name, req.URL, req.File, err)
		}
		if err := os.Rename(tmp, m); err != nil {
			return "", err
		}
		f.refreshed[req.URL] = true
		return m, nil
	}
	if refresh && !f.refreshed[req.URL] {
		if f.Offline {
			return "", offlineErr(req, "would need its mirror refreshed")
		}
		f.logf("turf: fetching %s (%s)", req.Name, req.URL)
		if _, err := run("--git-dir="+m, "fetch", "--quiet", "--prune", "--tags", "origin"); err != nil {
			return "", fmt.Errorf("cannot fetch '%s' from %s (declared in %s): %v", req.Name, req.URL, req.File, err)
		}
		f.refreshed[req.URL] = true
	}
	return m, nil
}

// resolve turns a ref into a full commit inside a mirror.
func resolve(mirror string, req Request) (string, error) {
	kind, value, err := ParseRef(req.Ref)
	if err != nil {
		return "", fmt.Errorf("dependency '%s' in %s: %v", req.Name, req.File, err)
	}
	var spec string
	switch kind {
	case "tag":
		spec = "refs/tags/" + value + "^{commit}"
	case "branch":
		spec = "refs/heads/" + value + "^{commit}"
	case "rev":
		if !hexRe.MatchString(value) {
			return "", fmt.Errorf("dependency '%s' in %s: rev \"%s\" is not a commit hash (4 to 40 hex digits)", req.Name, req.File, value)
		}
		spec = value + "^{commit}"
	}
	out, err := run("--git-dir="+mirror, "rev-parse", "--verify", "--quiet", spec)
	if err != nil || !commitRe.MatchString(out) {
		return "", &unknownRefError{req: req, kind: kind, value: value}
	}
	return out, nil
}

var commitRe = regexp.MustCompile(`^[0-9a-f]{40}$`)

type unknownRefError struct {
	req         Request
	kind, value string
}

func (e *unknownRefError) Error() string {
	what := map[string]string{"tag": "tag", "branch": "branch", "rev": "commit"}[e.kind]
	return fmt.Sprintf("%s has no %s named '%s' (dependency '%s' in %s)", e.req.URL, what, e.value, e.req.Name, e.req.File)
}

func hasCommit(mirror, commit string) bool {
	_, err := run("--git-dir="+mirror, "cat-file", "-e", commit+"^{commit}")
	return err == nil
}

// locked returns the lock entry that pins req: same name, same source URL and
// same ref string (design item 6). Any difference means the manifest changed
// since the lock was written, so the entry is not evidence about this request.
func (f *Fetcher) locked(req Request) *lockfile.Package {
	p := f.Lock.Find(req.Name)
	if p == nil || !p.IsGit() || p.GitURL() != req.URL || p.Ref != req.Ref {
		return nil
	}
	return p
}

// ResolveCommit implements Source.
func (f *Fetcher) ResolveCommit(req Request) (string, error) {
	if err := f.ready(req); err != nil {
		return "", err
	}
	m, err := f.ensureMirror(req, false)
	if err != nil {
		return "", err
	}
	c, err := resolve(m, req)
	var unk *unknownRefError
	if errors.As(err, &unk) && !f.refreshed[req.URL] && !f.Offline {
		// The mirror may simply predate the ref.
		if m, err = f.ensureMirror(req, true); err != nil {
			return "", err
		}
		return resolve(m, req)
	}
	return c, err
}

// Materialize implements Source: the design's item 6.
//
// A lock entry with the same name, URL and ref wins, and is used with no
// network when its tree is cached. Otherwise the ref is resolved against a
// freshly fetched mirror. A tag that was re-pointed upstream, or a branch that
// moved, is therefore invisible here and only `turf update` (Update set)
// notices: F-PKG-3's "turf update is the only command that moves a ref".
func (f *Fetcher) Materialize(req Request) (*Tree, error) {
	if err := f.ready(req); err != nil {
		return nil, err
	}
	entry := f.locked(req)
	if entry != nil && (f.Update == nil || !f.Update(req.Name)) {
		return f.fromLock(req, entry)
	}
	return f.pinNew(req, entry)
}

func mismatch(name, dir, want, got string) error {
	return fmt.Errorf("the cached tree of '%s' (%s) does not match turf.lock (expected %s, found %s): it was modified or corrupted\nhint: delete %s and build again to re-extract it; if turf.lock itself is wrong, 'turf update %s'", name, dir, want, got, dir, name)
}

func (f *Fetcher) fromLock(req Request, e *lockfile.Package) (*Tree, error) {
	dir := f.srcDir(req.Name, e.Commit)
	if exists(dir) {
		got, err := treehash.Dir(dir)
		if err != nil {
			return nil, fmt.Errorf("cannot hash the cached tree of '%s' (%s): %v", req.Name, dir, err)
		}
		if got != e.Hash {
			return nil, mismatch(req.Name, dir, e.Hash, got)
		}
		return &Tree{Dir: dir, Commit: e.Commit, Hash: e.Hash}, nil
	}

	// The tree is gone but the pin is intact: rebuild the tree from git.
	m := f.mirrorDir(req.URL)
	if !exists(m) || !hasCommit(m, e.Commit) {
		if f.Offline {
			return nil, offlineErr(req, fmt.Sprintf("is locked (commit %s) but neither its tree nor the commit is cached", e.Commit[:12]))
		}
		var err error
		if m, err = f.ensureMirror(req, true); err != nil {
			return nil, err
		}
		if !hasCommit(m, e.Commit) {
			return nil, fmt.Errorf("commit %s of %s, which turf.lock pins for '%s' (declared in %s), is no longer available upstream\nhint: 'turf update %s' re-resolves %s", e.Commit, req.URL, req.Name, req.File, req.Name, req.Ref)
		}
	}
	tmp, got, err := f.extract(m, req.Name, e.Commit)
	if err != nil {
		return nil, err
	}
	if got != e.Hash {
		os.RemoveAll(tmp)
		// Never installed: a tree that fails verification is not cached.
		return nil, mismatch(req.Name, dir, e.Hash, got)
	}
	if err := install(tmp, dir); err != nil {
		return nil, err
	}
	return &Tree{Dir: dir, Commit: e.Commit, Hash: got}, nil
}

// pinNew resolves req's ref to a commit against a fresh mirror and extracts
// it. old is the lock entry being replaced (an update), or nil for a new pin.
func (f *Fetcher) pinNew(req Request, old *lockfile.Package) (*Tree, error) {
	if f.Offline {
		return nil, offlineErr(req, "is not in turf.lock")
	}
	m, err := f.ensureMirror(req, true)
	if err != nil {
		return nil, err
	}
	commit, err := resolve(m, req)
	if err != nil {
		return nil, err
	}
	tmp, got, err := f.extract(m, req.Name, commit)
	if err != nil {
		return nil, err
	}
	dir := f.srcDir(req.Name, commit)
	if old != nil && old.Commit == commit && old.Hash != got {
		// The same commit id with different content: the upstream history was
		// rewritten under it, which should be impossible, and is exactly what
		// the hash is for.
		os.RemoveAll(tmp)
		return nil, mismatch(req.Name, dir, old.Hash, got)
	}
	if exists(dir) {
		if have, err := treehash.Dir(dir); err == nil && have == got {
			os.RemoveAll(tmp)
			return &Tree{Dir: dir, Commit: commit, Hash: got}, nil
		}
		// A cached tree that is not what git says this commit contains is
		// replaced, not trusted: there is no pin yet to compare it against.
		if err := os.RemoveAll(dir); err != nil {
			os.RemoveAll(tmp)
			return nil, err
		}
	}
	if err := install(tmp, dir); err != nil {
		return nil, err
	}
	return &Tree{Dir: dir, Commit: commit, Hash: got}, nil
}

// install moves a fully extracted and verified tree into place. The rename is
// what makes extraction atomic: an interrupted fetch leaves a `.tmp-` directory
// (swept by the next extraction), never a half tree under a real name.
func install(tmp, dir string) error {
	if err := os.Rename(tmp, dir); err != nil {
		if exists(dir) {
			// Another turf finished first. Its tree was verified by the same
			// rules and the next build hashes it again anyway.
			os.RemoveAll(tmp)
			return nil
		}
		os.RemoveAll(tmp)
		return err
	}
	return nil
}

// extract writes the tree of commit into a temporary directory beside its
// final place and hashes it. `git archive` never emits .git, and the tar is
// read with Go's own reader so no `tar` program is needed.
//
// core.autocrlf is forced off: `git archive` applies working-tree conversion,
// so a user's global autocrlf=true would otherwise make the same commit
// extract to different bytes, and hash differently, on a colleague's machine.
// Conversions the repository asks for itself (.gitattributes) still apply, and
// are the same everywhere.
func (f *Fetcher) extract(mirror, name, commit string) (tmp, hash string, err error) {
	parent := filepath.Join(f.Cache, "src")
	if err = os.MkdirAll(parent, 0o755); err != nil {
		return "", "", err
	}
	sweep(parent)
	if tmp, err = os.MkdirTemp(parent, ".tmp-"+name+"-"); err != nil {
		return "", "", err
	}
	fail := func(err error) (string, string, error) {
		os.RemoveAll(tmp)
		return "", "", fmt.Errorf("cannot extract '%s' at %s: %v", name, commit, err)
	}
	cmd := gitCmd("--git-dir="+mirror, "-c", "core.autocrlf=false", "archive", "--format=tar", commit)
	var stderr strings.Builder
	cmd.Stderr = &stderr
	out, err := cmd.StdoutPipe()
	if err != nil {
		return fail(err)
	}
	if err = cmd.Start(); err != nil {
		return fail(err)
	}
	unpackErr := unpack(out, tmp)
	io.Copy(io.Discard, out) // let git finish even if unpack stopped early
	waitErr := cmd.Wait()
	if waitErr != nil {
		msg := strings.TrimSpace(stderr.String())
		if msg == "" {
			msg = waitErr.Error()
		}
		return fail(errors.New(msg))
	}
	if unpackErr != nil {
		return fail(unpackErr)
	}
	if hash, err = treehash.Dir(tmp); err != nil {
		return fail(err)
	}
	return tmp, hash, nil
}

// sweep removes leftovers of extractions that were interrupted, once they are
// old enough that no live process can still be writing them.
func sweep(parent string) {
	entries, err := os.ReadDir(parent)
	if err != nil {
		return
	}
	for _, e := range entries {
		if !strings.HasPrefix(e.Name(), ".tmp-") {
			continue
		}
		if info, err := e.Info(); err == nil && time.Since(info.ModTime()) > time.Hour {
			os.RemoveAll(filepath.Join(parent, e.Name()))
		}
	}
}

// unpack writes a tar stream under dir. Names are cleaned and confined to dir,
// and no entry is written below a symlink the archive itself created: git
// cannot produce either, but the reader should not depend on that.
func unpack(r io.Reader, dir string) error {
	tr := tar.NewReader(r)
	links := map[string]bool{}
	for {
		h, err := tr.Next()
		if err == io.EOF {
			return nil
		}
		if err != nil {
			return err
		}
		if h.Typeflag == tar.TypeXGlobalHeader {
			continue // git archive's "comment=<commit>" record
		}
		name := path.Clean(h.Name)
		if name == "." {
			continue
		}
		if path.IsAbs(name) || name == ".." || strings.HasPrefix(name, "../") {
			return fmt.Errorf("the archive holds a path outside the tree: %q", h.Name)
		}
		for p := path.Dir(name); p != "."; p = path.Dir(p) {
			if links[p] {
				return fmt.Errorf("the archive writes %q below a symlink", h.Name)
			}
		}
		target := filepath.Join(dir, filepath.FromSlash(name))
		switch h.Typeflag {
		case tar.TypeDir:
			if err := os.MkdirAll(target, 0o755); err != nil {
				return err
			}
		case tar.TypeReg:
			if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
				return err
			}
			mode := os.FileMode(0o644)
			if h.Mode&0o111 != 0 {
				mode = 0o755
			}
			f, err := os.OpenFile(target, os.O_WRONLY|os.O_CREATE|os.O_EXCL, mode)
			if err != nil {
				return err
			}
			_, err = io.Copy(f, tr)
			if cerr := f.Close(); err == nil {
				err = cerr
			}
			if err != nil {
				return err
			}
		case tar.TypeSymlink:
			if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
				return err
			}
			if err := os.Symlink(h.Linkname, target); err != nil {
				return err
			}
			links[name] = true
		default:
			return fmt.Errorf("the archive holds %q, an entry of a kind turf does not extract (type %q)", h.Name, string(h.Typeflag))
		}
	}
}
