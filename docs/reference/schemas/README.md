# Schemas of what tools read

Every format a program other than the compiler reads is a versioned document
with a schema here and fixtures beside it (IF-2). A consumer's tests read the
fixtures (IF-5, `scripts/check/consumer_fixtures.sh`); the producers are
checked against the schemas by `scripts/check/diagnostics_json_check.py` in CI.

| Format | Schema | Version field | Producer | Fixtures |
|---|---|---|---|---|
| `turfc --diagnostics=json`, one line per diagnostic | `diagnostics.schema.json` | `"version"`, `"1.1"` | turfc | `diagnostics-fixtures/` |
| `turfc --version --json`, the interface manifest | `version.schema.json` | `"interface"` and `"schemas"` | turfc | (the check runs the built compiler) |
| `turfc --unsafe-report` | `unsafe-report.schema.json` | `"format"`, `1` | turfc | (the check builds a project) |
| A package graph: `turfc --package-graph`'s input, `turf graph`'s output | `package-graph.schema.json` | `"format"`, `1` | turf (`turf build`, `turf test`, `turf graph`) | `package-graph-fixtures/` |
| `turf graph --unsafe` | `package-graph-report.schema.json` | `"format"`, `1` | turf | `package-graph-fixtures/report.json` |

## The versioning rule

- A minor version adds fields. A reader written for an older minor keeps
  working: it ignores what it does not know.
- Changing what a field means, or removing one, is a new major version.
- A string version (`"1.1"`) carries both; an integer `"format"` is the major
  version alone. `turfc --version --json` names the versions this compiler
  writes and reads under `"schemas"`.
- A package graph is the one input turfc checks strictly: it rejects a package
  key it does not know, so a misspelt `"allow_unsafe"` never silently means
  false. A key is therefore added to it only together with the driver
  interface minor that reads it (`"tests"` came with 1.3), and a producer
  writes the key only to a compiler that declares that minor.
- A change to a schema here changes its fixtures and the producer in the same
  commit, and `diagnostics_json_check.py` fails until all three agree.
