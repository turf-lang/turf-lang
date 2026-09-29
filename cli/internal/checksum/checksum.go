package checksum

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
)

func Hash(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()

	h := sha256.New()

	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}

	return hex.EncodeToString(h.Sum(nil)), nil
}

func Verify(path, expected string) error {
	actual, err := Hash(path)
	if err != nil {
		return err
	}

	if actual != expected {
		return fmt.Errorf("checksum mismatch\n  expected: %s\n  got:      %s", expected, actual)
	}

	return nil
}
