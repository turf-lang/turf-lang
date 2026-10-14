# Random numbers: `random`

Pseudo-random integers, doubles and booleans. Not prelude: it needs
`import random;`.

```turflang
import random;
```

> **Not for security.** The generator is fast and evenly spread, but it is
> predictable: someone who sees a few outputs can work out the rest. Do not use
> it for passwords, tokens, keys, nonces or anything an attacker must not guess.
> There is no cryptographic generator in the standard library.

## How it is seeded

The runtime keeps one generator for the whole program. It seeds itself from the
operating system's entropy the first time you draw a number, and every draw
advances it. There is nothing to call first, and two runs of the same program
give different numbers.

The corollary is that **you cannot choose the seed**, so a sequence cannot be
reproduced. A test or simulation that needs repeatable "random" data must supply
its own generator (a few lines of arithmetic on an `int` it keeps).

## Functions

### `random_int(min: int, max: int): int`

A pseudo-random integer from `min` to `max`, **both included**.
`random_int(1, 6)` is a die roll, and `random_int(5, 5)` is always `5`.

- **`max < min` panics**, because no value fits. The message starts with your
  call's `file:line:`:

  ```text
  dice.tr:10: random_int: max (3) is less than min (7)
  ```

- Any two `int`s work, up to the whole range of `int`, and every value in the
  range can occur.
- A range of fewer than 2^62 values is served by `draw % width` from a 62-bit
  draw, which favours the low end by less than one part in 2^30: not noticeable
  for dice, shuffles or sampling, but not suitable for a statistical test of
  uniformity. A wider range draws whole 64-bit values and keeps the first one
  inside it, which is exact.

### `random_double(): double`

A pseudo-random double from `0.0` up to, but **not including**, `1.0`.
Multiply and add to scale it: `lo + random_double() * (hi - lo)`.

### `random_bool(): bool`

`true` or `false`, each about half the time.

### `reseed(): void`

Discards the current state and draws a fresh seed from the operating system.
You never need it for correct output; it exists to start a new sequence on
purpose.

### `RAND_MAX: int`

`2147483647` (2^31 - 1), the largest value one draw of the underlying generator
produces (`random_int` combines several). It is the same on every platform.

## Example: dealing and rolling

```turflang
import random;

fn main(): int {
    immut die: int = random::random_int(1, 6);
    printline("you rolled " + (die to string));

    immut chance: double = random::random_double();
    if chance < 0.25 {
        printline("a lucky roll");
    }

    if random::random_bool() {
        printline("heads");
    } else {
        printline("tails");
    }

    // A temperature between 15.0 and 25.0.
    immut celsius: double = 15.0 + random::random_double() * 10.0;
    printline(celsius >= 15.0 && celsius < 25.0);   // true
    return 0;
}
```

## Example: a shuffle

Shuffling a list is the standard use of `random_int` with a shrinking range:
walk from the end and swap each element with a random one at or before it.

```turflang
import random;
import list::List;

fn main(): int {
    mut deck: List<int> = List::default();
    mut n: int = 1;
    while n <= 5 {
        deck.push(n);
        n = n + 1;
    }

    mut i: int = deck.length() - 1;
    while i > 0 {
        immut j: int = random::random_int(0, i);
        immut held: int = deck.get(i);
        deck.set(i, deck.get(j));
        deck.set(j, held);
        i = i - 1;
    }

    mut line: string = "";
    mut k: int = 0;
    while k < deck.length() {
        line = line + (deck.get(k) to string) + " ";
        k = k + 1;
    }
    printline(line);   // for example: 3 5 1 4 2
    return 0;
}
```
