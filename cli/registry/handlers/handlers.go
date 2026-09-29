package handlers

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"

	"github.com/turf-lang/turf-cli/internal/checksum"
)

type PackageMeta struct {
	Name     string            `json:"name"`
	Versions map[string]string `json:"versions"`
}

func Publish(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	token := r.Header.Get("Authorization")
	if token != "Bearer "+os.Getenv("TURF_TOKEN") {
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return
	}

	name := r.FormValue("name")
	version := r.FormValue("version")

	if name == "" || version == "" {
		http.Error(w, "missing name or version", http.StatusBadRequest)
		return
	}

	pkgDir := filepath.Join("registry-data", name)
	if err := os.MkdirAll(pkgDir, 0o755); err != nil {
		http.Error(w, "internal error", http.StatusInternalServerError)
		return
	}

	archivePath := filepath.Join(pkgDir, version+".tar.gz")

	file, _, err := r.FormFile("archive")
	if err != nil {
		http.Error(w, "missing archive", http.StatusBadRequest)
		return
	}
	defer file.Close()

	out, err := os.Create(archivePath)
	if err != nil {
		http.Error(w, "internal error", http.StatusInternalServerError)
		return
	}
	defer out.Close()

	if _, err := io.Copy(out, file); err != nil {
		http.Error(w, "internal error", http.StatusInternalServerError)
		return
	}

	hash, err := checksum.Hash(archivePath)
	if err != nil {
		http.Error(w, "internal error", http.StatusInternalServerError)
		return
	}

	if err := saveMeta(name, version, hash); err != nil {
		http.Error(w, "internal error", http.StatusInternalServerError)
		return
	}

	fmt.Fprintf(w, "published %s@%s\nsha256: %s\n", name, version, hash)
}

func saveMeta(name, version, hash string) error {
	metaPath := filepath.Join("registry-data", name, "meta.json")

	meta := loadMeta(name)
	meta.Name = name
	meta.Versions[version] = hash

	data, err := json.MarshalIndent(meta, "", "  ")
	if err != nil {
		return err
	}

	return os.WriteFile(metaPath, data, 0o644)
}

func loadMeta(name string) PackageMeta {
	metaPath := filepath.Join("registry-data", name, "meta.json")

	data, err := os.ReadFile(metaPath)
	if err != nil {
		return PackageMeta{
			Name:     name,
			Versions: map[string]string{},
		}
	}

	var meta PackageMeta
	if err := json.Unmarshal(data, &meta); err != nil {
		return PackageMeta{
			Name:     name,
			Versions: map[string]string{},
		}
	}

	return meta
}

func Fetch(w http.ResponseWriter, r *http.Request) {
	parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/package/"), "/")

	if len(parts) == 1 {
		fetchMeta(w, r, parts[0])
		return
	}

	if len(parts) == 3 && parts[2] == "download" {
		fetchArchive(w, r, parts[0], parts[1])
		return
	}

	http.Error(w, "not found", http.StatusNotFound)
}

func fetchMeta(w http.ResponseWriter, r *http.Request, name string) {
	meta := loadMeta(name)
	if len(meta.Versions) == 0 {
		http.Error(w, "package not found", http.StatusNotFound)
		return
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(meta)
}

func fetchArchive(w http.ResponseWriter, r *http.Request, name, version string) {
	archivePath := filepath.Join("registry-data", name, version+".tar.gz")

	if _, err := os.Stat(archivePath); err != nil {
		http.Error(w, "version not found", http.StatusNotFound)
		return
	}

	w.Header().Set("Content-Type", "application/octet-stream")
	http.ServeFile(w, r, archivePath)
}
