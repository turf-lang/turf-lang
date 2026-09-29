package resolver

import (
	manifest "github.com/turf-lang/turf-lang/cli/internal/legacymanifest"
)

type Node struct {
	Manifest *manifest.Manifest
	Deps     []*Node
}

func Build(pkgPath string, visited map[string]*Node) (*Node, error) {
	m, err := manifest.Read(pkgPath + "/turf.toml")
	if err != nil {
		return nil, err
	}

	if existing, ok := visited[m.Name]; ok {
		return existing, nil
	}

	node := &Node{
		Manifest: m,
		Deps:     []*Node{},
	}

	visited[m.Name] = node

	for dep := range m.Dependencies {
		depPath := pkgPath + "/../" + dep
		depNode, err := Build(depPath, visited)
		if err != nil {
			return nil, err
		}
		node.Deps = append(node.Deps, depNode)
	}

	return node, nil
}

func Resolve(root *Node, visited map[string]bool, result *[]*Node) {
	if visited[root.Manifest.Name] {
		return
	}

	for _, dep := range root.Deps {
		Resolve(dep, visited, result)
	}

	visited[root.Manifest.Name] = true
	*result = append(*result, root)
}
