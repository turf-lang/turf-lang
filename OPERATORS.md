# Kirk Language Operator Precedence and Associativity

This document defines the complete set of operators available in the Kirk programming language, along with their precedence and associativity rules.

## Operator Precedence Table

Operators are listed from lowest to highest precedence. Operators on the same line have the same precedence level.

| Precedence | Operator             | Description                      | Associativity |
| ---------- | -------------------- | -------------------------------- | ------------- | ---------- | ------------- |
| 10         | `=`                  | Assignment                       | Right-to-left |
| 15         | `                    |                                  | `             | Logical OR | Left-to-right |
| 20         | `&&`                 | Logical AND                      | Left-to-right |
| 25         | `==`, `!=`           | Equality, Inequality             | Left-to-right |
| 30         | `<`, `>`, `<=`, `>=` | Relational comparison            | Left-to-right |
| 40         | `+`, `-`             | Addition, Subtraction            | Left-to-right |
| 50         | `*`, `/`, `%`        | Multiplication, Division, Modulo | Left-to-right |
| 60         | `^`                  | Exponentiation                   | Left-to-right |

## Operator Categories

### 1. Assignment Operators (Precedence: 10)

- `=` : Variable assignment

**Example:**

```kirk
int x = 5
double y = 3.14
```

### 2. Logical Operators (Precedence: 15-20)

- `||` (OR) : Returns true if at least one operand is true (Precedence: 15)
- `&&` (AND) : Returns true only if both operands are true (Precedence: 20)

**Example:**

```kirk
bool a = true || false    // true
bool b = true && false    // false
bool c = true || false && false  // true (AND evaluated first)
```

**Note:** AND has higher precedence than OR, so in complex expressions, AND operations are evaluated first.

### 3. Equality Operators (Precedence: 25)

- `==` : Equal to
- `!=` : Not equal to

**Example:**

```kirk
bool eq = 5 == 5     // true
bool neq = 10 != 5   // true
```

### 4. Relational Operators (Precedence: 30)

- `<` : Less than
- `>` : Greater than
- `<=` : Less than or equal to
- `>=` : Greater than or equal to

**Example:**

```kirk
bool lt = 5 < 10      // true
bool gte = 5 >= 5     // true
```

### 5. Additive Operators (Precedence: 40)

- `+` : Addition
- `-` : Subtraction

**Example:**

```kirk
int sum = 5 + 3       // 8
int diff = 10 - 4     // 6
```

### 6. Multiplicative Operators (Precedence: 50)

- `*` : Multiplication
- `/` : Division
- `%` : Modulo (remainder)

**Example:**

```kirk
int prod = 5 * 3      // 15
int quot = 10 / 2     // 5
int mod = 17 % 5      // 2
```

### 7. Exponentiation Operator (Precedence: 60)

- `^` : Power/Exponentiation

**Example:**

```kirk
double pow = 2.0 ^ 3.0   // 8.0
```

## Precedence Examples

### Example 1: Arithmetic Precedence

```kirk
int result = 2 + 3 * 4    // 14 (not 20)
```

Explanation: Multiplication has higher precedence than addition, so `3 * 4` is evaluated first (12), then `2 + 12` equals 14.

### Example 2: Comparison and Arithmetic

```kirk
bool result = 5 + 3 > 7    // true
```

Explanation: Addition has higher precedence than comparison, so `5 + 3` is evaluated first (8), then `8 > 7` equals true.

### Example 3: Logical Operators

```kirk
bool result = true || false && false    // true
```

Explanation: AND has higher precedence than OR, so `false && false` is evaluated first (false), then `true || false` equals true.

### Example 4: Complex Expression

```kirk
bool result = 2 + 3 * 4 > 10 && 5 < 10 || false
```

Evaluation order:

1. `3 * 4` = 12 (highest precedence: multiplication)
2. `2 + 12` = 14 (addition)
3. `14 > 10` = true (relational)
4. `5 < 10` = true (relational)
5. `true && true` = true (logical AND)
6. `true || false` = true (logical OR)

Result: true

### Example 5: Using Parentheses

```kirk
int result = (2 + 3) * 4    // 20
```

Explanation: Parentheses have the highest precedence, so `2 + 3` is evaluated first (5), then `5 * 4` equals 20.

## Associativity

All binary operators in Kirk are **left-to-right** associative, meaning that operators of the same precedence are evaluated from left to right.

### Example:

```kirk
int result = 10 - 5 - 2    // 3 (not 7)
```

Explanation: Evaluated as `(10 - 5) - 2` = `5 - 2` = 3

## Type Coercion

When operators are applied to operands of different types, Kirk automatically performs type coercion according to these rules:

1. **Numeric promotion hierarchy:** `bool` → `int` → `double`
2. Operands are promoted to the higher-ranked type
3. Strings cannot be coerced to/from other types

### Example:

```kirk
double result = 5 + 3.14    // 8.14 (int 5 promoted to double)
int cmp = 5 > 3.0           // true (5 promoted to double for comparison, result is bool)
```

## Best Practices

1. **Use parentheses for clarity:** When mixing operators, use parentheses to make your intent explicit, even when not strictly necessary.

   ```kirk
   bool clear = (a > b) && (c < d)    // Clearer than: a > b && c < d
   ```

2. **Avoid complex expressions:** Break complex expressions into multiple statements for better readability.

   ```kirk
   // Instead of:
   bool complex = a + b * c > d && e < f || g != h

   // Write:
   int temp = a + b * c
   bool cond1 = temp > d && e < f
   bool result = cond1 || g != h
   ```

3. **Mind the logical operator precedence:** Remember that AND has higher precedence than OR.
   ```kirk
   // These are different:
   bool a = x || y && z      // Evaluated as: x || (y && z)
   bool b = (x || y) && z    // Explicitly grouped
   ```
