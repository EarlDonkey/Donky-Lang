# Donky Language Semantics — v2.3

**Status:** Normative for the shipped engine (`aot.rs` + `runtime_template.c`).
**Last updated:** post-semantics-pass (Task A + B2 + C1 + A2 + D-fix).
**Frozen, non-normative:** `interpreter.rs`, `vm.rs`, `codegen.rs`. Where they
disagree with this document, this document is correct, and the divergence is
listed in §12.

---

## 0. Conventions

### 0.1 Value representation (NaN-boxing)

Every Donky value is a 64-bit word.

- **Number:** any word whose `(v & 0xFFF8_0000_0000_0000) != 0x7FF8_0000_0000_0000`.
  Numbers are the raw IEEE-754 `f64` bit pattern, with one exception:
  **canonical NaN is `0xFFF8_0000_0000_0000`** (a negative quiet NaN). Any NaN
  produced by the runtime or by an AOT inline arithmetic op is rewritten to
  this word before it becomes a value, so a NaN can never be mistaken for null.
  (`+0.0` and `-0.0` are distinct words; both compare equal via `==`.)
- **Tagged value:** a word whose masked bits equal `TAG_BASE = 0x7FF8_0000_0000_0000`.
  Bits 48–50 are the tag; the low 48 bits are a payload (usually a pointer).

| Tag | Name | Meaning |
|---|---|---|
| 0 | null | The word is exactly `TAG_BASE`. |
| 1 | bool | Low bit is the value (`1` = true, `0` = false). |
| 2 | string | Payload is a pointer to a GC-managed C string. |
| 3 | array | Payload is a pointer to a GC-managed `Array { data, len, cap }`. |
| 4 | map | Payload is a pointer to a GC-managed `Map { keys, vals, ... }`. |
| 5 | struct | **Unused at v2.3.** Structs are maps with a `__type__` entry (§7). |
| 6 | function | Payload is a pointer to a GC-managed code address (§9). |
| 7 | closure | **Unused at v2.3.** Closures capture environments (future). |

**Canonical word forms:**
- `null` = `0x7FF8_0000_0000_0000`
- `true` = `0x7FF9_0000_0000_0001`
- `false` = `0x7FF9_0000_0000_0000`
- A number `n` = `f64::to_bits(n)`, unless the result collides with `TAG_BASE`,
  in which case it is `0xFFF8_0000_0000_0000`.

### 0.2 Two kinds of booleans

Donky has **one** boolean type: the tagged word `true`/`false` (§0.1).
It has **no** separate "number 1.0/0.0 means true/false" convention.

Consequences:
- `print(true)` prints `true`; `print(1 < 2)` prints `true` (because `1 < 2`
  produces a tagged `true`, not the number `1.0`).
- `donky_eq(true, 1 < 2)` returns `1.0` (they are the same word).
- A function that returns "success" returns `true`/`false`, not `1.0`/`0.0`.
  See §6 for the exhaustive list.
- Comparisons, `!`, `&&`, `||` all produce tagged booleans.
- `donky_eq`, `donky_truthy`, and `donky_has_exception` return **numbers**
  (`1.0`/`0.0` or C `int` 0/1), because they are consumed by control-flow
  lowering, not by user code. This is an implementation detail; user code
  never sees these numeric returns.

### 0.3 Exceptions

Exceptions are **per-thread** (TLS). A thrown value is stored in the throwing
thread's slot via `donky_throw(v)`. `donky_has_exception()` returns `1` if the
slot is non-zero. `donky_clear_exception()` resets it.

`aot.rs` emits a `donky_has_exception()` check **after every call** that can
throw (§10), and **after every operator or index op** that can throw. When a
throw is pending:

- Inside `try { ... } catch e { ... }`: control jumps to the catch block,
  which reads `donky_get_exception()` into `e`, then calls
  `donky_clear_exception()`, then runs the catch body.
- Outside any `try`: the current function returns `null` immediately (the
  pending exception is **not** cleared, so it propagates up through every
  caller's `has_exception` check until a `try` catches it or `main` reports
  it at exit with `Uncaught Exception: <value>` and exit code 1).

**Any value can be thrown.** The exception slot uses `UINT64_MAX` as the "no exception" sentinel. `throw 0`, `throw null`, and `throw false` all work correctly and are caught by `try/catch`.

### 0.4 Number rendering

