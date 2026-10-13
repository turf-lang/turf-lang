package manifest

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

const header = "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"

func TestParseValid(t *testing.T) {
	tests := []struct {
		name string
		text string
		want Manifest
	}{
		{"minimal", header, Manifest{
			Package: Package{Name: "app", Version: "0.1.0"}, Deps: map[string]Dep{}}},
		{"turf requirement", "[package]\nname = \"app\"\nversion = \"1\"\nturf = \"2.0\"\n", Manifest{
			Package: Package{Name: "app", Version: "1", Turf: "2.0"}, Deps: map[string]Dep{}}},
		{"path dependencies keep file order", header + "\n[dependencies]\nzeta = { path = \"../zeta\" }\nalpha = { path = \"../alpha\" }\n", Manifest{
			Package:  Package{Name: "app", Version: "0.1.0"},
			Deps:     map[string]Dep{"zeta": {Path: "../zeta"}, "alpha": {Path: "../alpha"}},
			DepOrder: []string{"zeta", "alpha"}}},
		{"git tag", header + "[dependencies]\nnet = { git = \"https://x/net\", tag = \"v1\" }\n", Manifest{
			Package:  Package{Name: "app", Version: "0.1.0"},
			Deps:     map[string]Dep{"net": {Git: "https://x/net", Tag: "v1"}},
			DepOrder: []string{"net"}}},
		{"git rev", header + "[dependencies]\nnet = { git = \"u\", rev = \"abc123\" }\n", Manifest{
			Package:  Package{Name: "app", Version: "0.1.0"},
			Deps:     map[string]Dep{"net": {Git: "u", Rev: "abc123"}},
			DepOrder: []string{"net"}}},
		{"git branch", header + "[dependencies]\nnet = { git = \"u\", branch = \"main\" }\n", Manifest{
			Package:  Package{Name: "app", Version: "0.1.0"},
			Deps:     map[string]Dep{"net": {Git: "u", Branch: "main"}},
			DepOrder: []string{"net"}}},
		{"unsafe allow", header + "[unsafe]\nallow = [\"ffi\", \"gpu\"]\n", Manifest{
			Package: Package{Name: "app", Version: "0.1.0"}, Deps: map[string]Dep{},
			UnsafeAllow: []string{"ffi", "gpu"}}},
		{"empty dependencies table and comments", "# a comment\n" + header + "\n[dependencies] # none yet\n", Manifest{
			Package: Package{Name: "app", Version: "0.1.0"}, Deps: map[string]Dep{}}},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got, err := Parse("/p/turf.toml", []byte(tc.text))
			if err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
			tc.want.Path = "/p/turf.toml"
			if !reflect.DeepEqual(*got, tc.want) {
				t.Fatalf("got %+v\nwant %+v", *got, tc.want)
			}
		})
	}
}

