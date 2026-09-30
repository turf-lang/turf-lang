package manifest

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A manifest with everything a re-encoder would destroy: comments (including
// a trailing one), blank lines, and keys in a deliberate non-alphabetical order.
const rich = `# Top comment about the app.
[package]
name = "app"      # the name
version = "0.1.0"

# Dependencies, hand-ordered.
[dependencies]
zeta = { path = "../zeta" }   # last alphabetically, first here

# note about alpha
alpha = { path = "../alpha" }

[unsafe]
allow = ["zeta"]
`

func TestAddThenRemoveIsByteIdentical(t *testing.T) {
	added, err := AddDependency(rich, "geometry", "../geometry")
	if err != nil {
		t.Fatal(err)
	}
	// Exactly one line was inserted, right after the last non-blank line of the
	// table and before the blank line and [unsafe] that follow it.
	want := strings.Replace(rich, "alpha = { path = \"../alpha\" }\n",
		"alpha = { path = \"../alpha\" }\ngeometry = { path = \"../geometry\" }\n", 1)
	if added != want {
		t.Fatalf("add changed more than one line:\n%s", added)
	}
	m, err := Parse("/p/turf.toml", []byte(added))
	if err != nil {
		t.Fatalf("edited text must validate: %v", err)
	}
	if m.Deps["geometry"].Path != "../geometry" {
		t.Fatalf("geometry not added: %+v", m.Deps)
	}
	back, err := RemoveDependency(added, "geometry")
	if err != nil {
		t.Fatal(err)
	}
	if back != rich {
		t.Fatalf("remove did not restore the original byte for byte:\n%s", back)
	}
}

func TestAddGitDependencyIsOneLineAndRemovable(t *testing.T) {
	for _, tc := range []struct{ kind, ref string }{{"tag", "v1.2.0"}, {"branch", "main"}, {"rev", "3f2a9c1"}} {
		added, err := AddGitDependency(rich, "jsonx", "https://github.com/someone/jsonx", tc.kind, tc.ref)
		if err != nil {
			t.Fatal(err)
		}
		line := "jsonx = { git = \"https://github.com/someone/jsonx\", " + tc.kind + " = \"" + tc.ref + "\" }\n"
		want := strings.Replace(rich, "alpha = { path = \"../alpha\" }\n", "alpha = { path = \"../alpha\" }\n"+line, 1)
		if added != want {
			t.Fatalf("%s: add changed more than one line:\n%s", tc.kind, added)
		}
		m, err := Parse("/p/turf.toml", []byte(added))
		if err != nil {
			t.Fatalf("%s: edited text must validate: %v", tc.kind, err)
		}
		if d := m.Deps["jsonx"]; d.Git != "https://github.com/someone/jsonx" || d.RefSpec() != tc.kind+"="+tc.ref {
			t.Fatalf("%s: read back %+v (%s)", tc.kind, d, d.RefSpec())
		}
		back, err := RemoveDependency(added, "jsonx")
		if err != nil || back != rich {
			t.Fatalf("%s: remove did not restore the original: %v\n%s", tc.kind, err, back)
		}
	}
	if _, err := AddGitDependency(rich, "alpha", "u", "tag", "v"); err == nil {
		t.Error("adding a name that is already there must be refused")
	}
	// A URL with characters TOML must escape survives the round trip.
	odd := "C:\\repos\\my \"lib\""
	added, err := AddGitDependency("[package]\nname = \"a\"\nversion = \"1\"\n", "lib", odd, "rev", "abcd")
	if err != nil {
		t.Fatal(err)
	}
	m, err := Parse("/p/turf.toml", []byte(added))
	if err != nil || m.Deps["lib"].Git != odd {
		t.Fatalf("odd URL: %v %+v", err, m.Deps["lib"])
	}
}

func TestAddIntoEmptyTable(t *testing.T) {
	in := "[package]\nname = \"a\"\nversion = \"1\"\n\n[dependencies]\n"
	got, err := AddDependency(in, "g", "../g")
	if err != nil {
		t.Fatal(err)
	}
	if got != in+"g = { path = \"../g\" }\n" {
		t.Fatalf("got %q", got)
	}
	back, _ := RemoveDependency(got, "g")
	if back != in {
		t.Fatalf("remove after add on an empty table: %q", back)
	}
}

func TestAddWithoutDependenciesTable(t *testing.T) {
	in := "[package]\nname = \"a\"\nversion = \"1\"\n"
	got, err := AddDependency(in, "g", "../g")
	if err != nil {
		t.Fatal(err)
	}
	want := in + "\n[dependencies]\ng = { path = \"../g\" }\n"
	if got != want {
		t.Fatalf("got %q, want %q", got, want)
	}
	if _, err := Parse("/p/turf.toml", []byte(got)); err != nil {
		t.Fatal(err)
	}
	// No terminating newline on the last line, and a table that ends the file.
	got, err = AddDependency("[package]\nname = \"a\"\nversion = \"1\"", "g", "../g")
	if err != nil || !strings.HasSuffix(got, "[dependencies]\ng = { path = \"../g\" }\n") ||
		!strings.HasPrefix(got, "[package]\nname = \"a\"\nversion = \"1\"\n\n") {
		t.Fatalf("got %q, %v", got, err)
	}
}

func TestAddWhenTableIsLastAndUnterminated(t *testing.T) {
	in := "[package]\nname = \"a\"\nversion = \"1\"\n[dependencies]\nx = { path = \"../x\" }"
	got, err := AddDependency(in, "g", "../g")
	if err != nil {
		t.Fatal(err)
	}
	if got != in+"\ng = { path = \"../g\" }\n" {
		t.Fatalf("got %q", got)
	}
}

func TestEditsKeepCRLF(t *testing.T) {
	in := "[package]\r\nname = \"a\"\r\nversion = \"1\"\r\n\r\n[dependencies]\r\n"
	got, err := AddDependency(in, "g", "../g")
	if err != nil {
		t.Fatal(err)
	}
	if got != in+"g = { path = \"../g\" }\r\n" {
		t.Fatalf("a CRLF file must stay CRLF: %q", got)
	}
}

func TestAddDuplicateRefused(t *testing.T) {
	_, err := AddDependency(rich, "alpha", "../elsewhere")
	if err == nil || !strings.Contains(err.Error(), "'alpha' is already in [dependencies]") {
		t.Fatalf("got %v", err)
	}
}

func TestRemoveMultiLineRefused(t *testing.T) {
	cases := map[string]string{
		"inline table opened":  "a = {\n  path = \"../a\",\n}\n",
		"array opened":         "a = [\n \"x\",\n]\n",
		"multi-line string":    "a = \"\"\"\nx\"\"\"\n",
		"unterminated string":  "a = \"oops\n",
		"open brace, comment":  "a = { path = \"../a\", # hmm\n",
		"open brace, in quote": "a = { path = \"}\",\n",
	}
	for name, body := range cases {
		t.Run(name, func(t *testing.T) {
			in := "[package]\nname = \"p\"\nversion = \"1\"\n\n[dependencies]\n" + body
			_, err := RemoveDependency(in, "a")
			if err == nil || !strings.Contains(err.Error(), "edit turf.toml by hand") {
				t.Fatalf("want a refusal to edit by hand, got %v", err)
			}
		})
	}
}

func TestRemoveDependencyTable(t *testing.T) {
	in := "[package]\nname = \"p\"\nversion = \"1\"\n\n[dependencies.a]\npath = \"../a\"\n"
	_, err := RemoveDependency(in, "a")
	if err == nil || !strings.Contains(err.Error(), "edit turf.toml by hand") {
		t.Fatalf("got %v", err)
	}
}

func TestRemoveAbsent(t *testing.T) {
	_, err := RemoveDependency(rich, "nope")
	if err == nil || !strings.Contains(err.Error(), "'nope' is not in [dependencies]") {
		t.Fatalf("got %v", err)
	}
	_, err = RemoveDependency("[package]\nname = \"p\"\nversion = \"1\"\n", "a")
	if err == nil || !strings.Contains(err.Error(), "is not in [dependencies]") {
		t.Fatalf("no table: got %v", err)
	}
}

func TestRemoveOnlyTouchesTheDependenciesTable(t *testing.T) {
	// `name` and `version` are keys of [package]; a dependency of the same name
	// must not delete them.
	in := "[package]\nname = \"p\"\nversion = \"1\"\n\n[dependencies]\nname = { path = \"../name\" }\n"
	got, err := RemoveDependency(in, "name")
	if err != nil {
		t.Fatal(err)
	}
	if got != "[package]\nname = \"p\"\nversion = \"1\"\n\n[dependencies]\n" {
		t.Fatalf("got %q", got)
	}
	// Not present in [dependencies] even though [package] has that key.
	if _, err := RemoveDependency("[package]\nname = \"p\"\nversion = \"1\"\n\n[dependencies]\n", "name"); err == nil {
		t.Fatal("name is in [package], not [dependencies]: must be reported absent")
	}
}

func TestRemoveQuotedKey(t *testing.T) {
	in := "[package]\nname = \"p\"\nversion = \"1\"\n\n[dependencies]\n\"a\" = { path = \"../a\" }\nb = { path = \"../b\" }\n"
	got, err := RemoveDependency(in, "a")
	if err != nil || strings.Contains(got, "\"a\"") || !strings.Contains(got, "b = ") {
		t.Fatalf("got %q, %v", got, err)
	}
}

func TestValueContinues(t *testing.T) {
	yes := []string{` {`, ` [`, ` "abc`, ` { path = "x",`, ` """`}
	no := []string{` { path = "a]" }`, ` "x" # a { comment`, ` { path = "../a" }   # done`, ` "a\"b"`, ` []`}
	for _, s := range yes {
		if !valueContinues(s) {
			t.Errorf("%q should continue", s)
		}
	}
	for _, s := range no {
		if valueContinues(s) {
			t.Errorf("%q should not continue", s)
		}
	}
}

func TestWriteFileAtomic(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "turf.toml")
	if err := os.WriteFile(p, []byte("old"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := WriteFileAtomic(p, []byte("new")); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(p)
	if string(b) != "new" {
		t.Fatalf("content %q", b)
	}
	if st, _ := os.Stat(p); st.Mode().Perm() != 0o600 {
		t.Errorf("mode changed to %v", st.Mode().Perm())
	}
	entries, _ := os.ReadDir(dir)
	if len(entries) != 1 {
		t.Errorf("a temp file was left behind: %v", entries)
	}
}