`print(n)` / `str(n)` / JSON encoding for a number `n`:
- If `n` is `NaN`: `"nan"`
- If `n` is `+inf`: `"inf"`
- If `n` is `-inf`: `"-inf"`
- If `n` is an integer `|n| < 9.2e18`: printed as an integer (no decimal point)
- Otherwise: shortest `%g` (15–17 significant digits) that round-trips to the
  same `f64`

### 0.5 Container rendering

`print(array)` and `str(array)` render identically: `[e1, e2, ...]`, with each
element rendered as in §0.4/§0.5. **Strings inside a container are quoted**
(`"a"` renders as `"\"a\""`). Depth is capped at 32; at depth 32 the container
renders as `"..."` (protects against cycles and the stack).

`print(map)` and `str(map)` render identically: `{k1: v1, k2: v2, ...}` in
insertion order.

**`print(string)` and `str(string)` are different.** `print("hi")` prints
`hi` (no quotes); `str("hi")` returns the string `hi` (unchanged, because
`str(s)` for `s` a string is `s` itself). But `print(["hi"])` prints
`["hi"]` — the nested string is quoted. This is intentional: `print` is meant
to be human-readable for top-level strings and unambiguous inside containers.

---

## 1. Arithmetic

### 1.1 `+` (operator and `donky_add`)

- `number + number` → number (sum).
- `string + string` → string (concatenation).
- **Any other pair throws** `"cannot add <type(a)> and <type(b)>"` and
  returns `null`. The `AOT`'s `+` operator routes to `donky_add` unless both
  operands are statically known numeric, in which case it inlines `fadd` +
  `donky_make_number` (no throw, because both sides are numbers).
- Integer `+` of `1` and `2` yields `3` (an f64). There is no integer type.

### 1.2 `-`, `*`, `/` (operators) and `donky_sub`, `donky_mul`, `donky_div`

- `-`, `*`, and `/` throw on type mismatch (via `donky_sub`, `donky_mul`, `donky_fdiv`). When both operands are statically known numeric, they inline `fsub`/`fmul`/`fdiv` + `donky_make_number` for speed.
- `/` is inline `fdiv` on raw `f64` bits. `x / 0` yields `±inf` or `NaN`
  (IEEE-754), never throws. This is the "raw IEEE" path.
- **`donky_div` (the builtin, and the `div` operator, §1.3) is different.**

### 1.3 `div` operator and `donky_div`

`a div b` is **checked integer division**:
- `number div number` → `trunc(a/b)` (an f64 with no fractional part).
  `7 div 2` is `3.0`. `-7 div 2` is `-3.0` (truncation toward zero).
- **`b == 0.0` (or `-0.0`) throws** `"division by zero"` and returns `null`.
- **Any non-number operand throws** `"cannot divide <ta> and <tb>"`.
- `donky_div` (the runtime function) is what the `div` operator and the
  `div(a, b)` builtin call.

`/` and `div` are **different operators**. `/` is raw IEEE float division
(`5 / 2 == 2.5`); `div` is checked integer division (`5 div 2 == 2.0`).

### 1.4 `%` operator and `donky_mod`

- `a % b` calls `donky_mod(a, b)`, which is `fmod(a, b)` (sign follows the
  dividend: `-7 % 3 == -1.0`, `7 % -3 == 1.0`).
- **`b == 0` throws** `"modulo by zero"` and returns `null`.
- **Any non-number operand throws** `"cannot modulo <ta> and <tb>"`.

### 1.5 Unary `-`

- `-n` for a number: `fneg` + `donky_make_number`. Result is a number.
- `-x` for a non-number: `fneg` on the raw bits, canonicalized to a number
  (a NaN). **Does not throw.** Wart listed in §11.

### 1.6 Unary `!`

- `!x` returns a **tagged boolean**: `true` if `donky_truthy(x)` is `0.0`,
  else `false`. Always a tagged boolean, never a number.

### 1.7 Comparisons `==`, `!=`, `<`, `<=`, `>`, `>=`

- **Numeric comparison** (`number == number`, etc.) compares f64 values by
  IEEE rules: `NaN == NaN` is `false`, `+0.0 == -0.0` is `true`.
- **`==` / `!=` on non-numbers** calls `donky_eq`:
  - `bool == bool` by value.
  - `string == string` by content (byte-for-byte, `strcmp`).
  - Anything else: **reference identity** on the raw words. Two distinct
    arrays are never `==` even if their contents are equal. A map is `==` to
    itself (same word). `null == null` is `true`.
  - Mixed types: `1 == "1"` is `false`. `true == 1` is `false`. `null == 0`
    is `false`.
