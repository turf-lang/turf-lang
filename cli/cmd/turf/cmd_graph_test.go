package main

import (
	"encoding/json"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// graphOf runs `turf graph` with args and returns its exit status and stdout.
func graphOf(t *testing.T, args ...string) (int, string) {
	t.Helper()
	old := os.Stdout
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	os.Stdout = w
	done := make(chan string)
	go func() {
		b, _ := io.ReadAll(r)
		done <- string(b)
	}()
	code := cmdGraph(args)
	w.Close()
	os.Stdout = old
	return code, <-done
}

// The graph is the one `turf build` writes, printed instead of written: the
// root, its path dependency and where each src/ is, with tests/ when the
// project has one. No compiler is needed (none is on PATH here), and nothing
// is written -- not turf.lock, not build/.
func TestGraphPrintsTheProjectsPackages(t *testing.T) {
	dir := t.TempDir()
	writeFile(t, filepath.Join(dir, "app", "turf.toml"),
		"[package]\nname = \"app\"\nversion = \"0.1.0\"\nturf = \"2.0\"\n\n[dependencies]\nhelper = { path = \"../helper\" }\n")
	writeFile(t, filepath.Join(dir, "app", "src", "main.tr"), "fn main(): int { return 0; }\n")
	writeFile(t, filepath.Join(dir, "helper", "turf.toml"),
		"[package]\nname = \"helper\"\nversion = \"0.1.0\"\nturf = \"2.0\"\n")
	writeFile(t, filepath.Join(dir, "helper", "src", "helper.tr"), "pub fn one(): int { return 1; }\n")
	t.Chdir(filepath.Join(dir, "app", "src"))
	t.Setenv("TURF_COMPILER", filepath.Join(dir, "no-such-turfc"))

	code, out := graphOf(t)
	if code != 0 {
		t.Fatalf("exit status %d, output: %s", code, out)
	}
	var g struct {
		Format   int `json:"format"`
		Root     string
		Packages map[string]struct {
			Src   string
			Deps  []string
			Tests string
		}
	}
	if err := json.Unmarshal([]byte(out), &g); err != nil {
		t.Fatalf("not JSON: %v\n%s", err, out)
	}
	if g.Format != 1 || g.Root != "app" || len(g.Packages) != 2 {
		t.Fatalf("unexpected graph: %+v", g)
	}
	app, helper := g.Packages["app"], g.Packages["helper"]
	if !strings.HasSuffix(filepath.ToSlash(app.Src), "/app/src") || !strings.HasSuffix(filepath.ToSlash(helper.Src), "/helper/src") {
		t.Errorf("src paths: app %q helper %q", app.Src, helper.Src)
	}
	if len(app.Deps) != 1 || app.Deps[0] != "helper" {
		t.Errorf("app's deps = %v, want [helper]", app.Deps)
	}
	if app.Tests != "" {
		t.Errorf("a project with no tests/ must not name one: %q", app.Tests)
	}
	for _, name := range []string{"turf.lock", "build"} {
		if _, err := os.Stat(filepath.Join(dir, "app", name)); err == nil {
			t.Errorf("turf graph wrote %s", name)
		}
	}

	// With a tests/ directory the root names it, so test files resolve their
	// imports too (and the compiler does not reject a tests dir that is absent).
	writeFile(t, filepath.Join(dir, "app", "tests", "basic.tr"), "fn main(): int { return 0; }\n")
	_, out = graphOf(t)
	if !strings.Contains(out, `"tests"`) {
		t.Errorf("tests/ exists but the graph does not name it:\n%s", out)
	}
}

func TestGraphOutsideAProjectAndWithStrayArguments(t *testing.T) {
	dir := t.TempDir()
	t.Chdir(dir)
	if code, _ := graphOf(t); code != exitUsage {
		t.Errorf("outside a project: exit %d, want %d", code, exitUsage)
	}
	if code, _ := graphOf(t, "extra"); code != exitUsage {
		t.Errorf("a stray argument: exit %d, want %d", code, exitUsage)
	}
}
