package manifest

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

// The editors below change turf.toml as TEXT, one line at a time. They never
// decode and re-encode the file: BurntSushi/toml (like every TOML encoder)
// does not round-trip comments, blank lines or key order, so an `add` that
// re-encoded would silently rewrite the user's hand-tuned manifest. The edited
// text is then run through Parse before anything is written, so an edit that
// would produce an invalid manifest writes nothing.

var (
	depsHeaderRe = regexp.MustCompile(`^\s*\[\s*dependencies\s*\]\s*(#.*)?$`)
	anyHeaderRe  = regexp.MustCompile(`^\s*\[`)
)

func eolOf(text string) string {
	if strings.Contains(text, "\r\n") {
		return "\r\n"
	}
	return "\n"
}

// splitLines splits keeping each line's terminator, so joining the result
// reproduces the input byte for byte.
func splitLines(text string) []string {
	var lines []string
	for len(text) > 0 {
		i := strings.IndexByte(text, '\n')
		if i < 0 {
			lines = append(lines, text)
			break
		}
		lines = append(lines, text[:i+1])
		text = text[i+1:]
	}
	return lines
}

func trimEOL(s string) string { return strings.TrimRight(s, "\r\n") }

// depKeyRe matches the line declaring dependency `name`, bare or quoted.
func depKeyRe(name string) *regexp.Regexp {
	q := regexp.QuoteMeta(name)
	return regexp.MustCompile(`^\s*(` + q + `|"` + q + `"|'` + q + `')\s*=`)
}

// dependenciesRange returns the [start,end) line indexes of the body of the
// [dependencies] table (the lines after its header up to the next header or
// the end of the file), and whether the table exists.
func dependenciesRange(lines []string) (start, end int, ok bool) {
	for i, l := range lines {
		if depsHeaderRe.MatchString(trimEOL(l)) {
			start = i + 1
			end = len(lines)
			for j := start; j < len(lines); j++ {
				if anyHeaderRe.MatchString(lines[j]) {
					end = j
					break
				}
			}
			return start, end, true
		}
	}
	return 0, 0, false
}

// AddDependency returns text with `name = { path = "<path>" }` added to the
// [dependencies] table: after the table's last non-blank line, so it lands
// under the user's own entries and comments and before whatever table follows.
// With no [dependencies] table one is appended. Only lines are inserted; every
// existing byte is kept.
func AddDependency(text, name, path string) (string, error) {
	lines := splitLines(text)
	eol := eolOf(text)
	entry := fmt.Sprintf("%s = { path = %q }%s", name, path, eol)

	start, end, ok := dependenciesRange(lines)
	if !ok {
		out := text
		if out != "" && !strings.HasSuffix(out, "\n") {
			out += eol
		}
		if strings.TrimSpace(out) != "" {
			out += eol
		}
		return out + "[dependencies]" + eol + entry, nil
	}
	re := depKeyRe(name)
	for _, l := range lines[start:end] {
		if re.MatchString(l) {
			return "", fmt.Errorf("dependency '%s' is already in [dependencies]", name)
		}
	}
	at := start // an empty table: right under the header
	for i := start; i < end; i++ {
		if strings.TrimSpace(lines[i]) != "" {
			at = i + 1
		}
	}
	if at > 0 && !strings.HasSuffix(lines[at-1], "\n") {
		// The file's last line has no terminator; give it one so the new
		// entry is not glued onto it.
		lines[at-1] += eol
	}
	out := append(append(append([]string{}, lines[:at]...), entry), lines[at:]...)
	return strings.Join(out, ""), nil
}

// RemoveDependency returns text without the line declaring dependency `name`
// in [dependencies]. It refuses, and the caller must leave the file alone,
// when the dependency is declared in a form one line cannot express
// (a value spanning lines, or a [dependencies.name] table).
//
// Only the [dependencies] body is searched. A bare `^\s*name\s*=` over the
// whole file would delete `name = "app"` from [package] the day someone removes
// a dependency called `name` or `version`.
func RemoveDependency(text, name string) (string, error) {
	lines := splitLines(text)
	subRe := regexp.MustCompile(`^\s*\[\s*dependencies\s*\.\s*("` + regexp.QuoteMeta(name) + `"|'` + regexp.QuoteMeta(name) + `'|` + regexp.QuoteMeta(name) + `)\s*\]`)
	for _, l := range lines {
		if subRe.MatchString(l) {
			return "", fmt.Errorf("dependency '%s' is declared as its own table; edit turf.toml by hand", name)
		}
	}
	start, end, ok := dependenciesRange(lines)
	if !ok {
		return "", fmt.Errorf("dependency '%s' is not in [dependencies]", name)
	}
	re := depKeyRe(name)
	for i := start; i < end; i++ {
		l := lines[i]
		m := re.FindStringIndex(l)
		if m == nil {
			continue
		}
		if valueContinues(l[m[1]:]) {
			return "", fmt.Errorf("the value of dependency '%s' spans several lines; edit turf.toml by hand", name)
		}
		return strings.Join(append(append([]string{}, lines[:i]...), lines[i+1:]...), ""), nil
	}
	return "", fmt.Errorf("dependency '%s' is not in [dependencies]", name)
}

// valueContinues reports whether a TOML value, given as the text after the
// `=`, is still open at the end of the line: an unclosed { [ or string. A
// comment after the value is ignored. Triple-quoted (multi-line) strings count as
// open even if a same-line close exists, which errs toward refusing: a wrongly
// refused remove costs the user one manual edit, a wrongly accepted one
// deletes half of a value.
func valueContinues(rest string) bool {
	depth := 0
	var quote byte
	for i := 0; i < len(rest); i++ {
		c := rest[i]
		if quote != 0 {
			if c == '\\' && quote == '"' {
				i++
			} else if c == quote {
				quote = 0
			}
			continue
		}
		switch c {
		case '"', '\'':
			if strings.HasPrefix(rest[i:], strings.Repeat(string(c), 3)) {
				return true
			}
			quote = c
		case '#':
			return depth > 0
		case '{', '[':
			depth++
		case '}', ']':
			depth--
		}
	}
	return depth > 0 || quote != 0
}

// WriteFileAtomic writes data to path through a temporary file in the same
// directory and a rename, so a crash or a full disk leaves the manifest either
// as it was or fully rewritten, never truncated. The file's permission bits are
// kept.
func WriteFileAtomic(path string, data []byte) error {
	mode := os.FileMode(0o644)
	if st, err := os.Stat(path); err == nil {
		mode = st.Mode().Perm()
	}
	tmp, err := os.CreateTemp(filepath.Dir(path), ".turf.toml-*")
	if err != nil {
		return err
	}
	name := tmp.Name()
	cleanup := func() { os.Remove(name) }
	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		cleanup()
		return err
	}
	if err := tmp.Close(); err != nil {
		cleanup()
		return err
	}
	if err := os.Chmod(name, mode); err != nil {
		cleanup()
		return err
	}
	if err := os.Rename(name, path); err != nil {
		cleanup()
		return err
	}
	return nil
}