- **`<`, `<=`, `>`, `>=`** are numeric-only via inline `fcmp`. On
  string operands they compare the raw words, which is meaningless (a known
  wart, §11). On other non-numbers they compare the raw words.
- All comparison results are **tagged booleans**.

---

## 2. Logic

### 2.1 `&&` and `||`

Short-circuit, returning a **tagged boolean**:
- `a && b`: evaluate `a`. If `a` is falsy (per §3), the result is the tagged
  boolean `false` and `b` is not evaluated. Otherwise evaluate `b`, and the
  result is `b` coerced to a tagged boolean via `donky_truthy`.
- `a || b`: evaluate `a`. If `a` is truthy, the result is `true` and `b` is
  not evaluated. Otherwise evaluate `b`, and the result is `b` coerced.

**`&&`/`||` return tagged booleans, not the operands.** `0 || "hi"` returns
`true`, not `"hi"`. This is unlike JavaScript and unlike the frozen
interpreter (§12).

### 2.2 Truthiness (`donky_truthy`)

`donky_truthy(v)` returns the number `1.0` or `0.0`:

| Value kind | Truthy iff |
|---|---|
| number | `v != 0.0` (NaN is truthy) |
| bool | the value is `true` |
| null | **never** (returns `0.0`) |
| string | **always**, including `""` |
| array | **always**, including `[]` |
| map | **always**, including `{}` |

This is the semantics used by `if`, `while`, `?:`, `&&`, `||`, `!`. It is the
most common source of confusion with other languages, where `""` and `[]` are
falsy; Donky follows Ruby/Python-`__bool__` style: **only `false`, `null`, and
numeric `0` are falsy.** Empty containers and empty strings are truthy.

---

## 3. Indexing, fields, and containers

### 3.1 `a[i]` and `donky_index_get`

| Target type | Index type | Result |
|---|---|---|
| array | integer-valued number in `[0, len)` | element |
| array | integer-valued number outside range | `null` |
| array | non-number, NaN, or non-integer | `null` |
| map | any key (number by value, string by content, others by identity) | value if present, else `null` |
| string | integer-valued number in `[0, byte_len)` | one-byte string |
| string | otherwise | `null` |
| number, bool, null, function | anything | `null` (the number `0.0` — see §11) |

`a[i]` **never throws** on out-of-range; it returns `null`. This is the
deliberate "missing key" convention.

### 3.2 `a[i] = v` and `donky_index_set`

| Target type | Index type | Result |
|---|---|---|
| array | integer in `[0, len)` | element replaced |
| array | non-integer number | **throws** `"array index must be an integer"` |
| array | integer out of range | **throws** `"index out of range"` |
| map | any key | set or insert |
| struct (map with `__type__`) | string key | set or insert |
| anything else | anything | silent no-op |

`a[i] = v` on an array **does not grow** the array. Use `push(a, v)` to
append. **Throws** are catchable by `try` (§0.3).

### 3.3 `a.f` and `a.f = v`

`a.f` is `donky_index_get(a, alloc_string("f"))`. `a.f = v` is
`donky_index_set(a, alloc_string("f"), v)`. Same rules as §3.1/§3.2.

### 3.4 Array builtins

| Function | Behavior |
|---|---|
| `len(a)` | element count. Non-array → number `0.0`. |
| `push(a, v)` | appends `v`, returns the number `0.0`. Non-array → number `0.0`. |
| `pop(a)` | removes and returns last element. Empty or non-array → number `0.0`. |
| `keys(m)` | array of keys in insertion order. Non-map → empty array. |
| `contains(haystack, needle)` | see §6.4. |

`pop([])` returns the number `0.0`, **not** `null`. This is deliberate — a
"no element" return of `0.0` is a defensible numeric default; a caller who
wants null can check `len(a) > 0` before popping.

### 3.5 Iteration

- `for x in arr { ... }` iterates over an array's elements. Non-array
  iterables are compile-time errors in the AOT (§11 for the exception —
  `for x in "abc"` iterates bytes, `for x in map` looks up numeric keys).
- `for i in 0 .. n { ... }` iterates `i = 0, 1, ..., n-1` (exclusive upper
  bound). `n` is evaluated **once** at loop entry.

---

## 4. Strings

Strings are **byte sequences**, not Unicode sequences. `len(s)` is the byte
length; `s[i]` yields one byte; `substr(s, a, b)` indexes bytes. UTF-8 is not
processed; a multi-byte character is several "characters" to Donky.

### 4.1 String builtins

