# State Machines (`machine`)

A `machine` is an enum whose variants are **states**, together with the list
of moves that are allowed between them. A value of a machine type changes
state only by calling `.transition(State)`. Where the compiler can tell
which state the value is in, it checks the move against the declared graph
while compiling and rejects an illegal one. Where it cannot, the move is
checked when the program runs, and an illegal one aborts.

If you have modelled a state machine as a plain [`enum`](structs_and_enums.md#enums)
that functions change "by convention", a `machine` is the same thing with
the rulebook in the type.

## Declaring a machine

```turflang
machine ConnectionState {
    Idle -> Connecting,
    Connecting -> Connected,
    Connecting -> Failed,
    Connected -> Closing,
    Closing -> Idle,
}

fn main(): int {
    mut s: ConnectionState = ConnectionState::Idle;
    s.transition(ConnectionState::Connecting);
    return s to int;   // 1
}
```

The body is a comma-separated list of `From -> To` edges; a trailing comma is
allowed.

- **States are not declared separately.** A state exists because its name
  appears on either side of an edge. Its ordinal is its order of first
  appearance, counting from 0: here `Idle` is 0, `Connecting` 1, `Connected`
  2, `Failed` 3 and `Closing` 4.
- The same edge twice is `E114` (`Machine 'M' already declares a transition
  from 'A' to 'B'`).
- A missing `->` is `E110`.
- A state with no incoming edge anywhere in the machine produces a warning
  (`Machine 'ConnectionState' declares state 'Idle' with no incoming
  transitions`). That is the normal situation for a start state, so the
  warning is safe to ignore for it; for any other state it usually means a
  missing edge. There is no syntax to mark a start state.
- A state that has to exist must appear in at least one edge.
- `pub machine Name { ... }` makes the machine and all its states visible to
  other modules. `pub` on a single state or edge is `E087` (`'pub' can't be
  applied to a machine state or transition`).
- A machine has no `::default()` (`ConnectionState::default()` is `E135`).
  Initialise a variable with the state you want; a **struct field** of
  machine type that you leave alone starts as the machine's first state.

## A machine is an enum

Everything that works for an enum works for a machine, with no extra syntax:

| Form | Meaning |
|---|---|
| `Machine::State` | A state value. |
| `a == b`, `a != b` | Same state or not. |
| `(s to int)` | The ordinal. |
| `(s to string)` | The state's name. |
| `("Name" to Machine)` | The state with that name (aborts at run time if none). |
| `Machine::COUNT` | Number of states. |
| `compare s { ... }` | Matching, with the same exhaustiveness rules. |

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
    test::assert((idle to int) == 0, "Idle has ordinal 0");

    immut connecting: ConnectionState = ConnectionState::Connecting;
    test::assert(idle != connecting, "Idle is not Connecting");
    test::assert((connecting to string) == "Connecting", "state name");

    test::assert(ConnectionState::COUNT == 5, "five states");
    return 0;
}
```

Ordering operators are not defined for machines, as for enums. The
other rules for enums, including `compare` exhaustiveness (`E093`) and
casts, are on [Structs and Enums](structs_and_enums.md#enums).

## `.transition(State)`

`value.transition(Machine::State)` moves `value` to the named state. It has
no result. Two rules hold at every call:

1. It takes exactly one argument. Any other count is `E044` (`'transition'
   expects exactly 1 argument (the target state)`).
2. The argument must be written as a state of the **same** machine as the
   receiver, `Machine::State`. A state of another machine is `E036`
   (`'transition' target must be a state of 'ConnectionState', got
   'TrafficLight'`), and so is any other expression, such as a variable
   that holds a state. A name that is no state at all is the ordinary
   `E039` (`Enum 'ConnectionState' has no variant 'Bogus'`).

<!-- doc-example: fails E036 -->
```turflang
machine ConnectionState { Idle -> Connecting, Connecting -> Connected }
machine TrafficLight { Red -> Green, Green -> Yellow, Yellow -> Red }

fn main(): int {
    mut conn: ConnectionState = ConnectionState::Idle;
    conn.transition(TrafficLight::Red);   // E036: not a ConnectionState
    return 0;
}
```

`transition` is not a reserved word. It means "change state" only when the
receiver's type is a machine; on anything else, `.transition(...)` is an
ordinary call to a function you declared (see
[UFCS](functions_and_modules.md#3-ufcs-calling-a-function-as-a-method)):

```turflang
fn transition(x: int, y: int): int { return x + y; }

