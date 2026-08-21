# State Machines (`machine`)

`machine` declares an enum-like set of named states together with a
directed transition graph between them. Calling `.transition(TargetState)`
on a machine-typed value moves it to a new state — and depending on where
that call appears in your code, the compiler either **proves the transition
is valid at compile time** (a zero-cost, plain store, no branch) or falls
back to a **runtime-checked switch that panics on an invalid transition**.

If you've ever modeled a state machine with a plain `enum` and a function
that mutates it by convention, `machine` is the same idea with the
transition table promoted into the type system, so illegal transitions can
be caught before the program ever runs.

## Declaring a machine

```turflang
machine ConnectionState {
    Idle -> Connecting,
    Connecting -> Connected,
    Connecting -> Failed,
    Connected -> Closing,
    Closing -> Idle,
}
```

- The keyword `machine`, followed by a name, followed by `{ }`.
- The body is a comma-separated list of `From -> To` edges.
- **States are never declared in a separate list.** A state comes into
  existence the first time its name appears on either side of an edge.
  Just like enum variants, a state's ordinal is its first-appearance order
  (0-based) — in the example above, `Idle` = 0, `Connecting` = 1,
  `Connected` = 2, `Failed` = 3, `Closing` = 4.
- Declaring the exact same edge twice in one `machine` block is a
  parse-time error.

> [!NOTE]
> A state with no incoming edges anywhere in its machine's own declaration
> (nothing ever transitions *into* it) triggers `UnreachableStateWarning` —
> a warning, not an error. This is expected for a machine's legitimate
> start state: in the example above, nothing transitions into `Idle`, which
> is exactly what you want for a start state. There is currently no
> dedicated syntax to mark a state as "the" entry point; the warning is
> just a heads-up, not a rejection.

## Using a machine like an enum

A `machine` type behaves exactly like an `enum` type for everything except
transitions: `::`-qualified access, `==`/`!=` comparison, `(v to int)`
ordinal casting, `COUNT`, and `compare`/`matches` exhaustiveness checking
all work identically, with no special-casing needed on your part.

```turflang
machine ConnectionState {
    Idle -> Connecting,
    Connecting -> Connected,
    Connecting -> Failed,
    Connected -> Closing,
    Closing -> Idle,
}

fn main(): int {
    immut idle: ConnectionState = ConnectionState::Idle;
    test::assert((idle to int) == 0, "Idle should have ordinal 0");

    immut connecting: ConnectionState = ConnectionState::Connecting;
    test::assert(idle != connecting, "Idle should not equal Connecting");

    test::assert(ConnectionState::COUNT == 5, "ConnectionState should have 5 states");
    return 0;
}
```

This isn't a coincidence — under the hood, a `machine` type *is* an enum
type with one extra piece of data attached (its edge set). Because of that,
every part of the compiler that already knows how to handle enums (`::`
access, equality, ordinal casting, exhaustiveness checking) handles a
`machine` for free, with zero machine-specific logic required in any of
those code paths.

## `.transition(TargetState)`

To move a machine-typed value to a new state, call `.transition(...)` on
it with the target state:

```turflang
mut conn: ConnectionState = ConnectionState::Idle;
conn.transition(ConnectionState::Connecting);
```

A few rules apply to every `.transition()` call, regardless of where it
appears:

1. It must take exactly one argument (the target state) — any other arity
   is a compile error.
2. The argument must be a state belonging to the **same** `machine`
   declaration as the receiver. Passing a state from a different `machine`
   type is rejected:

   ```turflang
   machine ConnectionState { Idle -> Connecting, Connecting -> Connected }
   machine TrafficLight { Red -> Green, Green -> Yellow, Yellow -> Red }

   mut conn: ConnectionState = ConnectionState::Idle;
   conn.transition(TrafficLight::Red); // ERROR: TrafficLight::Red is not a ConnectionState
   ```

   (Referencing a genuinely nonexistent variant name, like
   `ConnectionState::Bogus`, is caught even earlier, by the ordinary
   `::`-access resolution that every enum-like type shares.)