func TestParseErrors(t *testing.T) {
	tests := []struct {
		name string
		text string
		frag string // every fragment must appear in the message
		also []string
	}{
		{"syntax error names the line", "[package\nname = \"a\"\n", "turf.toml:2:", nil},
		{"unknown top-level table", header + "[build]\nx = 1\n", "unknown key `build` at the top level", []string{"turf.toml:4"}},
		{"unknown top-level key", "flavour = \"x\"\n" + header, "unknown key `flavour`", nil},
		{"unknown key in package", "[package]\nname = \"app\"\nversion = \"1\"\nauthors = []\n", "unknown key `authors` in [package]", []string{"turf.toml:4"}},
		{"unknown key in unsafe", header + "[unsafe]\nallowed = []\n", "unknown key `allowed` in [unsafe]", nil},
		{"unknown key in a dependency", header + "[dependencies]\na = { path = \"x\", pth = \"y\" }\n", "unknown key `pth` in dependency 'a'", []string{"turf.toml:5"}},
		{"missing package table", "[dependencies]\n", "missing [package] table", nil},
		{"missing name", "[package]\nversion = \"1\"\n", "missing the required key `name`", nil},
		{"missing version", "[package]\nname = \"app\"\n", "missing the required key `version`", nil},
		{"name not a string", "[package]\nname = 3\nversion = \"1\"\n", "`name` in [package] must be a string", nil},
		{"name with a dash", "[package]\nname = \"my-app\"\nversion = \"1\"\n", "'my-app' is not a valid package name", nil},
		{"name starting with a digit", "[package]\nname = \"2fast\"\nversion = \"1\"\n", "'2fast' is not a valid package name", nil},
		{"reserved word name", "[package]\nname = \"fn\"\nversion = \"1\"\n", "'fn' is a reserved word", nil},
		{"empty version", "[package]\nname = \"a\"\nversion = \"\"\n", "`version` in [package] must not be empty", nil},
		{"turf requirement shape", "[package]\nname = \"a\"\nversion = \"1\"\nturf = \"2\"\n", "'2' is not \"<major>.<minor>\"", nil},
		{"registry string dependency", header + "[dependencies]\nmath = \"^1.2.0\"\n", "dependency 'math' is a version string", []string{"hint: registry versions are not part of TurfLang 2.0; use", "{ path = \"...\" }"}},
		{"non-table dependency", header + "[dependencies]\nmath = 3\n", "must be an inline table", nil},
		{"path with git", header + "[dependencies]\nm = { path = \"x\", git = \"y\", tag = \"t\" }\n", "has both `path` and `git`", nil},
		{"path with a tag", header + "[dependencies]\nm = { path = \"x\", tag = \"t\" }\n", "`tag`, `rev` and `branch` do not apply", nil},
		{"git with no ref", header + "[dependencies]\nm = { git = \"y\" }\n", "needs exactly one of `tag`, `rev` or `branch`, and has 0", nil},
		{"git with two refs", header + "[dependencies]\nm = { git = \"y\", tag = \"t\", rev = \"r\" }\n", "and has 2", nil},
		{"neither path nor git", header + "[dependencies]\nm = { }\n", "needs `path`, or `git`", nil},
		{"empty path", header + "[dependencies]\nm = { path = \"\" }\n", "has an empty `path`", nil},
		{"dependency on itself", header + "[dependencies]\napp = { path = \".\" }\n", "package 'app' depends on itself", nil},
		{"dependency name reserved", header + "[dependencies]\nfor = { path = \"x\" }\n", "'for' is a reserved word", nil},
		{"dependency name invalid", header + "[dependencies]\n\"a-b\" = { path = \"x\" }\n", "'a-b' is not a valid package name", nil},
		{"allow not a list", header + "[unsafe]\nallow = \"ffi\"\n", "must be a list of package names", nil},
		{"allow not strings", header + "[unsafe]\nallow = [1]\n", "must be a list of strings", nil},
		{"allow duplicate", header + "[unsafe]\nallow = [\"a\", \"a\"]\n", "lists 'a' twice", nil},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			_, err := Parse("/p/turf.toml", []byte(tc.text))
			if err == nil {
				t.Fatalf("expected an error containing %q, got none", tc.frag)
			}
			msg := err.Error()
			for _, f := range append([]string{tc.frag}, tc.also...) {
				if !strings.Contains(msg, f) {
					t.Errorf("error %q lacks %q", msg, f)
				}
			}
			if !strings.Contains(msg, "/p/turf.toml") {
				t.Errorf("error %q does not name the file", msg)
			}
		})
	}
}

func TestLoadMakesPathAbsolute(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "turf.toml"), []byte(header), 0o644); err != nil {
		t.Fatal(err)
	}
	wd, _ := os.Getwd()
	defer os.Chdir(wd)
	if err := os.Chdir(dir); err != nil {
		t.Fatal(err)
	}
	m, err := Load("turf.toml")
	if err != nil {
		t.Fatal(err)
	}
	if !filepath.IsAbs(m.Path) || filepath.Base(m.Path) != "turf.toml" {
		t.Fatalf("Path = %q, want an absolute path to turf.toml", m.Path)
	}
}

func TestLoadMissingFile(t *testing.T) {
	if _, err := Load(filepath.Join(t.TempDir(), "turf.toml")); !os.IsNotExist(err) {
		t.Fatalf("want a not-exist error the caller can test for, got %v", err)
	}
}

func TestReservedWords(t *testing.T) {
	for _, w := range []string{"fn", "import", "public", "caller_location", "int32"} {
		if !IsReserved(w) {
			t.Errorf("%q should be reserved", w)
		}
	}
	for _, w := range []string{"geometry", "util", "module", "main"} {
		if IsReserved(w) {
			t.Errorf("%q should not be reserved", w)
		}
	}
	if n := len(ReservedWords()); n != 56 {
		t.Errorf("reserved word count = %d; if the lexer's table changed, update the list (see Drift comment)", n)
	}
}