| Function | Behavior |
|---|---|
| `str(v)` | v as a string. For a string, returns `v` itself (identity). For other types, uses §0.4/§0.5 rendering. Never throws. |
| `num(s)` | `s` parsed as an f64 by `atof` (leading whitespace and trailing garbage ignored: `"12abc"` → `12.0`; `"abc"` → `0.0`). Number input returns unchanged. Never throws. |
| `upper(s)`, `lower(s)` | ASCII case mapping. Non-string → number `0.0`. |
| `substr(s, a, b)`, `byte_substr` | byte sub-range. `a`, `b` clamped into `[0, len]`; if `a > b`, returns `""`. Non-string `s` → number `0.0`. |
| `byte_at(s, i)` | the byte value at `i`, or `-1` if out of range or non-number. |
| `char_code(s)` | first byte of `s` as a number; `0` if empty or non-string. |
| `bytes_to_str(arr)` | array of byte values → string. Non-array → number `0.0`. Bytes outside `[0,255]` → `'?'`. |
| `split(s, delim)` | **`delim` is a SET of delimiter characters, not a substring.** `split("a,b", ",")` → `["a", "b"]`; `split("a, b", ", ")` splits on `,` and ` ` individually. Empty tokens are dropped: `split("a,,b", ",")` → `["a", "b"]`. |
| `join(arr, delim)` | array of strings → single string with `delim` between elements. **Throws** `"join: non-string element at index N"` if any element is not a string. |
| `contains(s, sub)` | substring test if both are strings (§6.4 for the full rules). |

### 4.2 Interpolated strings

`"a ${x} b"` is desugared to `"a " + str(x) + " b"`. Each `str(x)` always
returns a string, so the `+` is always string+string and never throws.
Expression evaluation inside `${...}` follows the normal rules; if the inner
expression throws, the outer interpolation throws.

---

## 5. Null

`null` is the word `0x7FF8_0000_0000_0000`.

- `null == null` → tagged `true`
- `null == 0` → tagged `false` (different kinds)
- `if null { ... }` → false branch (null is falsy, §2.2)
- `str(null)` → `"null"`, `print(null)` prints `null`
- `type(null)` → `"null"`
- `null + 1` → **throws** `"cannot add null and number"` (null is not a
  number, not a string)
- `null[0]` → returns the number `0.0` (wart, §11)
- `null.foo` → `null` (same wart)

**A function that falls off the end returns `null`.** `fn f() {}; f() == null`
is tagged `true`.

**An undefined variable evaluates to `null`** at compile time (with an
`eprintln!` warning). This is the current behavior; §11 lists the fix
("true globals").

---

## 6. Builtin functions by return type

A user-defined top-level `fn max(...)` (etc.) takes priority over the builtin.
Note that this also means `lib/math.donk`'s `abs`/`min`/`max`/`pow` shadow the
builtins for any code that imports it — see §11.24.

**Worked example.** `lib/micrograd.donk` is a scalar reverse-mode autograd
engine built entirely on the primitives in this document (no `pow`/`tanh`/
`exp` builtins at the time it was written; it composes them from `+ - * /
floor`). `tests/micrograd_test.donk` trains a 2→4→1 MLP on XOR from loss
1.374 to 0.014 in 50 epochs and passes 30+ gradient checks including
shared-subgraph (diamond) correctness. This is the current proof that
Donky's arithmetic, arrays, function calls, and GC are sufficient for
real numerical work.

### 6.1 Return a number

`len`, `num`, `donky_num`, `byte_at`, `char_code`, `floor`, `now`, `now_ms`,
`random`, `sys_epoch_ms`, `sys_timegm`, `sys_gmtime` (returns an array, but
its elements are numbers), `total_mem`, `used_mem`, `cpu_usage`, `cpu_cores`,
`uptime`, `sys_exit` (does not return), `sleep`, `tcp_listen`,
`tcp_accept`, `tcp_read` (returns a string; listed here as an integer-returning
complement), `tcp_write`, `tcp_close`, `http_status`, `ffi_alloc` (returns an
address-as-number), `ffi_free`, `ffi_peek`, `ffi_poke`, `dll_load`, `dll_sym`,
`dll_call`, `dll_calli`, `spawn` (returns `0.0` on success, `-1.0` on failure).

### 6.2 Return a tagged boolean

All of these return `true`/`false` (§0.2):
`contains`, `file_exists`, `write_file` (returns `true` on success, `false`
on failure), `kbhit`, `sys_chdir`, `sys_setenv`, `sys_remove`, `sys_rename`,
`sys_append_file`, `kill_process`.

### 6.3 Return a string