> [!NOTE]
> `transition` is **not** a reserved word. The compiler only intercepts a
> `.transition(...)` call when the *receiver's type* is a `machine` type.
> Calling `.transition(...)` on anything else (an `int`, for example) just
> resolves as an ordinary function call via UFCS, if you happen to have
> declared a function with that name:
>
> ```turflang
> fn transition(x: int, y: int): int { return x + y; }
>
> fn main(): int {
>     mut n: int = 3;
>     test::assert(n.transition(4) == 7, ""); // ordinary UFCS call, unrelated to any machine
> }
> ```

## Compile-time-proven transitions

This is the feature's central payoff: when the compiler can determine
**exactly** which state a machine value is in at a given `.transition()`
call site, it checks that transition against the declared edge graph right
then, at compile time — and if it's valid, the compiled code is just a
plain store with no runtime check at all.

This compile-time narrowing kicks in when **all** of the following hold:

- The `.transition()` call is inside the body of a `compare` arm.
- That arm has exactly **one** pattern (`matches SomeState:` — not a
  stacked `matches A: matches B:` arm sharing one body).
- The `compare` subject is a plain variable (not a field access or other
  expression).
- The arm's pattern is a `MachineName::State`-shaped scope access.

When those conditions hold, the compiler knows precisely which state the
receiver is in inside that arm's body, and can check the transition against
the declared graph immediately:

```turflang
fn advance(conn_ref: ConnectionState): ConnectionState {
    mut conn: ConnectionState = conn_ref;
    compare conn {
        matches ConnectionState::Idle:
            conn.transition(ConnectionState::Connecting); // PROVEN: Idle -> Connecting is declared
        done
        matches ConnectionState::Connecting:
            conn.transition(ConnectionState::Connected);  // PROVEN
        done
        matches ConnectionState::Connected:
            conn.transition(ConnectionState::Closing);    // PROVEN
        done
        matches ConnectionState::Closing:
            conn.transition(ConnectionState::Idle);       // PROVEN
        done
        otherwise:
            mut noop: int = 0;
    }
    return conn;
}
```

If the narrowed state has **no** declared edge to the requested target,
this is a genuine **compile-time error** — not deferred to runtime:

```turflang
compare conn {
    matches ConnectionState::Idle:
        conn.transition(ConnectionState::Closing); // COMPILE ERROR: Idle has no edge to Closing
    done
    ...
}
```

When a transition is proven this way, the generated code is a single store
of the target state's ordinal — no switch, no branch, no possibility of a
runtime panic for that call site.

## Runtime-checked transitions

Any `.transition()` call that does **not** meet all of the narrowing
conditions above compiles cleanly, but the compiler cannot statically prove
which state the receiver will actually be in when the code runs. For these
calls, the compiler emits a **runtime switch with a panic guard**:

- The generated code switches on the receiver's actual runtime value.
- There is one case for every source state that has a declared edge to the
  requested target.
- A matching case stores the target state and continues normally.
- No match falls through to a panic that aborts the program, with a
  message like:

  ```
  ConnectionState::transition(Closing) -- no such transition from the current state.
  ```

This happens in three situations:

**1. Outside any `compare`, but still valid at runtime.** This compiles and
runs successfully — the runtime check simply confirms what's true and
proceeds:

```turflang
mut conn: ConnectionState = ConnectionState::Idle;
conn.transition(ConnectionState::Connecting); // runtime-guarded; Idle really does have this edge
```

**2. Inside a stacked-pattern `matches` arm.** Narrowing only applies to
single-pattern arms — a `matches A: matches B:` arm sharing one body is
*outside* the compile-time-narrowing scope (this is a deliberate v1
limitation, not a bug), so any `.transition()` inside such an arm is
runtime-guarded even though the compiler could, in principle, reason about
the union of the two states:

```turflang
compare conn {
    matches ConnectionState::Idle:
    matches ConnectionState::Closing:
        // Both states have SOME edge to Connecting, but only from Idle --
        // the compiler doesn't statically know which of the two `conn`
        // actually is here, so this is runtime-guarded, not proven.
        conn.transition(ConnectionState::Connecting);
    done
    otherwise: mut noop: int = 0;
}
```

**3. A genuinely invalid runtime transition.** An unnarrowed call whose
actual runtime state has no edge to the requested target compiles cleanly
(the compiler has no static proof either way outside a narrowed context)
and aborts the program at runtime via the panic guard:

```turflang
mut conn: ConnectionState = ConnectionState::Idle;
conn.transition(ConnectionState::Closing); // Idle has no edge to Closing -- aborts at runtime
```

## Worked example: a traffic light

This is the canonical demonstration of the feature, rewritten from a
plain-`enum` version of the same state machine to show what `machine`
buys you. With a plain enum, nothing stops a bug from producing an invalid
transition — it would only surface (if ever) as a wrong value at runtime.
With `machine`, the identical transition table is checked for free at
compile time, because every `compare` arm below narrows the receiver to
one specific light color before calling `.transition()`.

```turflang
import test;

machine TrafficLight {
    Red -> Green,
    Green -> Yellow,
    Yellow -> Red,
}

fn next_state(light_ref: TrafficLight): TrafficLight {
    mut light: TrafficLight = light_ref;
    compare light {
        matches TrafficLight::Red:
            // Compile-time-proven: TrafficLight declares Red -> Green.
            light.transition(TrafficLight::Green);
        done
        matches TrafficLight::Green:
            // Compile-time-proven: TrafficLight declares Green -> Yellow.
            light.transition(TrafficLight::Yellow);
        done
        matches TrafficLight::Yellow:
            // Compile-time-proven: TrafficLight declares Yellow -> Red.
            light.transition(TrafficLight::Red);
        done
    }
    return light;
}

fn main(): int {
    mut light: TrafficLight = TrafficLight::Red;

    // Cycle through: Red -> Green -> Yellow -> Red -> Green
    light = next_state(light);
    test::assert(light == TrafficLight::Green, "Red -> Green");

    light = next_state(light);
    test::assert(light == TrafficLight::Yellow, "Green -> Yellow");

    light = next_state(light);
    test::assert(light == TrafficLight::Red, "Yellow -> Red");

    light = next_state(light);
    test::assert(light == TrafficLight::Green, "Red -> Green again");

    // Ordinals follow first-appearance order, identical to a plain enum:
    // Red = 0, Green = 1, Yellow = 2.
    test::assert((TrafficLight::Red    to int) == 0, "Red == 0");
    test::assert((TrafficLight::Green  to int) == 1, "Green == 1");
    test::assert((TrafficLight::Yellow to int) == 2, "Yellow == 2");
    return 0;
}
```

Notice that `next_state()`'s `compare` is exhaustive over all three states
of `TrafficLight` (no `otherwise` needed), and every arm narrows `light` to
exactly one state before transitioning — so every `.transition()` call
here is compile-time-proven, and this function compiles down to code with
no runtime transition-validity check anywhere at all.

## Errors and warnings reference

| Error/Warning | Trigger |
|---|---|
| `InvalidTransitionTargetError` | `.transition(X)` where `X` isn't a state belonging to the same `machine` type as the receiver. |
| `InvalidTransitionError` | `.transition(X)` inside a narrowed `matches` arm where the narrowed state has no declared edge to `X` — a genuine compile-time error with a caret at the call site. |
| `UnreachableStateWarning` | A declared state with zero incoming edges. Warning only — expected for a legitimate start state. |
| (parse error, unnamed) | The same `From -> To` edge declared twice in one `machine` block. |
| Runtime panic | An unnarrowed `.transition(X)` call whose actual runtime state has no edge to `X` — compiles cleanly, aborts at runtime. |

## Gotchas

- Narrowing only fires for a **single-pattern** `matches` arm on a **plain
  variable** subject matched against a **scope-access pattern**
  (`MachineName::State`). Field accesses, expressions, and stacked patterns
  all fall back to the runtime-checked path — this is a scope limitation of
  the current implementation, not a rejection; your code still compiles and
  behaves correctly, it just loses the zero-cost guarantee for that call
  site.
- There is no dedicated syntax to mark a particular state as the machine's
  entry point — `UnreachableStateWarning` is your signal that a state has
  no incoming edges, which is usually (but not verified to be) intentional.
- A `machine`'s states are not declared in a separate list; they are
  inferred entirely from what appears in the edges. If you need a state
  with no outgoing *or* incoming edges to exist for some reason, `machine`
  as it stands today cannot express that — every state must appear in at
  least one edge.