fn main(): int {
    mut n: int = 3;
    return n.transition(4);   // an ordinary call: 7
}
```

`transition` is the only operation a machine adds. There are no entry or
exit hooks and no data attached to a state; keep that data in the struct
that holds the machine value (see the [example below](#a-complete-example)).

## Checked while compiling

When the compiler knows exactly which state a value is in at a `.transition`
call, it checks the move against the declared edges. A legal move compiles
to a single store of the new state, with no test and no possible abort. An
illegal move is a compile error, not a run-time failure.

The compiler knows the state in the body of a `compare` arm when:

- the arm has exactly one pattern (`matches M::State:`; not several stacked
  `matches` lines sharing a body);
- the `compare` subject is a plain variable (not `obj.field` or another
  expression);
- the pattern is written `Machine::State`; and
- the `.transition` receiver is that same variable.

```turflang
machine ConnectionState {
    Idle -> Connecting,
    Connecting -> Connected,
    Connecting -> Failed,
    Connected -> Closing,
    Closing -> Idle,
}

fn advance(start: ConnectionState): ConnectionState {
    mut conn: ConnectionState = start;
    compare conn {
        matches ConnectionState::Idle:
            conn.transition(ConnectionState::Connecting);   // checked: Idle -> Connecting exists
        done
        matches ConnectionState::Connecting:
            conn.transition(ConnectionState::Connected);    // checked
        done
        matches ConnectionState::Connected:
            conn.transition(ConnectionState::Closing);      // checked
        done
        matches ConnectionState::Closing:
            conn.transition(ConnectionState::Idle);         // checked
        done
        otherwise:
            mut nothing: int = 0;
    }
    return conn;
}

fn main(): int {
    return advance(ConnectionState::Idle) to int;   // 1
}
```

If the matched state has no edge to the target, the program does not
compile. The error is `E035`, at the `.transition` call:

<!-- doc-example: fails E035 -->
```turflang
machine ConnectionState {
    Idle -> Connecting,
    Connecting -> Connected,
    Connecting -> Failed,
    Connected -> Closing,
    Closing -> Idle,
}

fn main(): int {
    mut conn: ConnectionState = ConnectionState::Idle;
    compare conn {
        matches ConnectionState::Idle:
            conn.transition(ConnectionState::Closing);   // E035: no edge Idle -> Closing
        done
        otherwise:
            mut nothing: int = 0;
    }
    return 0;
}
```

```text
error [E035]: 'ConnectionState' has no transition from 'Idle' to 'Closing'.
  Hint: This arm has already narrowed the receiver to 'Idle', which has no
  declared edge to 'Closing' -- add `Idle -> Closing,` to the machine's
  declaration, or transition to a state 'Idle' actually has an edge to.
```

The check compares the target with the state the **arm matched**. Make one
transition per arm: a second `.transition` in the same arm is also checked
against the matched state, not the state the first call produced, and is
rejected unless the machine declares that edge from the matched state
itself. To chain moves, use one call per `compare`, or a loop that matches
again.

## Checked when it runs

Every `.transition` call that does not meet the conditions above compiles
without complaint and is checked at run time instead. The generated code
looks at the receiver's current state: if it has an edge to the target, the
state is replaced; if not, the program aborts with a message such as

```text
panic: runtime check: ConnectionState::transition(Closing) -- no such transition from the current state. (prog.tr:12)
```

The message names the target state and the source line. This is how the
calls below are handled:

- **Outside any `compare`**, or in a function that received the value as
  a parameter. The check cannot fail if the program's logic is right:

  ```turflang
  machine ConnectionState {
      Idle -> Connecting,
      Connecting -> Connected,
      Connecting -> Failed,
      Connected -> Closing,
      Closing -> Idle,
  }

  fn main(): int {
      mut conn: ConnectionState = ConnectionState::Idle;
      conn.transition(ConnectionState::Connecting);   // checked at run time; legal
      return conn to int;
  }
  ```

- **In an arm with stacked patterns.** The arm is entered from either state,
  so the compiler does not pick one; the call is checked at run time:

  ```turflang
  machine ConnectionState {
      Idle -> Connecting,
      Connecting -> Connected,
      Connecting -> Failed,
      Connected -> Closing,
      Closing -> Idle,
  }

  fn main(): int {
      mut conn: ConnectionState = ConnectionState::Idle;
      compare conn {
          matches ConnectionState::Idle:
          matches ConnectionState::Closing:
              // Both states are possible here, so this is not decided at
              // compile time. Idle -> Connecting exists; Closing -> Connecting does not.
              conn.transition(ConnectionState::Connecting);
          done
          otherwise:
              mut nothing: int = 0;
      }
      return conn to int;
  }
  ```

- **On a field or other expression**, such as `self.state.transition(...)`
  or a `compare` over `obj.state`.
- **A move the machine does not allow, where nothing narrows the state.**
  This compiles and aborts when the call runs:

  ```turflang
  machine ConnectionState {
      Idle -> Connecting,
      Connecting -> Connected,
      Connecting -> Failed,
      Connected -> Closing,
      Closing -> Idle,
  }

  fn main(): int {
      mut conn: ConnectionState = ConnectionState::Idle;
      conn.transition(ConnectionState::Closing);   // aborts: Idle has no edge to Closing
      return 0;
  }
  ```

When the transition is guarded by a test the compiler does not follow
(`if (state == M::Placed)`), the run-time check still protects you. A
program that must never abort should match on the state in a `compare` over a
plain variable, so that every move is proved while compiling.

## A complete example

An order moves through a fixed lifecycle. The machine holds the rules; the
struct holds the order's data and the current state.

```turflang
import io;