`str`, `read_file`, `upper`, `lower`, `substr`, `byte_substr`, `bytes_to_str`,
`split` (returns an array), `join`, `keys` (returns an array — listed here
for completeness), `type`, `now` (a number — see §6.1), `json_encode`,
`json_pretty`, `json_error`, `http_get`, `http_request`, `sys_cwd`,
`sys_strftime`, `sys_md5`, `sys_sha1`, `sys_sha256`, `sys_hmac_sha256`,
`input`, `read_line`, `getch` (a number — see §6.1).

### 6.4 `contains` — full rules

`contains(haystack, needle)`:
- `haystack` is a **string**: returns `true` if `needle` is a string and is a
  substring of `haystack`; otherwise returns `false`.
- `haystack` is an **array**: returns `true` if any element `donky_eq`s
  `needle` (§1.7 reference identity rules apply — a number equals a number,
  a string equals a string, but two arrays are never `==`).
- `haystack` is a **map**: returns `true` if any **key** `donky_eq`s
  `needle`. Values are not searched.
- **Any other haystack type throws** `"contains: unsupported haystack type"`
  and returns `null`.

### 6.5 Return null on "nothing"

The following return `null` (§0.1) when there is genuinely no answer:
- `sys_getenv(name)` when the variable is unset
- `global_get(name)` when the name was never set
- `json_decode(s)` on JSON `null`, or on any parse error (check
  `json_error()` to distinguish)
