# `random`

Pseudo-random numbers, built on libc `rand()`. Not prelude — requires an
explicit `import random;`.

```turflang
import random;
```

> **Not cryptographically secure.** This is an ordinary `rand()`-based PRNG
> — fine for games, sampling, jitter, test data. Do not use it for anything
> security-sensitive (tokens, keys, nonces). A dedicated CSPRNG binding is
> a possible future addition, not this module.

> **Reseeds on every call, not once.** This language has no mutable
> module-level state at all (`export mut` is a hard compile error — there's
> no way to hide a "have I seeded yet?" flag), so instead of the usual
> "seed once at startup" pattern, every function here reseeds from a mix of
> `time::now_seconds()` and `time::clock_ticks()` (the latter advances at
> sub-second granularity even within the same wall-clock second, which is
> what keeps rapid repeated calls from collapsing onto one repeated value).
> This is simple and has no setup step, but it's neither "seed once, draw a
> long unbroken sequence" nor suitable for anything needing reproducible
> seeding.

### `random_int(min: int, max: int): int`

A pseudo-random integer in `[min, max]`, inclusive on both ends.

```turflang
immut roll: int = random::random_int(1, 6);
test::assert(roll >= 1 && roll <= 6, "");
```

### `random_double(): double`

A pseudo-random value in `[0.0, 1.0)`.

### `random_bool(): bool`

A coin flip, built on `random_int(0, 1)`.
