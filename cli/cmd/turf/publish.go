// Pre-2.0 registry-model command; not dispatched. Reworked by LEC-001 stage 5/6 (docs/development/changes/LEC-001.md).

package main

import (
	"bytes"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"os"
	"path/filepath"

	"github.com/turf-lang/turf-lang/cli/internal/archive"
	"github.com/turf-lang/turf-lang/cli/internal/checksum"
	"github.com/turf-lang/turf-lang/cli/internal/config"
	manifest "github.com/turf-lang/turf-lang/cli/internal/legacymanifest"
)

func runPublish(args []string) error {
	m, err := manifest.Read("turf.toml")
	if err != nil {
		return fmt.Errorf("reading manifest: %w", err)
	}

	cfg, err := config.Load()
	if err != nil {
		return fmt.Errorf("loading config: %w", err)
	}

	filename := fmt.Sprintf("%s-%s.tar.gz", m.Name, m.Version)

	if err := archive.Pack(".", filename); err != nil {
		return fmt.Errorf("packing archive: %w", err)
	}

	absPath, err := filepath.Abs(filename)
	if err != nil {
		return fmt.Errorf("resolving path: %w", err)
	}

	hash, err := checksum.Hash(filename)
	if err != nil {
		return fmt.Errorf("hashing archive: %w", err)
	}

	fmt.Printf("packed %s@%s  →  %s\n", m.Name, m.Version, absPath)
	fmt.Printf("sha256: %s\n", hash)

	if err := pushToRegistry(filename, m.Name, m.Version, hash, cfg.Registry, cfg.Token); err != nil {
		return fmt.Errorf("pushing to registry: %w", err)
	}

	return nil
}

func pushToRegistry(filename, name, version, hash, registry, token string) error {
	var body bytes.Buffer
	writer := multipart.NewWriter(&body)

	writer.WriteField("name", name)
	writer.WriteField("version", version)

	part, err := writer.CreateFormFile("archive", filename)
	if err != nil {
		return err
	}

	f, err := os.Open(filename)
	if err != nil {
		return err
	}
	defer f.Close()

	if _, err := io.Copy(part, f); err != nil {
		return err
	}

	writer.Close()

	req, err := http.NewRequest("POST", registry+"/publish", &body)
	if err != nil {
		return err
	}

	req.Header.Set("Content-Type", writer.FormDataContentType())
	req.Header.Set("Authorization", "Bearer "+token)

	client := &http.Client{}
	resp, err := client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()

	respBody, _ := io.ReadAll(resp.Body)
	fmt.Print(string(respBody))
	return nil
}
