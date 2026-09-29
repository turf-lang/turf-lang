# Turf CLI

The official command-line interface for the Turflang ecosystem.

Turf CLI is responsible for project creation, dependency management, package publishing, compilation, formatting, testing, and future tooling across the Turflang platform.

## Vision

The long-term goal of Turf CLI is to provide a complete developer experience similar to what Cargo provides for Rust or what Go provides through its standard tooling.

A Turflang project should be manageable through a single tool.

```bash
turf init
turf install
turf build
turf run
turf test
turf fmt
turf publish
```

## Current Features

* Project initialization
* Package manifest management
* Build orchestration
* Compiler integration
* Cross-platform support

## Planned Features

### Package Management

Install, update, and remove packages from the Turf Registry.

```bash
turf install math
turf remove math
turf update
```

### Registry Integration

Publish packages directly to the official registry.

```bash
turf publish
```

### Dependency Resolution

Automatic dependency graph construction and version resolution.

```toml
[dependencies]
math = "^1.2.0"
utils = "~0.8.0"
```

### Lockfiles

Deterministic builds through version locking.

```bash
turf.lock
```

### Package Verification

Integrity verification using cryptographic checksums.

```text
SHA256 verification
Package integrity checks
Tamper detection
```

### Build System

Unified project builds.

```bash
turf build
turf run
turf clean
```

### Testing Framework

Built-in test execution.

```bash
turf test
```

### Formatter

Official code formatter.

```bash
turf fmt
```

### Language Server Integration

Tooling support for editors and IDEs.

```bash
turf lsp
```

### Documentation Generation

Generate project documentation directly from source code.

```bash
turf docs
```

### Workspace Support

Manage multi-package repositories.

```bash
turf workspace init
```

### Local Package Development

Develop packages locally before publishing.

```bash
turf install ../my-package
```

### Caching

Package and build caching for faster iteration.

```text
~/.turf/cache
```

## Long-Term Roadmap

The Turf ecosystem aims to include:

* Turf Compiler
* Turf CLI
* Turf Registry
* Turf Package Manager
* Turf Formatter
* Turf Language Server
* Turf Documentation Generator
* Turf Testing Framework
* Turf Build System

