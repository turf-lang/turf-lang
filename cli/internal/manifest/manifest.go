// Package manifest reads and validates turf.toml, the 2.0 project manifest
// (docs/development/FREEZE.md 6.16, F-PKG-2). It is the only place that
// interprets the file: the compiler never reads it (F-PKG-1), so every rule
// the manifest can break is decided here and reported with the file's name.
package manifest

import (
	"bufio"
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"

	"github.com/BurntSushi/toml"
)

// Manifest is a parsed, validated turf.toml.
type Manifest struct {
	Path        string         // the turf.toml file, absolute
	Package     Package        // [package]
	Deps        map[string]Dep // [dependencies]
	DepOrder    []string       // the keys of Deps in file order
	UnsafeAllow []string       // [unsafe] allow
}

// Dir is the directory holding the manifest, which every relative path in it
// is relative to.
func (m *Manifest) Dir() string { return filepath.Dir(m.Path) }

// Package is the [package] table.
type Package struct {
	Name, Version, Turf string
}

// Dep is one [dependencies] entry. Exactly one of Path or Git is set; a git
// dependency has exactly one of Tag, Rev, Branch. Git dependencies parse (so
// `turf add` and the errors are uniform) but building one is stage 6.
type Dep struct {
	Path, Git, Tag, Rev, Branch string
}

// Error is a manifest failure the user can act on: what is wrong, where, and
// optionally how to fix it. Printed as "msg\nhint: ...", the same shape as the
// driver's errors, so turf's own failures all read alike.
type Error struct {
	Msg  string
	Hint string
}

func (e *Error) Error() string {
	if e.Hint == "" {
		return e.Msg
	}
	return e.Msg + "\nhint: " + e.Hint
}

// Drift: this list is copied by hand from the keyword table in src/Lexer.cpp
// (the `{"word", TOK_...}` entries of `Keywords`). A package name is spelled in
// `import`, so a reserved word could never be imported; the compiler would say
// so late, deep inside some other package's build. scripts/project_smoke.sh
// compares this list against the lexer's table so the two cannot drift apart
// without a failing check.
var reserved = []string{
	"if", "else", "while", "do", "int", "float", "double", "bool", "char",
	"true", "false", "string", "void", "return", "fn", "break", "continue",
	"for", "in", "step", "elseif", "struct", "self", "sizeof", "import",
	"enum", "machine", "null", "mutable", "mut", "immutable", "immut", "to",
	"give", "otherwise", "extern", "ptr", "compare", "matches", "done",
	"export", "static", "operator", "int32", "error", "recover", "produce",
	"run", "as", "unsafe", "guess", "defer", "pub", "public",
	"caller_location",
}

// ReservedWords returns the reserved-word list, for the drift check and tests.
func ReservedWords() []string { return append([]string(nil), reserved...) }