machine Order {
    Placed -> Paid,
    Placed -> Cancelled,
    Paid -> Shipped,
    Paid -> Cancelled,
    Shipped -> Delivered,
}

struct Ticket {
    id: int,
    state: Order,
}

fn Ticket.new(id: int): void {
    self.id = id;
    self.state = Order::Placed;
}

// Copy the state into a variable and match on it. Each arm names the state
// it handles, so each transition is checked against the declared edges while
// compiling: changing `Order::Shipped` below to `Order::Paid` would not build.
fn Ticket.advance(): void {
    mut s: Order = self.state;
    compare s {
        matches Order::Placed:
            s.transition(Order::Paid);
        done
        matches Order::Paid:
            s.transition(Order::Shipped);
        done
        matches Order::Shipped:
            s.transition(Order::Delivered);
        done
        otherwise:
            io::printline("nothing to do");
    }
    self.state = s;
}

// `self.state` is a field, so this transition is checked when it runs; the
// `if` keeps a Shipped or Delivered order from reaching it.
fn Ticket.cancel(): bool {
    if (self.state == Order::Placed || self.state == Order::Paid) {
        self.state.transition(Order::Cancelled);
        return true;
    }
    return false;
}

fn main(): int {
    mut t: Ticket = Ticket::new(7);
    t.advance();                              // Placed -> Paid
    io::printline(t.state to string);         // Paid
    t.advance();                              // Paid -> Shipped
    io::printline(t.state to string);         // Shipped

    if (t.cancel()) {
        io::printline("cancelled");
    } else {
        io::printline("too late to cancel");  // printed: it already shipped
    }

    t.advance();                              // Shipped -> Delivered
    t.advance();                              // nothing to do
    io::printline(t.state to string);         // Delivered
    return 0;
}
```

This prints `Paid`, `Shipped`, `too late to cancel`, `nothing to do`,
`Delivered`. The compiler warns that `Placed` has no incoming transition;
it is the start state, so that is expected.

## Errors and warnings

| Code | Message | Cause |
|---|---|---|
| `E035` | `'M' has no transition from 'A' to 'B'` | A `.transition` in a single-pattern `compare` arm whose matched state has no edge to the target. |
| `E036` | `'transition' target must be a state of 'M', got 'X'` | The target is not written as a state of the receiver's machine. |
| `E039` | `Enum 'M' has no variant 'X'` | The state name does not exist. |
| `E044` | `'transition' expects exactly 1 argument` | Wrong number of arguments. |
| `E087` | `'pub' can't be applied to a machine state or transition` | `pub` on a state; write `pub machine`. |
| `E110` | expected `->` | A malformed edge. |
| `E114` | `Machine 'M' already declares a transition from 'A' to 'B'` | The same edge twice. |
| `E135` | `'M::default' is an enum variant, not a function` | `M::default()`; name a state instead. |
| warning | `Machine 'M' declares state 'S' with no incoming transitions` | Nothing leads to `S`; expected for a start state. |
| abort | `M::transition(X) -- no such transition from the current state` | A run-time-checked move with no edge. |
