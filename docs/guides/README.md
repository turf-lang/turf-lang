# TurfLang guides

Task-oriented walkthroughs: how to do something with TurfLang, step by step,
with real commands and their output. The rules behind each step are in the
[language reference](../reference/README.md); a guide links to the part it
relies on instead of restating it. Every TurfLang example in these pages is
compiled by CI, like the reference's.

1. [Working on a project](projects.md): `turf new` to a release build. Modules,
   formatting, diagnostics, path and git dependencies, `turf.lock`, CI flags,
   debug and release builds.
2. [Writing a library](libraries.md): the root module and what it exposes,
   following the standard library's conventions, error types, generics,
   releasing with git tags, and wrapping a C library.
3. [Testing a project](testing.md): a test suite from the first assertion to CI.
   Fallible code, programs the compiler must refuse, panics, warnings, files,
   random numbers and time.

Working on the compiler itself rather than in TurfLang? See
[CONTRIBUTING.md](../../CONTRIBUTING.md) and
[Testing the compiler](../development/TESTING.md).