var (
	identRe   = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*$`)
	turfReqRe = regexp.MustCompile(`^[0-9]+\.[0-9]+$`)
)

// IsReserved reports whether name is a TurfLang reserved word.
func IsReserved(name string) bool {
	for _, r := range reserved {
		if r == name {
			return true
		}
	}
	return false
}

// CheckName returns a description of why name cannot be a package name, or ""
// if it can. Package names are spelled in `import`, so they must be
// identifiers and not reserved words.
func CheckName(name string) string {
	if !identRe.MatchString(name) {
		return fmt.Sprintf("'%s' is not a valid package name: it must match [A-Za-z_][A-Za-z0-9_]* because it is spelled in `import`", name)
	}
	if IsReserved(name) {
		return fmt.Sprintf("'%s' is a reserved word, so `import %s;` could never be written", name, name)
	}
	return ""
}

// Load reads and validates the manifest at path.
func Load(path string) (*Manifest, error) {
	abs, err := filepath.Abs(path)
	if err != nil {
		return nil, err
	}
	data, err := os.ReadFile(abs)
	if err != nil {
		return nil, err
	}
	return Parse(abs, data)
}

// Parse validates manifest text. It exists apart from Load so `turf add` and
// `turf remove` can validate the text they are about to write before writing
// it (they edit the text, never re-encode it: TOML encoders drop comments).
func Parse(path string, data []byte) (*Manifest, error) {
	var raw map[string]any
	md, err := toml.Decode(string(data), &raw)
	if err != nil {
		if pe, ok := err.(toml.ParseError); ok && pe.Position.Line > 0 {
			return nil, &Error{Msg: fmt.Sprintf("%s:%d: %s", path, pe.Position.Line, pe.Message)}
		}
		return nil, &Error{Msg: fmt.Sprintf("%s: %v", path, err)}
	}
	p := &parser{path: path, data: data}
	m := &Manifest{Path: path, Deps: map[string]Dep{}}

	for _, k := range sortedKeys(raw) {
		switch k {
		case "package", "dependencies", "unsafe":
		default:
			return nil, p.errAt("", k, "unknown key `%s` at the top level: the tables are [package], [dependencies] and [unsafe]", k)
		}
	}

	pkg, ok := raw["package"]
	if !ok {
		return nil, p.errf("missing [package] table", "")
	}
	pkgTable, ok := pkg.(map[string]any)
	if !ok {
		return nil, p.errf("`package` must be a table ([package])", "")
	}
	if err := p.readPackage(m, pkgTable); err != nil {
		return nil, err
	}

	if d, ok := raw["dependencies"]; ok {
		dt, ok := d.(map[string]any)
		if !ok {
			return nil, p.errf("`dependencies` must be a table ([dependencies])", "")
		}
		// File order, so the graph lists dependencies as declared.
		var order []string
		for _, key := range md.Keys() {
			if len(key) == 2 && key[0] == "dependencies" {
				order = append(order, key[1])
			}
		}
		if err := p.readDeps(m, dt, order); err != nil {
			return nil, err
		}
	}

	if u, ok := raw["unsafe"]; ok {
		ut, ok := u.(map[string]any)
		if !ok {
			return nil, p.errf("`unsafe` must be a table ([unsafe])", "")
		}
		if err := p.readUnsafe(m, ut); err != nil {
			return nil, err
		}
	}
	return m, nil
}

type parser struct {
	path string
	data []byte
}

// errf reports a problem with no better position than the file.
func (p *parser) errf(msg, hint string) error {
	return &Error{Msg: fmt.Sprintf("%s: %s", p.path, msg), Hint: hint}
}

// errAt reports a problem with the key `key` of `table` ("" for the top level),
// with the key's line where it can be found. BurntSushi/toml keeps no
// positions for decoded keys, so the line is recovered from the text; if the
// key is spelled in an unusual way and is not found, the message simply has no
// line rather than a wrong one.
func (p *parser) errAt(table, key, format string, a ...any) error {
	msg := fmt.Sprintf(format, a...)
	if line := p.lineOf(table, key); line > 0 {
		return &Error{Msg: fmt.Sprintf("%s:%d: %s", p.path, line, msg)}
	}
	return &Error{Msg: fmt.Sprintf("%s: %s", p.path, msg)}
}

func (p *parser) errAtHint(table, key, hint, format string, a ...any) error {
	err := p.errAt(table, key, format, a...).(*Error)
	err.Hint = hint
	return err
}

func (p *parser) lineOf(table, key string) int {
	tableHeader := regexp.MustCompile(`^\s*\[\s*([A-Za-z0-9_.\-"' ]+?)\s*\]\s*(#.*)?$`)
	keyLine := regexp.MustCompile(`^\s*["']?` + regexp.QuoteMeta(key) + `["']?\s*=`)
	current := ""
	sc := bufio.NewScanner(bytes.NewReader(p.data))
	n := 0
	for sc.Scan() {
		n++
		line := sc.Text()
		if m := tableHeader.FindStringSubmatch(line); m != nil {
			current = strings.Trim(m[1], `"' `)
			if table == "" && current == key {
				return n
			}
			continue
		}
		if current == table && keyLine.MatchString(line) {
			return n
		}
	}
	return 0
}

func (p *parser) readPackage(m *Manifest, t map[string]any) error {
	for _, k := range sortedKeys(t) {
		switch k {
		case "name", "version", "turf":
		default:
			return p.errAt("package", k, "unknown key `%s` in [package]: the keys are name, version and turf", k)
		}
	}
	str := func(key string, required bool) (string, error) {
		v, ok := t[key]
		if !ok {
			if required {
				return "", p.errAt("", "package", "[package] is missing the required key `%s`", key)
			}
			return "", nil
		}
		s, ok := v.(string)
		if !ok {
			return "", p.errAt("package", key, "`%s` in [package] must be a string", key)
		}
		return s, nil
	}
	var err error
	if m.Package.Name, err = str("name", true); err != nil {
		return err
	}
	if m.Package.Version, err = str("version", true); err != nil {
		return err
	}
	if m.Package.Turf, err = str("turf", false); err != nil {
		return err
	}
	if msg := CheckName(m.Package.Name); msg != "" {
		return p.errAt("package", "name", "invalid `name` in [package]: %s", msg)
	}
	if m.Package.Version == "" {
		return p.errAt("package", "version", "`version` in [package] must not be empty")
	}
	if _, has := t["turf"]; has && !turfReqRe.MatchString(m.Package.Turf) {
		return p.errAtHint("package", "turf", `write the minimum compiler version as "<major>.<minor>", for example "2.0"`,
			"invalid `turf` in [package]: '%s' is not \"<major>.<minor>\"", m.Package.Turf)
	}
	return nil
}

const registryHint = "registry versions are not part of TurfLang 2.0 (FUTURE F-7c); use `{ path = \"...\" }` or `{ git = \"...\", tag = \"...\" }`"

func (p *parser) readDeps(m *Manifest, t map[string]any, order []string) error {
	// A key decode saw but md.Keys() did not list (it lists every key, so this
	// only happens if the two ever disagree) still gets read, in sorted order.
	seen := map[string]bool{}
	names := make([]string, 0, len(t))
	for _, k := range order {
		if _, ok := t[k]; ok && !seen[k] {
			seen[k] = true
			names = append(names, k)
		}
	}
	for _, k := range sortedKeys(t) {
		if !seen[k] {
			names = append(names, k)
		}
	}
	for _, name := range names {
		if msg := CheckName(name); msg != "" {
			return p.errAt("dependencies", name, "invalid dependency name in [dependencies]: %s", msg)
		}
		if name == m.Package.Name {
			return p.errAt("dependencies", name, "package '%s' depends on itself", name)
		}
		tbl, ok := t[name].(map[string]any)
		if !ok {
			if _, isStr := t[name].(string); isStr {
				return p.errAtHint("dependencies", name, registryHint,
					"dependency '%s' is a version string, not a table", name)
			}
			return p.errAt("dependencies", name, "dependency '%s' must be an inline table such as { path = \"../%s\" }", name, name)
		}
		var d Dep
		for _, k := range sortedKeys(tbl) {
			s, isStr := tbl[k].(string)
			if !isStr {
				return p.errAt("dependencies", name, "`%s` of dependency '%s' must be a string", k, name)
			}
			switch k {
			case "path":
				d.Path = s
			case "git":
				d.Git = s
			case "tag":
				d.Tag = s
			case "rev":
				d.Rev = s
			case "branch":
				d.Branch = s
			default:
				return p.errAt("dependencies", name, "unknown key `%s` in dependency '%s': the keys are path, git, tag, rev and branch", k, name)
			}
		}
		refs := 0
		for _, r := range []string{d.Tag, d.Rev, d.Branch} {
			if r != "" {
				refs++
			}
		}
		_, hasPath := tbl["path"]
		_, hasGit := tbl["git"]
		switch {
		case hasPath && hasGit:
			return p.errAt("dependencies", name, "dependency '%s' has both `path` and `git`: it is one or the other", name)
		case hasPath:
			if d.Path == "" {
				return p.errAt("dependencies", name, "dependency '%s' has an empty `path`", name)
			}
			if refs > 0 {
				return p.errAt("dependencies", name, "dependency '%s' is a path dependency, so `tag`, `rev` and `branch` do not apply", name)
			}
		case hasGit:
			if d.Git == "" {
				return p.errAt("dependencies", name, "dependency '%s' has an empty `git` URL", name)
			}
			if refs != 1 {
				return p.errAtHint("dependencies", name, "pin the dependency to a release with `tag`, or to a commit with `rev`",
					"git dependency '%s' needs exactly one of `tag`, `rev` or `branch`, and has %d", name, refs)
			}
		default:
			return p.errAt("dependencies", name, "dependency '%s' needs `path`, or `git` with one of `tag`, `rev` or `branch`", name)
		}
		m.Deps[name] = d
		m.DepOrder = append(m.DepOrder, name)
	}
	return nil
}

func (p *parser) readUnsafe(m *Manifest, t map[string]any) error {
	for _, k := range sortedKeys(t) {
		if k != "allow" {
			return p.errAt("unsafe", k, "unknown key `%s` in [unsafe]: the only key is allow", k)
		}
	}
	v, ok := t["allow"]
	if !ok {
		return nil
	}
	list, ok := v.([]any)
	if !ok {
		return p.errAt("unsafe", "allow", "`allow` in [unsafe] must be a list of package names")
	}
	seen := map[string]bool{}
	for _, e := range list {
		s, ok := e.(string)
		if !ok {
			return p.errAt("unsafe", "allow", "`allow` in [unsafe] must be a list of strings")
		}
		if seen[s] {
			return p.errAt("unsafe", "allow", "`allow` in [unsafe] lists '%s' twice", s)
		}
		seen[s] = true
		m.UnsafeAllow = append(m.UnsafeAllow, s)
	}
	return nil
}

func sortedKeys(m map[string]any) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	// Map order is random; a deterministic first error beats a random one.
	sort.Strings(keys)
	return keys
}