- `a[i]` when out of range or missing (§3.1)
- `a.f` / `a[i]` on a non-indexable target (§3.1's last row — wart)
- any function that falls off the end (§5)

### 6.6 Return the number `0.0` on "no result"

The following return `0.0` (not null):
- `pop([])` and `pop(non_array)`
- `push(non_array, _)` and `push(array, _)` (returns `0.0`, not the array)
- `index_get` on a non-indexable target
- `len(non_container)`
- `num("nonnumeric")` (parses to `0.0`)
- `char_code("")`
- `non-string args to upper/lower/split/join/substr/bytes_to_str/read_file`
- `json_encode`/`json_pretty` on error (check `json_error()`)

---

## 7. Structs

A **struct** is syntactically declared with `struct Name { f: T, ... }`, but at
runtime it is **a map with an additional `__type__` entry** whose value is
the struct's name as a string.

`struct Point { x: int, y: int }` at top level in the AOT is a **no-op**
(the declaration is parsed and discarded). It does not register a type, does
not check field names, does not enforce types. It exists so the parser
accepts the syntax; struct semantics are not enforced at v2.3.

`Point { x: 1, y: 2 }` is `map_alloc()` followed by
`map_set(m, "__type__", "Point")`, `map_set(m, "x", 1)`, `map_set(m, "y", 2)`.

Consequences:
- `type(Point { x: 1, y: 2 })` returns `"Point"` (not `"map"`), because
  `donky_type` special-cases `__type__` (§6.3).
- `len(Point { x: 1, y: 2 })` returns `3` (includes `__type__`).
- `keys(Point { x: 1, y: 2 })` returns `["__type__", "x", "y"]` in insertion
  order.
- `json_encode(Point { x: 1, y: 2 })` encodes `{"__type__": "Point", "x": 1, "y": 2}`.
- `struct Foo` field-name typos are **not caught** — `Point { z: 3 }`
  produces a map with `__type__="Point"` and `z=3`, no error.
- Methods (`fn Point.method(self) { ... }`, called as `p.method()`) are **not
  implemented** in the AOT. §11 lists this.

This is a deliberate "structs are tagged maps" design. §11 lists the
alternative (a real `TAG_STRUCT` with a runtime type table) as a possible
future direction.

---

## 8. JSON

`json_decode(s)`:
- Parses RFC 8259 JSON. Objects → maps (string keys; later duplicates win).
  Arrays → arrays. Numbers → numbers. `true`/`false` → tagged booleans.
  **`null` → the canonical `null` word** (§0.1).
- On parse failure, returns `null` **and** sets `json_error()` to a message.
  To distinguish "parsed a real `null`" from "parse error", check
  `json_error() == ""` (empty string means success).
- Non-string argument: returns `null`, sets `json_error()`.
- UTF-8 BOM at the start of the input is skipped.
- Nesting depth > 2048 → returns `null`, sets `json_error()`.

`json_encode(v)`:
- Compact encoding. NaN/Inf → `null`. Integers `|n| < 1e15` exact; otherwise
  shortest round-trip. Strings escaped (`\" \\ \n \r \t \b \f`, other control
  chars `\u00xx`; bytes ≥ `0x80` passed through unmodified).
- Map keys: strings as-is, numbers stringified, others `"null"`.
- On error (depth > 2048 or a cyclic structure), returns the **number
  `0.0`** and sets `json_error()`. To distinguish a real `0` from an error,
  check `json_error() == ""`.

`json_pretty(v)`: same as `json_encode` with 2-space indentation.

`json_error()`: returns the last error message for the current thread, or
`""` if the last call succeeded.

---

## 9. Functions and function values

### 9.1 Named functions

`fn f(x) { ... }` at top level compiles to a native function
`donky_fn_f`. Calls (`f(1)`) become direct `call` instructions to the
native symbol. This works and is fast.

**Functions can only be declared at top level.** A `fn` nested inside
another function, an `if`, or a block is **silently discarded** at v2.3.
§11 lists this.

### 9.2 Function values (`Expr::Function`)

Functions are first-class values tagged with `TAG_FUNCTION` (6). 
- `let g = fn(x) { x + 1 }` stores a tagged function object in `g`.
- `type(g)` returns `"function"`.
- You can pass functions as arguments (e.g. `map(arr, double)`).
- When you call a variable as a function `g(2)`, the compiler packs the arguments into an array and calls `donky_call_function(g, args)`.
- Passing a function to `spawn` or `dll_callback` automatically unwraps the raw code address.

Consequences:
- `type(g)` returns `"number"` (not `"function"`).
- `g == g` is `true` (same bit pattern), but `g == h` for two distinct
  functions is also some bit pattern comparison. Do not compare functions.
- **`spawn(fn, arg)` works** because `spawn` receives a raw code address and
  calls it (via the thread pool's function-pointer cast). But it accepts
  *any* number and calls it, so passing a non-function `spawn(42, 0)` will
  segfault the process.
- **`let g = f` where `f` is a top-level function** stores `f`'s address in
  `g`. Calling `g(2)` calls `f(2)`. Works.

§11 lists "first-class functions with a real `TAG_FUNCTION`" as the largest
outstanding feature.

### 9.3 Arity checking

`aot.rs`'s `pad_args` pads missing arguments with the number `0.0` and
truncates extra arguments to the declared arity. **Calling `f(1, 2, 3)` on a
function declared `f(a)` silently calls `f(1)`.** §11 lists this.

---

## 10. Exceptions and `try` / `catch` / `throw`

### 10.1 What can throw

The following builtins **throw** on bad input (return `null` after throwing,
or `0` where noted):
- `donky_add` (type mismatch)
- `donky_sub`, `donky_mul`, `donky_div`, `donky_mod` (type mismatch)
- `donky_div`, `donky_mod` (division / modulo by zero)
- `donky_index_set` (non-integer index, out-of-range array index)
- `donky_contains` (unsupported haystack)
- `donky_join` (non-string element)
- `donky_read_file` (file not found, unreadable, non-string path)
- `donky_write_file` (non-string args, open failure — returns `false` on
  open failure, throws nothing; check `false`)

### 10.2 What cannot throw

`donky_eq`, `donky_truthy`, `donky_len`, `donky_push`, `donky_pop`,
`donky_keys`, `donky_str`, `donky_num`, `donky_type`, `donky_substr`,
`donky_split`, `donky_getenv`, `donky_global_get`, `donky_json_*`,
`donky_print`, all comparison operators, all logical operators.

### 10.3 Catchability

A throw is catchable by any enclosing `try`, no matter how deep the call
stack. `aot.rs` emits a `donky_has_exception` check after **every** call to
a function that can throw, after `+`, `%`, `div`, `a[i]`, `a[i]=v`, `a.f`,
`a.f=v`, and after `print`. If the check fires, control jumps to the
enclosing `try`'s catch block (or returns `null` from the current function
if there is no enclosing `try`).

**`-`, `*`, `/` (the operators) do not check for exceptions** — they cannot
throw, because they are inline `fsub`/`fmul`/`fdiv` on raw bits with no
type check. This is the A2 design: `/` is raw IEEE, `div` is checked.

### 10.4 Uncaught at exit

If a throw propagates past `donky_toplevel`, `main` prints
`Uncaught Exception: <value>` and exits with code 1.

---

## 11. Known limitations and open decisions

These are v2.3 facts. Each is a candidate for a future change; none of them
are bugs (the current behavior is intentional and documented) unless marked
**bug**.

1. **Arity is not checked.** Missing arguments are padded with `0.0`,
   extras are truncated.
12. **`null[i]` and `null.f` return the number `0.0`, not `null`.**
13. **`donky_div` truncates**, matching the interpreter's `div`. `7 div 2`
    is `3.0`, not `3.5`. This is intentional (decision A2).
14. **`donky_index_get` on a non-indexable target (number, bool, null,
    function) returns the number `0.0`, not `null`.** Wart. Should probably
    be `null` in a future version.
15. **`donky_json_decode("null")` returns `null`.** This was changed in the
    semantics pass (B2). It used to return `0.0`.
16. **`donky_sys_getenv` on an unset variable returns `null`.** Same pass.
17. **`donky_global_get` before any set returns `null`.** Same pass.
18. **`donky_json_encode` / `donky_json_pretty` return the number `0.0` on
    error**, not `null`. Check `json_error()` to distinguish.
19. **Inline `fadd`/`fsub`/`fmul`/`fdiv` route through `donky_make_number`**,
    so `NaN` is always canonicalized. This costs a C call per op. If it shows
    up in benchmarks, the canonicalization can be inlined (a `select` against
    `TAG_BASE`) in a future AOT pass.
20. **Import resolution is silent on failure.** `resolve_imports` prints a
    `DEBUG:` line and continues if an `import` path can't be found. The
    failure surfaces later as `unknown function 'X'` at compile time, not as
    an import error. Import paths resolve against the **build's working
    directory** (`"."`), not against the importing file's directory. Build
    from the repo root.
21. **`donky test` does not exercise the AOT.** `cmd_test` in `main.rs` runs
    each `tests/*.donk` file through the frozen VM (`vm.rs`), not the AOT.
    Tests that depend on AOT-only behavior (e.g. the semantics in this
    document) must be built with `build-exe` and run as a standalone binary.
22. **There is no automatic GC.** `gc_collect` is reached only via the `gc()`
    builtin (not exposed as a `STDLIB_BUILTINS` entry; available as the
    `gc` builtin in `aot.rs`'s dispatch). A training loop or any hot loop
    that allocates fresh arrays/maps will grow the heap until the process
    exits. Call `gc()` manually at a safe point if allocation is unbounded.
23. **`Expr::Ternary` and `Expr::MethodCall` do compile in AOT** (verified:
    `Ternary` at `aot.rs` `compile_expr`'s `Expr::Ternary` arm; `MethodCall`
    desugars to `Expr::Call(method, [obj, args...])` before codegen). A
    previous report claimed they don't — that report was based on an
    outdated `aot.rs`. `a > b ? x : y` and `obj.method(args)` both work.
24. **`lib/math.donk` shadows builtins.** `lib/math.donk` defines `abs`,
    `min`, `max`, `pow`, and `floor` as top-level `fn`. Since user-defined
    functions resolve before builtins (§6.1.1), any code that imports
    `lib/math.donk` gets the Donky versions, not the C-runtime builtins.
    The Donky versions are correct for small inputs but `pow` is O(e) (a
    loop, not exponentiation-by-squaring) and `sqrt` (also in that file) is
    Newton-from-above and only converges for `n` near 1 in 20 iterations.
    Preferred: don't import `lib/math.donk` and use the builtins directly.
    A cleanup pass renames these to `math_*` — see the file's header.

---

## 14. Native Interop & System Programming

Donky is a C-parity systems language. The runtime exposes raw memory, typed FFI, process control, and POSIX security.

### 14.1 Executable memory and JIT
`mem_alloc(size)` allocates read/write memory. `mem_write(ptr, bytes)` copies bytes. `mem_protect(ptr, size, prot)` changes protection (1=RW, 2=RX, 3=RWX). You can write x86 machine code and execute it via `dll_call_sig`.

### 14.2 Raw syscalls
`syscall(nr, args)` invokes a raw OS syscall (POSIX only). `args` is an array of up to 6 numbers or strings. Returns the raw signed result or `-1`.

### 14.3 Process Control
`fork()`, `exec(path, args)`, `wait(pid)`, `pipe()`, `dup2(old, new)`, `mmap(len, prot, flags)`, `munmap(ptr, len)`. POSIX only. On Windows, use `dll_call_sig` with `CreateProcessA`.

### 14.4 Security and Privilege (POSIX)
`setuid(uid)`, `setgid(gid)`, `seteuid(uid)`, `setegid(gid)`, `chroot(path)`, `unshare(flags)`, `prctl(...)`, `seccomp(op, flags, arg)`. Windows stubs throw an error.

### 14.5 Typed FFI (libffi)
`dll_call_sig(fn, ret_sig, arg_sig, args)` calls any C function with exact types.
- Type letters: `v` void, `i` int32, `u` uint32, `l` int64, `L` uint64, `f` float, `d` double, `s` char*, `p` void*.
- `dll_callback(ret_sig, arg_sig, func)` creates a C function pointer that calls a Donky function.
- `dll_load` / `dll_sym` locate the library and function address.

### 14.6 OS Identity (POSIX)
`getpwnam(name)` / `getpwuid(uid)` return `{name, uid, gid, dir, shell}`. `uname()` returns OS info. `getrlimit` / `setrlimit` manage process resources.

## 12. Divergences from the frozen engines

`interpreter.rs` and `vm.rs` are frozen and non-normative. Where they
differ from this document, **this document wins.** A future cleanup pass may
reconcile them or delete them; for now, treat them as a historical reference
for "what Donky might have been," not "what Donky is."

| Rule | This document | `interpreter.rs` |
|---|---|---|
| `a[99]` on `[1,2,3]` | `null` | error |
| `m["missing"]` | `null` | error |
| `s[99]` | `null` | error; also indexes by Unicode char, not byte |
| `1 + "a"` | throws | error |
| `"a" - 1` | NaN (no throw) | error |
| `x / 0` | `inf`/`nan` | error |
| `x div 0` | throws | error |
| `7 div 2` | `3.0` | `3` (truncated) |
| `x % 0` | throws | error |
| `x % y` (general) | `fmod` | error (interpreter has no `%`) |
| `!5` | tagged `false` (via `truthy`) | error (`Bang` requires bool) |
| `1 < 2` printed | `true` | (depends on `heap.rs`) |
| `donky_eq(true, 1 < 2)` | `1.0` | (depends on `heap.rs`) |
| `str([1, "a"])` | `[1, "a"]` | `[1, a]` (unquoted) |
| `join([1,2], ",")` | throws | error |
| `split("a,,b", ",")` | `["a", "b"]` (drop empty) | `["a", "", "b"]` (keep empty) |
| `type(Point{x:1})` | `"Point"` | `"struct"` |
| `type(null)` | `"null"` | error (no `null` literal) |
| `null == m["missing"]` | `true` (both null) | error (lookup fails) |
| `if m["missing"] {}` | false branch (null falsy) | error |
| `throw 0` in `try` | silent no-op | caught normally |
| `throw null` in `try` | caught (tagged null) | N/A |
| `a[3] = v` on `[1,2,3]` | throws | error |
| top-level `let x = 1; fn f() { print x; }` | prints `null` (warning) | prints `1` |
| `now_ms()` | ms since boot | error (no builtin) |
| `contains(42, "a")` | throws | error |
| `for x in map` | `map[0], map[1], ...` (numeric keys) | error (only arrays) |
| `f(1,2,3)` on `f(a)` | calls `f(1)` (silent) | error |
| `match` comparison | works on numbers, strings, bools; reference on everything else | error on non-primitive |
| `int` type | no int type; all numbers are f64 | same |

**Frozen `codegen.rs` (the JIT)** is even further from this document — it
has its own value model, its own `TAG_*` constants (which contradict
`runtime_template.c`'s), its own `NativeObj` enum without `Bool`/`Null`, and
its own `print_value` that doesn't know about booleans. It is not on the
shipped path. Do not use it as a reference.

---

## 13. Versioning

This document describes **Donky v2.3, semantics-pass-revision-4**. Any change
to the observable behavior of a builtin, operator, or language construct
requires bumping the revision and updating §11.

**Changelog:**

- **v2.3 semantics-pass-revision-4** (current):
  - Added First-Class Functions (`TAG_FUNCTION`). Functions can be passed as arguments and called indirectly.
  - Added Typed FFI (`libffi`): `dll_call_sig` and `dll_callback`.
  - Added Executable Memory: `mem_alloc`, `mem_write`, `mem_protect`, `mem_free`.
  - Added Process Control: `fork`, `exec`, `wait`, `pipe`, `dup2`, `mmap`.
  - Added Security/OS Config: `setuid`, `chroot`, `seccomp`, `uname`, `getpwnam`, etc.
  - Fixed `throw 0` (sentinel is now `UINT64_MAX`).
  - Fixed silent NaN on `-`, `*`, `/` (now throw on type mismatch via `donky_sub`, `donky_mul`, `donky_fdiv`).
- **v2.3 semantics-pass-revision-3**:
  - Added math builtins `pow`, `exp`, `log`, `tanh`, `max`, `min`, `abs`
...