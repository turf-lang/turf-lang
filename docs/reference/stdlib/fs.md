# `fs`

Path-string manipulation and directory operations. Not prelude — requires
an explicit `import fs;`. For file *content* I/O (reading/writing a file's
bytes), see [`io`](./io_and_net.md) instead — this module is specifically
about paths and directories.

```turflang
import fs;
```

## Pure path-string helpers

No FFI, no filesystem access — plain string manipulation.

### `join(a: string, b: string): string`

Joins two path segments with exactly one `/` between them (never `a//b` or
`ab`, regardless of whether `a` already ends in `/`).

### `dirname(path: string): string`

Everything before the last `/` (or `"."` if there's no `/`).

### `basename(path: string): string`

Everything after the last `/` (or the whole string if there's no `/`).

### `extension(path: string): string`

Everything after the last `.` in the basename, or `""` if there's none. A
leading-dot "hidden file" name like `.gitignore` (no `.` after the first
character) has no extension, correctly returning `""`, not `"gitignore"`.

### `is_absolute(path: string): bool`

`true` iff `path` starts with `/`.

```turflang
test::assert(fs::basename("a/b/c.txt") == "c.txt", "");
test::assert(fs::extension("a/b/c.txt") == "txt", "");
test::assert(fs::extension(".gitignore") == "", "hidden files have no extension");
test::assert(fs::is_absolute("/etc/hosts"), "");
```

## Directory operations

Real `extern fn` bindings.

### `mkdir(path: string): bool`

Creates a directory with mode `0755`. Returns `true` on success.

### `remove_file(path: string): bool`

Deletes a file (`remove(2)`).

### `remove_dir(path: string): bool`

Removes a directory — **only if it's empty** (`rmdir(2)`'s own restriction,
not additionally checked/worked around here).

### `is_dir(path: string): bool`

`true` iff `path` can be `opendir`'d. Implemented via the
`opendir`/non-null/`closedir` trick rather than parsing `struct stat` —
this compiler can't bind an arbitrary C struct's field layout (see
[`docs/architecture/pointers_and_references.md`](../../architecture/pointers_and_references.md)),
and `opendir` succeeding is a sufficient, simpler test.

```turflang
fs::mkdir("scratch");
test::assert(fs::is_dir("scratch"), "");
fs::remove_dir("scratch");
```
