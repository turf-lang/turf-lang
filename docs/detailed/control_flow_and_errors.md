# Control Flow and Errors

Turf-Lang features a robust set of control flow structures and a structured exception handling system to build complex logic safely.

---

## Conditionals

### If / Elseif / Else

Turf-Lang provides two syntactical forms for conditionals depending on your needs.

#### 1. Expression Form
Useful for quick, single-line assignments based on a condition.
```turf
int val;
if x > 0 then val = 1 else val = -1;
```

#### 2. Block Form
Standard branching logic. Note that `elseif` requires its own block `{}`.

```turf
if score >= 90 {
    printline("A");
} elseif score >= 80 {
    printline("B");
} else {
    printline("F");
}
```

---

## Loops

### While Loops

The `while` loop executes a block of code as long as the condition evaluates to `true`.

```turf
int count = 0;
while count < 5 {
    printline(count);
    count++;
}
```

### For Loops

Turf-Lang's `for` loop is range-based and explicitly requires a `step` expression.

```turf
// syntax: for var in start .. end step size { ... }
for i in 0 .. 10 step 1 {
    printline(i);
}
```
*Note: The loop condition implicitly checks if the iterator has reached the `end` value. It does not include the end value (i.e. strictly `<` or `>`).*

### Break and Continue

You can manipulate loop execution using standard `break` and `continue` keywords.

```turf
for i in 0 .. 100 step 1 {
    if i % 2 == 0 {
        continue; // Skip even numbers
    }
    if i > 50 {
        break; // Stop entirely if over 50
    }
    printline(i);
}
```

---

## Error Handling

Turf-Lang supports structured exception handling. This helps catch runtime errors (such as out-of-bounds arrays) or manual errors raised in your code.

### Raise

To throw an exception, use the `raise` keyword followed by an expression or payload. Commonly, this payload is an instantiated exception struct.

```turf
import prelude

fn int divide(int a, int b) {
    if b == 0 {
        raise ArithmeticException("Division by zero");
    }
    return a / b;
}
```

### Try / OnError

To catch exceptions, use the `try { ... } onError (err) { ... }` construct.

```turf
fn int safe_math() {
    try {
        int result = divide(10, 0);
        printline(result);
    } onError (err) {
        printline("Caught an error!");
        // 'err' contains the payload raised
    }
    return 0;
}
```

---

## Frequently Asked Questions (FAQs)

**Q: Do I need parentheses around `if` and `while` conditions?**
A: No, parentheses are not required around conditions in Turf-Lang, though you can use them if you prefer.

**Q: Can I use `return` inside a `try` block?**
A: Yes, you can `return` safely from within `try` or `onError` blocks. The internal scoping correctly handles early exits.

**Q: What happens if an exception is raised but not caught?**
A: The program will terminate and standard error traces (if implemented by the LLVM host) will be triggered.
