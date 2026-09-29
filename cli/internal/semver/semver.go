package semver

import (
	"fmt"
	"strconv"
	"strings"
)

type Version struct {
	Major int
	Minor int
	Patch int
}

func Parse(s string) (Version, error) {
	s = strings.TrimPrefix(s, "v")
	parts := strings.Split(s, ".")
	if len(parts) != 3 {
		return Version{}, fmt.Errorf("invalid version %q", s)
	}

	major, err := strconv.Atoi(parts[0])
	if err != nil {
		return Version{}, fmt.Errorf("invalid major in %q", s)
	}

	minor, err := strconv.Atoi(parts[1])
	if err != nil {
		return Version{}, fmt.Errorf("invalid minor in %q", s)
	}

	patch, err := strconv.Atoi(parts[2])
	if err != nil {
		return Version{}, fmt.Errorf("invalid patch in %q", s)
	}

	return Version{Major: major, Minor: minor, Patch: patch}, nil
}

func (v Version) String() string {
	return fmt.Sprintf("%d.%d.%d", v.Major, v.Minor, v.Patch)
}

func Satisfies(constraint string, v Version) (bool, error) {
	if strings.HasPrefix(constraint, "^") {
		required, err := Parse(strings.TrimPrefix(constraint, "^"))
		if err != nil {
			return false, err
		}
		return v.Major == required.Major &&
			compare(v, required) >= 0, nil
	}

	if strings.HasPrefix(constraint, "~") {
		required, err := Parse(strings.TrimPrefix(constraint, "~"))
		if err != nil {
			return false, err
		}
		return v.Major == required.Major &&
			v.Minor == required.Minor &&
			compare(v, required) >= 0, nil
	}

	required, err := Parse(constraint)
	if err != nil {
		return false, err
	}
	return compare(v, required) == 0, nil
}

func compare(a, b Version) int {
	if a.Major != b.Major {
		return a.Major - b.Major
	}
	if a.Minor != b.Minor {
		return a.Minor - b.Minor
	}
	return a.Patch - b.Patch
}

func Resolve(constraint string, available []string) (string, error) {
	if constraint == "*" {
		constraint = "^0.0.0"
		var best *Version
		for _, raw := range available {
			v, err := Parse(raw)
			if err != nil {
				continue
			}
			if best == nil || compare(v, *best) > 0 {
				best = &v
			}
		}
		if best == nil {
			return "", fmt.Errorf("no versions available")
		}
		return best.String(), nil
	}

	var best *Version

	for _, raw := range available {
		v, err := Parse(raw)
		if err != nil {
			continue
		}

		ok, err := Satisfies(constraint, v)
		if err != nil {
			return "", err
		}

		if ok {
			if best == nil || compare(v, *best) > 0 {
				best = &v
			}
		}
	}

	if best == nil {
		return "", fmt.Errorf("no version satisfying %q", constraint)
	}

	return best.String(), nil
}
